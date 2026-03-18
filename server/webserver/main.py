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

# TA5 challenge1

MQTT_BROKER = os.getenv("MQTT_BROKER", "broker.emqx.io")
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "ece140a/ta7/autograder")
MQTT_COMMAND_TOPIC = f"{MQTT_TOPIC}/command"
MQTT_DATA_TOPIC = f"{MQTT_TOPIC}/thermal"

DB_HOST = os.getenv("DB_HOST", "db")
DB_PORT = int(os.getenv("DB_PORT", 3306))
DB_USER = os.getenv("DB_USER", "root")
DB_PASSWORD = os.getenv("DB_PASSWORD", "")
DB_NAME = os.getenv("DB_NAME", "ta7db")

clients: list[WebSocket] = []
latest_reading = None
continuous_mode = False


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


# MQTT

def on_connect(client, userdata, flags, reason_code, properties):
    print(f"[MQTT] Connected: {reason_code}")
    client.subscribe(MQTT_DATA_TOPIC)
    print(f"[MQTT] Subscribed to {MQTT_DATA_TOPIC}")


def on_message(client, userdata, msg):
    global latest_reading
    try:
        data = json.loads(msg.payload.decode())
        if "pixels" in data and len(data["pixels"]) == 64:
            latest_reading = data
    except Exception as e:
        print(f"[MQTT] Parse error: {e}")


mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message

# TA5 challenge1

async def broadcast_readings():
    global latest_reading
    while True:
        if latest_reading is not None and clients:
            msg = json.dumps({"type": "reading", "data": latest_reading})
            for client in clients:
                try:
                    await client.send_text(msg)
                except Exception:
                    pass
            latest_reading = None
        await asyncio.sleep(0.1)


# App lifespan - TA5 challenge1

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


# WebSocket - TA5 challenge1

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    clients.append(websocket)
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        clients.remove(websocket)


# ESP32 command endpoint

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


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
