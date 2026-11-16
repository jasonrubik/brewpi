# BrewPi — Raspberry Pi + Arduino HERMS brewing controller

**Status: experimental / bench testing. Not validated for unattended heating.**

A DIY homebrew control project combining a Raspberry Pi web dashboard with an Arduino Uno controller. The Raspberry Pi runs a Flask/Flask-SocketIO application for monitoring, recipes, commands and session logs. The Arduino runs the temperature-control and recipe-step firmware and communicates with the Pi over USB serial.

> This repository is a personal project, not the unrelated commercial/open-source BrewPi fermentation controller. It is not a certified safety controller.

## Architecture

```text
Browser on LAN
    | HTTP / Socket.IO (port 5000)
Raspberry Pi (Raspberry Pi OS Bookworm)
    | Flask app: brewpi/app.py
    | USB serial, 9600 baud
Arduino Uno (brewpi-arduino/brewpi-arduino.ino)
    |-- A0: 10k NTC vessel probe (planned)
    |-- A1: LM35 heat-exchanger outlet probe (planned)
    |-- D9: heater SSR command (DO NOT CONNECT YET)
    |-- D4-D8: local buttons
    |-- D11-D13: alarm/status indicators
```

The current firmware uses the Arduino PID library and implements a default five-stage brewing sequence. The web UI provides a live chart, log, command buttons, session controls and recipe entry. **Some functions and safety behavior still need validation.**

## Repository layout

| Path | Purpose |
| --- | --- |
| `brewpi/app.py` | Flask server, serial bridge, HTTP endpoints, Socket.IO and logging |
| `brewpi/templates/index.html` | Browser dashboard |
| `brewpi-arduino/brewpi-arduino.ino` | Arduino Uno controller firmware |
| `arduino-deploy.sh` | Arduino compile/upload helper; see safety warning below |
| `Arduino/libraries/PID/` | Included Arduino PID library tree |
| `arduinoOTA-1.3.0-linux_arm.tar.bz2` | Bundled tool archive; review provenance/necessity before reuse |
| `LICENSE` | MIT license |

## Hardware and operating environment

- Raspberry Pi running Raspberry Pi OS Lite / Debian Bookworm (tested on ARMv6 Raspberry Pi hardware).
- Arduino Uno connected over USB, normally `/dev/ttyACM0` (the device number can change).
- 10 kOhm NTC vessel sensor and LM35 HEX outlet sensor are **planned inputs**; verify wiring and calibration before use.
- A 1200 W heating element and SSR are part of the intended HERMS installation, **not approved for connection to this experimental software**.
- GPIO 27 on the Pi is intended for a READY indicator; GPIO 17 has been used for a boot indicator in the local installation.

See [Hardware](docs/HARDWARE.md) for the firmware pin assignments and safety issues.

## Getting started: software-only bench setup

1. Install Raspberry Pi OS Lite, enable SSH and connect the Pi to the local network.
2. Install Python dependencies using the distribution's packages (see [Installation](docs/INSTALLATION.md)).
3. Connect an Arduino Uno over USB **without any mains/heater circuit connected**.
4. Install Arduino CLI and the `arduino:avr` core, plus the `PID` library.
5. Compile the sketch first; inspect the deployment script before uploading.
6. Run the Flask application and open `http://<pi-ip>:5000/` on your LAN.
7. Confirm serial messages appear in the live log. An empty chart with disconnected sensors is not proof of a working control system.

**Do not expose port 5000 to the public internet.** The application exposes control endpoints and should be restricted to a trusted network during development.

## Useful commands

```bash
# Pi IP address
hostname -I

# Arduino identification
arduino-cli board list

# Compile without uploading
arduino-cli compile --fqbn arduino:avr:uno ~/brewpi-arduino

# Systemd service (only if installed)
systemctl status brewpi --no-pager
sudo journalctl -u brewpi -n 50 --no-pager -l

# Find serial-port users
sudo lsof /dev/ttyACM0

# Check dashboard listener
sudo ss -ltnp | grep ':5000'
```

**Serial access:** Stop the Flask service before opening an Arduino serial monitor; only one application should read the port. `arduino-cli monitor -p /dev/ttyACM0 -c baudrate=9600` exits with Ctrl+C. Restart the service afterward.

## Default recipe in the firmware

| Stage | Target | Duration / transition |
| --- | --- | --- |
| Heat to Strike | 145 F | Advance when target reached |
| Initial Grain Rest | 145 F | 10 minutes; heater disabled |
| Mash Hold | 148 F | 60 minutes |
| Sparge Heat | 165 F | 30 minutes |
| Done | — | Heater disabled |

This table describes **intended firmware behavior**, not verified physical performance. Pump control and operator interventions require review.

## Safety and current limitations

**Do not connect or energize the heater based on the current code.** A review of the checked-in sketch found `analogWrite()` driving D9 for the SSR. That is high-frequency PWM on an Uno, which is generally unsuitable for a conventional AC zero-cross heater SSR; use a properly engineered time-proportioning scheme instead. Sensor disconnection, overtemperature, reboot behavior and fail-safe outputs need independent validation. Provide a physical manual disconnect and an independent thermal cutoff.

The checked-in `arduino-deploy.sh` uses `sudo kill -9` on any process holding the serial port. It can kill the Flask service and interrupt the controller, and its final-attempt test may incorrectly report failure after a successful third attempt. **Do not run this script during a brew.** Stop the known service cleanly and review the script before use.

See [Troubleshooting](docs/TROUBLESHOOTING.md) and [Roadmap](docs/ROADMAP.md).

## Documentation

- [Installation and service setup](docs/INSTALLATION.md)
- [Hardware, pinout and safety](docs/HARDWARE.md)
- [Troubleshooting and diagnostics](docs/TROUBLESHOOTING.md)
- [Development roadmap](docs/ROADMAP.md)

## License

MIT; see [LICENSE](LICENSE). Verify the license and provenance of any vendored third-party components separately.
