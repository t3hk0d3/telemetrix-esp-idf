# telemetrix-esp-idf

An ESP-IDF component that serves the stock Telemetrix4Esp32 3.2.0 protocol over TCP, so unmodified
[telemetrix](https://github.com/MrYsLab/telemetrix) clients can drive GPIO, PWM, servos, ADC, I2C, 1-wire, DHT and
sonar on an ESP32 board. Targets: esp32, esp32s3, esp32c6, esp32p4 and the IDF `linux` host target. The application
owns the network and hands the component a profile of pins and peripheral grants.

The wire protocol and the Python client come from Telemetrix4Esp32 by Alan Yorinks; this component reimplements the
server side from that protocol. Licensed under AGPL-3.0-or-later, copyright Igor Yamolov.

## Protocol

To be written: framing, the supported command ids and the reports.

## Deviations from Telemetrix4Esp32

To be written. Planned entries:

- The servo-unavailable reply carries its length byte (upstream omits it).
- RESET releases pins and peripherals and never reboots.
- Every pin and count is checked against the profile.
- Report 7 (I2C too few) is never sent.

## Development

`make help` lists the targets. See `STYLEGUIDE.md` for the conventions.
