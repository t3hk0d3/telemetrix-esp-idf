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
| `i2c[]` | Buses: `bus`, `port`, `sda`, `scl` (-1 = the client chooses), `speed_hz`, optional application-owned `i2c_master_bus_handle_t`. A shared bus is never deleted by the component. |
| `onewire[]` | Buses: `bus` and `gpio` (-1 = the pin of `ONE_WIRE_INIT`). |

Stock commands always use bus 0 (I2C bus 0 and 1-wire bus 0). `I2C_BEGIN` pins are used only when bus 0's grant says
"client chooses pins" (and must be profile pins with the I2C capability), otherwise they are ignored; `[0,0]` with
"client chooses" is refused.

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

The network task's stack is 6144 bytes (internal constant).

**Hooks** (`telemetrix_hooks_t`, all optional): `access(peer, ctx)` (NULL = allow), `reset(ctx)` (called after a RESET
has released everything), `connected(peer, ctx)`, `disconnected(ctx)`.

**Status**: `telemetrix_get_status` returns whether the server runs and listens, the client's address, the counters
`refused_connections`, `dropped_packets`, `ignored_commands`, `refused_commands`, `coalesced_reports`,
`stalled_clients`, and what each claimed pin is used for. `telemetrix_kick_client` drops the current client.

**Build switches** (menuconfig, "Telemetrix"): `CONFIG_TELEMETRIX_DHT`, `_SONAR`, `_ONE_WIRE`, `_SERVO`, `_I2C`,
`_ANALOG`, each on by default.

## The stock protocol

Framing both ways: `[len][id][payload]`, `len` counts the id and the payload. Payload cap 30: `len` 2-31 is a command
with a payload, `len` 1 a command without; `len` 0 consumes one byte and counts `dropped_packets`; `len` over 31
skips `len` bytes and counts `dropped_packets`. A packet whose bytes do not all arrive within 2000 ms is dropped and
counted. The connection always stays (stock mode is lenient). 16-bit values are big-endian; DHT floats (`float`) and the
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
`SET_PIN_MODE` mode 7 (touch), and every id of 57 and above.

Timing: digital inputs are polled every 2 ms while any reports; analog every interval (default 19 ms, floor 10 ms);
sonar one device per 33 ms in turn, echo timeout 25 ms, cm = echo_us / 58, reported only on change; DHT every 2000 ms,
all devices, DHT22/AM2302 decoding. `DHT_NEW` registers the device and schedules its first read at once; every failed
read sends an error report and the device stays registered. Sonar and DHT never block the network task: a GPIO ISR
(sonar) or an RMT callback (DHT) completes them. The I2C timeout comes from the settings; 1-wire operations are bounded
by their slot timing.

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
Dead clients are found by TCP keepalive (idle 10 s, interval 2 s, count 3) and by the send-stall timeout: a client that
stops reading and leaves unsent data with no progress for `send_stall_ms` (10 s) is dropped. Sends are non-blocking from
a per-connection queue; replies are never dropped, digital and analog reports are coalesced to the latest value per
pin when the queue is full, sonar and DHT keep the latest reading per device.

## Sockets and memory

- Sockets: `TELEMETRIX_SOCKETS_M1` = 3 (listener, client, a transient one while a second connection is refused);
  eventfds: `TELEMETRIX_EVENTFDS` = 1 (wakes the network task from ISRs and other tasks).
- Network task stack: 6144 bytes, internal RAM.
- Send queue: `send_queue_bytes` (default 4096 bytes), allocated once at start, internal RAM when available.
- Per-connection state `tmx_conn_t`: 4064 bytes on a 32-bit target (measured with `-m32`; 4072 on the 64-bit host). Of
  that, the unsent-entry index is 160 entries x 6 bytes and the pending table, the keyed reports waiting for room, is
  192 entries x 16 bytes (3072 bytes; room for 64 digital pins, 64 analog pins and up to 64 sonar and DHT devices).
- Adapter state `tmx_adapter_t` (claims, per-pin tables, sensors): 1604 bytes on a 32-bit target.
- The server never allocates after `telemetrix_start` returns, except driver handles created on a client's request.

## Testing

`make help` lists the targets; see `STYLEGUIDE.md` for the conventions.

- `make lint`: astyle and ruff through pre-commit.
- `make test-host`: Unity tests on the IDF `linux` target.
- `make fuzz`: libFuzzer on the frame assembler and the adapter (60 s per target).
- `make host-server`: build the real server for `linux` with a fake driver layer.
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
GPIO32 (output) to GPIO33 (input) on the Olimex example. `--i2c-addr`, `--dht PIN`, `--sonar TRIG,ECHO` and
`--onewire PIN` add the devices you have. `--strict` fails when the board sends a report id or length the stock client
does not dispatch (the client silently ignores those). `--compare` normalises analog, sonar and DHT values,
timestamps and the firmware build byte (3.2.0 against 3.2.100) and prints any remaining difference with context.
`recording_proxy.py --listen PORT --target HOST:PORT --log FILE [--strict]` runs the proxy alone.
