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
