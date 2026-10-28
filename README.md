# eye-control — live people tracker

Pulls the Scrypted RTSP camera stream, detects and tracks people with YOLO11s (on the Apple GPU via MPS) and ByteTrack, and serves a browser page with the live preview, labelled boxes, ~2 s motion trails, a person count and a confidence slider.

## Run

```sh
uv run main.py
```

Open http://localhost:8000 (or `http://<this-mac>:8000` from another device on the LAN). There is no authentication.

The pipeline only runs while a page is open. It starts on the first viewer and stops `IDLE_STOP_SECONDS` after the last one leaves. If the stream drops, it reconnects with backoff (1 → 10 s) and the page shows a banner.

## Configuration

Copy `.env.example` to `.env` and adjust. Key settings: `RTSP_URL`, `MODEL`, `IMGSZ` (inference size), `CONF`, `PREVIEW_WIDTH`, `MAX_FPS`, `PORT`.

Each preview frame is roughly 300 KB at 1280 px wide, so at 15 fps that's about 35 Mbit/s per viewer. Lower `PREVIEW_WIDTH` or `MAX_FPS` for viewers on Wi-Fi.

## How it works

- `pipeline.py`: a reader thread keeps only the newest RTSP frame. A worker thread runs `model.track` (people only), downscales and JPEG-encodes the frame, and publishes one binary WebSocket message per frame: `[uint32 LE meta length][meta JSON][JPEG]`. Box coordinates are normalised to 0–1.
- `main.py`: FastAPI. `/ws` streams to each viewer through a 2-slot queue that drops old frames for slow clients. `static/` serves the page.
- `static/index.html`: draws each frame and its own boxes together on a canvas, so the boxes always line up with the image.

Ultralytics is AGPL-3.0, which is fine for personal use.
