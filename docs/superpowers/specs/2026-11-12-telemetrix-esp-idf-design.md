# telemetrix-esp-idf: a modular Telemetrix server for ESP-IDF and its Python client: design

Status: revision 3 for the user's review, 2026-11-16: an adversarial architecture review of revision 2 folded in, v1
split into milestones M1–M3 (section 6). A standalone project, built from scratch in this repository. An earlier
implementation of a first revision (`~/dev/telemetrix-esp32-idf`, never published, never on a board) may be read for
reference; nothing is carried over wholesale (it has the trailing-bus-byte bug of 4.4). Two deliverables: the ESP-IDF
component (section 4) and the Python client in `client/` (section 5).

## 1. Intent

### Why

ESP32 boards often have spare GPIO headers. The user wants to drive miscellaneous devices wired to them (relays,
motors, buttons, sensors, serial devices such as a GPS receiver or a Modbus RS485 adapter) from software running
elsewhere on the network, with mqtt.io as the first application on the network side. Any ESP-IDF application can embed
the component next to its own work.

### Why Telemetrix

Compared in the brainstorm (sources in section 9):

- **pigpio** (what gpiozero uses remotely): no release since March 2021; edge timestamps only, so DHT, HC-SR04 and
  1-wire would need microsecond timing across the network.
- **lgpio / rgpiod**: small protocol, but little activity, no gpiozero remote factory, same timing problem.
- **Greybus**: the only option that makes the board look like local Linux hardware, but the Linux host side needs
  staging drivers, an out-of-tree `gb-netlink` module and `gbridge` (last commit 2019).
- **Firmata**: many clients, but a MIDI-style 7-bit protocol, and DHT/1-wire are extensions not every client speaks.
- **Telemetrix** (chosen): actively maintained, an ESP32 TCP server already exists, and the device runs
  timing-critical sensor drivers itself and reports values. AGPL-3.0.

The upstream ESP32 server (`Telemetrix4Esp32`, an Arduino sketch) cannot be used as is: it targets the ESP32-classic
(pin, touch, DAC and ADC assumptions), indexes fixed arrays with unchecked pin numbers, has no access control or idle
timeout, keeps pin state across connections, and reboots the chip on RESET. Hence our own ESP-IDF implementation of the
same wire protocol.

### What success looks like

- An ESP-IDF application adds the component, passes a board profile, and our client (any chip) or the stock
  Telemetrix ESP32 client (original ESP32 only: its pin lists are ESP32-classic) switches relays, reads buttons and
  drives PWM on that board over the network.
- DHT, HC-SR04 and 1-wire sensors report values, with the timing done on the board; I2C devices are reached on more
  than one bus.
- A client opens a UART or an SPI device on the board for the length of its session and talks to it over a side TCP
  port (4.5).
- The component runs on any ESP32 chip the IDF supports, next to application code that uses the same kinds of
  peripherals, because it only touches what the profile grants (4.2).
- When the client vanishes, every pin goes to its configured safe state; a relay never switches because a client left.
- Our Python client works with this server in extended mode (discovery, buses, streams, plugins, confirmed calls,
  errors tied to the call that caused them) and with stock Telemetrix4Esp32 firmware in stock mode (5.3).
- Nothing a client does reboots the board or starves the application's own tasks.

## 2. Goals, non-goals, anti-goals

Goals:

- An ESP-IDF component published to the ESP Component Registry, AGPL-3.0.
- Wire compatibility with Telemetrix4Esp32 3.2.0 for the commands in scope, plus an extended mode (4.4) entered by a
  safe handshake: numbered commands, error reports, discovery, bus selection, a plugin envelope; UART and SPI streams
  as a built-in plugin (4.5).
- A modular component: internal drivers per chip capability, and a public API for extension plugins (4.1, 4.6).
- Board profiles supplied by the application that grant pins, peripheral instances and per-pin safe states; nothing
  chip- or board-specific hard-coded in the protocol layer.
- Our own async Python client with extension support and a stock-firmware mode (section 5).
- Delivered in three milestones (section 6).

Non-goals (v1; possible later):

- Steppers, touch, BLE transport, scripts; upstream's in-band SPI commands (SPI goes through streams).
- DAC (deferred by the user after the review; DAC commands are refused as out of scope).
- Fixed UART bridges (a UART served on a TCP port with no Telemetrix client); deferred.
- A synchronous wrapper for the client, and third-party client plugins loaded through Python entry points; deferred.
- A protocol-neutral core with more than one wire protocol (4.1).
- Our client talking to Telemetrix4Arduino or other non-ESP32 Telemetrix servers.
- The mqtt.io module (5.6) and a patch to the upstream Python client's pin lists.

Anti-goals (a failure even if it works):

- A client can reboot the board, reach a pin or a peripheral instance the profile does not grant, or disturb a
  peripheral the application uses.
- A malformed or hostile packet crashes the board or corrupts memory.
- A pin driven before the client asked for it, or left in anything but its safe state after the client is gone.
- A stock Telemetrix client that breaks against this server, or receives a report it does not know.
- Our client sending anything to stock firmware that could crash, desync or reboot it.
- An error raised on the wrong call.
- A client that stops reading wedging the server or the application.

## 3. Constraints

- ESP-IDF v6.1; must build for `esp32`, `esp32s3`, `esp32c6` and `esp32p4`.
- One styleguide for the repository (written in M1's first task): astyle, `snake_case`, public prefix `telemetrix_`,
  internal `tmx_`, `esp_err_t`, host tests on the IDF `linux` target with Unity; for `client/`, ruff and pytest. The
  styleguide bans `gpio_reset_pin` in the component (it enables the pull-up, which is not a safe state, 4.3).
- No pin or peripheral knowledge outside the profile; no array indexed by an unchecked number from the wire.
- lwIP sockets and TCP PCBs are a shared, small budget in embedding applications: the component publishes the count it
  needs (main listener, main client, one transient socket for refusals, and two per stream) and reserves stream
  sockets at open, not lazily.
- Memory placement (4.8) is specified, not left to "PSRAM where latency allows".
- Hardware verification happens on spare boards (Olimex ESP32-POE, ESP32-S3, ESP32-C6) in batched passes after review,
  never inside implementation tasks.

## 4. The ESP-IDF component

### 4.1 Shape

- One ESP-IDF component in this repository, published to the ESP Component Registry. Name `telemetrix-esp-idf`,
  subject to the courtesy check in section 8.
- Examples: the Olimex ESP32-POE (original ESP32, LAN8720 Ethernet) over Ethernet and over Wi-Fi, an ESP32-S3 dev
  board over Wi-Fi, and the S3 example also built for the ESP32-C6. A host build of the server (4.7).
- The application owns the network (Wi-Fi, Ethernet) and calls the component to start and stop the server.

Inside, four parts:

- **Core.** The board profile, pin claims and peripheral-instance claims (one owner each), safe states and release
  (4.3), the scheduler, the network task and sockets, the access check, back-pressure, and the plugin registry. It
  knows the Telemetrix framing but no command's meaning.
- **Drivers.** Internal, one per on-chip capability: GPIO, LEDC (PWM and servo), ADC, I2C, 1-wire, pulse capture (DHT,
  sonar), UART, SPI. Each is compiled only for chips that have the hardware (the IDF's `SOC_*` capabilities) and when
  its menuconfig switch is on, and uses only peripheral instances the profile grants. Drivers are not a public API in
  v1: adding one (touch, PCNT) is a change to this repository.
- **Telemetrix adapter.** Maps the stock commands onto the drivers and implements extended mode (4.4).
- **Extension plugins.** Features beyond stock Telemetrix, reached through the plugin envelope (4.4) in extended mode
  and announced by discovery. Built in: UART and SPI streams (4.5). Third-party ones are registered by the application
  through the public plugin API (4.6).

Not built (the user's decision): a protocol-neutral core with Telemetrix as one of several wire protocols.

Trust model: third-party plugins are trusted code compiled into the application. A fault in one can crash the board;
the core measures their timing (4.3) but does not sandbox them.

### 4.2 Configuration

**Board profile** (passed in at start, owned by the application):

- *Pins.* A table of GPIOs the component may use. Per GPIO: capabilities (digital in, digital out, pull-up, pull-down,
  PWM, ADC with unit and channel, I2C, 1-wire, stream pin, plus named capabilities that third-party plugins declare:
  a plugin may only claim pins the profile grants it by that name), and a **safe state**: float (input, no pull),
  pull-up, pull-down, drive low or drive high. The safe state is applied when the server starts, on disconnect, on
  RESET and on stop. Default: float. The table is the only source of pin knowledge.
- *Peripheral grants.* Which instances the component may use: LEDC timers and channels (speed mode included), RMT
  channels (TX and RX counts), ADC units as handles the application passes in (so an application that also reads ADC1
  shares the one handle the IDF allows per unit), I2C buses as fixed ports with pins or as an existing
  `i2c_master_bus_handle_t` the application shares, UART ports and SPI hosts for streams. A shared bus is never
  deleted by the component; only its own device handles are.
- *Buses.* The I2C and 1-wire buses the client may select, each with its bus number and its grant.
- *Stream resources.* The UART ports and SPI hosts streams may use, and the GPIOs marked "stream pin".

The limits announced by discovery (servos, PWM frequencies, DHT and sonar devices, buses, streams) are computed from the
grants, never larger than the hardware can serve.

**Runtime settings:** TCP port (default 31336), enabled, allowed client addresses, keepalive, send-stall timeout,
report interval floor, I2C transaction timeout (default 50 ms), handler time budget, the side port range (default
31337–31344), the most open streams at once, the stream connect timeout (default 10 s), the stream idle limit (default
60 s without a side client), the largest SPI frame (default 4096 bytes), task priority and core.

**Hooks:**

- `access`: decides whether a connecting peer is allowed. Precedence: a peer must be in the allowed-address list (empty
  list = any) **and** be allowed by the hook; the hook is called only for peers the list admits. Applies to every
  socket the core or a plugin accepts.
- `reset`: called on the RESET command after the release. It cannot be used to reboot in v1 (the component documents
  why: the stock client sends RESET on every exit).
- `connected` / `disconnected`: for status and logging.

**Build-time switches** (menuconfig): one per driver and per built-in plugin, each removable to save flash and RAM.

### 4.3 Server behaviour

Connections:

- TCP server, one client at a time. A second connection while one is active is accepted and closed with RST
  (`SO_LINGER` 0, so no TIME_WAIT PCB is left), counted. `TCP_NODELAY` on every accepted socket.
- Dead-client detection: TCP keepalive (idle, interval, count in the settings) **and** a send-stall timeout: a client
  whose receive window stays closed (it stopped reading) for longer than the timeout (default 10 s) is dropped. The
  application can also kick the client.
- On disconnect, RESET and stop: every claimed pin to its safe state, PWM stopped, the component's own buses and device
  handles closed, reports stopped, every plugin's release hook run, streams closed.

Framing and errors:

- Every received packet is length-checked before it is read.
- In stock mode (4.4): an over-long length, a short payload, an unknown or out-of-scope command id are dropped and
  counted, and the connection stays, as stock clients expect. Length 0: a one-byte packet with no command, ignored and
  counted.
- In extended mode: any framing violation (length 0, a length over the extended cap, an unknown command id) closes the
  connection, because the stream can no longer be trusted. A well-framed command that is refused produces an error
  report (4.4) and the connection stays.

Timing (the single network task must never block for long):

- Every driver operation the network task calls is non-blocking or bounded: sonar and DHT are asynchronous (started
  by the scheduler, completed from a GPIO ISR or an RMT callback, then reported); I2C transactions carry the
  configured timeout (the IDF's "wait forever" is never used); 1-wire operations are bounded by their slot timing;
  SPI transfers run on a worker task (4.5).
- Plugin handlers and poll hooks get a time budget (default 5 ms). The core measures every call, counts overruns per
  plugin and shows them in the status. It does not abort a handler.
- Other tasks (ISR callbacks, the SPI worker, plugins' own tasks) wake the network task through an eventfd in the
  `select()` set; UART receive is seen through the UART's VFS file descriptor in the same set.

Back-pressure:

- Sends are non-blocking with a bounded queue per connection (size: builder's call).
- Replies to commands (I2C data, 1-wire results, discovery, error reports, sync replies, stream opened) are never
  dropped. Periodic input reports (digital, analog) are coalesced to the latest value per pin when the queue is full;
  sonar and DHT keep only the latest reading per device.
- A client that keeps the queue full past the send-stall timeout is dropped (above).

Values (the stock layer's semantics, as upstream 3.2.0): analog reports are raw 12-bit ADC readings at 12 dB
attenuation; PWM duty is in the client's resolution; servo pulse = min + angle × (max − min) / 180; I2C reads are clamped
to the upstream maximum; DHT is DHT22/AM2302.

### 4.4 Wire protocol

Framing both ways: `[len][id][payload]`, `len` counting `id` and payload.

**Stock mode** (every connection starts here). Byte-identical to Telemetrix4Esp32 3.2.0 for the commands in scope,
including upstream's own refusal reports (servo unavailable, I2C too few/too many bytes), whose layouts we emit as
upstream does except that the servo report carries its missing length byte. Payload cap 30 bytes. 16-bit values
big-endian; DHT floats and the PWM frequency double in little-endian. Section 9 links the sketch and client constants
that define it.

In scope: `LOOP_COMMAND`, `SET_PIN_MODE` (input, input pull-up, input pull-down, output, analog, PWM),
`DIGITAL_WRITE`, `ANALOG_WRITE`, `MODIFY_REPORTING`, `GET_FIRMWARE_VERSION`, `SERVO_ATTACH`, `SERVO_WRITE`,
`SERVO_DETACH`, `I2C_BEGIN`, `I2C_READ`, `I2C_WRITE`, `SONAR_NEW`, `DHT_NEW`, `STOP_ALL_REPORTS`,
`SET_ANALOG_SCANNING_INTERVAL`, `ENABLE_ALL_REPORTS`, `ANALOG_OUT_ATTACH`, `ANALOG_OUT_DETACH`, `RESET` (releases,
never reboots), and the 1-wire set (`ONE_WIRE_INIT` through `ONE_WIRE_CRC8`). Stock commands always act on bus 0.
The PWM channel a client names is a request: the server allocates from its grant and ignores the number (the stock
client asks for channel 0 for every pin).

Out of scope: touch, DAC, upstream's SPI commands, steppers: accepted, ignored and counted in stock mode; refused with
an error report in extended mode.

**Handshake.** `GET_FIRMWARE_VERSION` with an empty payload gets the stock 5-byte answer. With a payload of a magic
byte and the client's extension-protocol version (major, minor), the answer is longer: the three stock version bytes,
the magic byte, and the server's extension-protocol version. Checked against upstream 2.0.1–3.2.0: every release reads
and ignores a short payload on this command and sends its fixed 5 bytes, so the probe is safe on stock firmware. Rules:

- Same major: the session enters extended mode at once, at the lower minor of the two.
- Different major: the server answers with its version and stays in stock mode; our client stops with an
  "incompatible firmware" error.
- The client waits for the answer with a timeout (default 3 s; a stock board busy with another client never serves a
  second connection) and discards any report that arrives before it (stock firmware keeps reporting pins from earlier
  sessions).
- Once in extended mode, a session stays there; a later `GET_FIRMWARE_VERSION` gets the long answer again.

**Extended mode** (only after the handshake; a stock client never enters it):

- *Command numbering.* The server numbers every command it receives in the session with a 16-bit counter, starting at
  0 with the first command after the handshake reply; the client knows each command's number because handling is
  strictly in order. Wraps at 65536.
- *Error report.* `[seq u16][command id][reason]`, plus `[slot][sub]` for plugin commands. Every refused command,
  including out-of-scope ones, produces exactly one, before the server handles the next command. Reasons: a fixed,
  documented table (no such pin, capability missing, pin busy, peripheral exhausted, bad argument, bus unknown, not in
  scope, hardware error, plugin-specific).
- *Sync.* `SYNC [tag u16][flags]` → `SYNC_REPLY [tag u16][seq u16 of the sync]`. Sent after the server has handled
  and queued the replies of every earlier command. The client's barrier for confirmed calls (5.4). Flag bit 0
  ("abort on error"): if any command since the previous sync was refused, the commands after the refused one were
  skipped, each skip reported as an error with reason "skipped"; the client sets it by default so a chain is all or
  nothing up to its first refusal.
- *Bus selection.* `ON_BUS [bus][stock command id][stock payload]` runs one stock I2C or 1-wire command on that bus;
  its reply comes back as `BUS_REPORT [bus][stock report bytes]`. Stock layouts stay byte-exact (no trailing bytes:
  `ONE_WIRE_CRC8` already sends one byte more than its count, and `I2C_BEGIN` takes optional pins, so a trailing bus
  byte is ambiguous).
- *Discovery.* `GET_CAPABILITIES [page]` → `CAPABILITIES [page][last page][records]`. Records are type-length-value:
  `[type][len][value]`. Types: chip (model, revision, IDF version string), pin (GPIO, capability bits u16, named
  capability ids, safe state, ADC unit and channel), bus (kind, number, fixed pins or "client chooses"), limit (kind,
  count), plugin (slot, extension-protocol major and minor, name), named capability (id, name). Unknown types are
  skipped by the client, so later versions can add types.
- *Caps.* Command payload up to 254 bytes (the largest the length byte allows). Reports up to 255 bytes on the wire.
- *Plugin envelope.* `PLUGIN [slot][sub][args]` and `PLUGIN_REPORT [slot][sub][data]`. Slots are assigned at
  registration and valid for the session only; the client never caches them across connections. Plugin names are
  namespaced (`vendor.name`; built-ins use `telemetrix.`), unique, with a major.minor version: the client half refuses
  a different major. A plugin result that is not sent before the next command is handled (work on the plugin's own
  task) carries a request id the client chose in the command.

All new command and report ids are above upstream's range (57 or higher), chosen by the builder, listed in the README,
and confirmed in the courtesy check (section 8).

### 4.5 Streams plugin (UART and SPI over a side port)

The user's idea: a command on the main connection opens a session on a UART or an SPI host; the server answers with a
TCP port, and the data flows over that side connection. A built-in extension plugin (`telemetrix.streams`).

Sub-commands and reports:

- *Open*: kind (UART or SPI), resource (from the profile's stream resources), pins, settings. UART: TX, RX, optional
  DE (RS485), optional RTS and CTS (hardware flow control), baud rate, data bits, parity, stop bits. SPI: SCLK, MOSI,
  MISO (each optional where the device does not need it), CS, mode (0–3), clock. Optional: "token required".
- *Opened*: a stream id, the side TCP port, and a 16-byte token when one was requested; or a status code (pin not a
  stream pin, pin or resource busy, stream limit, no free port, settings refused). Failure is reported here only, not
  also as an error report.
- *Close*: by stream id. *Closed*: sent with the reason when a stream ends without the client asking (connect timeout,
  idle limit, hardware error).

Side connection:

- Accepted only from the main session's peer address, compared after normalisation (`::ffff:a.b.c.d` equals
  `a.b.c.d`), and only when the access check (4.2) also allows it. One side client per stream; a second is refused
  with RST and counted.
- With a token, the side client's first 16 bytes must be the token, or it is closed; the same holds for reconnects.
  Without one, unmodified tools (gpsd, socat, a Modbus library) connect directly. The client library always asks for
  a token; tools that cannot send one use a stream opened without it.
- If nothing connects within the connect timeout, or no side client is connected for the idle limit, the stream
  closes and reports it. A side client may reconnect while the stream is open.
- Lifetime: a stream ends on *Close*, on RESET, or when the main session ends; then its port closes, its hardware is
  released and its pins go to their safe states.

Data:

- UART: raw bytes both ways, through a per-stream buffer each way (sizes in 4.8). When the side client reads too
  slowly, bytes from the UART are dropped and counted (or held back by RTS when flow control is on); when the UART is
  slower than the side client, the server stops reading the socket until the TX buffer drains. With DE, the UART runs
  in hardware RS485 half-duplex mode.
- SPI: framed transactions, lengths big-endian. Request: 16-bit length, a flags byte (bit 0: keep CS asserted after
  this transfer), the bytes to send. Reply: 16-bit length, the bytes read back. Transfers run on the SPI worker task,
  never on the network task. An oversize frame or a malformed header closes the side connection (not the stream) and
  is counted.

### 4.6 Plugin API (public, for extension plugins)

- Registration when the application starts the component; built-ins are registered first, then the application's in
  the order given. Slots follow that order. Two plugins with the same name: start fails.
- A plugin provides: its namespaced name and version, the named pin capabilities it needs, a sub-command handler, an
  optional poll hook with its next deadline, and a release hook (disconnect, RESET, stop).
- The core gives a plugin: pin claim and release against the profile, peripheral-instance claims from the grants,
  report emit through the envelope, error reports with a plugin-specific reason, scheduler deadlines, side-port
  sockets (listen, accept with the access check, the main-peer binding and optional token of 4.5, keepalive, RST
  refusal), a wake call and a thread-safe emit for work on the plugin's own task, counters, and a status entry.
- Handlers run on the network task within the time budget (4.3).
- A plugin cannot change stock-mode behaviour or send anything to a client that is not in extended mode.

### 4.7 Tests

- **Host unit tests** (Unity, IDF `linux` target): framing, every stock command's parsing against byte fixtures
  taken from the upstream client's code, refusals, safe states, peripheral grants and limits, the handshake (each
  rule of 4.4), command numbering and error reports, sync with and without abort, bus selection (including
  `ONE_WIRE_CRC8` from the stock client unchanged on bus 0), discovery pages, the plugin registry and envelope,
  back-pressure (coalescing, never dropping replies), and the streams plugin's table and SPI framing. A test plugin
  exercises the public API end to end.
- **Host-built server.** The real server built for the `linux` target with a fake driver layer, run as a process in
  CI. Both clients run against it: the stock `telemetrix-esp32` client (stock mode) and ours (both modes). This tests
  the real socket code, the framing policy and the client together, but not lwIP; lwIP behaviour (keepalive, RST,
  socket limits) is checked on boards.
- **Fuzzing.** A libFuzzer target on the host over the framing and the command checker, in both modes.
- **Hostile-client script** (host server and boards): floods, garbage, a client that stops reading, a storm of second
  connections; the server stays up, the stalled client is dropped, PCBs do not leak.
- **Compatibility on the Olimex ESP32-POE.** A recording proxy sits between the stock client and the board; one
  script drives upstream's Telemetrix4Esp32 firmware and then ours. The comparison normalises values that legitimately
  differ (ADC noise, DHT and sonar readings, timing, the firmware version) and asserts that our server sends only
  report ids and lengths the stock client knows. Over Wi-Fi (upstream's sketch is Wi-Fi only); the Ethernet example
  runs the same script afterwards.
- **Other boards.** ESP32-S3 over Wi-Fi with our client; the C6 example is a compile check. Streams on a spare board
  with loopback wires (UART TX↔RX, SPI MOSI↔MISO), checked byte for byte through our client.

### 4.8 Memory

- Internal RAM: the network task's stack, the socket send queues, ISR-shared state, and SPI DMA buffers (DMA-capable,
  cache-line aligned on S3 and P4; one transmit and one receive buffer per SPI stream, `spi_frame_max` each).
- PSRAM when the application enables it, otherwise internal: UART stream buffers (default 4 KB each way per stream),
  the discovery page buffer, and plugin-owned buffers.
- The README lists the totals per stream and per connection so the application can budget them.

## 5. The Python client (`client/`), and later the mqtt.io module

### 5.1 Shape

- A Python library in this repository's `client/` folder (the user's choice: a wire-contract change lands on both
  sides in one commit), published to PyPI, AGPL-3.0. Provisional name `telemetrix-idf-client` (import
  `telemetrix_idf`), settled with the courtesy check (section 8).
- asyncio only in v1 (mqtt.io is asyncio). Python version floor: the oldest mqtt.io supports, at least 3.10.
- Plugin halves for the built-in plugins ship in the package. Third-party halves are registered explicitly by the
  application in code; loading through entry points is deferred.

### 5.2 Connecting

- The client sends `GET_FIRMWARE_VERSION` with the magic payload, waits with the handshake timeout, discards earlier
  reports, and picks the mode from the answer (4.4).
- Extended mode: it fetches every discovery page and builds the board object (pins and their capabilities, buses,
  limits, plugins and their slots for this session).
- Stock mode: 5.3.
- A lost connection is reported to the user; no automatic reconnect in v1.

### 5.3 Stock mode

For Telemetrix4Esp32 3.2.0 (older 3.x only where their layouts match, checked in the plan; below 3.0 not supported):

- Pins and capabilities from upstream's ESP32 pin lists.
- The client never sends RESET (it reboots stock firmware); `reset()` raises "not supported by this firmware".
  Releasing pins is the caller's job: stock firmware keeps pin modes and reports across connections and does not
  release on disconnect. The client says so in its documentation.
- On connect it sends `ENABLE_ALL_REPORTS` (stock firmware keeps "reports stopped" across sessions) and ignores
  reports for pins it has not configured.
- Upstream's own refusal reports (servo unavailable, I2C too few/too many) become exceptions; other refusals are
  silent on stock firmware, so a stock-mode chain completes on its `LOOP` barrier with no refusal check, and the
  documentation says so.
- Calls that need extended mode (bus selection, discovery, plugins, streams) raise "not supported by this firmware".

### 5.4 Async chaining API (the user's choice)

- Calls chain into a batch, and awaiting the chain sends it: `await board.pin(17).output().write(1)`. The batch and its
  barrier (`SYNC` in extended mode, `LOOP` in stock mode) go out in one write with no await in between, so batches from
  different coroutines never interleave.
- The await completes when the barrier's reply arrives. In extended mode, errors are matched to commands by their
  sequence number, never by arrival order. The raised exception carries a result per command (done, refused with
  reason, skipped). By default a chain is "all or nothing up to the first refusal" (`SYNC` abort flag); a chain can opt
  out.
- Barrier tags are 16-bit, and a tag whose wait timed out is not reused until its reply arrives or the connection
  drops.
- Reads and requests with replies (I2C read, 1-wire, discovery, stream open) complete with their reply.
- Cancellation: commands already written have been applied by the board; cancelling only stops the wait. Documented.
- Timeouts on every await (default 5 s).
- A chain built and never awaited sends nothing; the client warns about it when the chain object is collected.
- Input changes (digital, analog, sonar, DHT) are delivered through callbacks and async iterators. Callbacks run off
  the connection's reader task (a dispatch task with its own queue), so a slow callback cannot stall barrier replies.
- Streams: opening a UART stream returns an asyncio reader and writer (the token is sent for the caller); an SPI stream
  returns an object with a `transfer` call.

### 5.5 Client tests

- Unit tests with a small fake server for stock mode only (the protocol from upstream's client and sketch).
- Integration tests against the host-built server (4.7) for extended mode: handshake, discovery, sequence-numbered
  errors under concurrent coroutines, abort flag, timeouts and late replies, cancellation, back-pressure from a slow
  callback, streams.
- On spare boards: one run of our client in stock mode against upstream's firmware on the ESP32-POE; the stream and
  extended-mode checks of 4.7.

### 5.6 Later: the mqtt.io module

Separate brainstorm once M3 is done. mqtt.io's `gpiozero` module cannot reach this server and its sensor modules call
local libraries (smbus2, w1thermsensor, RPi.GPIO, Adafruit DHT); the module will build on this client.

## 6. Milestones

Each milestone ends with review, fixes and one batched spare-board pass.

- **M1: stock server.** Styleguide and scaffold; core (profile with pin capabilities, safe states and peripheral
  grants; claims; scheduler; network task with back-pressure, send-stall timeout, RST refusals, eventfd); drivers
  (GPIO, LEDC, ADC, I2C with timeout, 1-wire, asynchronous pulse capture); the stock layer; the host-built server;
  host unit tests and the fuzzer; the ESP32-POE and S3 examples, the C6 compile check; the recording-proxy
  compatibility test with the stock client. Done when the stock client works against the ESP32-POE and the recording
  proxy finds nothing the stock client does not know.
- **M2: extended mode and our client.** Handshake, command numbering, error reports, sync with abort, bus selection,
  discovery, caps, the close-on-framing-error policy; the plugin registry, envelope and public plugin API with the test
  plugin; the Python client with both modes and the async chaining API; integration tests against the host-built
  server; the hostile-client script. Done when our client drives the ESP32-POE in extended mode and upstream's
  firmware in stock mode.
- **M3: streams.** The streams plugin with UART (DE, flow control, buffers) and SPI (worker task, DMA buffers), side
  ports with address binding and tokens, idle and connect limits; client stream API; loopback tests on spare boards.
  Done when a UART and an SPI stream pass the loopback tests through our client.
- **M4: documentation and site.** A documentation site on GitHub Pages covering the finished v1 (server, client,
  extended mode, plugins, streams). Built with MkDocs Material; deployed by a GitHub Actions workflow on pushes to
  `main` and on release tags, versioned per release with `mike` (`latest` plus each release). Content, arranged by
  what the reader is doing:
  - Getting started: one tutorial per example board, from flashing to the first awaited chain in our client;
    a stock-client tutorial for existing Telemetrix users.
  - How-to guides: writing a board profile (capabilities, safe states, grants); embedding in an existing
    application (shared I2C/ADC handles, Wi-Fi/Ethernet bring-up, the requirements now in the README); writing a
    plugin; porting to a new chip or board; hardening (allowed addresses, access hook); using streams.
  - Reference: the wire protocol byte by byte for stock and extended mode (every command, report, reason code and
    discovery record); Kconfig options; the C API generated from `telemetrix.h` and the plugin header (Doxygen
    through mkdoxy); the Python client API generated from its docstrings (mkdocstrings).
  - Explanation: architecture (layers, network task, completion ring), back-pressure and coalescing, the handshake
    and mode selection, safety model, deviations from upstream and the upstream bugs found.
  - Compatibility matrix (chips, boards, upstream firmware and client versions), troubleshooting, contributing,
    changelog.
  The README shrinks to a landing page (what it is, a 30-line quick start, links into the site). CI fails the docs
  build on broken internal links, missing API docs for public symbols, and on code samples that no longer run:
  Python samples execute against the host-built server; C samples compile in a sample-build target.
  Done when the site is live on Pages, every public C symbol and client method has reference docs, and the docs CI
  job passes.

## 7. Decided vs left to the builder

Decided by the user:

- Telemetrix as the protocol; our own wire-compatible implementation as a standalone, configurable component.
- More than one I2C and 1-wire bus, chosen through the protocol.
- UART and SPI through a protocol extension that opens a side TCP port per session (4.5).
- A modular component with extension plugins and no protocol-neutral core (4.1). (2026-11-12)
- Our own Python client with extension support, which also works with stock Telemetrix4Esp32 firmware (5.3).
  (2026-11-12)
- The handshake: `GET_FIRMWARE_VERSION` with a payload, the reply's length deciding the mode (4.4). (2026-11-12)
- The client's API: async chaining, each awaited chain confirmed by the board (5.4). (2026-11-12)
- This repository, built from scratch with the earlier implementation as reference only; the client in `client/`.
  (2026-11-12)
- After the adversarial review: fold its fixes in as milestones M1–M3; keep SPI streams and stock mode in v1.
  (2026-11-16)
- Comprehensive documentation as milestone M4, published on GitHub Pages with MkDocs Material; public repository
  from the start, before the Telemetrix author is contacted. (2026-11-16)

Claude's calls (open to the user's correction), most from the review:

- Bus selection by an `ON_BUS` wrapper with `BUS_REPORT` replies instead of a trailing byte.
- Implicit 16-bit command numbers in error reports; `SYNC` with 16-bit tags and an abort flag, on by default.
- Per-pin safe states in the profile, default float; `gpio_reset_pin` banned.
- Peripheral grants in the profile, shared ADC and I2C handles, limits computed from grants; client PWM channel
  numbers ignored.
- Asynchronous sonar and DHT, I2C timeout 50 ms, SPI on a worker task, a 5 ms plugin handler budget measured but not
  enforced.
- Back-pressure: bounded send queue, replies never dropped, input reports coalesced, send-stall timeout 10 s.
- Close on framing errors in extended mode only; lenient in stock mode.
- RST on refusals; `TCP_NODELAY`; stream sockets reserved at open.
- Address normalisation on side ports; optional 16-byte stream token (the client always uses one); stream idle limit.
- Extended command cap 254 bytes; discovery as paged TLV records.
- Allowed-address list and access hook both required, list first; the `reset` hook cannot reboot.
- Plugin names namespaced and versioned, slots per session, deferred results with client-chosen request ids; plugins
  are trusted code.
- Deferred after the review (not asked to keep): DAC, fixed UART bridges, the sync wrapper, entry-point plugin
  loading. Touch, steppers and upstream's SPI commands stay out.
- The handshake timeout 3 s and the default await timeout 5 s; stock-mode client never sends RESET.
- The Olimex ESP32-POE as the main compatibility board with a recording proxy; the S3 second.
- Client provisional name; asyncio only in v1; no automatic reconnect.
- Docs: reader-task arrangement (tutorials, how-tos, reference, explanation); `mike` versioning; README slimmed to a
  landing page in M4; samples executed in CI; Doxygen via mkdoxy for the C reference.

Left to the builder: byte values of new ids and reason codes, exact field widths inside discovery records, queue and
buffer sizes not fixed here, the task layout within each milestone, ready-made dev-board profiles, and CI.

## 8. Before publishing

While it is built, the library and the client live in its own repository under the user's GitHub account with the provisional name;
the firmware pulls it with a git dependency in `idf_component.yml` (and `override_path` to a local checkout while
developing both). Building does not wait for the Telemetrix author; a rename later costs a prefix change. The user
contacts the author (Claude does not send messages on their behalf). (Claude's call.)

- Contact the Telemetrix author (Alan Yorinks, MrYsLab) before publishing under a name that contains "Telemetrix":
  ask whether the name is fine (for the library and for our client), whether the extensions (handshake, discovery,
  bus numbers, plugin envelope) could become part of the protocol or get reserved ids, and whether pin lists for newer chips
  in the Python client would be welcome. Their answer may change the names, the new ids and the firmware version
  scheme.
- Upstream bugs found while reading (section 9 has the details): the SPI init layout mismatch, the
  `restart_on_shutdown` tuple bug, 1-wire subtype constants in the client docs, the client's off-by-one sonar and DHT
  limits, and the server overflows. These can be reported upstream; not a v1 deliverable.

## 9. Sources

- Telemetrix4Esp32 server sketch, 3.2.0:
  https://github.com/MrYsLab/Telemetrix4Esp32 (`examples/Telemetrix4Esp32WIFI/Telemetrix4Esp32WIFI.ino`)
- Telemetrix ESP32 Python client and its constants:
  https://github.com/MrYsLab/telemetrix-esp32 (`telemetrix_esp32/telemetrix_esp32.py`,
  `telemetrix_esp32_common/private_constants.py`), user guide https://mryslab.github.io/telemetrix-esp32/
- Telemetrix (Arduino): https://github.com/MrYsLab/telemetrix
- mqtt-io: https://github.com/flyte/mqtt-io
- Alternatives compared: pigpio https://github.com/joan2937/pigpio, lg/rgpiod https://github.com/joan2937/lg,
  ConfigurableFirmata https://github.com/firmata/ConfigurableFirmata, Linux greybus
  https://github.com/torvalds/linux/tree/master/drivers/greybus, gbridge https://github.com/anobli/gbridge
