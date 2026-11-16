# Installation and operation

These are reproducible setup notes, **not a tested one-command installer**. Paths assume the user `rubik`; adapt to your installation. Keep all mains-powered heater circuits disconnected.

## Raspberry Pi

Install Raspberry Pi OS Lite (Bookworm), configure Ethernet, SSH, and a user account. From another machine:

```bash
ssh rubik@rubikbrewpi.local
```

Alternatively use the IP reported by `hostname -I`. The IP address is router-assigned and should not be hard-coded in documentation.

## Python dependencies

The existing application uses Flask, Flask-SocketIO, pyserial and RPi.GPIO. On Bookworm, system packages avoid the externally-managed Python environment restriction:

```bash
sudo apt update
sudo apt install python3-flask python3-flask-socketio python3-serial python3-rpi.gpio python3-venv lsof
```

Check the actual imports in `brewpi/app.py` for any additional requirements. Do not mix system Python packages and `pip --break-system-packages` casually.

### November 2026 runtime: Python threading and WebSocket support

The tested Pi deployment explicitly selects `SocketIO(app, async_mode="threading")` instead of automatically choosing installed Eventlet. The APT package `python3-simple-websocket` was unavailable in the configured repositories, so the deployment uses an isolated virtual environment while retaining system-provided Python modules:

```bash
cd ~/brewpi
/usr/bin/python3 -m venv --system-site-packages .venv
.venv/bin/python -m pip install simple-websocket==1.1.0
.venv/bin/python -m py_compile app.py
```

The deployed script on the Pi is `/home/rubik/brewpi/app.py`; a fresh repository checkout may instead have a nested `brewpi/app.py`. Adjust paths accordingly. In threading mode, the currently tested startup uses `socketio.run(app, host="0.0.0.0", port=5000, allow_unsafe_werkzeug=True)` **only for trusted-LAN bench testing**. Do not expose Werkzeug or the command endpoints to the internet.

## Project files

```bash
git clone https://github.com/jasonrubik/brewpi.git
cd brewpi
python3 -m py_compile brewpi/app.py
```

The checked-in paths differ from the original Pi deployment (`/home/rubik/brewpi/app.py` and `/home/rubik/brewpi-arduino/`). If using the repository directly, the application lives at `~/brewpi/brewpi/app.py`. Check any absolute paths, log directories and service files before starting.

## Arduino CLI

Install Arduino CLI using official instructions for your Pi architecture, then:

```bash
arduino-cli core update-index
arduino-cli core install arduino:avr
arduino-cli lib install "PID"
arduino-cli board list
arduino-cli compile --fqbn arduino:avr:uno brewpi-arduino
```

The correct Uno FQBN is `arduino:avr:uno`, **not** `arduino:avr` (which is only the core name). Sketch serial baud is 9600; this is separate from bootloader upload baud.

To upload during a bench test, first stop any serial reader and ensure all heater power is disconnected:

```bash
sudo systemctl stop brewpi
arduino-cli upload -p /dev/ttyACM0 --fqbn arduino:avr:uno brewpi-arduino
```

Replace the port with the one actually shown by `arduino-cli board list`.

## Run the web server

From the repository root:

```bash
python3 brewpi/app.py
```

Open `http://<pi-ip>:5000/` on a trusted LAN. The November 2026 Pi deployment uses `socketio = SocketIO(app, async_mode="threading")`; other installations may use `SocketIO(app)` or explicitly call `socketio.init_app(app)` before `socketio.run(...)`. Do not instantiate a second uninitialized SocketIO object.

## Optional systemd service

The original Pi uses a `brewpi.service`. Here is an **example** unit for a repository cloned to `/home/rubik/brewpi`; adjust paths and permissions:

```ini
[Unit]
Description=BrewPi Flask App
After=network.target

[Service]
Type=simple
User=rubik
WorkingDirectory=/home/rubik/brewpi
ExecStart=/home/rubik/brewpi/.venv/bin/python /home/rubik/brewpi/app.py
Restart=always

[Install]
WantedBy=multi-user.target
```

Save as `/etc/systemd/system/brewpi.service` and run:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now brewpi
systemctl status brewpi --no-pager
```

A running service does **not** prove serial telemetry or safe heater control. Check the logs and browser separately. To open a serial monitor, stop the service first and restart it afterward.

### Current Pi service (observed November 15, 2026)

The active unit uses `User=rubik`, `WorkingDirectory=/home/rubik/brewpi`, `ExecStart=/home/rubik/brewpi/.venv/bin/python /home/rubik/brewpi/app.py`, and `Restart=always`. This differs from a nested repository checkout. A brief `active (running)` result immediately after startup is not proof of health; also inspect `journalctl` and confirm live telemetry.
