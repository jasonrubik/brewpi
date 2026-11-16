# Troubleshooting

Keep heater power disconnected during diagnostics.

## SSH access

```bash
ssh rubik@rubikbrewpi.local
```

If mDNS does not resolve, find the Pi's current DHCP address in the router or run `hostname -I` locally. The IP can change after a router or network replacement.

## Web dashboard unavailable

```bash
systemctl status brewpi --no-pager
sudo journalctl -u brewpi -n 50 --no-pager -l
sudo ss -ltnp | grep ':5000'
curl -I --max-time 5 http://127.0.0.1:5000/
```

The application listens on TCP 5000. `LISTEN` confirms a socket, not a functioning Arduino connection.

## `AttributeError: 'NoneType' object has no attribute 'eio'`

Check that Flask-SocketIO is initialized with the app:

```python
app = Flask(__name__)
socketio = SocketIO(app)
```

Do not replace it later with `socketio = SocketIO()` unless `socketio.init_app(app)` is called.

## Missing arguments to `serial_loop` or `safe_open_serial`

The checked-in Python has `serial_loop(find_arduino_port, BAUD_RATE, ...)` and `safe_open_serial(port, baud)`. Thread startup must pass the required arguments. Do not add a second serial-port open in the main startup path.

## Arduino not found

```bash
arduino-cli board list
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

The Uno board identifier is `arduino:avr:uno`; `arduino:avr` is the core, not a valid FQBN.

## Empty chart and live log

Check whether Python owns the serial port:

```bash
sudo lsof /dev/ttyACM0
```

To inspect raw Arduino output, stop Flask **first**:

```bash
sudo systemctl stop brewpi
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=9600
```

Wait for output, then Ctrl+C and `sudo systemctl start brewpi`. If raw serial output exists but the dashboard is empty, inspect `socketio.emit('serial_update', ...)` and the browser's event handler. Disconnected A0/A1 sensors explain invalid temperatures, but not necessarily a completely empty serial log.

## Serial read reports readiness but returns no data

This can occur when the device disconnects/resets or multiple processes contend for the port. Check `lsof`; stop known services gracefully. Do not blindly kill arbitrary processes. Avoid repeated open attempts from separate threads.

## GPIO channel already in use

Inspect duplicate GPIO setup, Pi boot-time pin configuration and any other GPIO consumer. Do not merely suppress warnings until the ownership conflict is understood.

## Upload script caveats

The repository's `arduino-deploy.sh` kills processes using the serial port with `sudo kill -9`. Its retry-loop completion check also deserves correction. Prefer an explicit controlled deployment with the heater disconnected and the Flask service stopped.

## WebSocket connects, then drops (`ping timeout` / `transport close`)

On November 15, 2026, the browser successfully upgraded from polling to WebSocket, but both ordinary and WebSocket-only connections experienced disconnects. The server auto-selected Eventlet (`SocketIO(app)`), even without an explicit `eventlet.monkey_patch()` call. This was not proof Eventlet alone caused the issue.

For a controlled bench comparison, the Pi was switched to `SocketIO(app, async_mode="threading")` with `simple-websocket` installed in a `--system-site-packages` virtual environment. Desktop and mobile dashboards subsequently showed fresh telemetry. A longer-duration stability test remains outstanding.

Check `sudo journalctl -u brewpi -f`, `systemctl status brewpi`, and the browser Console/Network tabs. In a browser console with Socket.IO already loaded, a temporary WebSocket-only test is:

```javascript
const testSocket = io({ transports: ["websocket"], reconnection: true });
testSocket.on("connect", () => console.log("connected", testSocket.io.engine.transport.name));
testSocket.on("serial_update", entry => console.log("telemetry", entry.parsed?.TIME));
testSocket.on("disconnect", reason => console.warn("disconnected", reason));
testSocket.on("connect_error", error => console.error(error.message));
```

This creates an extra connection alongside the dashboard; close/reload the page after testing. A green Socket connected indicator does not independently prove fresh telemetry: use the dashboard's telemetry heartbeat.

## Threading mode fails with Werkzeug production-server error

Flask-SocketIO 5.3.2 raises `RuntimeError: The Werkzeug web server is not designed to run in production` when threading mode is started without explicit opt-in. The bench-only Pi startup uses `allow_unsafe_werkzeug=True` in `socketio.run(...)`. This bypass is **not** a production hardening measure; keep port 5000 private. If the service is restart-looping, stop it before editing and validate syntax with `.venv/bin/python -m py_compile app.py` before restarting.

## SP+ / SP− sends HTTP 200 but setpoint does not change

The dashboard sends `{"cmd":"UP"}` / `{"cmd":"DOWN"}` to `POST /send`; the Arduino sketch recognizes these commands. During diagnosis, Flask returned `{"status":"error","sent":"UP"}` with HTTP 200 while logging `Serial port not open.`, despite continuing serial telemetry. Check the shared global `ser` used by the serial reader and `send_to_arduino()`, its open state, and whether the port was reconnected. Only one application process was observed in the November test.

After subsequent debugging, the mobile screenshot showed the setpoint moving down to 98°F and a command-sent message. This is evidence of successful command behavior in that session, **not** a full safety or endurance test. Consider returning HTTP 503 when `send_to_arduino()` fails, and adding a controller acknowledgement rather than relying solely on an HTTP success response.

## Python indentation error after edits

If `IndentationError: unexpected indent` appears after inserting a debug `print(...)`, inspect the affected function's four-space indentation, run `.venv/bin/python -m py_compile app.py`, and restart only after it passes.
