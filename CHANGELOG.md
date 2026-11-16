# Changelog

## Unreleased

- Repository skeleton, styleguide and public API header.
- M1, the stock Telemetrix4Esp32 3.2.0 server: protocol framing and the stock command set, GPIO, PWM, servo, ADC, I2C,
  1-wire, DHT22 and HC-SR04 drivers on the IDF, the TCP server with back-pressure, keepalive, an allowed-address list
  and an access hook, a host build with fake drivers, integration tests with the stock client, fuzz targets, and
  examples for the Olimex ESP32-POE (Ethernet and Wi-Fi), ESP32-S3 and ESP32-C6 (a P4 build check).
- Compatibility tools: a recording proxy and a script that compares upstream firmware with this component.
- README: profile and grants, protocol table, deviations from upstream, requirements and security notes.
