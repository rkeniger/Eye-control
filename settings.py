import os
from dataclasses import dataclass

from dotenv import load_dotenv

load_dotenv()


@dataclass(frozen=True)
class Settings:
    rtsp_url: str = os.getenv("RTSP_URL", "rtsp://scrypted.local:43705/27de8bbd9f1f48a5")
    model: str = os.getenv("MODEL", "yolo11s.pt")
    imgsz: int = int(os.getenv("IMGSZ", "1280"))
    conf: float = float(os.getenv("CONF", "0.25"))
    preview_width: int = int(os.getenv("PREVIEW_WIDTH", "1280"))
    jpeg_quality: int = int(os.getenv("JPEG_QUALITY", "75"))
    max_fps: float = float(os.getenv("MAX_FPS", "15"))
    # ESP32 eyeball; empty disables eye control
    eye_url: str = os.getenv("EYE_URL", "http://eyeball.local")
    eye_send_hz: float = float(os.getenv("EYE_SEND_HZ", "15"))
    calibration_path: str = os.getenv("CALIBRATION_PATH", "calibration.json")
    # polygon outside which people are ignored, drawn on the page
    zone_path: str = os.getenv("ZONE_PATH", "zone.json")
    host: str = os.getenv("HOST", "0.0.0.0")
    port: int = int(os.getenv("PORT", "8000"))


settings = Settings()
