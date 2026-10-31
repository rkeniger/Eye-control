"""The detection zone: a polygon in normalised image coordinates. People standing outside it are ignored."""

import json
import logging
import os
import tempfile
import threading
from pathlib import Path

log = logging.getLogger("zone")


def feet(person: dict) -> tuple[float, float]:
    """Where a person is standing: the bottom centre of their box."""
    x1, _, x2, y2 = person["box"]
    return (x1 + x2) / 2, y2


def inside(points: list[list[float]], x: float, y: float) -> bool:
    """Even-odd ray cast."""
    hit = False
    for (x1, y1), (x2, y2) in zip(points, points[-1:] + points[:-1]):
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
            hit = not hit
    return hit


class Zone:
    """Fewer than 3 points means no zone: the whole frame counts."""

    def __init__(self, path: str):
        self.path = Path(path)
        self._lock = threading.Lock()
        self._points: list[list[float]] = []
        if self.path.exists():
            try:
                self._points = json.loads(self.path.read_text())["points"]
            except (ValueError, KeyError) as e:
                log.warning("Ignoring unreadable zone %s: %s", self.path, e)

    @property
    def points(self) -> list[list[float]]:
        with self._lock:
            return self._points

    def set(self, points: list[list[float]]):
        points = [[round(min(1.0, max(0.0, x)), 4), round(min(1.0, max(0.0, y)), 4)] for x, y in points]
        if len(points) < 3:
            points = []
        with self._lock:
            self._points = points
            fd, tmp = tempfile.mkstemp(dir=self.path.parent.resolve(), suffix=".tmp")
            with os.fdopen(fd, "w") as f:
                json.dump({"points": points}, f, indent=2)
            os.replace(tmp, self.path)

    def split(self, people: list[dict]) -> tuple[list[dict], list[dict]]:
        """(inside, outside)"""
        points = self.points
        if not points:
            return people, []
        kept, ignored = [], []
        for p in people:
            (kept if inside(points, *feet(p)) else ignored).append(p)
        return kept, ignored
