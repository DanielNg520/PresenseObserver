from contextlib import asynccontextmanager
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request, HTTPException
from fastapi.templating import Jinja2Templates
from fastapi.staticfiles import StaticFiles
from fastapi.responses import JSONResponse
import uvicorn
import asyncio
import json
import os
import time
import paho.mqtt.client as mqtt
import mysql.connector
from dotenv import load_dotenv

load_dotenv()

# Configuration

MQTT_BROKER = os.getenv("MQTT_BROKER", "broker.emqx.io")
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "presence/observer/node-1")
MQTT_COMMAND_TOPIC = f"{MQTT_TOPIC}/command"   # legacy command path (unused by firmware now)
MQTT_DATA_TOPIC = f"{MQTT_TOPIC}/thermal"      # legacy single-reading live heatmap path
MQTT_EVENT_TOPIC = f"{MQTT_TOPIC}/event"       # event_start / event_end log messages
MQTT_FRAME_TOPIC = f"{MQTT_TOPIC}/frame"       # per-second thermal frames within an event

DB_HOST = os.getenv("DB_HOST", "db")
DB_PORT = int(os.getenv("DB_PORT", 3306))
DB_USER = os.getenv("DB_USER", "root")
DB_PASSWORD = os.getenv("DB_PASSWORD", "")
DB_NAME = os.getenv("DB_NAME", "presencedb")

clients: list[WebSocket] = []
latest_reading = None
continuous_mode = False
armed = True  # device-reported arm state (the web "off switch")

# Messages queued by the MQTT thread to be pushed to WebSocket clients by the
# asyncio broadcast loop (event_start / frame / event_end notifications).
outbound: list[dict] = []


# Database helpers

def get_db():
    return mysql.connector.connect(
        host=DB_HOST,
        port=DB_PORT,
        user=DB_USER,
        password=DB_PASSWORD,
        database=DB_NAME
    )


def init_db():
    conn = get_db()
    cur = conn.cursor()
    cur.execute("""
        CREATE TABLE IF NOT EXISTS devices (
            id INT AUTO_INCREMENT PRIMARY KEY,
            mac_address VARCHAR(17) UNIQUE NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    """)
    cur.execute("""
        CREATE TABLE IF NOT EXISTS readings (
            id INT AUTO_INCREMENT PRIMARY KEY,
            mac_address VARCHAR(17) NOT NULL,
            thermistor_temp FLOAT NOT NULL,
            prediction VARCHAR(16) NOT NULL,
            confidence FLOAT NOT NULL,
            pixels JSON NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (mac_address) REFERENCES devices(mac_address)
        )
    """)
    # Presence events: one row per detected episode. Persisted (unlike the
    # legacy live-only readings). false_alarm lets an operator dispute a detection.
    cur.execute("""
        CREATE TABLE IF NOT EXISTS events (
            id INT AUTO_INCREMENT PRIMARY KEY,
            event_uid VARCHAR(64) UNIQUE NOT NULL,
            mac_address VARCHAR(17) NOT NULL,
            trigger_confidence FLOAT,
            audio_level FLOAT,
            started_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            ended_at TIMESTAMP NULL,
            frame_count INT DEFAULT 0,
            false_alarm TINYINT(1) DEFAULT 0,
            note VARCHAR(255) NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (mac_address) REFERENCES devices(mac_address)
        )
    """)
    # Individual thermal frames belonging to an event (1 fps).
    cur.execute("""
        CREATE TABLE IF NOT EXISTS event_frames (
            id INT AUTO_INCREMENT PRIMARY KEY,
            event_id INT NOT NULL,
            seq INT NOT NULL,
            pixels JSON NOT NULL,
            thermistor FLOAT,
            confidence FLOAT,
            audio_level FLOAT,
            captured_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            INDEX idx_event_seq (event_id, seq),
            FOREIGN KEY (event_id) REFERENCES events(id) ON DELETE CASCADE
        )
    """)
    conn.commit()
    cur.close()
    conn.close()


def upsert_device(mac_address: str):
    conn = get_db()
    cur = conn.cursor()
    cur.execute(
        "INSERT IGNORE INTO devices (mac_address) VALUES (%s)",
        (mac_address,)
    )
    conn.commit()
    cur.close()
    conn.close()


# Event ingest helpers (called from the MQTT thread)

def insert_event(data: dict):
    """Create an events row from an event_start message; returns its id."""
    mac = data.get("mac_address", "")
    upsert_device(mac)
    conn = get_db()
    cur = conn.cursor()
    cur.execute(
        """INSERT INTO events (event_uid, mac_address, trigger_confidence, audio_level)
           VALUES (%s, %s, %s, %s)
           ON DUPLICATE KEY UPDATE trigger_confidence = VALUES(trigger_confidence)""",
        (data.get("event_uid"), mac, data.get("trigger_confidence"), data.get("audio_level")),
    )
    conn.commit()
    cur.close()
    conn.close()


def insert_frame(data: dict):
    """Append one frame to its event and bump the event's frame_count."""
    conn = get_db()
    cur = conn.cursor()
    cur.execute("SELECT id FROM events WHERE event_uid = %s", (data.get("event_uid"),))
    row = cur.fetchone()
    if row is None:
        cur.close()
        conn.close()
        return
    event_id = row[0]
    cur.execute(
        """INSERT INTO event_frames (event_id, seq, pixels, thermistor, confidence, audio_level)
           VALUES (%s, %s, %s, %s, %s, %s)""",
        (event_id, data.get("seq"), json.dumps(data.get("pixels")),
         data.get("thermistor"), data.get("confidence"), data.get("audio_level")),
    )
    cur.execute("UPDATE events SET frame_count = frame_count + 1 WHERE id = %s", (event_id,))
    conn.commit()
    cur.close()
    conn.close()


def finalize_event(data: dict):
    """Mark an event ended and store its final frame count."""
    conn = get_db()
    cur = conn.cursor()
    cur.execute(
        """UPDATE events SET ended_at = CURRENT_TIMESTAMP, frame_count = %s
           WHERE event_uid = %s""",
        (data.get("frame_count"), data.get("event_uid")),
    )
    conn.commit()
    cur.close()
    conn.close()


# MQTT

def on_connect(client, userdata, flags, reason_code, properties):
    print(f"[MQTT] Connected: {reason_code}")
    for topic in (MQTT_DATA_TOPIC, MQTT_EVENT_TOPIC, MQTT_FRAME_TOPIC):
        client.subscribe(topic)
        print(f"[MQTT] Subscribed to {topic}")


def on_message(client, userdata, msg):
    global latest_reading
    try:
        data = json.loads(msg.payload.decode())
        topic = msg.topic

        if topic == MQTT_EVENT_TOPIC:
            # Device state report (arm/disarm confirmation).
            if data.get("type") == "state":
                global armed
                armed = bool(data.get("armed"))
                outbound.append({"type": "state", "data": {"armed": armed,
                                                            "mac_address": data.get("mac_address")}})
                print(f"[Cam] device reports {'ARMED' if armed else 'DISARMED'}")
                return
            # Event log: start or end.
            if data.get("type") == "event_start":
                insert_event(data)
                outbound.append({"type": "event_start", "data": data})
                print(f"[Event] start {data.get('event_uid')}")
            elif data.get("type") == "event_end":
                finalize_event(data)
                outbound.append({"type": "event_end", "data": data})
                print(f"[Event] end   {data.get('event_uid')} ({data.get('frame_count')} frames)")

        elif topic == MQTT_FRAME_TOPIC:
            # One thermal frame within an event.
            if "pixels" in data and len(data["pixels"]) == 64:
                insert_frame(data)
                outbound.append({"type": "frame", "data": data})

        else:
            # Legacy live-heatmap path (<TOPIC>/thermal), not persisted.
            if "pixels" in data and len(data["pixels"]) == 64:
                latest_reading = data
    except Exception as e:
        print(f"[MQTT] Parse error: {e}")


mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message


async def _send_to_all(msg: str):
    # Iterate a snapshot; prune any sockets that error out so dead clients
    # don't accumulate in the list.
    dead = []
    for client in list(clients):
        try:
            await client.send_text(msg)
        except Exception:
            dead.append(client)
    for client in dead:
        if client in clients:
            clients.remove(client)


async def broadcast_readings():
    global latest_reading
    while True:
        # Legacy live heatmap.
        if latest_reading is not None and clients:
            await _send_to_all(json.dumps({"type": "reading", "data": latest_reading}))
            latest_reading = None

        # Event notifications queued by the MQTT thread (event_start / frame /
        # event_end). Drain even if there are no clients so the queue can't grow
        # unbounded.
        if outbound:
            pending, outbound[:] = outbound[:], []
            if clients:
                for item in pending:
                    await _send_to_all(json.dumps(item))

        await asyncio.sleep(0.1)


# App lifespan

@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    mqtt_client.connect(MQTT_BROKER, 1883, 60)
    mqtt_client.loop_start()
    asyncio.create_task(broadcast_readings())
    yield
    mqtt_client.loop_stop()
    mqtt_client.disconnect()


app = FastAPI(lifespan=lifespan)
templates = Jinja2Templates(directory="templates")
app.mount("/static", StaticFiles(directory="static"), name="static")


# Frontend

@app.get("/")
async def home(request: Request):
    return templates.TemplateResponse("index.html", {"request": request})


# WebSocket

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    clients.append(websocket)
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        clients.remove(websocket)


# Arm / disarm (camera on-off switch)

@app.get("/api/arm")
async def get_arm():
    return JSONResponse({"armed": armed})


@app.post("/api/arm")
async def set_arm(request: Request):
    global armed
    data = await request.json()
    want = bool(data.get("armed"))
    # Publish the command; the device confirms via a /event "state" message,
    # which updates `armed` for real. Set optimistically so the caller gets
    # immediate feedback.
    mqtt_client.publish(MQTT_COMMAND_TOPIC, "arm" if want else "disarm")
    armed = want
    return JSONResponse({"status": "ok", "armed": armed})


# ESP32 command endpoint (legacy — firmware no longer acts on these)

VALID_COMMANDS = {"get_one", "start_continuous", "stop"}


@app.post("/api/command")
async def send_command(request: Request):
    global continuous_mode, latest_reading
    data = await request.json()
    command = data.get("command")

    if command not in VALID_COMMANDS:
        raise HTTPException(status_code=400, detail=f"Unknown command: {command}")

    if command == "get_one":
        latest_reading = None
        mqtt_client.publish(MQTT_COMMAND_TOPIC, "get_one")
        start = time.time()
        while latest_reading is None and time.time() - start < 5.0:
            await asyncio.sleep(0.1)
        return JSONResponse({"status": "ok", "received": latest_reading is not None})

    elif command == "start_continuous":
        continuous_mode = True
        mqtt_client.publish(MQTT_COMMAND_TOPIC, "start_continuous")
        return JSONResponse({"status": "ok"})

    elif command == "stop":
        continuous_mode = False
        mqtt_client.publish(MQTT_COMMAND_TOPIC, "stop")
        return JSONResponse({"status": "ok"})


# Readings CRUD

@app.post("/api/readings")
async def add_reading(request: Request):
    data = await request.json()
    mac = data.get("mac_address")
    pixels = data.get("pixels")
    thermistor = data.get("thermistor")
    prediction = data.get("prediction")
    confidence = data.get("confidence")

    if not mac or pixels is None or len(pixels) != 64 or thermistor is None \
            or prediction is None or confidence is None:
        raise HTTPException(status_code=400, detail="Invalid reading data")

    upsert_device(mac)

    conn = get_db()
    cur = conn.cursor()
    cur.execute(
        """INSERT INTO readings (mac_address, thermistor_temp, prediction, confidence, pixels)
           VALUES (%s, %s, %s, %s, %s)""",
        (mac, thermistor, prediction, confidence, json.dumps(pixels))
    )
    conn.commit()
    reading_id = cur.lastrowid
    cur.close()
    conn.close()

    return JSONResponse({"id": reading_id})


@app.get("/api/readings")
async def get_readings(device_mac: str = None):
    conn = get_db()
    cur = conn.cursor(dictionary=True)
    if device_mac:
        cur.execute("SELECT * FROM readings WHERE mac_address = %s", (device_mac,))
    else:
        cur.execute("SELECT * FROM readings")
    rows = cur.fetchall()
    cur.close()
    conn.close()

    result = []
    for row in rows:
        row["pixels"] = json.loads(row["pixels"]) if isinstance(row["pixels"], str) else row["pixels"]
        row["created_at"] = str(row["created_at"])
        result.append(row)

    return JSONResponse(result)


@app.delete("/api/readings/{reading_id}")
async def delete_reading(reading_id: int):
    conn = get_db()
    cur = conn.cursor()
    cur.execute("DELETE FROM readings WHERE id = %s", (reading_id,))
    conn.commit()
    affected = cur.rowcount
    cur.close()
    conn.close()

    if affected == 0:
        raise HTTPException(status_code=404, detail="Reading not found")
    return JSONResponse({"status": "ok"})


# Devices

@app.get("/api/devices")
async def get_devices():
    conn = get_db()
    cur = conn.cursor(dictionary=True)
    cur.execute("SELECT * FROM devices")
    rows = cur.fetchall()
    cur.close()
    conn.close()

    for row in rows:
        row["created_at"] = str(row["created_at"])

    return JSONResponse(rows)


# Events

@app.get("/api/events")
async def get_events(device_mac: str = None):
    conn = get_db()
    cur = conn.cursor(dictionary=True)
    if device_mac:
        cur.execute(
            "SELECT * FROM events WHERE mac_address = %s ORDER BY started_at DESC",
            (device_mac,),
        )
    else:
        cur.execute("SELECT * FROM events ORDER BY started_at DESC")
    rows = cur.fetchall()
    cur.close()
    conn.close()

    for row in rows:
        row["started_at"] = str(row["started_at"]) if row["started_at"] else None
        row["ended_at"] = str(row["ended_at"]) if row["ended_at"] else None
        row["created_at"] = str(row["created_at"])
        row["false_alarm"] = bool(row["false_alarm"])

    return JSONResponse(rows)


@app.get("/api/events/{event_id}/frames")
async def get_event_frames(event_id: int):
    conn = get_db()
    cur = conn.cursor(dictionary=True)
    cur.execute(
        "SELECT seq, pixels, thermistor, confidence, audio_level, captured_at "
        "FROM event_frames WHERE event_id = %s ORDER BY seq",
        (event_id,),
    )
    rows = cur.fetchall()
    cur.close()
    conn.close()

    for row in rows:
        row["pixels"] = json.loads(row["pixels"]) if isinstance(row["pixels"], str) else row["pixels"]
        row["captured_at"] = str(row["captured_at"])

    return JSONResponse(rows)


@app.post("/api/events/{event_id}/false_alarm")
async def set_false_alarm(event_id: int, request: Request):
    data = await request.json()
    false_alarm = 1 if data.get("false_alarm") else 0
    note = data.get("note")

    conn = get_db()
    cur = conn.cursor()
    cur.execute("SELECT id FROM events WHERE id = %s", (event_id,))
    if cur.fetchone() is None:
        cur.close()
        conn.close()
        raise HTTPException(status_code=404, detail="Event not found")
    cur.execute(
        "UPDATE events SET false_alarm = %s, note = %s WHERE id = %s",
        (false_alarm, note, event_id),
    )
    conn.commit()
    cur.close()
    conn.close()

    return JSONResponse({"status": "ok", "id": event_id, "false_alarm": bool(false_alarm)})


@app.delete("/api/events/{event_id}")
async def delete_event(event_id: int):
    conn = get_db()
    cur = conn.cursor()
    # event_frames cascade-delete via FK ON DELETE CASCADE.
    cur.execute("DELETE FROM events WHERE id = %s", (event_id,))
    conn.commit()
    affected = cur.rowcount
    cur.close()
    conn.close()

    if affected == 0:
        raise HTTPException(status_code=404, detail="Event not found")
    return JSONResponse({"status": "ok"})


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
