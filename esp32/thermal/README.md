# esp32/thermal/ — main firmware

Autonomous thermal presence recorder for the Adafruit ESP32-S3 Feather + AMG8833
8×8 thermal camera, with an optional INMP441 I2S microphone.

Reads the sensor at 1 Hz, computes a 76-feature vector, runs a TensorFlow Lite
Micro model on-device, and — on a debounced presence detection — publishes an
event plus a 1 fps thermal clip over MQTT until the space is empty (5-min cap).
Accepts `arm` / `disarm` from the dashboard and reports its state back.

## Build & flash

```bash
cp env.example .env    # WIFI_SSID, NON_ENTERPRISE_WIFI_PASSWORD / UCSD_*, MQTT_TOPIC, ...
pio run --target upload --target monitor -e adafruit_feather_esp32s3
```

`.env` values are injected as build-time macros by `pre_extra_script.py`.

## Layout

- `src/main.cpp` — pipeline: Wi-Fi/MQTT bring-up, presence state machine, event lifecycle.
- `src/Microphone.*` — optional INMP441 (guarded by `-D USE_MIC`; see `platformio.ini`).
- `src/ECE140_WIFI.*` — Wi-Fi with multi-SSID failover + captive-portal recovery.
- `src/ECE140_MQTT.*` — PubSubClient wrapper that restores callback + subscriptions on reconnect.
- `include/model_data.h`, `include/model_params.h` — the TFLite blob + scaler params.

The MQTT broker host is hardcoded in `include/ECE140_MQTT.h`. Optional mic pins
default to `WS=12 SCK=13 SD=14` (override with `-D MIC_WS=` etc.).

See [`../../DEPLOYMENT_GUIDE.md`](../../DEPLOYMENT_GUIDE.md) for the full setup.
