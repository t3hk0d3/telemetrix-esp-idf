# telemetrix-esp-idf

An ESP-IDF component that serves the stock Telemetrix4Esp32 3.2.0 protocol over TCP, so unmodified
[telemetrix-esp32](https://github.com/MrYsLab/telemetrix-esp32) clients can drive GPIO, PWM, servos, ADC, I2C, 1-wire,
DHT and sonar on an ESP32 board. The application owns the network (Wi-Fi or Ethernet) and the pins: it hands the
component a profile that lists the GPIOs and peripherals the client may use, and the component refuses everything
else. It exists because the upstream firmware is an Arduino sketch that owns the whole board, reboots on RESET, and
trusts every pin number a client sends; this component lives next to your own application code and checks every request
against the profile. Targets: esp32, esp32s3, esp32c6, esp32p4 and the IDF `linux` host target (for tests). ESP-IDF
v6.1 or later.

The wire protocol and the Python client come from [Telemetrix4Esp32](https://github.com/MrYsLab/Telemetrix4Esp32) by
Alan Yorinks (MrYsLab); this component reimplements the server side of that protocol. Licensed under
AGPL-3.0-or-later, copyright Igor Yamolov.

## Quick start

The `examples/esp32_poe` example targets the Olimex ESP32-POE (original ESP32, LAN8720 Ethernet); the same example
builds for Wi-Fi. It uses the component from this checkout.

```sh
cd examples/esp32_poe
idf.py set-target esp32
idf.py menuconfig          # "Telemetrix example": Ethernet or Wi-Fi (SSID and password)
idf.py build flash monitor
```

For Wi-Fi from the start, layer the Wi-Fi defaults: `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wifi" build`.
`make build-examples` builds every example. The monitor shows the board's address and
`Telemetrix server listening on port 31336`. Then, from a machine on the same network:

```sh
pip install telemetrix-esp32==2.1.1
python3 - <<'PY'
import time
from telemetrix_esp32 import telemetrix_esp32

board = telemetrix_esp32.TelemetrixEsp32("192.168.1.50")   # the board's address
board.set_pin_mode_digital_output(32)
for level in (1, 0, 1, 0):
    board.digital_write(32, level)
    time.sleep(0.5)
board.shutdown()
PY
```

Using it in your own application: add the component, build a `telemetrix_profile_t`, then call `telemetrix_start` once
the network is up (see `examples/esp32_poe/main/main.c`).

## Profile and grants

`telemetrix_start(profile, config, hooks)` copies all three, arrays included. The profile is the only source of
hardware knowledge.

**Pins** (`telemetrix_pin_t`): `gpio`, `caps`, `safe`, `adc_unit`, `adc_channel` (-1 = no ADC). Capability bits:
`TELEMETRIX_CAP_DIN`, `_DOUT`, `_PULL_UP`, `_PULL_DOWN`, `_PWM`, `_ADC`, `_I2C`, `_ONE_WIRE` (bits 8-15 reserved). A
request for a pin that is not in the table, or lacks the capability, is refused and counted. At most 64 pins.

**Safe states** (`telemetrix_safe_state_t`): `FLOAT` (input, no pull; the default), `PULL_UP`, `PULL_DOWN`,
`DRIVE_LOW`, `DRIVE_HIGH`. Applied to every profile pin at `telemetrix_start`, and to every claimed pin on disconnect,
RESET and `telemetrix_stop`. The component never calls `gpio_reset_pin` (it enables the pull-up).

**Peripheral grants**:

| Grant | Meaning |
|---|---|
| `ledc` | `speed_mode`, `timer_mask`, `channel_mask`: the LEDC timers and channels PWM and servos may use. The channel the client names is ignored; channels and timers are allocated from the grant, timers shared by equal (frequency, resolution). No free channel or timer: refused. |
| `rmt` | `rx_channels`, `tx_channels`: RMT channels the server may create for DHT and 1-wire. |
| `adc[]` | One entry per ADC unit: `unit` and an optional application-owned `adc_oneshot_unit_handle_t` (NULL = the component makes its own). Channels the server reads are configured at 12 dB on every read. |
| `i2c[]` | Buses: `bus`, `port`, `sda`, `scl` (-1 = the client chooses), `speed_hz`, optional application-owned `i2c_master_bus_handle_t` (see below). |
| `onewire[]` | Buses: `bus` and `gpio` (-1 = the pin of `ONE_WIRE_INIT`). |

Stock commands always use bus 0 (I2C bus 0 and 1-wire bus 0). `I2C_BEGIN` pins are used only when bus 0's grant says
"client chooses pins" (and must be profile pins with the I2C capability), otherwise they are ignored; `[0,0]` with
"client chooses" is refused.

**Shared I2C bus**: a bus grant with `handle` set is the application's bus. Its `sda` and `scl` must name the bus's pins
and must not be profile pins; `telemetrix_start` refuses -1 or a profile pin with `ESP_ERR_INVALID_ARG`. `I2C_BEGIN`
attaches to the handle; the component never claims, configures or puts those pins in a safe state (not at start, not on
disconnect, RESET or stop) and never deletes the handle, only the device handles it added.

The stock client's `set_pin_mode_i2c()` always sends a bare `I2C_BEGIN`, so a profile meant for it gives bus 0 fixed
(or shared) pins; the examples do (Olimex ESP32-POE: SDA GPIO13, SCL GPIO16 on UEXT; S3 DevKitC: 13, 14; C6 DevKitC:
23, 22).

**Settings** (`telemetrix_config_default` fills every field; `telemetrix_config_t`):

| Field | Default |
|---|---|
| `server_enabled` | true |
| `port` | 31336 |
| `keepalive_idle_s`, `keepalive_intvl_s`, `keepalive_count` | 10 s, 2 s, 3 |
| `send_stall_ms` | 10000 |
| `send_queue_bytes` | 4096 |
| `i2c_timeout_ms` | 50 |
| `analog_interval_floor_ms` | 10 |
| `max_dht`, `max_sonar`, `max_servo` | 6, 6, 4 (servos never more than the free LEDC channels) |
| `allowed`, `n_allowed` | empty = any peer (`telemetrix_cidr_parse` builds entries) |
| `task_priority`, `task_core` | 5, -1 (no affinity) |
| `extended` | true: offer the extended protocol (false = serve every client stock) |
| `plugin_budget_us` | 5000 |
| `plugins`, `n_plugins` | none (see the extended protocol) |

The network task's stack is 6144 bytes (internal constant).

**Hooks** (`telemetrix_hooks_t`, all optional): `access(peer, ctx)` (NULL = allow), `reset(ctx)` (called after a RESET
has released everything), `connected(peer, ctx)`, `disconnected(ctx)`.

**Status**: `telemetrix_get_status` returns whether the server runs and listens, the client's address, the counters
`refused_connections`, `dropped_packets`, `ignored_commands`, `refused_commands`, `coalesced_reports`,
`stalled_clients`, and what each claimed pin is used for. For the extended protocol: whether the client is in
extended mode (`extended`), `protocol_violations`, `error_reports`, and per plugin (`plugins`, `n_plugins`) its name,
slot, `commands`, `refusals`, `overruns`, `max_us`, `async_dropped` and its status text. `telemetrix_kick_client` drops
the current client.

**Build switches** (menuconfig, "Telemetrix"): `CONFIG_TELEMETRIX_DHT`, `_SONAR`, `_ONE_WIRE`, `_SERVO`, `_I2C`,
`_ANALOG`, `_EXTENDED` (the extended protocol and plugins), each on by default. The commands of a switched-off feature are ignored and counted in `ignored_commands`,
like an id out of scope: DHT `DHT_NEW` (13), sonar `SONAR_NEW` (12), 1-wire ids 27-35, servo ids 6-8, I2C ids 9-11,
analog `SET_PIN_MODE` mode 3.

## The stock protocol

Framing both ways: `[len][id][payload]`, `len` counts the id and the payload. Payload cap 30: `len` 2-31 is a command
with a payload, `len` 1 a command without; `len` 0 consumes one byte and counts `dropped_packets`; `len` over 31
skips `len` bytes and counts `dropped_packets`. A packet whose bytes do not all arrive within 2000 ms, or that the
client's disconnect cuts short, is dropped and counted. The connection always stays (stock mode is lenient). 16-bit values are big-endian; DHT floats (`float`) and the
PWM frequency (`double`) are little-endian.

Commands in scope (upstream `private_constants.py` ids):

| Id | Command | Payload | Reply |
|---|---|---|---|
| 0 | LOOP | `[c]` | `[2,0,c]` |
| 1 | SET_PIN_MODE | `[pin,0\|2\|9,report]` input, pull-up, pull-down; `[pin,1]` output; `[pin,3,diff_hi,diff_lo,report]` analog; `[pin,8,channel,resolution,freq f64 x8]` PWM (resolution 1-14) | |
| 2 | DIGITAL_WRITE | `[pin,v]` | |
| 3 | ANALOG_WRITE | `[pin,v_hi,v_lo]` | |
| 4 | MODIFY_REPORTING | `[mode,pin]`: 0 disable all, 1 analog enable, 2 digital enable, 3 analog disable, 4 digital disable | |
| 5 | GET_FIRMWARE_VERSION | `[]` (any payload ignored) | `[4,5,3,2,100]` |
| 6 | SERVO_ATTACH | `[pin,min_hi,min_lo,max_hi,max_lo]` | none free: `[2,6,pin]` |
| 7 | SERVO_WRITE | `[pin,angle]` | |
| 8 | SERVO_DETACH | `[pin]` | |
| 9 | I2C_BEGIN | `[]` or `[sda,scl]` | |
| 10 | I2C_READ | `[addr,reg,count,stop]` | `[count+4,9,count,addr,reg,data...]`; count clamped to 59; NACK or short read: `[3,8,1,addr]` |
| 11 | I2C_WRITE | `[count,addr,data x count]` | |
| 12 | SONAR_NEW | `[trig,echo]` | `[4,10,trig,cm_hi,cm_lo]` |
| 13 | DHT_NEW | `[pin]` | `[11,11,0,pin,hum f32,temp f32]`; error `[4,11,1,pin,err]` (1 checksum, 2 timeout) |
| 14 | STOP_ALL_REPORTS | `[]` | |
| 15 | SET_ANALOG_SCANNING_INTERVAL | `[ms]` (floor 10 ms) | |
| 16 | ENABLE_ALL_REPORTS | `[]` | |
| 17 | ANALOG_OUT_ATTACH | `[pin,channel]` | |
| 18 | ANALOG_OUT_DETACH | `[pin]` | |
| 20 | RESET | `[]` | |
| 27 | ONE_WIRE_INIT | `[pin]` | |
| 28 | ONE_WIRE_RESET | `[]` | `[3,14,28,presence]` |
| 29 | ONE_WIRE_SELECT | `[8 bytes]` | |
| 30 | ONE_WIRE_SKIP | `[]` | |
| 31 | ONE_WIRE_WRITE | `[byte,power]` | |
| 32 | ONE_WIRE_READ | `[]` | `[3,14,32,byte]` |
| 33 | ONE_WIRE_RESET_SEARCH | `[]` | |
| 34 | ONE_WIRE_SEARCH | `[]` | `[10,14,34,8 bytes]` (all `0xff` when none) |
| 35 | ONE_WIRE_CRC8 | `[n,bytes...]`; the stock client sends one byte more than `n` and ignores the extra | `[3,14,35,crc over n bytes]` |

Periodic reports: digital `[3,2,pin,v]` and analog `[4,3,pin,v_hi,v_lo]` (raw 12-bit at 12 dB).

Ignored and counted (`ignored_commands`), payload consumed: 19 and 21 (DAC), 22-26 (SPI), 36-56 (steppers),
`SET_PIN_MODE` mode 7 (touch), and every id of 57 and above (in stock mode; see the extended protocol).

Timing: digital inputs are polled every 2 ms while any reports; analog every interval (default 19 ms, floor 10 ms);
sonar one device per 33 ms in turn, echo timeout 25 ms, cm = echo_us / 58, reported only on change; DHT every 2000 ms,
all devices, DHT22/AM2302 decoding. `DHT_NEW` registers the device and schedules its first read at once, without
reading the other devices early (no device is read twice within 2000 ms); every failed read sends an error report and
the device stays registered. Sonar and DHT never block the network task: a GPIO ISR
(sonar) or an RMT callback (DHT) completes them. The I2C timeout comes from the settings; 1-wire operations are bounded
by their slot timing.

## The extended protocol

A client that asks for it gets the extended protocol (version 1.0) on the same connection: numbered commands, one
ERROR report per refused command, a barrier (SYNC), a second bus through ON_BUS, discovery of the board, and
plugins. A client that does not ask stays in stock mode, and stock-mode bytes are exactly the ones above. The
application can turn it off at run time (`config.extended = false`: every client is served stock) or at build time
(`CONFIG_TELEMETRIX_EXTENDED` off); either way the stock answer is sent to every handshake.

**Handshake.** `GET_FIRMWARE_VERSION` with a payload of exactly `[0xA7, major, minor]` (magic `0xA7`, the client's
extension version) gets the long answer `[7,5,3,2,100,0xA7,1,0]`: the stock version bytes, the magic and the server's
extension version 1.0. Any other payload gets the stock `[4,5,3,2,100]`. With the same major the session enters
extended mode at the lower of the two minors (the client computes it; the server stores it). With another major the
long answer is sent and the session stays in stock mode. In extended mode `GET_FIRMWARE_VERSION` always gets the long
answer. A session stays extended until the client disconnects; RESET releases the hardware and keeps the session (mode,
minor and numbering).

**Numbering.** Every packet after the handshake reply is numbered by a 16-bit counter `seq` that starts at 0 and wraps
at 65536, SYNC included. The handshake's own `GET_FIRMWARE_VERSION` is not numbered. 16-bit values are big-endian.

**Framing in extended mode.** The payload cap is 254 (`len` up to 255). These are protocol violations: `len` 0, a packet
unfinished after 2000 ms, an id above 60. A violation closes the client with an RST (`SO_LINGER` {1,0}), counts
`protocol_violations`, stops handling the rest of what was received, and releases the session as a disconnect does
(pins to their safe states, plugin release hooks). The next client starts in stock framing. Ids 0-56 are known:
those out of scope (DAC, SPI, steppers, touch) get ERROR `NOT_IN_SCOPE`; a short payload gets ERROR `BAD_ARGUMENT` and
the connection stays.

New commands:

| Id | Command | Payload |
|---|---|---|
| 57 | SYNC | `[tag_hi, tag_lo, flags]` |
| 58 | ON_BUS | `[bus, stock_id, stock payload...]` |
| 59 | GET_CAPABILITIES | `[page]` |
| 60 | PLUGIN | `[slot, sub, args...]` |

New reports (each at most 255 bytes on the wire, length byte included):

| Id | Report | Payload |
|---|---|---|
| 57 | ERROR | `[seq_hi, seq_lo, cmd, reason]`, then `[slot, sub]` when `cmd` is 60 and the payload had them, then `[code]` when `reason` is PLUGIN |
| 58 | SYNC_REPLY | `[tag_hi, tag_lo, seq_hi, seq_lo]`; `seq` is the SYNC's own number |
| 59 | BUS_REPORT | `[bus, stock_id, stock payload...]`: the inner command's stock report without its length byte |
| 60 | CAPABILITIES | `[page, last_page, records...]` |
| 61 | PLUGIN_REPORT | `[slot, sub, data...]` (data at most 251 bytes) |

Reasons (`telemetrix_reason_t`):

| Code | Reason | Meaning |
|---|---|---|
| 0 | NONE | done (never sent) |
| 1 | NO_SUCH_PIN | the pin is not in the profile |
| 2 | NO_CAPABILITY | the pin lacks the capability the use needs |
| 3 | PIN_BUSY | the pin is claimed by another use |
| 4 | EXHAUSTED | no LEDC channel or timer, RMT channel or device slot left |
| 5 | BAD_ARGUMENT | short payload, value out of range, unknown mode, wrong state, page past the last |
| 6 | BUS_UNKNOWN | the bus is not in the profile |
| 7 | NOT_IN_SCOPE | the command is not served by this server (or its feature is switched off in Kconfig) |
| 8 | HARDWARE | the driver failed |
| 9 | NO_ANSWER | the I2C device did not answer |
| 10 | PLUGIN | a plugin refused; its own code follows |
| 11 | SKIPPED | not executed: an earlier command was refused while abort was on |
| 12 | NO_SUCH_SLOT | no plugin in that slot |

**Errors.** Every refused command produces exactly one ERROR, queued as a reply (never dropped) before the next
command is handled. In extended mode ERROR replaces upstream's refusal reports, so a refusal is reported once: servo
unavailable is ERROR `EXHAUSTED` instead of `[2,6,pin]`, an I2C NACK or short read ERROR `NO_ANSWER` instead of
`[3,8,1,addr]`. DHT error reports and sonar timeouts are not refusals and stay stock reports in both modes.
`error_reports` counts ERROR reports; `refused_commands` counts refusals except NOT_IN_SCOPE and SKIPPED.

**SYNC.** The reply is queued after every reply and error of the commands before it. Flag bit 0, "abort on error",
governs the commands after that SYNC up to the next one; a session starts with abort on. While abort is on and a
command is refused, every later command up to the next SYNC is skipped: not executed, and ERROR `SKIPPED` with its own
`seq`. A SYNC is never skipped. (The spec's wording, "the commands since the previous sync", cannot be served: the
server cannot skip what it has already executed. A client that wants abort off for a batch sends a SYNC with flags 0
before it.) A SYNC shorter than 3 bytes is refused with `BAD_ARGUMENT`, gets no SYNC_REPLY and changes nothing.

**ON_BUS.** `bus` is the profile's bus number. The inner id must be 9-11 (I2C) or 27-35 (1-wire), else
`BAD_ARGUMENT`; the inner payload is checked like a stock packet's; a bus number not in the profile gets
`BUS_UNKNOWN`. The inner command's reply comes back as BUS_REPORT and its refusal as ERROR with `cmd` 58. Stock commands
sent directly act on bus 0 in both modes. Stock clamps stay (an I2C read returns at most 59 bytes, so a BUS_REPORT is
at most 66 bytes).

**Discovery.** `GET_CAPABILITIES [page]` answers with CAPABILITIES; a page past `last_page` gets ERROR
`BAD_ARGUMENT`. Records are `[type][len][value]` (`len` counts the value), never split across pages, at most 251
record bytes a page. Order: CHIP, NAMED_CAP..., PIN... (profile order), BUS... (I2C then 1-wire, profile order),
LIMIT..., PLUGIN... (slot order). Signed bytes are two's complement, multi-byte values big-endian, strings ASCII
without a terminator (cut to fit a page).

| Type | Record | Value |
|---|---|---|
| 1 | CHIP | `[model u16][revision u16 (major x 100 + minor)][IDF version, at most 32 bytes]` |
| 2 | PIN | `[gpio][caps u16][safe][adc_unit i8][adc_channel i8][named capability ids...]`; caps are the `TELEMETRIX_CAP_*` bits, safe the `telemetrix_safe_state_t` value |
| 3 | BUS | I2C `[0][bus][sda i8][scl i8][speed_hz u32]`; 1-wire `[1][bus][gpio i8]`; -1 = the client chooses |
| 4 | LIMIT | `[kind][count u16]` |
| 5 | PLUGIN | `[slot][major][minor][name]` |
| 6 | NAMED_CAP | `[id][name]` |

LIMIT kinds, computed from the grants:

| Kind | Limit | Count |
|---|---|---|
| 1 | servos | min(`max_servo`, granted LEDC channels) |
| 2 | PWM channels | granted LEDC channels |
| 3 | PWM timers | granted LEDC timers |
| 4 | DHT | `max_dht` when an RMT RX channel is granted, else 0 |
| 5 | sonar | `max_sonar` |
| 6 | analog interval floor, ms | `analog_interval_floor_ms` |

**Plugins** (`include/telemetrix_plugin.h`). A plugin is trusted application code registered through
`config.plugins` (at most `TELEMETRIX_PLUGINS_MAX`, 8); its slot is its index there. A client in extended mode reaches it
with `PLUGIN [slot, sub, args...]`, and the plugin answers with PLUGIN_REPORT. In stock mode plugins are unreachable.

- Names match `^[a-z0-9_]+\.[a-z0-9_.]+$`, at most 32 bytes, unique; the `telemetrix.` prefix is reserved for
  built-ins (there are none yet).
- Named capabilities: a plugin declares names (`named_caps`); a profile pin lists the names it grants
  (`telemetrix_pin_t.named`, `n_named`). At most 16 names per server, each at most 24 bytes; ids in order of first
  declaration. A plugin claims a granted pin with `telemetrix_plugin_claim_pin`, and RMT channels of the grant with
  `telemetrix_plugin_claim_rmt`; the server puts its pins back in their safe states on release.
- `telemetrix_start` returns `ESP_ERR_INVALID_ARG` for a pin naming a capability no registered plugin declares, a bad
  or duplicate plugin name, or too many plugins or names; `ESP_ERR_NOT_SUPPORTED` for plugins with
  `CONFIG_TELEMETRIX_EXTENDED` off.
- Every call into a plugin (command, poll, release) runs on the network task. Command and poll calls are timed against
  `config.plugin_budget_us` (default 5000 us); an overrun is counted (`overruns`, `max_us`), never aborted, and the
  network task waits for it, so a slow plugin delays every client report.
- `poll` runs only during an extended session: at once, then when the milliseconds it returned are up or after
  `telemetrix_plugin_wake` (`UINT32_MAX` = only on a wake). The network task never sleeps past the next poll.
- `release` runs on disconnect, RESET and stop, in slot order, before the server releases the pins and channels.
- `telemetrix_plugin_emit` (network task only) queues a PLUGIN_REPORT as a reply. `telemetrix_plugin_emit_async` (any
  task, not an ISR) puts it in a ring of 8 entries of up to 251 data bytes and wakes the network task, which sends it.
  Both need a client in extended mode (`ESP_ERR_INVALID_STATE` otherwise). A full ring gives `ESP_ERR_NO_MEM`, counted
  in `async_dropped`; entries queued in a session that has ended are dropped.
- The ctx a plugin is handed lives as long as the server. A plugin's own tasks must stop calling `emit_async` and
  `wake` before its release hook at stop returns; a later call fails with `ESP_ERR_INVALID_STATE` without touching the
  ctx.

## Deviations from upstream 3.2.0

- The servo-unavailable report carries its length byte: `[2,6,pin]` (upstream omits it).
- RESET releases pins and peripherals and never reboots. The stock client sends RESET on every exit, so a reboot would
  drop the network each time. The `reset` hook runs after the release.
- Every pin and count is checked against the profile; a bad request is refused and counted in `refused_commands`.
- Report 7 (I2C too few bytes) is never sent. A NACK or a short read produces report 8 (`[3,8,1,addr]`).
- `I2C_READ` with count 0 is refused.
- `MODIFY_REPORTING` works as upstream (modes 0-4 above).
- The PWM channel byte the client sends is ignored; the server allocates from the LEDC grant.
- Servo pulse = min + angle x (max - min) / 180 at 50 Hz and 14 bits.

## Upstream bugs found

Found while reading the sketch and the client; reportable upstream, none is worked around beyond what is listed above.

- The SPI init command's payload layout does not match between the client and the sketch.
- The client's `restart_on_shutdown` is stored as a one-element tuple, so it is always true and RESET is always sent.
- The client's documentation gives 1-wire report subtypes that differ from the constants the sketch sends (28, 32, 34,
  35).
- The client's sonar and DHT limits are off by one (`count < MAX - 1`).
- The server has buffer overflows on over-long input.

## Requirements for embedding applications

- `CONFIG_LWIP_SO_LINGER=y`: a refused second connection is closed with an RST through `SO_LINGER`; without it the
  refusal becomes a FIN.
- `CONFIG_FREERTOS_HZ=1000`: the 2 ms digital poll needs 1 ms ticks.
- While sonar is used, an application-installed GPIO ISR service must not use `ESP_INTR_FLAG_IRAM` (the sonar echo
  handler completes through flash code). The server never uninstalls the GPIO ISR service.
- `CONFIG_RMT_RX_ISR_CACHE_SAFE` is not supported (DHT channel creation would fail).
- On the original ESP32 ADC2 is unusable while Wi-Fi runs: put only ADC1 pins in the profile (the Olimex example does,
  for Ethernet too).
- The stock client's digital pin lists (4, 5, 12-14, 16-19, 21-23, 25-27, 32, 33) exclude the input-only GPIO 34-39,
  so the client refuses them before the server sees them; list them for ADC use only.
- Budget `TELEMETRIX_SOCKETS_M1` sockets (3) and `TELEMETRIX_EVENTFDS` eventfds (1) in the application's lwIP and VFS
  limits.

## Security

There is no authentication. Anyone who can open the TCP port can drive every pin in the profile. Restrict the
listener with the allowed-address list (`config.allowed`, CIDR) and the `access` hook, which may apply any other policy
(the hook is called only for peers the list admits). A peer that fails either gets an RST and `refused_connections`
goes up. Never expose the port to an untrusted network or the internet; keep the profile to the pins the client needs
and choose safe states for them.

One client at a time: a second connection is accepted and closed with an RST (`SO_LINGER` 0, so no TIME_WAIT is left).
Dead clients are found by TCP keepalive (idle 10 s, interval 2 s, count 3; the host tests check the socket options, the
timing on lwIP is confirmed on the board pass) and by the send-stall timeout: a client that
stops reading and leaves unsent data with no progress for `send_stall_ms` (10 s) is dropped. A stalled client and one
whose socket fails are closed with an RST (`SO_LINGER` 0), so no closing socket keeps retransmitting to a dead peer;
an orderly end, a kick and a stop close normally. Sends are non-blocking from
a per-connection queue; replies are never dropped. Digital and analog reports are coalesced to the latest value per pin,
sonar and DHT to the latest reading per device: a new report replaces the same pin's (or device's) report whenever one
is still queued unsent, not only when the queue is full, and waits for room as the latest value when there is none. So
while the socket is backed up a fast pulse (0, 1, 0) on a digital input can reach the client as just the final 0: the
1 is overwritten before it is sent.

## Sockets and memory

- Sockets: `TELEMETRIX_SOCKETS_M1` = 3 (listener, client, a transient one while a second connection is refused);
  eventfds: `TELEMETRIX_EVENTFDS` = 1 (wakes the network task from ISRs, other tasks and plugin `emit_async` and `wake`;
  the extended protocol adds no socket or eventfd).
- Network task stack: 6144 bytes, internal RAM. The status snapshot (1440 bytes on a 32-bit target) is built in the
  server's heap state, not on this stack; the deepest frames the task itself adds are a few hundred bytes (the emit-ring
  drain 560, a CAPABILITIES page 304, an ERROR 304, measured with `-fstack-usage` on esp32). Plugin calls run on this
  stack too.
- Plugin emit ring: 8 x 260 bytes (2080), static.
- Send queue: `send_queue_bytes` (default 4096 bytes), allocated once at start, internal RAM when available.
- Per-connection state `tmx_conn_t`: 4064 bytes on a 32-bit target (measured with `-m32`; 4072 on the 64-bit host). Of
  that, the unsent-entry index is 160 entries x 6 bytes and the pending table, the keyed reports waiting for room, is
  192 entries x 16 bytes (3072 bytes; room for 64 digital pins, 64 analog pins and up to 64 sonar and DHT devices).
- Adapter state `tmx_adapter_t` (claims, per-pin tables, sensors, session, plugin registry): 2536 bytes on a 32-bit
  target.
- The server never allocates after `telemetrix_start` returns, except driver handles created on a client's request.

## Testing

`make help` lists the targets; see `STYLEGUIDE.md` for the conventions.

- `make lint`: astyle and ruff through pre-commit.
- `make test-host`: Unity tests on the IDF `linux` target, twice: `test_host/` with every feature on and
  `test_host/features_off/` with every `CONFIG_TELEMETRIX_*` switch off.
- `make fuzz`: libFuzzer on the frame assembler, the adapter, and an extended session (handshake, frame and adapter
  together with the test plugin, ON_BUS on two buses), 60 s per target.
- `make host-server`: build the real server for `linux` with a fake driver layer. `--stock-only` sets
  `config.extended` false; `--test-plugin` registers the test plugin `test.echo` and grants gpio 36 its named
  capability `test.led`. The host profile adds I2C bus 1 (sda 2, scl 15, a device at 0x48), 1-wire bus 1 (gpio 35, one
  ROM) and gpio 36 to the stock client's pins.
- `make test-integration`: pytest against the host server with the stock client, hostile clients, and the recording
  proxy tests (including one run of the compatibility script).
- `make build-examples`: build every board example.

**Compatibility pass on a board** (`tools/compat`, needs `tools/.venv` from `make test-integration`). The recording
proxy sits between the stock client and the board, frames both directions and writes one JSON line per packet:

```sh
tools/.venv/bin/python tools/compat/compat_test.py --host 192.168.1.50 --loopback 32,33 --log ours.log --strict
tools/.venv/bin/python tools/compat/compat_test.py --compare upstream.log ours.log
```

Run the script once against upstream Telemetrix4Esp32 (`--log upstream.log`) and once against this firmware. Wire
GPIO32 (output) to GPIO33 (input) on the Olimex example (the default `--loopback 32,33`). `--i2c-addr`, `--dht PIN`, `--sonar TRIG,ECHO` and
`--onewire PIN` add the devices you have. `--strict` fails when the board sends a report id or length the stock client
does not dispatch (the client silently ignores those). `--compare` normalises timestamps and the firmware build byte
(3.2.0 against 3.2.100); reduces digital reports, per pin, to the sequence of distinct consecutive values (so an initial
or repeated report that one board sends and the other does not is not a difference); leaves out a digital report on an
input that arrives after its `SET_PIN_MODE` and before the next `DIGITAL_WRITE` (the initial report: one board may send
it and the other not, and its value depends on the input's state before the loopback drives it); and takes the
periodic analog, sonar and DHT reports out of the sequence, comparing only which (kind, pin, data or error) appeared. Everything else is
byte for byte, and any difference is printed with context (exit 1). A log with no server packets also fails.
Because periodic reports are not compared byte for byte, `--strict` is what catches a malformed one (and a connection
that ends inside a packet).
`recording_proxy.py --listen PORT --target HOST:PORT --log FILE [--strict]` runs the proxy alone.
