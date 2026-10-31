# eye-control — people tracker + eyeball automaton

Pulls the Scrypted RTSP camera stream and detects and tracks people with YOLO11s (on the Apple GPU via MPS) and ByteTrack. It serves a browser page with the live preview, labelled boxes, ~2 s motion trails, a person count and a confidence slider. It also points a 3-servo eyeball, driven by an ESP32-S3, at the tracked person (see [Eyeball](#eyeball)).

## Run

```sh
uv run main.py
```

Open http://localhost:8000 (or `http://<this-mac>:8000` from another device on the LAN). There is no authentication.

**Detection zone:** click **Edit zone** and draw a polygon on the picture: click to add corners (once there are 3, a click is inserted into the nearest edge), drag a corner to move it, and double-click or right-click a corner to remove it. Save. People whose feet (the bottom centre of their box) are outside it are ignored: they aren't counted, the eye won't look at them, and the page shows them as faint dashed boxes. The zone is saved to `zone.json`. Clear it to use the whole picture again.

Detection runs continuously while the server is up. Preview frames are only encoded and sent while a page is open. If the stream drops, it reconnects with backoff (1 → 10 s) and the page shows a banner.

## Configuration

Copy `.env.example` to `.env` and adjust. Key settings: `RTSP_URL`, `MODEL`, `IMGSZ` (inference size), `CONF`, `PREVIEW_WIDTH`, `MAX_FPS`, `PORT`, `EYE_URL` (empty disables the eye), `CALIBRATION_PATH`, `ZONE_PATH`.

Each preview frame is roughly 300 KB at 1280 px wide, so at 15 fps that's about 35 Mbit/s per viewer. Lower `PREVIEW_WIDTH` or `MAX_FPS` for viewers on Wi-Fi.

## How it works

- `pipeline.py`: a reader thread keeps only the newest RTSP frame. A worker thread runs `model.track` (people only), downscales and JPEG-encodes the frame, and publishes one binary WebSocket message per frame: `[uint32 LE meta length][meta JSON][JPEG]`. Box coordinates are normalised to 0–1.
- `zone.py`: the detection zone. People are split into inside/outside before the eye sees them; the outside ones are sent to the page as `ignored`.
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

PlatformIO's espressif32 platform needs Python ≤3.13, so run it through `uvx`. The web page (`firmware/web/index.html`) is compiled into the firmware, so one image updates everything.

The first install, or recovery, goes over USB:

```sh
cd firmware
uvx --python 3.12 platformio run -t upload      # build and flash over USB
uvx --python 3.12 platformio device monitor     # serial log
```

After that, update over Wi-Fi:

```sh
cd firmware
scripts/ota.sh                                  # build, upload, wait for the restart, print the new version
scripts/ota.sh 192.168.1.50                     # another host (default: $EYE_URL, then eyeball.local)
uvx --python 3.12 platformio run -e ota -t upload   # the same, through PlatformIO
```

You can also install a `firmware.bin` from the *Firmware* card on http://eyeball.local. During an upload the servos go limp. A new firmware is on probation until it's back on Wi-Fi, or until it has run for 60 s if Wi-Fi is unavailable. If it crashes or restarts before then, the board goes back to the previous firmware. The script and the page both report this, and the log says so. A file that isn't ESP32-S3 firmware is rejected before anything changes.

### First-time setup

1. Power it on. With no saved Wi-Fi it starts an **Eyeball-Setup** access point. Join it from a phone, and the setup page opens (otherwise go to http://192.168.4.1). Pick your network and enter the password. It restarts and joins.
2. Open **http://eyeball.local**. On the *Calibration* card, move each servo slowly with its raw slider and set the min/centre/max (and lid closed/open) just short of the printed hard stops. Tick *Invert* if pad-right or pad-up moves the eye the wrong way. Save.
3. On the tracker page (http://localhost:8000), click **Calibrate eye**. Stand in view, alone, drag the pad until the eye looks at you, and press **Save point**. Repeat at 3 or more spread-out spots, near and far. Points are saved to `calibration.json`. Until 3 exist, a rough default mapping is used. Then switch on **Follow people**, on the tracker page or the ESP32 page.

### Behaviour

- **Log:** the *Log* card on http://eyeball.local shows everything printed since boot (the last 32 KB), including the reset reason, so a `BROWNOUT` restart shows up there.
- **Status LED:** the board's RGB LED is green when it's on Wi-Fi with the server running, and red otherwise (while joining, in setup mode, or after losing Wi-Fi). If it stays dark, change `PIN_STATUS_LED` in `config.h` from 48 to 38.
- **Auto switch:** off (manual) by default. It's saved on the board, so it survives restarts. Flip it on either the ESP32 page or the tracker page (*Follow people*). Using the ESP32 page's gaze pad, lid slider or calibration controls switches it off.
- **Manual (Auto off):** the eye stays exactly where it was last put, with no blinking and no timeout. Tracker commands are ignored.
- **Teaching:** the tracker page's *Calibrate eye* moves the eye in either mode. When teaching ends, the eye goes back to what the switch says.
- **Tracking (Auto on):** the eye looks at the highest-confidence person, sticking with them unless someone else is ≥0.15 more confident for ~1 s, or they've been gone for 0.5 s. It aims at face height (15% down the box). It blinks every 3–8 s, and also when it switches to a new person.
- **Nobody in view (Auto on):** it holds its gaze for 2 s, glances around slowly for 30 s, then centres, closes the lid and stops driving the servos (sleep). It wakes when someone appears.
- **Failsafe:** if no tracker command arrives for 5 s, it behaves as if nobody is in view.

### ESP32 API

| Endpoint | Body / notes |
|---|---|
| `WS /ws` | Send `{"yaw":deg,"pitch":deg[,"newTarget":true]}`, `{"present":false}`, `{"yaw":deg,"pitch":deg,"teach":true}` (moves it in any mode) or `{"auto":bool}`. Receives status ~5 Hz |
| `POST /api/eye` | Same as the WebSocket command, over HTTP |
| `GET /api/status` | Mode, state, angles, lid, limits, RSSI, uptime, firmware `version`/`built`, `boot` (changes on restart) |
| `POST /api/manual` | `{"yaw","pitch","lid"}` (any subset; lid 0–1), or `{"servo":"yaw\|pitch\|lid","us":1500}` raw |
| `POST /api/auto` | `{"auto":true\|false}`, saved |
| `POST /api/blink` | |
| `GET/POST /api/calibration` | `{"yaw":{minUs,centerUs,maxUs,invert},"pitch":{…},"lid":{closedUs,openUs}}` |
| `GET /api/log?since=N` | Log text since boot from byte offset N; headers `X-Log-Start`, `X-Log-End` (next `since`), `X-Log-Boot` (changes on restart). The page's *Log* card follows it live |
| `POST /api/update` | Multipart upload, field `firmware` = `firmware.bin`. Installs and restarts; 400 with `{"error"}` if the image is bad |
| `POST /api/wifi/reset` | Forget Wi-Fi and restart into setup mode |

Angles are degrees around the calibrated centre (≈11 µs/°).
