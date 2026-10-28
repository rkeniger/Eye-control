"""Point the ESP32 eyeball at the tracked person.

Per frame (worker thread): pick a target person with hysteresis, take a face-height anchor point on
their box, smooth it, and map it to eye yaw/pitch using point-and-teach calibration.
An asyncio task streams the latest command to the ESP32 over WebSocket and receives its status.
"""

import asyncio
import json
import logging
import os
import tempfile
import threading
import time
from pathlib import Path

import websockets

log = logging.getLogger("eye")

ANCHOR_FROM_TOP = 0.15     # fraction down the box: roughly the face
SMOOTHING = 0.5            # EMA weight of the newest anchor point
ABSENT_INTERVAL = 0.5      # how often to repeat {"present": false}
TEACH_TIMEOUT = 60.0       # teach mode ends if the page stops sending aims
MIN_TEACH_POINTS = 3
REPLACE_RADIUS = 0.03      # saving within this distance (image fraction) of a point replaces it
# Used until enough points are taught: image edges map to these angles.
DEFAULT_YAW_RANGE = 30.0
DEFAULT_PITCH_RANGE = 20.0


class TargetSelector:
    """Stick with the current person; switch only when someone is clearly more confident for a while,
    or the current person has been gone for a moment."""

    def __init__(self, margin: float = 0.15, switch_after: float = 1.0, lost_after: float = 0.5):
        self.margin = margin
        self.switch_after = switch_after
        self.lost_after = lost_after
        self.current_id: int | None = None
        self._last_seen = 0.0
        self._challenger: int | None = None
        self._challenger_since = 0.0

    def update(self, people: list[dict], now: float) -> tuple[dict | None, bool, bool]:
        """Returns (target, switched, holding). `holding` means the target just vanished and we're
        waiting out the grace period before giving up on them."""
        best = max(people, key=lambda p: p["conf"], default=None)
        current = next((p for p in people if p["id"] == self.current_id), None)

        if current:
            self._last_seen = now
            if best["id"] != current["id"] and best["conf"] >= current["conf"] + self.margin:
                if self._challenger != best["id"]:
                    self._challenger, self._challenger_since = best["id"], now
                elif now - self._challenger_since >= self.switch_after:
                    return self._switch(best, now), True, False
            else:
                self._challenger = None
            return current, False, False

        if self.current_id is not None and now - self._last_seen < self.lost_after:
            return None, False, True
        if best:
            return self._switch(best, now), True, False
        self.current_id = None
        return None, False, False

    def _switch(self, person: dict, now: float) -> dict:
        self.current_id = person["id"]
        self._last_seen = now
        self._challenger = None
        return person


class GazeCalibration:
    """Taught pairs of image anchor point (u, v in 0..1) -> eye angles, blended by inverse distance."""

    def __init__(self, path: str):
        self.path = Path(path)
        self.points: list[dict] = []
        if self.path.exists():
            try:
                self.points = json.loads(self.path.read_text())["points"]
            except (ValueError, KeyError) as e:
                log.warning("Ignoring unreadable calibration %s: %s", self.path, e)

    @property
    def ready(self) -> bool:
        return len(self.points) >= MIN_TEACH_POINTS

    def map(self, u: float, v: float) -> tuple[float, float]:
        if not self.ready:
            return (u - 0.5) * 2 * DEFAULT_YAW_RANGE, (0.5 - v) * 2 * DEFAULT_PITCH_RANGE
        total = yaw = pitch = 0.0
        for p in self.points:
            d2 = (p["u"] - u) ** 2 + (p["v"] - v) ** 2
            if d2 < 1e-8:
                return p["yaw"], p["pitch"]
            w = 1.0 / d2
            total += w
            yaw += w * p["yaw"]
            pitch += w * p["pitch"]
        return yaw / total, pitch / total

    def add(self, u: float, v: float, yaw: float, pitch: float):
        """Add a point, replacing any existing one at (almost) the same spot."""
        point = {"u": round(u, 4), "v": round(v, 4), "yaw": round(yaw, 1), "pitch": round(pitch, 1)}
        self.points = [p for p in self.points if (p["u"] - u) ** 2 + (p["v"] - v) ** 2 > REPLACE_RADIUS**2]
        self.points.append(point)
        self._save()

    def remove(self, index: int):
        del self.points[index]
        self._save()

    def clear(self):
        self.points = []
        self._save()

    def _save(self):
        fd, tmp = tempfile.mkstemp(dir=self.path.parent.resolve(), suffix=".tmp")
        with os.fdopen(fd, "w") as f:
            json.dump({"points": self.points}, f, indent=2)
        os.replace(tmp, self.path)


def anchor(person: dict) -> tuple[float, float]:
    x1, y1, x2, y2 = person["box"]
    return (x1 + x2) / 2, y1 + ANCHOR_FROM_TOP * (y2 - y1)


class EyeController:
    def __init__(self, url: str, calibration_path: str, send_hz: float):
        self.enabled = bool(url)
        self.ws_url = url.replace("https://", "wss://").replace("http://", "ws://").rstrip("/") + "/ws"
        self.send_interval = 1.0 / send_hz
        self.selector = TargetSelector()
        self.calibration = GazeCalibration(calibration_path)
        self.connected = False
        self.eye_status: dict | None = None

        self._lock = threading.Lock()
        self._target_id: int | None = None
        self._anchor: tuple[float, float] | None = None  # smoothed, for tracking and teaching
        self._gaze: tuple[float, float] | None = None
        self._new_target = False
        self._teach_until = 0.0
        self._teach_gaze = (0.0, 0.0)

    # ---- worker thread -----------------------------------------------------

    def on_people(self, people: list[dict], captured_at: float) -> dict:
        target, switched, holding = self.selector.update(people, time.monotonic())
        with self._lock:
            if target:
                u, v = anchor(target)
                if switched or self._anchor is None:
                    self._anchor = (u, v)
                else:
                    au, av = self._anchor
                    self._anchor = (au + SMOOTHING * (u - au), av + SMOOTHING * (v - av))
                self._target_id = target["id"]
                self._gaze = self.calibration.map(*self._anchor)
                self._new_target = self._new_target or switched
            elif not holding:
                self._target_id = None
                self._anchor = None
                self._gaze = None
            return {"target": self._target_id, "eye": self._summary()}

    # ---- page API (event loop) ---------------------------------------------

    @property
    def teaching(self) -> bool:
        return time.monotonic() < self._teach_until

    def set_teach(self, active: bool):
        with self._lock:
            self._teach_until = time.monotonic() + TEACH_TIMEOUT if active else 0.0
            if active and self.eye_status:
                self._teach_gaze = (self.eye_status.get("yaw", 0.0), self.eye_status.get("pitch", 0.0))

    def teach_aim(self, yaw: float, pitch: float):
        with self._lock:
            self._teach_until = time.monotonic() + TEACH_TIMEOUT
            self._teach_gaze = (yaw, pitch)

    def save_teach_point(self) -> str | None:
        """Pair the current target's anchor with the taught angles. Returns an error message or None."""
        with self._lock:
            if not self.teaching:
                return "Teach mode is not active"
            if self._anchor is None:
                return "No person in view to pair with"
            self.calibration.add(*self._anchor, *self._teach_gaze)
            self._teach_until = time.monotonic() + TEACH_TIMEOUT
        return None

    def remove_point(self, index: int) -> bool:
        with self._lock:
            if not 0 <= index < len(self.calibration.points):
                return False
            self.calibration.remove(index)
            return True

    def clear_points(self):
        with self._lock:
            self.calibration.clear()

    def state(self) -> dict:
        with self._lock:
            return {
                **self._summary(),
                "target": self._target_id,
                "anchor": self._anchor,
                "teachGaze": self._teach_gaze,
                "points": self.calibration.points,
                "minPoints": MIN_TEACH_POINTS,
                "status": self.eye_status,
            }

    def _summary(self) -> dict:
        s = self.eye_status or {}
        return {
            "enabled": self.enabled,
            "connected": self.connected,
            "mode": s.get("mode"),
            "state": s.get("state"),
            "limits": s.get("limits"),
            "teaching": self.teaching,
            "calibrated": self.calibration.ready,
        }

    # ---- link to the ESP32 -------------------------------------------------

    def _next_message(self, now: float, last_absent: float) -> dict | None:
        with self._lock:
            if self.teaching:
                yaw, pitch = self._teach_gaze
                return {"yaw": round(yaw, 1), "pitch": round(pitch, 1)}
            if self._gaze is not None:
                msg = {"yaw": round(self._gaze[0], 1), "pitch": round(self._gaze[1], 1)}
                if self._new_target:
                    msg["newTarget"] = True
                    self._new_target = False
                return msg
            absent = self._target_id is None
        if absent and now - last_absent >= ABSENT_INTERVAL:
            return {"present": False}
        return None

    async def _receive(self, ws):
        async for raw in ws:
            try:
                msg = json.loads(raw)
            except ValueError:
                continue
            if msg.get("type") == "status":
                self.eye_status = msg

    async def run(self):
        if not self.enabled:
            log.info("EYE_URL is empty; eye control disabled")
            return
        backoff = 1.0
        while True:
            try:
                async with websockets.connect(self.ws_url, open_timeout=5, ping_interval=10) as ws:
                    log.info("Connected to eye at %s", self.ws_url)
                    self.connected = True
                    backoff = 1.0
                    receiver = asyncio.create_task(self._receive(ws))
                    last_absent = 0.0
                    try:
                        while not receiver.done():
                            now = time.monotonic()
                            msg = self._next_message(now, last_absent)
                            if msg is not None:
                                if "present" in msg:
                                    last_absent = now
                                await ws.send(json.dumps(msg))
                            await asyncio.sleep(self.send_interval)
                    finally:
                        receiver.cancel()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                if self.connected or backoff == 1.0:
                    log.warning("Eye link to %s unavailable: %s", self.ws_url, e)
            self.connected = False
            self.eye_status = None
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 10.0)
