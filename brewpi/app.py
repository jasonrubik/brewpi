#!/usr/bin/env python3
"""
Flask + SocketIO app to interface with Arduino brewer controller.
- Reads serial lines from Arduino and emits them to web clients.
- Accepts commands from web UI and forwards to Arduino serial (one-line commands).
- Saves session logs and uploaded recipes to files.
"""

import os
import time
import json
import fcntl
import serial
import threading
import subprocess
import serial.tools.list_ports
import RPi.GPIO as GPIO
from datetime import datetime
from flask import Flask, render_template, request, jsonify
from flask_socketio import SocketIO


# --------- FIND ARDUINO PORT ----------
def find_arduino_port():
    """Try to auto-detect the Arduino serial port."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if "Arduino" in p.description or "ttyACM" in p.device or "ttyUSB" in p.device:
            print(f"Auto-detected Arduino on {p.device}")
            return p.device
    print("No Arduino found, falling back to /dev/ttyACM0")
    return "/dev/ttyACM0"


def is_port_busy(port):
    """Check if another process is using the serial port."""
    try:
        result = subprocess.run(["lsof", port], capture_output=True, text=True)
        return result.returncode == 0 and port in result.stdout
    except Exception:
        return False


# -------- Configuration --------
BAUD_RATE = 9600
SESSION_LOG_MAX_LINES = 2000
READY_PIN = 27  # GPIO pin for "READY" LED
running = True
SERIAL_PORT = find_arduino_port()
LOG_DIR = os.path.expanduser("~/brewpi_logs")

# -------- GPIO Setup --------
GPIO.setmode(GPIO.BCM)
GPIO.setup(READY_PIN, GPIO.OUT)
GPIO.output(READY_PIN, GPIO.LOW)


# --------- APP SETUP ----------
app = Flask(__name__)
socketio = SocketIO(app)

# ensure log dir exists
os.makedirs(LOG_DIR, exist_ok=True)

# --------- GLOBALS ----------
ser = None
serial_thread = None
running = True

# in-memory log buffer for UI (list of dicts: {time, raw, parsed})
log_buffer = []
session_filename = None
current_recipe = None

# --------- SERIAL READER THREAD ----------
def safe_open_serial(port, baud):
    """Open the serial port safely with file lock."""
    try:
        ser = serial.Serial(port, baud, timeout=1)
        fd = ser.fileno()
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        print(f"Opened serial on {port}")
        return ser
    except (OSError, serial.SerialException) as e:
        print(f"Port {port} is busy, retrying: {e}")
        return None

def serial_loop(find_arduino_port, BAUD_RATE, session_filename=None):
    log_buffer = []
    ser = None
    retry_delay = 2
    running = True

    while running:
        if ser is None or not ser.is_open:
            port = find_arduino_port()
            if not port:
                print("No Arduino detected. Retrying...")
                time.sleep(retry_delay)
                continue
            ser = safe_open_serial(port, BAUD_RATE)
            if ser is None:
                time.sleep(retry_delay)
                retry_delay = min(retry_delay + 1, 10)
                continue
            retry_delay = 2  # reset after successful open

        try:
            raw = ser.readline().decode(errors="ignore").strip()
            if not raw:
                raise serial.SerialException("No data received")

            ts = datetime.now().isoformat()
            entry = {"time": ts, "raw": raw}

            parsed = {}
            parts = [p.strip() for p in raw.replace(",", " ").split() if "=" in p or ":" in p]
            for p in parts:
                if "=" in p:
                    k, v = p.split("=", 1)
                elif ":" in p:
                    k, v = p.split(":", 1)
                else:
                    continue
                parsed[k.strip().upper()] = v.strip()
            if parsed:
                entry["parsed"] = parsed

            log_buffer.append(entry)
            if len(log_buffer) > SESSION_LOG_MAX_LINES:
                log_buffer.pop(0)

            if session_filename:
                try:
                    with open(session_filename, "a") as f:
                        f.write(json.dumps(entry) + "\n")
                except Exception as e:
                    print("Failed writing session log:", e)

            socketio.emit("serial_update", entry)

        except serial.SerialException as e:
            print("Serial read error:", e)
            if ser:
                try:
                    ser.close()
                except Exception:
                    pass
            ser = None
            time.sleep(2)

        except Exception as e:
            print("Unexpected error:", e)
            if ser:
                try:
                    ser.close()
                except Exception:
                    pass
            ser = None
            time.sleep(2)

# start serial thread
def start_serial_thread():
    global serial_thread
    serial_thread = threading.Thread(
        target=serial_loop,
        args=(find_arduino_port, BAUD_RATE),
        daemon=True
    )
    serial_thread.start()

start_serial_thread()

# --------- HELPERS ----------
def send_to_arduino(cmd: str):
    """Write command line to Arduino (terminates with LF)."""
    global ser
    if ser and ser.is_open:
        try:
            ser.write((cmd.strip() + "\n").encode())
            return True
        except Exception as e:
            print("Serial write error:", e)
            return False
    else:
        print("Serial port not open.")
        return False

def start_session_file():
    global session_filename
    now = datetime.now().strftime("%Y%m%d_%H%M%S")
    session_filename = os.path.join(LOG_DIR, f"brew_session_{now}.log")
    with open(session_filename, "w") as f:
        f.write("# session started: " + datetime.now().isoformat() + "\n")
    return session_filename

def save_recipe_to_file(recipe_list):
    now = datetime.now().strftime("%Y%m%d_%H%M%S")
    fname = os.path.join(LOG_DIR, f"recipe_{now}.json")
    with open(fname, "w") as f:
        json.dump({"timestamp": now, "recipe": recipe_list}, f, indent=2)
    return fname

# --------- FLASK ROUTES ----------
@app.route("/", methods=["GET"])
def index():
    return render_template("index.html")

@app.route("/start_session", methods=["POST"])
def api_start_session():
    fname = start_session_file()
    return jsonify({"status":"ok", "session_file": fname})

@app.route("/get_log", methods=["GET"])
def api_get_log():
    # return last N entries
    return jsonify(log_buffer[-200:])

@app.route("/send", methods=["POST"])
def api_send():
    data = request.json or {}
    cmd = data.get("cmd") or data.get("command")
    if not cmd:
        return jsonify({"status":"error", "msg":"no command provided"}), 400
    ok = send_to_arduino(cmd)
    return jsonify({"status":"ok" if ok else "error", "sent": cmd})

@app.route("/load_recipe", methods=["POST"])
def api_load_recipe():
    """
    Expects JSON {recipe: [{temp: 145, time: 0}, ...]} where time is in minutes (or seconds if you prefer).
    We'll convert to the Arduino LOADRECIPE:<temp,sec;...> format (temps F, secs).
    """
    data = request.json or {}
    recipe_list = data.get("recipe")
    if not recipe_list or not isinstance(recipe_list, list):
        return jsonify({"status":"error", "msg":"invalid recipe"}), 400

    # build LOADRECIPE payload: temp,sec;temp,sec;...
    segments = []
    for step in recipe_list:
        t = step.get("temp")
        time_min = step.get("time")
        if t is None or time_min is None:
            continue
        secs = int(time_min) * 60
        segments.append(f"{float(t):.1f},{secs}")
    if not segments:
        return jsonify({"status":"error", "msg":"no valid steps"}), 400

    payload = "LOADRECIPE:" + ";".join(segments)
    ok = send_to_arduino(payload)
    if ok:
        save_recipe_to_file(recipe_list)
        return jsonify({"status":"ok", "sent": payload})
    else:
        return jsonify({"status":"error", "msg":"serial send failed"}), 500

@app.route("/download_session", methods=["GET"])
def download_session():
    if not session_filename or not os.path.exists(session_filename):
        return jsonify({"status":"error", "msg":"no session"}), 404
    return app.send_static_file(session_filename)

# --------- SOCKETIO EVENTS ----------
@socketio.on("connect")
def on_connect():
    print("Client connected")

@socketio.on("request_recent")
def on_request_recent(data=None):
    # send recent log buffer
    socketio.emit("recent_log", log_buffer[-200:])

# --------- MAIN ----------
if __name__ == "__main__":
    print("Starting Flask SocketIO server...")
    print("Serial port set to:", SERIAL_PORT)
    # try open serial immediately
    socketio.run(app, host="0.0.0.0", port=5000)