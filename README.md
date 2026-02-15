# 🐦 BirdCam — ESP32 Camera + Home Assistant (MQTT Discovery)

BirdCam is an ESP32 camera firmware that provides:
- 📡 MJPEG live stream
- 📸 Snapshots + snapshot archive
- 🏠 Native Home Assistant integration via MQTT Discovery
- ⚙️ Web settings page (camera + storage + diagnostics)

> **Security note:** this repo ships with `secrets.h.example`.
> Copy it to `secrets.h` locally and **never commit** your real credentials.

## Features

- Live MJPEG streaming endpoint (`/mjpeg`)
- Live snapshot endpoint (`/snapshot`)
- Snapshot archive + viewer (`/archive`, `/view`)
- Home Assistant MQTT discovery (diagnostics + camera controls)
- Camera controls exposed to HA (brightness/contrast/saturation/sharpness, gain/exposure, manual gain/exposure)
- URLs exposed to HA (stream URL + snapshot URL + last snapshot URLs)

## Hardware

Typical targets:
- ESP32-CAM (AI Thinker or similar)
- A supported camera sensor (OV2640 is common)
- Optional battery + external power (USB)

## Quick start (Arduino IDE)

1. Install ESP32 board support in Arduino IDE.
2. Copy `secrets.h.example` → `secrets.h`
3. Edit `secrets.h` with your Wi‑Fi + MQTT broker details.
4. Open `BirdCam.ino`, select your ESP32 board, and upload.

## Endpoints

- `http://<device-ip>/mjpeg` — MJPEG stream
- `http://<device-ip>/snapshot` — live JPEG snapshot
- `http://<device-ip>/archive` — snapshot archive
- `http://<device-ip>/view` — archive viewer / UI

## Home Assistant

BirdCam publishes MQTT Discovery config so the device and entities appear automatically in Home Assistant.

## Stability notes (resolutions)

Higher resolutions can fail if JPEG frames get too large for the available buffers.
If you see missing stream/snapshot at VGA+:
- Increase `jpeg_quality` (higher number = more compression, smaller frames)
- Use PSRAM for frame buffers when available
- Keep `fb_count` reasonable (2 is often a sweet spot)
- Prefer grab-latest mode for streaming

## License

MIT — see `LICENSE`.
