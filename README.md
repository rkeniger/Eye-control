# eye-control — people tracker + eyeball automaton

Pulls the Scrypted RTSP camera stream and detects and tracks people with YOLO11s (on the Apple GPU via MPS) and ByteTrack. It serves a browser page with the live preview, labelled boxes, ~2 s motion trails, a person count and a confidence slider. It also points a 3-servo eyeball, driven by an ESP32-S3, at the tracked person (see [Eyeball](#eyeball)).

## Run

```sh
uv run main.py
```

Open http://localhost:8000 (or `http://<this-mac>:8000` from another device on the LAN). There is no authentication.

Detection runs continuously while the server is up. Preview frames are only encoded and sent while a page is open. If the stream drops, it reconnects with backoff (1 → 10 s) and the page shows a banner.

## Configuration

Copy `.env.example` to `.env` and adjust. Key settings: `RTSP_URL`, `MODEL`, `IMGSZ` (inference size), `CONF`, `PREVIEW_WIDTH`, `MAX_FPS`, `PORT`, `EYE_URL` (empty disables the eye), `CALIBRATION_PATH`.

Each preview frame is roughly 300 KB at 1280 px wide, so at 15 fps that's about 35 Mbit/s per viewer. Lower `PREVIEW_WIDTH` or `MAX_FPS` for viewers on Wi-Fi.

## How it works

- `pipeline.py`: a reader thread keeps only the newest RTSP frame. A worker thread runs `model.track` (people only), downscales and JPEG-encodes the frame, and publishes one binary WebSocket message per frame: `[uint32 LE meta length][meta JSON][JPEG]`. Box coordinates are normalised to 0–1.
- `main.py`: FastAPI. `/ws` streams to each viewer through a 2-slot queue that drops old frames for slow clients. `static/` serves the page.
- `static/index.html`: draws each frame and its own boxes together on a canvas, so the boxes always line up with the image.

Ultralytics is AGPL-3.0, which is fine for personal use.

## Eyeball

```
tracker (Mac) ── WebSocket ~15 Hz {yaw,pitch} | {present:false} ──► ESP32-S3 (eyeball.local) ──► 3× SG90
```

The Mac chooses who to look at and where. The ESP32 handles motion, blinking, idle behaviour and safety on its own, so the eye behaves sensibly even if the Mac or Wi-Fi drops.

### Wiring (ESP32-S3-WROOM-1 N16R8)

| From | To |
|---|---|
| 5 V ≥2 A supply (+) | Red wire of all three SG90s |
| 5 V supply (−) | Brown wire of all three SG90s **and** an ESP32 GND pin (shared ground is required) |
| 1000 µF / ≥10 V electrolytic | Across the servo 5 V/GND rail, near the servos (stripe = −) |
| GPIO4 (optional 330 Ω in series) | Yaw servo signal (orange) |
| GPIO5 (optional 330 Ω) | Pitch servo signal |
| GPIO6 (optional 330 Ω) | Eyelid servo signal |
| ESP32 power | USB (the UART/CH343 port), **or** the 5 V supply into the 5V pin, never both at once |

Don't power the servos from the board's 3V3 pin. GPIO 33–37 are used by the octal PSRAM on this module.

### Flash the firmware

PlatformIO's espressif32 platform needs Python ≤3.13, so run it through `uvx`:

```sh
cd firmware
uvx --python 3.12 platformio run -t upload      # firmware
uvx --python 3.12 platformio run -t uploadfs    # web UI (data/ -> LittleFS)
uvx --python 3.12 platformio device monitor     # serial log
```

### First-time setup

1. Power it on. With no saved Wi-Fi it starts an **Eyeball-Setup** access point. Join it from a phone, and the setup page opens (otherwise go to http://192.168.4.1). Pick your network and enter the password. It restarts and joins.
2. Open **http://eyeball.local**. On the *Calibration* card, move each servo slowly with its raw slider and set the min/centre/max (and lid closed/open) just short of the printed hard stops. Tick *Invert* if pad-right or pad-up moves the eye the wrong way. Save.
3. On the tracker page (http://localhost:8000), click **Calibrate eye**. Stand in view, alone, drag the pad until the eye looks at you, and press **Save point**. Repeat at 3 or more spread-out spots, near and far. Points are saved to `calibration.json`. Until 3 exist, a rough default mapping is used.

### Behaviour

- **Log:** the *Log* card on http://eyeball.local shows everything printed since boot (the last 32 KB), including the reset reason, so a `BROWNOUT` restart shows up there.
- **Status LED:** the board's RGB LED is green when it's on Wi-Fi with the server running, and red otherwise (while joining, in setup mode, or after losing Wi-Fi). If it stays dark, change `PIN_STATUS_LED` in `config.h` from 48 to 38.
- **Tracking:** the eye looks at the highest-confidence person, sticking with them unless someone else is ≥0.15 more confident for ~1 s, or they've been gone for 0.5 s. It aims at face height (15% down the box). It blinks every 3–8 s, and also when it switches to a new person.
- **Nobody in view:** it holds its gaze for 2 s, glances around slowly for 30 s, then centres, closes the lid and stops driving the servos (sleep). It wakes when someone appears.
- **Failsafe:** if no tracker command arrives for 5 s, it behaves as if nobody is in view.
- **Manual:** any input on the ESP32 page takes over. It returns to auto after 60 s without input, or via the *Auto* button.

### ESP32 API

| Endpoint | Body / notes |
|---|---|
| `WS /ws` | Send `{"yaw":deg,"pitch":deg[,"newTarget":true]}` or `{"present":false}`. Receives status ~5 Hz |
| `POST /api/eye` | Same as the WebSocket command, over HTTP |
| `GET /api/status` | Mode, state, angles, lid, limits, RSSI, uptime |
| `POST /api/manual` | `{"yaw","pitch","lid"}` (any subset; lid 0–1), or `{"servo":"yaw\|pitch\|lid","us":1500}` raw |
| `POST /api/mode` | `{"mode":"auto"\|"manual"}` |
| `POST /api/blink` | |
| `GET/POST /api/calibration` | `{"yaw":{minUs,centerUs,maxUs,invert},"pitch":{…},"lid":{closedUs,openUs}}` |
| `GET /api/log?since=N` | Log text since boot from byte offset N; headers `X-Log-Start`, `X-Log-End` (next `since`), `X-Log-Boot` (changes on restart). The page's *Log* card follows it live |
| `POST /api/wifi/reset` | Forget Wi-Fi and restart into setup mode |

Angles are degrees around the calibrated centre (≈11 µs/°).
