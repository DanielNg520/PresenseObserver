# Presence Observer

Thermal presence detection that runs a neural net **on the microcontroller** and
records what it sees. An ESP32-S3 Feather with an AMG8833 8×8 thermal camera (and
an optional microphone) watches a space autonomously; when it detects a person it
logs the event and streams a short thermal "clip" to a server, where a live
dashboard lets you review events and flag false alarms.

No camera images ever leave the device as pixels a human could recognize — it's
an 8×8 heat grid — so it's a privacy-preserving occupancy sensor.

---

## What it does

- **On-device ML.** 64 raw thermal pixels → a 76-feature vector → a TensorFlow
  Lite Micro model (8 KB tensor arena) → presence confidence, all on the ESP32.
- **Autonomous event recording.** No polling from the server: the device
  monitors at 1 Hz, and a debounced detection starts an event — an instant log
  plus a 1 fps thermal clip that runs until the space is empty (5-minute cap).
- **Live dashboard.** WebSocket-driven: a toast the moment presence is detected,
  an events table with at-a-glance metrics, and heatmap **playback** of any
  recorded clip.
- **False-alarm review.** Flag any event as a false alarm; it's persisted.
- **Arm / disarm "off switch."** Toggle monitoring from the web UI; the device
  persists the state in NVS and reports it back so every browser stays in sync.
- **Optional microphone (INMP441).** Adds a sound level per frame as secondary
  confirmation — fully optional; the firmware runs identically without it.
- **Resilient networking.** Multi-SSID failover, WPA2-Enterprise, and a
  captive-portal recovery flow so you can reconfigure Wi-Fi without reflashing.

## Architecture

```
ESP32-S3 + AMG8833 (+ INMP441)          Droplet
  thermal → TFLite → event recorder      Caddy :443 (HTTPS/WSS)
        │  MQTT: event / frame              → FastAPI :8000
        │  ◄── arm / disarm                     ├── MySQL (events + frames)
        ▼                                        └── MQTT broker
   broker  ──────────────────────────────────►  subscribes, persists,
                                                 pushes to browser dashboard
```

## Tech stack

| Layer | Tech |
|-------|------|
| Firmware | C++ / Arduino (PlatformIO), TensorFlow Lite Micro, PubSubClient, I2S |
| Transport | MQTT (event / frame / command topics) |
| Server | FastAPI, WebSockets, paho-mqtt, MySQL |
| Frontend | Vanilla JS + Canvas heatmap, responsive dark UI |
| Ops | Docker Compose, Caddy (HTTPS reverse proxy) |

## Repository layout

```
esp32/
  thermal/         MAIN firmware — sensor + on-device TFLite + MQTT event recorder
  ESP32-feather/   diagnostics sketch (Wi-Fi / sensor bring-up only)
server/
  webserver/       FastAPI app, static dashboard, HTML template
  docker-compose.yml
DEPLOYMENT_GUIDE.md  full, beginner-friendly droplet deployment walkthrough
```

## Quick start (server)

```bash
cd server
cp .env.example .env   # set DB_*, MQTT_BROKER, MQTT_TOPIC
docker compose up --build
```

Open the dashboard at `http://localhost:8000`.

## Quick start (firmware)

```bash
cd esp32/thermal
cp env.example .env    # set WIFI_SSID, MQTT_TOPIC (must match the server), etc.
pio run --target upload --target monitor -e adafruit_feather_esp32s3
```

## Deployment

See **[DEPLOYMENT_GUIDE.md](DEPLOYMENT_GUIDE.md)** for a complete,
beginner-friendly walkthrough: droplet setup, hardening, Docker, a private MQTT
broker (self-hosted Mosquitto or managed HiveMQ Cloud), HTTPS via Caddy, flashing
the board, end-to-end verification, and a hardening/optimization checklist. It
includes an appendix for squeezing onto a 512 MB droplet.
