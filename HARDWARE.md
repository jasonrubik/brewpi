# Hardware, signals and safety

This page describes pin assignments visible in the checked-in Arduino firmware, not a verified electrical schematic. No mains wiring instructions are provided.

## Arduino Uno pin assignments

| Pin | Firmware purpose | Notes |
| --- | --- | --- |
| A0 | Vessel NTC thermistor | 10 kOhm nominal at 25 C; voltage-divider wiring must match firmware formula |
| A1 | LM35 heat-exchanger outlet | 10 mV/C nominal; validate reference and sensor-fault behavior |
| D9 | Heater SSR control | Current sketch uses `analogWrite`; **not safe for a typical zero-cross AC SSR** |
| D13 | Heat status LED | Firmware indication, not physical proof of heater state |
| D12 | Mash OK LED | Temperature-in-band indication |
| D11 | Alarm/buzzer | Logic-level output; check drive circuitry |
| D7 | Reset button | Active-low / INPUT_PULLUP |
| D8 | Skip button | Active-low / INPUT_PULLUP |
| D4 | Pause button | Active-low / INPUT_PULLUP |
| D5 | Setpoint up | Active-low / INPUT_PULLUP |
| D6 | Setpoint down | Active-low / INPUT_PULLUP |
| USB | Pi serial link | 9600 baud in sketch |

The Raspberry Pi installation also uses GPIO 27 as a READY LED output and GPIO 17 as a boot indicator. Verify actual pin setup and LED wiring in the deployed Python and Pi configuration.

## Temperature measurement

The project uses an NTC vessel sensor and an LM35 HEX outlet sensor. Calibration measurements and the Steinhart-Hart coefficients must be reconciled before enabling heater control. A disconnected analog sensor may return plausible-looking noise rather than a recognizable fault.

The proposed divider is 5 V -> NTC -> A0 -> 10 kOhm fixed resistor -> ground. For this orientation, `R_NTC = R_fixed * (1023 / ADC - 1)` (idealized 10-bit ADC). If wired in the reverse orientation, the formula differs. Check the checked-in sketch against the physical circuit before relying on its readings.

## Heater and pump

The intended HERMS heater is 1200 W with an SSR; the pump is a separate process component. The checked-in sketch uses Uno PWM on D9. Before connecting a mains heater, replace this with a suitable low-frequency time-proportioning output and validate OFF-state behavior through startup, reset, sensor failure, watchdog failure, and loss of USB/Pi connectivity.

**Minimum safety requirements before wet/hot testing:** independently rated high-limit cutoff, manual mains disconnect, properly protected and grounded electrical enclosure, appropriate overcurrent and GFCI protection, dry-run/flow protection, fail-off hardware behavior, and supervised testing. These are design requirements, not features proven to exist in the repository.

## READY indicator semantics

A successful `serial.Serial(...)` open only proves a device file opened. A useful READY signal should require actual fresh, valid Arduino telemetry and a healthy web application. On stale data, invalid sensor values, or disconnection, READY should turn off. This behavior needs verification.
