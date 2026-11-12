# telemetrix-esp-idf: a modular Telemetrix server for ESP-IDF and its Python client: design

Status: for the user's review, 2026-11-12. A standalone project, built from scratch in this repository. An earlier
implementation of a first revision (`~/dev/telemetrix-esp32-idf`, never published, never on a board) may be read for
reference; nothing is carried over wholesale. Two deliverables: the ESP-IDF component (section 4) and the Python client
in `client/` (section 5).

## 1. Intent

### Why

ESP32 boards often have spare GPIO headers. The user wants to drive miscellaneous devices wired to them (relays,
motors, buttons, sensors, serial devices such as a GPS receiver or a Modbus RS485 adapter) from software running
elsewhere on the network, with mqtt.io as the first application on the network side. Any ESP-IDF application can embed
the component next to its own work.

### Why Telemetrix

Compared in the brainstorm (sources in section 8):

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
timeout, and reboots the chip on RESET. Hence our own ESP-IDF implementation of the same wire protocol.

### What success looks like

- An ESP-IDF application adds the component, passes a board profile, and any Telemetrix client switches relays, reads
  buttons and drives PWM on that board over the network.
- DHT, HC-SR04 and 1-wire sensors report values, with the timing done on the board; I2C devices are reached on more
  than one bus.
- A client opens a UART or an SPI device on the board for the length of its session and talks to it over a side TCP
  port (4.5).
- The component runs on any ESP32 chip the IDF supports; a chip's missing hardware simply means a missing plugin.
  Others add support for their own hardware as a plugin, without forking (4.1, 4.6).
- Our Python client works with this server in extended mode (discovery, buses, streams, plugins, confirmed calls,
  clear errors) and with stock Telemetrix4Esp32 firmware in stock mode, choosing the mode safely on connect (5.2).
- Nothing a client does reboots the board or disturbs the application's own work.

## 2. Goals, non-goals, anti-goals

Goals:

- An ESP-IDF component published to the ESP Component Registry, AGPL-3.0.
- Wire compatibility with Telemetrix4Esp32 3.2.0 for the commands in scope, plus core extensions (handshake,
  discovery, error reports, bus numbers) and a plugin envelope (4.4); UART and SPI streams as a built-in plugin (4.5).
- A modular component: hardware support as per-chip plugins, third-party extension plugins through a plugin API.
- Board profiles supplied by the application; nothing chip- or board-specific hard-coded in the protocol layer.
- Our own Python client with extension support and a stock-firmware fallback (section 5).

Non-goals (v1):

- Steppers, touch, BLE transport, scripts (reasons in 4.4).
- Upstream's in-band SPI commands; SPI goes through the streams plugin.
- A protocol-neutral core with more than one wire protocol (4.1).
- Our client talking to Telemetrix4Arduino or other non-ESP32 Telemetrix servers (5.2).
- The mqtt.io module (5.4) and a patch to the upstream Python client's pin lists.

Anti-goals (a failure even if it works):

- A client can reboot the board or reach a pin the profile does not grant.
- A malformed or hostile packet crashes the board or corrupts memory (the upstream sketch's overflows).
- A pin driven before the client asked for it, or left driven after the client is gone.
- A stock Telemetrix client that breaks against this server, or receives a report it does not know.
- Our client sending anything to stock firmware that could crash or desync it (4.4).
- Server load (reports, stream traffic) starving the application's own tasks: the server runs at a priority the
  application chooses.

## 3. Constraints

- ESP-IDF v6.1; must build for `esp32`, `esp32s3`, `esp32c6` and `esp32p4`.
- One styleguide for the repository (written in the plan's first task): astyle, `snake_case`, public prefix
  `telemetrix_`, internal `tmx_`, `esp_err_t`, host tests on the IDF `linux` target with Unity; for `client/`, ruff and
  pytest.
- No dynamic pin knowledge outside the profile; no fixed arrays indexed by an unchecked number from the wire.
- lwIP sockets are a shared, small budget in embedding applications: the component publishes how many it needs
  (main listener and client, two per stream, two per bridge) so the application can size lwIP.
- Internal RAM is scarce on some targets: buffers go to PSRAM where the application enables it and latency allows.
- Hardware verification happens on spare boards (Olimex ESP32-POE, ESP32-S3, ESP32-C6) in batched passes after review,
  never inside implementation tasks.

## 4. The ESP-IDF component

### 4.1 Shape

- One ESP-IDF component, its own git repository, published to the ESP Component Registry. Name
  `telemetrix-esp-idf`, subject to the courtesy check in section 7.
- Examples in the repository: the Olimex ESP32-POE (original ESP32, LAN8720 Ethernet) over Ethernet, an ESP32-S3 dev
  board over Wi-Fi, and the S3 example also built for the ESP32-C6.
- The application owns the network (Wi-Fi, Ethernet) and calls the library to start and stop the server.

Inside, four parts (revised 2026-11-12 with the user: the library is modular so it ports across ESP32 chips and others
can add hardware support without forking):

- **Core.** The board profile, pin claims (one owner per pin), safe release on disconnect and RESET (4.3), the scan
  scheduler, the network task and sockets, the access hook, and the plugin registry. It knows the Telemetrix framing
  but no command's meaning.
- **Hardware plugins.** One per piece of on-chip logic: GPIO, LEDC (PWM and servo), ADC, DAC, I2C, 1-wire, RMT pulse
  capture (DHT, sonar), UART, SPI. Each is compiled only for chips that have the hardware (the IDF's `SOC_*`
  capabilities) and only when its menuconfig switch is on, and the library registers the compiled ones by itself. A
  chip without a DAC simply has no DAC plugin. Later hardware plugins (touch, PCNT, addressable LEDs over RMT) are added
  the same way, without core changes.
- **Telemetrix adapter.** Maps the stock Telemetrix commands (4.4) onto the hardware plugins. A command whose plugin
  is absent is refused like a capability the pin lacks. The adapter also implements the core extensions (4.4).
- **Extension plugins.** Features beyond stock Telemetrix, addressed through the plugin envelope (4.4) and discovered
  by the client. Built in: UART and SPI streams (4.5). Third-party ones are registered by the application (4.6).

Not built (the user's decision after weighing it): a protocol-neutral core with Telemetrix as one of several wire
protocols. Nobody needs a second protocol, and a "neutral" API designed from one protocol would only rename its
fields. Extensibility comes from plugins and from our own client (section 5) instead.

### 4.2 Configuration

**Board profile** (passed in at start, owned by the application): a table of GPIOs the library may use. Per GPIO: which
capabilities it has (digital in, digital out, pull-up, pull-down, PWM, ADC with its unit and channel, DAC with its
channel, I2C, 1-wire, stream pin), plus capabilities that third-party plugins declare (how the profile names those is
the builder's call; a plugin may only claim pins the profile grants it). DAC is offered only on chips that have one
(the original ESP32: GPIO 25 and 26); P4, S3 and C6 have none. The library refuses every command naming a GPIO outside
the table or asking a capability the GPIO lacks. The table is the only source of pin knowledge; there is no 40-pin array
indexed by pin number. The component ships ready-made profiles for common dev boards (which ones: builder's call).

**Bus table** (part of the profile): the I2C and 1-wire buses the application allows, each with its bus number, its
fixed pins or "client chooses pins", and for I2C its port and speed.

**Stream resources** (part of the profile): the UART ports and SPI hosts the library may hand to streams (4.5), and the
GPIOs marked "stream pin" that a stream may route them to. Pins come from the client in the open request; the library
refuses any pin not marked as a stream pin. An application that offers no stream resources has no stream plugin.

**Runtime settings:** TCP port (default 31336), enabled, allowed client addresses, keepalive, report interval floor,
limits (DHT, sonar, servo counts), and for streams: the side port range (default 31337–31344), the most open streams
at once, the connect timeout (default 10 s) and the largest SPI frame (default 4096 bytes).

**Hooks** (callbacks the application registers):

- `access`: decides whether a connecting peer is allowed (for example the application's own firewall rules).
  Applies to every socket the
  library or a plugin accepts.
- `reset`: called on the RESET command. Default: release every pin and bus and stop all reports; never reboots unless
  the application's hook does.
- `connected` / `disconnected`: for status and logging.

**Build-time switches** (menuconfig): one per built-in plugin (DHT, sonar, 1-wire, servo, I2C, analog, DAC, UART
streams, SPI streams), each removable to save flash and RAM.

### 4.3 Server behaviour

- TCP server, one client at a time, like upstream. A second connection while one is active is closed at once and
  counted ("first keeps it", as upstream).
- Dead-client detection: TCP keepalive on the client socket (interval and count in the runtime settings), so a client
  whose host vanished is dropped and cannot lock others out. Not a "nothing received" timeout: stock Telemetrix clients
  send no heartbeat, and a client that only listens to inputs must stay connected.
- On disconnect: every claimed pin returns to its unassigned state (input, no pull), PWM stops, buses close, reports
  stop, and every plugin's release hook runs. A relay must not stay on because a client vanished.
- Every received packet is length-checked before it is read: a declared length past the buffer, a payload shorter than
  the command needs, an unknown command id, a count byte that exceeds the payload: the packet is dropped and counted,
  the connection stays.
- Input reporting as upstream: digital inputs reported on change; analog inputs at the scanning interval with the
  per-pin differential; sonar every 33 ms on change; DHT every 2 s.
- Timing-critical sensors run on the board with ESP-IDF peripherals (RMT, GPIO ISR timestamps, `onewire_bus`).
- Plugins run on the server's task. A plugin's command handler and poll hook must return within a bounded time (the
  sonar ping, about 30 ms worst case, is the longest built-in one); longer work belongs in the plugin's own task.

### 4.4 Wire protocol

Three layers.

**Layer 1: stock.** Byte-identical to Telemetrix4Esp32 3.2.0 for the commands in scope: `[len][cmd][payload]` both
ways, `len` counting `cmd` and payload; 16-bit values big-endian; DHT floats and the PWM frequency double in
little-endian (native ESP32 order); report layouts as upstream. Section 10 links the sketch and the client constants
that define it. A stock client sees only this layer and never receives anything it does not know.

In scope (v1): `LOOP_COMMAND`, `SET_PIN_MODE` (input, input pull-up, input pull-down, output, analog, PWM),
`DIGITAL_WRITE`, `ANALOG_WRITE`, `MODIFY_REPORTING`, `GET_FIRMWARE_VERSION`, `SERVO_ATTACH`, `SERVO_WRITE`,
`SERVO_DETACH`, `I2C_BEGIN`, `I2C_READ`, `I2C_WRITE`, `SONAR_NEW`, `DHT_NEW`, `STOP_ALL_REPORTS`,
`SET_ANALOG_SCANNING_INTERVAL`, `ENABLE_ALL_REPORTS`, `ANALOG_OUT_ATTACH`, `ANALOG_OUT_DETACH`, `RESET`, the
1-wire set (`ONE_WIRE_INIT` through `ONE_WIRE_CRC8`), and the DAC commands on chips with a DAC plugin.

Out of scope: touch, upstream's SPI commands, steppers. These are accepted and ignored (no report), so a client that
sends them does not desync; the drop is counted. Why: upstream's SPI init layout differs between its client and
server and the 30-byte cap is too small for SPI devices (SPI goes through streams, 4.5); steppers are a full
AccelStepper motion controller (about 20 commands) mqtt.io does not use; touch is left out of v1 to keep it small, and
is the natural first hardware plugin after v1.

**Layer 2: core extensions.** Always present in this library; a stock client never triggers them.

- *Handshake.* `GET_FIRMWARE_VERSION` with an empty payload gets the stock 5-byte answer, as upstream. With a
  payload (a magic byte and the client's extension-protocol version), the answer is longer: the same three version
  bytes, then the magic byte and the server's extension-protocol version. That reply switches the session to extended
  mode. Checked against upstream 2.0.1 to 3.2.0: every release reads and ignores a payload on this command and sends the
  fixed 5 bytes, so the probe is safe on stock firmware and needs no timeout. (A probe with an unknown command id is
  not safe: releases up to 3.1.0 jump through an unchecked function table.) The version number is never used to tell
  firmwares apart.
- *Discovery.* `GET_CAPABILITIES` (valid only in extended mode) returns, in pages because one report is small: the
  chip, every profile pin with its capabilities, the buses, the limits, and every registered plugin with its name,
  version and slot. Layout and paging: the builder's.
- *Error report.* In extended mode, every refused command produces a report with the command id (and the plugin slot
  and sub-command for plugin commands), a reason code, and the first bytes of the refused payload (enough to carry the
  pin or stream id), so the client can tell pipelined commands apart. In stock mode refusals stay silent and counted,
  as upstream. A plugin request that has its own reply carrying a status (the streams plugin's *Opened*) reports
  failure there only, never twice.
- *Ordering.* The server handles a connection's commands strictly in order and sends every report a command causes
  before it handles the next command. The client relies on this: `LOOP_COMMAND` (a stock command) is its barrier, and
  every error report that arrives before the barrier's echo belongs to a command sent before it (5.2).
- *Extension-protocol version* (major.minor), exchanged in the handshake. The server answers with its own version;
  both sides then use the lower minor. A different major: our client stops with an "incompatible firmware" error, and
  does not fall back to stock mode (whose pin lists are wrong for these boards). (Claude's call.)
- *Bus number.* I2C and 1-wire commands accept one optional trailing byte, the bus number; the replies to a command
  that carried it carry it too. Without the byte: bus 0. Telemetrix4Arduino's inline I2C port layout would break stock
  ESP32 clients, so the trailing byte stands. Works in both modes.

**Layer 3: plugins.** One envelope command, `PLUGIN [slot][sub-command][args]`, and one envelope report,
`PLUGIN_REPORT [slot][sub][data]`. The slot is assigned when the plugin registers and announced by discovery; the
sub-commands and their layouts are fixed by the plugin's author, so the client half of a plugin knows them without
discovery. Two plugins can never collide on ids and the id space never runs out; the cost is two bytes of the 30-byte
payload. Plugin commands are valid only in extended mode.

All new command and report ids are above upstream's range (commands 57 or higher, report ids upstream does not use),
chosen by the builder and confirmed in the courtesy check (section 7).

### 4.5 Streams plugin (UART and SPI over a side port)

Idea (the user's): a command on the main connection opens a session on a UART or an SPI host; the server answers with
a TCP port, and the data flows over that side connection, outside Telemetrix's 30-byte packets. Revised 2026-11-12:
this is a built-in extension plugin, addressed through the plugin envelope, no longer fixed command ids 64/65.

Sub-commands and reports (the contract; byte layout is the builder's, all within the cap):

- *Open*: kind (UART or SPI), resource number (UART port or SPI host, from the profile), pins, and settings. UART: TX,
  RX, optional DE (RS485), baud rate, data bits, parity, stop bits. SPI: SCLK, MOSI, MISO (each optional where the
  device does not need it), CS, mode (0–3), clock.
- *Opened* report: a stream id and the side TCP port, or an error code (pin not a stream pin, pin or resource busy,
  stream limit reached, no free port, setting the hardware refuses). On error nothing is claimed.
- *Close*: closes a stream by id.
- *Closed* report: sent with the reason when a stream ends while the main session is still open and the client did not
  ask for it (connect timeout, hardware error).

Side connection:

- Accepted only from the main session's peer address, and only when the access hook also allows it. One client per
  stream; a second connection is refused and counted. No token: a raw UART stream must work with unmodified tools
  (gpsd, socat, a Modbus library).
- If nothing connects within the connect timeout after *Opened*, the stream closes and reports it. After the first
  connection the side client may disconnect and reconnect while the stream is open.
- Lifetime: a stream ends on *Close*, on `RESET`, or when the main session ends; then its port closes, its hardware is
  released and its pins return to the unassigned state.
- TCP keepalive on the side socket as on the main one.

Data on the side connection:

- UART: raw bytes both ways. With DE given, the UART runs in hardware RS485 half-duplex mode. Settings are fixed at
  open.
- SPI: framed transactions, lengths big-endian. Request: 16-bit length, one flags byte (bit 0: keep CS asserted after
  this transfer), then that many bytes to send. Reply: 16-bit length, then the same number of bytes read back (full
  duplex). A frame longer than the largest SPI frame setting, or a malformed header, closes the side connection (not
  the stream) and is counted.

The library also offers the application fixed UART bridges (a UART served raw on its own TCP port with no Telemetrix
client, for tools like gpsd or a Modbus master); they share the UART serving code with UART streams and the rule that
one UART has one user.

### 4.6 Plugin API (firmware side)

- The application registers third-party plugins when it starts the library; built-in plugins are registered by the
  library itself. Registration order fixes the slots for the life of the server. A limit on the number of plugins and
  the name length: builder's call.
- A plugin provides: a name and version (announced by discovery), a sub-command handler, an optional poll hook (with
  its next deadline), a release hook (called on disconnect, RESET and stop), and the pin capabilities it uses.
- The core gives a plugin: pin claim and release against the profile, report emit through the envelope, scheduler
  deadlines, side-port sockets (listen, accept with the access hook and the main-peer binding of 4.5, keepalive), a
  refusal counter, and a status entry for the application's status view.
- The built-in streams plugin uses only this API, so the API is proven by a real plugin. The hardware plugins use an
  internal interface (the adapter calls them directly); it need not be public in v1.
- A plugin cannot change stock-layer behaviour or send anything to a client that is not in extended mode.

### 4.7 Library tests

- Host tests (Unity, `linux` target): framing and length checks, every stock command's parsing against fixtures taken
  from the upstream client's byte layouts, board-profile refusals, the bus extension, the handshake (empty payload →
  stock answer and stock mode; magic payload → long answer and extended mode), discovery pages, error reports only in
  extended mode, plugin registration and slots, envelope routing, plugin commands refused in stock mode, reset and
  disconnect release including plugin release hooks, and the streams plugin: open refusals, the peer-address check,
  connect timeout, release on close/reset/disconnect, SPI frame parsing and oversize frames.
- A test plugin (in the test tree, not shipped) exercises the plugin API end to end.
- Compatibility proof on the Olimex ESP32-POE (original ESP32, the chip upstream targets): one script using the real
  upstream Python client (`telemetrix-esp32`) runs against upstream's Telemetrix4Esp32 firmware and then against ours
  on the same board, and the replies are compared byte for byte. Upstream's sketch is Wi-Fi only, so the side-by-side
  runs over Wi-Fi; the Ethernet example runs the same script afterwards.
- Pin conflict to check on the ESP32-POE: its Ethernet uses the RMII pins, which on the original ESP32 include GPIO 25
  and 26, the only DAC pins. The DAC test needs the Wi-Fi build and those pins on a header; otherwise DAC is covered by
  host tests only.
- Second target: an ESP32-S3 dev board over Wi-Fi. The C6 example build is a compile check. All of this runs on spare
  boards.
- Streams and the extended protocol on a spare board, driven by our client (section 5): a UART stream through a
  TX↔RX loopback wire and an SPI stream through a MOSI↔MISO loopback wire, checked byte for byte.

## 5. The Python client (`client/`), and later the mqtt.io module

Added 2026-11-12 with the user: we own the client side too, so extensions are usable end to end. The user's aim: "our
awesome version of the Telemetrix protocol client that supports extensions".

### 5.1 Shape

- A Python library in this repository's `client/` folder (the user's choice: one repository, so a wire-contract change lands on both sides in one commit), published to PyPI, AGPL-3.0. Provisional name `telemetrix-idf-client`
  (import `telemetrix_idf`), settled with the courtesy check (section 7). (Claude's call.)
- asyncio at the core, because mqtt.io is asyncio and its module will sit directly on top; a thin synchronous wrapper
  for scripts and notebooks. Python version floor: builder's call (the oldest one mqtt.io supports, at least 3.10).
- Order: after the component's wire contract is fixed (section 4).

### 5.2 Behaviour

- **Two modes, chosen by the handshake (4.4).** On connect the client sends `GET_FIRMWARE_VERSION` with the magic
  payload. The long answer means extended mode: it then sends `GET_CAPABILITIES` and builds the board object from the
  answer (pins with their real capabilities, buses, limits, plugins and their slots). The stock 5-byte answer means
  stock mode: upstream Telemetrix4Esp32's ESP32 pin lists and the stock commands only; calls that need extended mode
  (plugins, streams, a second bus, error reports) fail at once with a clear "not supported by this firmware" error.
  Supported stock firmware: Telemetrix4Esp32 3.2.0; older 3.x releases only where their command layouts match 3.2.0
  (checked in the plan); below 3.0 not supported. (Claude's call.) Telemetrix4Arduino and other Telemetrix servers are
  not supported (different command layouts).
- **No hard-coded pin lists in extended mode.** The same client works on the P4, S3, C6 and original ESP32 because
  discovery tells it what each pin can do. A call naming a pin or capability the board lacks fails on the client
  before anything is sent.
- **Async chaining API (the user's choice, 2026-11-12).** A new API designed for asyncio and mqtt.io, not a mirror of
  upstream `telemetrix-esp32`'s method names and list-shaped callbacks. Calls chain into a batch, and awaiting the chain
  sends it: for example `await board.pin(17).output().write(1)`. The client sends the batch's commands, then one
  `LOOP_COMMAND` barrier, and the await completes when the barrier's echo arrives, or raises the first refusal from the
  error reports that came before it (4.4). So every awaited chain is confirmed by the board, and a batch of many
  commands costs one round trip. Reads and requests with their own replies (I2C read, 1-wire, discovery, stream open)
  complete with their reply. In stock mode there are no error reports, so a chain completes on the barrier with no
  refusal check; the client says so in its documentation. Input changes (digital, analog, sonar, DHT) are delivered
  through callbacks and async iterators; exact API shape: builder's call within this style.
- **API covers everything in layer 1 and 2:** digital in/out with change callbacks, PWM, servo, analog in with
  callbacks, DAC, I2C per bus, 1-wire per bus, DHT, sonar, reset, reporting control.
- **Plugins mirror the firmware.** Each firmware plugin has a client half that knows its sub-commands and exposes
  methods on the board (for example `board.streams`). Third-party client halves load through Python entry points, so
  installing a plugin package adds its methods; a client half whose firmware plugin is not in discovery is not
  offered.
- **Streams are first class.** Opening a UART stream returns an asyncio reader and writer; opening an SPI stream returns
  an object with a transfer call. The side socket and SPI framing are hidden.
- **Errors are explicit.** Refusals become exceptions carrying the reason from the error report; every request that
  expects an answer has a timeout.
- **Disconnect is visible.** The client reports a lost connection to its user; it does not silently reconnect (the
  board has already released every pin). Whether to offer an opt-in reconnect: builder's call.

### 5.3 Client tests

- Unit tests against a fake server in Python that speaks both modes (stock and extended), including the handshake on
  each, discovery paging, error reports, envelope routing and a fake streams plugin.
- On spare boards: the stream and extended-protocol checks of 4.7 are written on this client. The byte-for-byte
  compatibility proof against upstream firmware keeps using the stock client, so it does not depend on ours. One run of
  our client in stock mode against upstream's firmware on the ESP32-POE.

### 5.4 Later: the mqtt.io module

Separate brainstorm once part A and this client exist. Known so far: mqtt.io's existing `gpiozero` module cannot reach
this server, its sensor modules call local libraries (smbus2, w1thermsensor, RPi.GPIO, Adafruit DHT), and the module
will build on this client, so it gets discovery, buses, streams and plugins for free.

## 6. Decided in the brainstorm vs left to the builder

Decided by the user:

- Telemetrix as the protocol; our own wire-compatible implementation as a standalone, configurable component.
- Bus numbers in the protocol for more than one I2C and 1-wire bus.
- UART and SPI through a protocol extension: a command on the main connection opens a session and the server starts a
  TCP proxy for it (4.5).
- A modular component: per-chip hardware plugins and extension plugins (UART/SPI streams built in), with no
  protocol-neutral core (4.1). (2026-11-12)
- Our own Python Telemetrix client with extension support, which also works with stock Telemetrix4Esp32 firmware in a
  fallback mode (section 5). (2026-11-12)
- The handshake: `GET_FIRMWARE_VERSION` with a payload, the reply's length deciding the mode; no version-number
  heuristics and no unknown-id probe (4.4). (2026-11-12)
- The client's API: a new async chaining API, each awaited chain confirmed by the board (5.2). (2026-11-12)
- This repository: `telemetrix-esp-idf`, built from scratch with the earlier implementation as reference only; the
  client in `client/` of the same repository. (2026-11-12)

Claude's calls (open to the user's correction):

- The v1 command scope and what is out (4.4); DAC in scope for chips that have one.
- The Olimex ESP32-POE as the main compatibility board, side by side with upstream's firmware; the S3 second.
- Safe release of every pin on disconnect (4.3); RESET never reboots.
- The bus-number extension as an optional trailing byte.
- Stream details: side ports bound to the main session's peer address with no token; a fixed side port range; connect
  timeout; side client may reconnect while the stream is open; SPI framing with a keep-CS flag.
- Refuse-and-count for unknown or out-of-scope commands rather than closing the connection.
- Discovery (`GET_CAPABILITIES`) and error reports as core extensions, error reports only in extended mode and echoing
  the refused payload's first bytes; `LOOP_COMMAND` as the confirmation barrier; the extension-protocol version rule
  (lower minor, major mismatch is an error); stock mode targets Telemetrix4Esp32 3.2.0.
- One plugin envelope command and report with slots instead of per-plugin command ids; touch as the first post-v1
  hardware plugin; built-in hardware plugins on an internal interface, only the extension plugin API public.
- The client's provisional name, asyncio core with a sync wrapper, entry-point plugins, no automatic reconnect.

Left to the builder: the items marked "builder's call" above, byte layouts not fixed here, the task layout, the
ready-made dev-board profiles, and CI.

## 7. Before publishing

While it is built, the library and the client live in its own repository under the user's GitHub account with the provisional name;
the firmware pulls it with a git dependency in `idf_component.yml` (and `override_path` to a local checkout while
developing both). Building does not wait for the Telemetrix author; a rename later costs a prefix change. The user
contacts the author (Claude does not send messages on their behalf). (Claude's call.)

- Contact the Telemetrix author (Alan Yorinks, MrYsLab) before publishing under a name that contains "Telemetrix":
  ask whether the name is fine (for the library and for our client), whether the extensions (handshake, discovery,
  bus numbers, plugin envelope) could become part of the protocol or get reserved ids, and whether pin lists for newer chips
  in the Python client would be welcome. Their answer may change the names, the new ids and the firmware version
  scheme.
- Upstream bugs found while reading (section 8 has the details): the SPI init layout mismatch, the
  `restart_on_shutdown` tuple bug, 1-wire subtype constants in the client docs, the client's off-by-one sonar and DHT
  limits, and the server overflows. These can be reported upstream; not a v1 deliverable.

## 8. Sources

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
