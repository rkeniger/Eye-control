"""RTSP ingest -> YOLO person tracking -> JPEG + box metadata fan-out to WebSocket viewers.

Threads:
  reader  pulls frames from RTSP, keeping only the newest one (no backlog = no lag build-up)
  worker  runs detection/tracking on the newest frame, encodes a preview, publishes it

The pipeline only runs while at least one viewer is subscribed (plus a short grace period).
"""

import asyncio
import json
import logging
import os
import struct
import threading
import time
from dataclasses import dataclass, field

# Must be set before OpenCV opens any FFmpeg capture.
os.environ.setdefault("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp")

import cv2  # noqa: E402
import torch  # noqa: E402
from ultralytics import YOLO  # noqa: E402

from settings import Settings  # noqa: E402

log = logging.getLogger("pipeline")

PERSON_CLASS = 0
MAX_BACKOFF_SECONDS = 10.0


class LatestFrame:
    """Single-slot frame buffer: writers overwrite, readers wait for something newer."""

    def __init__(self):
        self._cond = threading.Condition()
        self._seq = 0
        self._item = None

    def put(self, frame, captured_at: float, stream_gen: int):
        with self._cond:
            self._seq += 1
            self._item = (self._seq, frame, captured_at, stream_gen)
            self._cond.notify_all()

    def wait_newer(self, seq: int, timeout: float):
        with self._cond:
            self._cond.wait_for(lambda: self._seq > seq, timeout=timeout)
            return self._item if self._seq > seq else None


@dataclass
class Session:
    stop: threading.Event = field(default_factory=threading.Event)
    frames: LatestFrame = field(default_factory=LatestFrame)
    stream_gen: int = 0


class Pipeline:
    def __init__(self, settings: Settings):
        self.settings = settings
        self.device = "mps" if torch.backends.mps.is_available() else "cpu"
        self._model: YOLO | None = None
        self._model_lock = threading.Lock()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._subscribers: set[asyncio.Queue] = set()
        self._session: Session | None = None
        self._stop_timer: asyncio.TimerHandle | None = None
        self._status = {"type": "status", "state": "idle"}

    # ---- model -------------------------------------------------------------

    def load_model(self):
        """Load and warm up the model (blocking). Safe to call more than once."""
        with self._model_lock:
            if self._model is None:
                log.info("Loading %s on %s", self.settings.model, self.device)
                model = YOLO(self.settings.model)
                model.predict(
                    torch.zeros(1, 3, 640, 640), device=self.device, verbose=False
                )
                self._model = model
        return self._model

    # ---- subscription / lifecycle (event loop thread) ----------------------

    def subscribe(self) -> asyncio.Queue:
        self._loop = asyncio.get_running_loop()
        if self._stop_timer:
            self._stop_timer.cancel()
            self._stop_timer = None
        if self._session is None:
            self._start()
        queue: asyncio.Queue = asyncio.Queue(maxsize=2)
        queue.put_nowait(json.dumps(self._status))
        self._subscribers.add(queue)
        return queue

    def unsubscribe(self, queue: asyncio.Queue):
        self._subscribers.discard(queue)
        if not self._subscribers and self._session and not self._stop_timer:
            self._stop_timer = self._loop.call_later(
                self.settings.idle_stop_seconds, self._stop_if_idle
            )

    def shutdown(self):
        if self._session:
            self._session.stop.set()
            self._session = None

    def _start(self):
        log.info("Starting pipeline")
        session = Session()
        self._session = session
        self._status = {"type": "status", "state": "connecting", "detail": ""}
        threading.Thread(target=self._reader, args=(session,), daemon=True).start()
        threading.Thread(target=self._worker, args=(session,), daemon=True).start()

    def _stop_if_idle(self):
        self._stop_timer = None
        if not self._subscribers and self._session:
            log.info("No viewers for %ss, stopping pipeline", self.settings.idle_stop_seconds)
            self._session.stop.set()
            self._session = None
            self._status = {"type": "status", "state": "idle"}

    # ---- publishing (any thread) -------------------------------------------

    def _publish(self, message: bytes | str):
        if self._loop:
            self._loop.call_soon_threadsafe(self._fanout, message)

    def _fanout(self, message: bytes | str):
        for queue in self._subscribers:
            if queue.full():
                queue.get_nowait()  # slow viewer: drop its oldest message
            queue.put_nowait(message)

    def _set_status(self, session: Session, state: str, detail: str = ""):
        if session.stop.is_set():
            return
        self._status = {"type": "status", "state": state, "detail": detail}
        log.info("Status: %s %s", state, detail)
        self._publish(json.dumps(self._status))

    # ---- threads -----------------------------------------------------------

    def _reader(self, session: Session):
        backoff = 1.0
        while not session.stop.is_set():
            self._set_status(session, "connecting")
            cap = cv2.VideoCapture(
                self.settings.rtsp_url,
                cv2.CAP_FFMPEG,
                [cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, 5000, cv2.CAP_PROP_READ_TIMEOUT_MSEC, 5000],
            )
            if cap.isOpened():
                session.stream_gen += 1
                backoff = 1.0
                while not session.stop.is_set():
                    ok, frame = cap.read()
                    if not ok:
                        break
                    session.frames.put(frame, time.time(), session.stream_gen)
            cap.release()
            if session.stop.is_set():
                break
            self._set_status(session, "reconnecting", f"retrying in {backoff:.0f}s")
            session.stop.wait(backoff)
            backoff = min(backoff * 2, MAX_BACKOFF_SECONDS)
        log.info("Reader stopped")

    def _worker(self, session: Session):
        model = self.load_model()
        s = self.settings
        last_seq = 0
        live_gen = 0
        first_call = True
        fps = 0.0
        last_done = None

        while not session.stop.is_set():
            item = session.frames.wait_newer(last_seq, timeout=0.5)
            if item is None:
                continue
            last_seq, frame, captured_at, stream_gen = item
            started = time.time()

            with self._model_lock:
                # persist=False on the first call of a session gives a fresh tracker.
                result = model.track(
                    frame,
                    persist=not first_call,
                    classes=[PERSON_CLASS],
                    imgsz=s.imgsz,
                    conf=s.conf,
                    device=self.device,
                    tracker="bytetrack.yaml",
                    verbose=False,
                )[0]
            first_call = False

            people = []
            boxes = result.boxes
            if boxes is not None and boxes.id is not None:
                for xyxyn, track_id, conf in zip(
                    boxes.xyxyn.tolist(), boxes.id.int().tolist(), boxes.conf.tolist()
                ):
                    people.append(
                        {"id": track_id, "conf": round(conf, 3), "box": [round(v, 4) for v in xyxyn]}
                    )

            h, w = frame.shape[:2]
            if w > s.preview_width:
                frame = cv2.resize(
                    frame, (s.preview_width, round(h * s.preview_width / w)), interpolation=cv2.INTER_AREA
                )
            ok, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, s.jpeg_quality])
            if not ok:
                continue

            now = time.time()
            if last_done is not None:
                instant = 1.0 / max(now - last_done, 1e-6)
                fps = 0.9 * fps + 0.1 * instant if fps else instant
            last_done = now

            if stream_gen != live_gen:
                live_gen = stream_gen
                self._set_status(session, "live")

            meta = json.dumps(
                {
                    "type": "frame",
                    "people": people,
                    "fps": round(fps, 1),
                    "lag_ms": round((now - captured_at) * 1000),
                }
            ).encode()
            # Binary message: [uint32 LE meta length][meta JSON][JPEG bytes]
            self._publish(struct.pack("<I", len(meta)) + meta + jpeg.tobytes())

            if s.max_fps > 0:
                session.stop.wait(max(0.0, 1.0 / s.max_fps - (time.time() - started)))
        log.info("Worker stopped")
