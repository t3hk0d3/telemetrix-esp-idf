# Changelog

## Unreleased

- Repository skeleton, styleguide and public API header.
- M1, the stock Telemetrix4Esp32 3.2.0 server: protocol framing and the stock command set, GPIO, PWM, servo, ADC, I2C,
  1-wire, DHT22 and HC-SR04 drivers on the IDF, the TCP server with back-pressure, keepalive, an allowed-address list
  and an access hook, a host build with fake drivers, integration tests with the stock client, fuzz targets, and
  examples for the Olimex ESP32-POE (Ethernet and Wi-Fi), ESP32-S3 and ESP32-C6 (a P4 build check).
- Compatibility tools: a recording proxy and a script that compares upstream firmware with this component.
- README: profile and grants, protocol table, deviations from upstream, requirements and security notes.
- M2, extended mode and the Python client: the handshake and sequence-numbered commands with per-command ERROR and
  SYNC barriers (SYNC flag governs the commands after it), ON_BUS for I2C and 1-wire buses beyond 0, capability
  discovery (chip, pins, buses, limits, plugins), the plugin API with named pin capabilities, `cfg.extended` and
  `CONFIG_TELEMETRIX_EXTENDED`, status counters, an extended fuzz target, and the asyncio client `telemetrix_idf` in
  `client/` (chains, events, stock-mode fallback, plugin halves with request ids). Plus `tools/hostile/hostile_client.py`,
  a flood/garbage/stall/storm script for the host server and boards.
- `telemetrix_config_t.task_stack_bytes` (default 6144, at least 4096): the network task's stack, on which plugin hooks
  run. Applications must start from `telemetrix_config_default`: a configuration built from zeroes now gets `extended`
  false, `plugin_budget_us` 0 and `task_stack_bytes` 0, which `telemetrix_start` refuses.
