# Development roadmap

This is a proposed work plan, not a claim that the listed features are already implemented or tested.

## P0: Make the controller safe for bench testing

- [ ] Audit firmware for all possible SSR ON paths and enforce fail-off startup/reset behavior.
- [ ] Replace high-frequency PWM on D9 with appropriate time-proportioning control for the selected SSR.
- [ ] Detect open, shorted, out-of-range and implausible NTC/LM35 readings.
- [ ] Verify actual Steinhart-Hart coefficients and NTC divider orientation against calibration measurements.
- [ ] Confirm high-temperature and excessive rate-of-rise trips latch heater OFF.
- [ ] Add independent hardware high-limit and manual disconnect before mains testing.
- [ ] Define behavior when the Pi disconnects, serial data becomes stale or Arduino reboots.

## P1: Restore reliable telemetry

- [ ] Ensure a single serial-port owner in the Flask process.
- [ ] Verify the dashboard's `serial_update` and `recent_log` handlers against actual firmware output.
- [ ] Make READY LED depend on recent valid telemetry, not only opening the serial port.
- [ ] Add serial reconnect integration tests and a simulated serial input mode.
- [ ] Confirm session downloads work with actual saved log paths.

## P2: Deployment and reproducibility

- [ ] Replace blind `kill -9` in deployment with controlled service stop/restart.
- [ ] Correct deployment retry success/failure reporting.
- [ ] Add a noninteractive compile-only check and firmware build CI.
- [ ] Pin/record Arduino CLI core and library versions.
- [ ] Document the exact Pi service unit and GPIO boot configuration.
- [ ] Remove or document the purpose and provenance of bundled tool archives.

## P3: Brewing workflow

- [ ] Confirm serial command semantics match web buttons, including RESUME and SHUTDOWN.
- [ ] Test recipe upload, step transitions, timers, pause/resume and reset behavior.
- [ ] Validate pump/operator prompts for strike, grain rest, mash and sparge stages.
- [ ] Add unit and hardware-in-the-loop tests for recipe timing and failure cases.
- [ ] Add clear disconnected/stale sensor indicators to the UI.

## Release criterion

Do not describe the controller as ready for live heating until the safety items above are tested with documented results and independent electrical protections are installed.
