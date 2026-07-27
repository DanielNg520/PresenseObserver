# server/ — FastAPI + MySQL

Backend for Presence Observer. Subscribes to the device's MQTT event/frame
topics, persists them to MySQL, and serves a live WebSocket dashboard.

- `webserver/main.py` — app: MQTT ingest, WebSocket broadcast, REST API.
- `webserver/static/` — dashboard (`script.js`, `style.css`) + `index.html` template.
- `docker-compose.yml` — MySQL + webserver (add a broker / reverse proxy per the guide).

## Run

```bash
cp .env.example .env    # DB_*, MQTT_BROKER, MQTT_TOPIC
docker compose up --build
```

Dashboard at `http://localhost:8000`.

## Key HTTP API

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/events` | list presence events (newest first) |
| GET | `/api/events/{id}/frames` | ordered frames for playback |
| POST | `/api/events/{id}/false_alarm` | flag / unflag an event |
| DELETE | `/api/events/{id}` | delete an event (frames cascade) |
| GET, POST | `/api/arm` | read / set the camera arm state |
| WS | `/ws` | live notifications + frames |

See [`../DEPLOYMENT_GUIDE.md`](../DEPLOYMENT_GUIDE.md) for production deployment.
