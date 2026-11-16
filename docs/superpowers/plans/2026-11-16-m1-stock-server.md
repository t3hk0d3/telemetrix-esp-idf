# M1: stock server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An ESP-IDF component that serves the stock Telemetrix4Esp32 3.2.0 wire protocol over TCP from an
application-supplied board profile (pins, safe states, peripheral grants), proven against the stock Python client on
a host-built server in CI and on the Olimex ESP32-POE-ISO.

**Architecture:** Pure layers (framing, stock command checks, report encoders, profile and claims, scheduler, the
Telemetrix adapter) drive the hardware through one driver interface (`tmx_drv_t`), so everything above the drivers is
host-tested with fake drivers. One network task owns every socket, wakes on an eventfd, and sends through a bounded,
coalescing queue. Target drivers wrap the IDF (GPIO, LEDC, ADC oneshot, I2C master, `onewire_bus`, RMT RX, GPIO ISR).
The same component builds for the IDF `linux` target with fake drivers as a host server process.

**Tech Stack:** ESP-IDF v6.1 (targets `esp32`, `esp32s3`, `esp32c6`, `esp32p4`, `linux`), `espressif/onewire_bus`
^1.0, Unity host tests, clang libFuzzer, Python 3 with pytest and the stock `telemetrix-esp32` client.

**Spec:** `docs/superpowers/specs/2026-11-12-telemetrix-esp-idf-design.md` revision 3 (approved 2026-11-16): sections
3, 4.1–4.4 (stock mode only), 4.7, 4.8, 6 (M1). Extended mode, plugins and the client are M2; streams are M3.

**Where:** this repository, branch `main` (no remote yet; creating `t3hk0d3/telemetrix-esp-idf` on GitHub needs the
user's yes, asked at the end of M1). Reference only, never copied wholesale: `~/dev/telemetrix-esp32-idf` (revision 1;
its trailing-bus-byte code in `src/proto/commands.c` is wrong).

## Styleguide

No styleguide exists yet. Task 1 writes `STYLEGUIDE.md`; every later task follows it. Content:

- **C formatting:** astyle via pre-commit (`astyle_py`, rules in `astyle-rules.yml`: 4-space indent, attached braces,
  120 columns, pointer to name). `make lint` must pass and leave `git status` clean.
- **C naming:** `snake_case`. Public API prefix `telemetrix_` / `TELEMETRIX_` only in `include/`. Internal prefix
  `tmx_` / `TMX_`. One module per file; a module `foo` lives in `foo.c` with declarations in its layer's header
  (`src/proto/tmx_proto.h`, `src/core/tmx_core.h`, `src/adapter/tmx_adapter.h`, `src/drivers/tmx_drv.h`,
  `src/net/tmx_net.h`). Static functions have no prefix.
- **Layers and includes:** `proto` and `core` and `adapter` include only C standard headers, `esp_err.h` and
  `include/telemetrix.h` (they build for every target and for the fuzzer). Only `src/drivers/esp/` includes IDF driver
  headers. `src/net/` includes POSIX socket headers (`sys/socket.h`, `netinet/in.h`, `netinet/tcp.h`) that resolve to
  lwIP on targets and to the host on `linux`.
- **Errors:** functions that can fail return `esp_err_t`; `ESP_ERR_INVALID_ARG` for bad input, `ESP_ERR_INVALID_STATE`
  for wrong order, `ESP_ERR_NO_MEM`, `ESP_ERR_NOT_FOUND`, `ESP_ERR_TIMEOUT` from drivers for "no answer". Never
  `abort()`/`assert()` on input from the wire or the application. Every wire-caused refusal increments a counter.
- **Logging:** one `static const char *TAG = "telemetrix";` per file that logs; no logging on hot paths below `D`.
- **Banned:** `gpio_reset_pin` (enables the pull-up; spec 4.3); dynamic allocation after `telemetrix_start` returns
  except the send queue and driver handles created on a client's request; any IDF call with an infinite timeout.
- **Config:** compile-time switches in `Kconfig` as `CONFIG_TELEMETRIX_<FEATURE>`; runtime values in
  `telemetrix_config_t`; hardware facts in the profile, never in Kconfig.
- **Tests:** host unit tests in `test_host/main/test_<module>.c` using Unity,
  `TEST_CASE("<scenario>: <effect>", "[<module>]")`, one test per effect of the BDD skeleton below. Python in `tools/`
  formatted and linted with ruff; pytest files `tools/tests/test_<topic>.py`, functions `test_<scenario>_<effect>`.
- **Commands:** every documented command is a `make` target with a `## ` help comment: `make lint`, `make test-host`,
  `make fuzz` (60 s per target), `make host-server`, `make test-integration`, `make build-examples`, `make help`.
  Run them as `<cmd> 2>&1 | tee $SCRATCH/<name>.log` (`set -o pipefail;` first when the exit code matters).
- **Commits:** Conventional Commits (`feat(core): …`, `fix(net): …`, `test: …`, `docs: …`, `chore: …`), each ending
  with the `Co-Authored-By` line the session gives.

## Global Constraints

- ESP-IDF v6.1; component builds for `esp32`, `esp32s3`, `esp32c6`, `esp32p4` and `linux`. AGPL-3.0 (`LICENSE`),
  copyright naming the user; README credits Telemetrix4Esp32 by Alan Yorinks as the protocol source.
- Framing both ways: `[len][id][payload]`, `len` counts `id` and payload. Stock mode payload cap 30: `len` 2–31 valid
  with a command; `len` 1 = command with no payload; `len` 0 → one byte consumed, `dropped_packets` +1; `len` > 31 →
  skip `len` bytes, `dropped_packets` +1. A packet whose bytes do not all arrive within 2000 ms is dropped and counted.
  The connection always stays (stock mode is lenient; spec 4.3).
- 16-bit values big-endian. DHT floats (`float`) and the PWM frequency (`double`) little-endian, byte-wise
  regardless of host order.
- Command ids (upstream `private_constants.py`): in scope 0–18, 20, 27–35. Ignored and counted (`ignored_commands`):
  19 and 21 (DAC, out of v1), 22–26 (SPI), 36–56 (steppers), `SET_PIN_MODE` mode 7 (touch), every id ≥ 57 (extended
  ids arrive in M2). Ignored commands still have their payload consumed.
- Stock payloads and replies, byte for byte (upstream 3.2.0):
  `0` LOOP `[c]` → `[2,0,c]`; `1` SET_PIN_MODE `[pin,0|2|9,report]` (input, pull-up, pull-down), `[pin,1]` (output),
  `[pin,3,diff_hi,diff_lo,report]` (analog), `[pin,8,channel,resolution,freq f64 LE ×8]` (PWM);
  `2` DIGITAL_WRITE `[pin,v]`; `3` ANALOG_WRITE `[pin,v_hi,v_lo]`; `4` MODIFY_REPORTING `[mode 0–4,pin]`
  (0 disable all, 1 analog enable, 2 digital enable, 3 analog disable, 4 digital disable);
  `5` GET_FIRMWARE_VERSION `[]` → `[4,5,3,2,100]` (any payload ignored in M1); `6` SERVO_ATTACH
  `[pin,min_hi,min_lo,max_hi,max_lo]`, none free → `[2,6,pin]`; `7` SERVO_WRITE `[pin,angle]`; `8` SERVO_DETACH `[pin]`;
  `9` I2C_BEGIN `[]` or `[sda,scl]` (`[0,0]` = no pins); `10` I2C_READ `[addr,reg,count,stop]` →
  `[count+4,9,count,addr,reg,data…]`, count clamped to 59, a NACK or short read → `[3,8,1,addr]`;
  `11` I2C_WRITE `[count,addr,data×count]`; `12` SONAR_NEW `[trig,echo]` → `[4,10,trig,cm_hi,cm_lo]`;
  `13` DHT_NEW `[pin]` → `[11,11,0,pin,hum f32 LE,temp f32 LE]`, error `[4,11,1,pin,err]` (1 checksum, 2 timeout);
  `14` STOP_ALL_REPORTS; `15` SET_ANALOG_SCANNING_INTERVAL `[ms]`; `16` ENABLE_ALL_REPORTS; `17` ANALOG_OUT_ATTACH
  `[pin,channel]`; `18` ANALOG_OUT_DETACH `[pin]`; `20` RESET `[]`; `27` ONE_WIRE_INIT `[pin]`; `28` ONE_WIRE_RESET →
  `[3,14,28,presence]`; `29` SELECT `[8 bytes]`; `30` SKIP; `31` WRITE `[byte,power]`; `32` READ → `[3,14,32,byte]`;
  `33` RESET_SEARCH; `34` SEARCH → `[10,14,34,8 bytes]` (all `0xff` when none); `35` CRC8 `[n,bytes…]` with the stock
  client sending one byte more than `n` (it ignores the extra) → `[3,14,35,crc over n bytes]`.
  Digital report `[3,2,pin,v]`; analog report `[4,3,pin,v_hi,v_lo]` (raw 12-bit at 12 dB).
- Deviations, all in the README: `[2,6,pin]` carries its length byte (upstream omits it); RESET releases and never
  reboots; every pin and count is checked against the profile; report 7 (I2C too few) is never sent.
- Stock commands use bus 0 (I2C bus 0 and 1-wire bus 0 of the profile). `I2C_BEGIN` pins are used only when bus 0's
  grant says "client chooses pins" (and must be profile pins with the I2C capability), else ignored. `[0,0]` with
  "client chooses" → refused.
- The PWM channel the client names is ignored; channels and timers are allocated from the LEDC grant, timers shared by
  equal (frequency, resolution); no free channel or timer → refused. Servo: 50 Hz, 14-bit, pulse = min + angle × (max −
  min) / 180, `max_servo` default 4 but never more than free grant channels; resolution 1–14 accepted for PWM.
- Timing: digital inputs polled every 2 ms while any reports; analog every interval (default 19 ms, floor 10 ms);
  sonar one device per 33 ms round-robin, echo timeout 25 ms, cm = echo_us / 58, reported only on change; DHT every
  2000 ms, all devices, DHT22/AM2302 decoding (40 bits, 1 ms start pulse, checksum). DHT_NEW registers the device and
  schedules its first read at once; every failed read sends an error report; the device stays registered.
- Sonar and DHT never block the network task: the scheduler starts them, a GPIO ISR (sonar) or RMT RX callback (DHT)
  completes them through `tmx_completion_post`, the network task finishes the report. I2C timeout from the config
  (default 50 ms). 1-wire operations are bounded by their slot timing.
- Safe states: `FLOAT` (input, no pull), `PULL_UP`, `PULL_DOWN`, `DRIVE_LOW`, `DRIVE_HIGH`; applied to every profile pin
  at `telemetrix_start`, and to every claimed pin on disconnect, RESET and `telemetrix_stop`. Default `FLOAT`.
- Defaults (`telemetrix_config_default`): port 31336; keepalive idle 10 s, interval 2 s, count 3; send-stall timeout
  10000 ms; send queue 4096 bytes; I2C timeout 50 ms; analog interval floor 10 ms; `max_dht` 6, `max_sonar` 6,
  `max_servo` 4; task priority 5, core -1 (no affinity); task stack 6144 (internal constant).
- TCP: dual-stack listener (`in6addr_any`, IPv4-only when the stack has no IPv6), backlog 1; client sockets
  `TCP_NODELAY` 1, `SO_KEEPALIVE` 1 with idle/interval/count. A connection while a client is connected is accepted and
  closed with `SO_LINGER` {1,0} (RST), `refused_connections` +1. Access: peer must match the allowed-address list
  (empty = any; IPv4-mapped IPv6 compared as IPv4) and then the `access` hook (NULL = allow); else RST and counted.
- Back-pressure: non-blocking sends from a per-connection queue (`send_queue_bytes`). Replies (LOOP, firmware, I2C,
  1-wire, servo unavailable, I2C error, DHT/sonar error reports) are never dropped. Digital and analog reports are
  coalesced to the latest value per pin when the queue lacks room; sonar and DHT data keep the latest per device. A
  client that leaves unsent data with no send progress for `send_stall_ms` is dropped.
- Sockets the application must budget: `TELEMETRIX_SOCKETS_M1` = 3 (listener, client, transient refusal); eventfds:
  `TELEMETRIX_EVENTFDS` = 1.
- Memory (spec 4.8): task stack, send queue and ISR-shared completion state in internal RAM.

## Review Focus

1. A client that sets an output high, then its host vanishes: within keepalive (10 + 2 × 3 s) the pin goes to its safe
   state and the next client connects (Task 8 host test with a scripted dead peer; spare-board pass step 5).
2. A client that stops reading while 20 inputs toggle: the server neither blocks nor grows memory; after
   `send_stall_ms` it drops the client; replies queued before were never dropped (Task 8 `test_conn` cases).
3. Bytes past a 31-byte packet or a lying count byte (`I2C_WRITE` count 30 with 3 data bytes): nothing is written past
   any buffer, the next valid packet works (Task 2 tests and the fuzz targets in Tasks 2 and 7).
4. The stock client's `onewire_crc8` (one byte more than `n`) and `i2c_begin()` with no pins: handled exactly as
   upstream, bus 0 (Task 7 tests named for them).
5. An application that already owns ADC1 or an I2C bus: the component uses the handle it is given and never deletes it
   on disconnect (Task 4 grant tests, Task 11 driver rule).

## File map

- Root: `CMakeLists.txt` (`linux` target compiles `src/proto`, `src/core`, `src/adapter`, `src/net`,
  `src/drivers/fake`; other targets compile `src/drivers/esp` instead of `fake`), `idf_component.yml`, `Kconfig`,
  `LICENSE`, `README.md`, `STYLEGUIDE.md`, `CHANGELOG.md`, `Makefile`, `.pre-commit-config.yaml`, `astyle-rules.yml`,
  `.gitignore`
- `include/telemetrix.h`: public API
- `src/proto/`: `tmx_proto.h`, `frame.c` (assembler), `stock_cmds.c` (ids, length checks), `stock_reports.c` (encoders)
- `src/core/`: `tmx_core.h`, `profile.c` (validation, lookup), `claims.c` (pin and peripheral claims, safe states),
  `sched.c` (deadlines), `completion.c` (cross-context completion queue, platform wake through `tmx_net_wake`)
- `src/adapter/`: `tmx_adapter.h`, `adapter.c` (dispatch, counters, reporting flags, release), `digital.c`,
  `analog.c`, `pwm.c` (PWM and servo), `i2c.c`, `onewire.c`, `sensors.c` (DHT, sonar)
- `src/drivers/`: `tmx_drv.h` (the driver interface), `esp/drv_gpio.c`, `drv_ledc.c`, `drv_adc.c`, `drv_i2c.c`,
  `drv_onewire.c`, `drv_pulse.c` (RMT RX for DHT, GPIO ISR for sonar), `drv.c` (the `tmx_drv_t` instance);
  `fake/drv_fake.c`, `fake/tmx_drv_fake.h` (scriptable fake used by tests, fuzzer and host server)
- `src/net/`: `tmx_net.h`, `net.c` (task, select loop), `conn.c` (send queue, coalescing, stall), `sock.c` (listen,
  accept, options, refuse, address match), `wake.c` (eventfd: `esp_vfs_eventfd` on targets, `eventfd(2)` on linux),
  `api.c` (public functions, status snapshot)
- `test_host/`: Unity project, `main/test_frame.c`, `test_stock_cmds.c`, `test_reports.c`, `test_profile.c`,
  `test_claims.c`, `test_sched.c`, `test_adapter_digital.c`, `test_adapter_analog.c`, `test_adapter_pwm.c`,
  `test_adapter_i2c.c`, `test_adapter_onewire.c`, `test_adapter_sensors.c`, `test_conn.c`
- `test/fuzz/`: plain CMake with clang, `fuzz_frame.c`, `fuzz_adapter.c`, `corpus/`
- `host/server/`: IDF `linux`-target app: starts the component with fake drivers and the host profile
- `examples/esp32_poe/` (Ethernet; `sdkconfig.defaults.wifi` for the Wi-Fi variant), `examples/devkit_wifi/` (S3;
  also built for C6), `examples/p4_build_check/` (compile check only)
- `tools/compat/`: `recording_proxy.py`, `compat_test.py`, `known_reports.py`, `requirements.txt`;
  `tools/tests/`: pytest integration tests against the host server

## BDD skeleton

```
tmx_frame
    tmx_frame_feed
        On a complete packet in one call
            - emits (id, payload, len) once
        On a packet split byte by byte across calls
            - emits it once, unchanged
        On len 0
            - consumes one byte, dropped_packets +1, next packet emitted normally
        On len 32..255
            - skips len bytes, dropped_packets +1, next packet emitted normally
        On a gap over 2000 ms inside a packet
            - drops the partial packet, dropped_packets +1
tmx_stock_check
    tmx_stock_check
        On each in-scope id with its exact payload length
            - returns TMX_STOCK_OK
        On DIGITAL_WRITE with 1 payload byte
            - returns TMX_STOCK_SHORT
        On I2C_WRITE whose count exceeds the payload
            - returns TMX_STOCK_SHORT
        On ONE_WIRE_CRC8 [n, n+1 bytes] (stock client)
            - returns TMX_STOCK_OK (extra byte ignored)
        On ids 19, 21, 22-26, 36-56, 57-255 and SET_PIN_MODE mode 7
            - returns TMX_STOCK_IGNORED
        On a non-bus command with trailing extra bytes
            - returns TMX_STOCK_OK (as upstream)
tmx_reports
    each encoder
        On upstream example values
            - produces the exact bytes of Global Constraints
tmx_profile
    tmx_profile_validate
        On a valid profile
            - ESP_OK
        On a GPIO listed twice, more than 64 pins, a safe state out of range
            - ESP_ERR_INVALID_ARG
        On an I2C bus with fixed pins that are not profile pins with the I2C capability
            - ESP_ERR_INVALID_ARG
        On an LEDC grant with no channel or no timer while a pin has the PWM capability
            - ESP_ERR_INVALID_ARG
    tmx_profile_pin
        On a GPIO in the profile
            - returns its entry
        On a GPIO not in the profile (40, 255)
            - returns NULL
tmx_claims
    tmx_claim_pin
        On a free pin with the capability the use needs
            - claims it, returns ESP_OK
        On a pin lacking the capability
            - ESP_ERR_NOT_SUPPORTED, no claim
        On a pin claimed by another use
            - ESP_ERR_INVALID_STATE
        On reclaiming a digital pin for another digital use
            - ESP_OK
    tmx_claim_ledc
        On a request for (1000 Hz, 8 bit) twice
            - two channels, one shared timer
        On a fifth distinct (frequency, resolution) with 4 timers granted
            - ESP_ERR_NOT_FOUND, nothing claimed
    tmx_claims_release_all
        On release
            - every claimed pin gets its safe state through the driver, in profile order
            - every LEDC channel stopped, every timer freed
            - shared (application-owned) I2C and ADC handles untouched
tmx_sched
    tmx_sched_next
        On deadlines 19 ms and 2 ms
            - returns 2 ms; after 2 ms, the 2 ms task is due
        On a millisecond counter wrap
            - deadlines still fire in order
tmx_adapter
    SET_PIN_MODE
        On output on a profile pin
            - driver gpio output called once, pin claimed as output
        On input pull-up with report 1
            - a later level change is reported [3,2,pin,v] once
        On input pull-down on a pin without pull-down
            - refused_commands +1, no driver call
        On analog on a pin with an ADC channel
            - [4,3,pin,v_hi,v_lo] every interval when |delta| >= differential
        On PWM with a fifth distinct frequency
            - refused_commands +1, no channel taken, the pin's previous mode untouched
        On touch (mode 7)
            - ignored_commands +1
    DIGITAL_WRITE / ANALOG_WRITE
        On a pin not claimed for that use
            - refused_commands +1, no driver call
    MODIFY_REPORTING / STOP_ALL_REPORTS / ENABLE_ALL_REPORTS
        On disable all
            - no digital or analog report until enabled
    SERVO_ATTACH / SERVO_WRITE
        On more servos than max_servo or free channels
            - [2,6,pin] for the extra one
        On angle 90 with min 544, max 2400
            - pulse 1472 us
    I2C_BEGIN / I2C_READ / I2C_WRITE
        On a read before begin
            - refused_commands +1
        On i2c_begin() with no pins and bus 0 fixed
            - bus 0 opened on its fixed pins
        On a read the device NACKs
            - [3,8,1,addr]
        On a read of 3 bytes from 0x76 reg 0xD0
            - [7,9,3,0x76,0xD0,b0,b1,b2]
        On a read of 200 bytes
            - clamped to 59
    SONAR_NEW / DHT_NEW
        On more devices than max
            - refused_commands +1
        On a sonar echo of 5800 us
            - [4,10,trig,0,100] once; the same distance again is not reported
        On a DHT22 frame 02 8C 01 5F EE
            - humidity 65.2, temperature 35.1
        On a DHT frame with a bad checksum
            - [4,11,1,pin,1]; device stays registered
        On a sonar ping in progress
            - the network task is not blocked (poll returns before completion)
    ONE_WIRE_*
        On reset with a device present
            - [3,14,28,1]
        On search with no more devices
            - [10,14,34,ff×8]
        On the stock client's crc8 of 28 FF 4C 61 92 16 05 plus its CRC byte
            - [3,14,35,crc of the 7 bytes]
    RESET / release
        On RESET
            - every claimed pin to its safe state, buses ended, reports cleared, reset hook called, no reboot
        On disconnect
            - same release; reporting enabled for the next session
tmx_conn (send queue)
    tmx_conn_queue_reply
        On a full queue
            - the reply is kept (queue grows only into its reply reserve); never dropped
    tmx_conn_queue_input
        On a digital report for a pin already queued, with no room
            - the queued value is replaced, not duplicated
    tmx_conn_flush
        On a peer that accepts no bytes for send_stall_ms
            - returns TMX_CONN_STALLED
telemetrix (public)
    telemetrix_start
        On a valid profile and config
            - task running, listening, every profile pin in its safe state
        On a second call
            - ESP_ERR_INVALID_STATE
        On an invalid profile
            - ESP_ERR_INVALID_ARG, nothing started
    telemetrix_get_status
        On a connected client with two claimed pins
            - client address, both pin uses, counters
    telemetrix_kick_client
        On a connected client
            - client closed, pins to safe state
    telemetrix_stop
        On a call with a client
            - everything released, sockets closed, task gone
network behaviour (host server, pytest)
    On a second connection
        - RST, refused_connections +1, first client unaffected
    On a peer outside the allowed list
        - RST before any byte is read
    On a client that stops reading
        - dropped after send_stall_ms
    On the stock telemetrix-esp32 client
        - firmware version, digital out/in, PWM, analog, I2C, sonar, DHT and 1-wire calls all succeed against fakes
```

---

### Task 1: Repository, styleguide, build skeleton, public header

**Files:** Create every root file of the file map, `include/telemetrix.h`, `test_host/{CMakeLists.txt,
main/CMakeLists.txt,main/test_main.c}`, `src/` layer headers (empty declarations), `.gitignore`

**Interfaces — Produces (`include/telemetrix.h`, the M1 public API; later tasks implement it):**
- `typedef enum { TELEMETRIX_CAP_DIN = 1 << 0, TELEMETRIX_CAP_DOUT = 1 << 1, TELEMETRIX_CAP_PULL_UP = 1 << 2,
  TELEMETRIX_CAP_PULL_DOWN = 1 << 3, TELEMETRIX_CAP_PWM = 1 << 4, TELEMETRIX_CAP_ADC = 1 << 5,
  TELEMETRIX_CAP_I2C = 1 << 6, TELEMETRIX_CAP_ONE_WIRE = 1 << 7 } telemetrix_cap_t;` (bits 8–15 reserved for M2/M3)
- `typedef enum { TELEMETRIX_SAFE_FLOAT, TELEMETRIX_SAFE_PULL_UP, TELEMETRIX_SAFE_PULL_DOWN,
  TELEMETRIX_SAFE_DRIVE_LOW, TELEMETRIX_SAFE_DRIVE_HIGH } telemetrix_safe_state_t;`
- `typedef struct { uint8_t gpio; uint16_t caps; telemetrix_safe_state_t safe; int8_t adc_unit; int8_t adc_channel; }
  telemetrix_pin_t;` (-1 = no ADC)
- `typedef struct { uint8_t speed_mode; uint8_t timer_mask; uint16_t channel_mask; } telemetrix_ledc_grant_t;`
- `typedef struct { uint8_t rx_channels; uint8_t tx_channels; } telemetrix_rmt_grant_t;`
- `typedef struct { uint8_t unit; void *handle; } telemetrix_adc_grant_t;` (`handle` an `adc_oneshot_unit_handle_t`
  the application owns, or NULL: the component creates and deletes its own)
- `typedef struct { uint8_t bus; uint8_t port; int8_t sda; int8_t scl; uint32_t speed_hz; void *handle; }
  telemetrix_i2c_bus_t;` (`sda`/`scl` -1 = client chooses; `handle` an application-owned `i2c_master_bus_handle_t`
  or NULL)
- `typedef struct { uint8_t bus; int8_t gpio; } telemetrix_onewire_bus_t;` (-1 = ONE_WIRE_INIT's pin)
- `typedef struct { const telemetrix_pin_t *pins; size_t n_pins; telemetrix_ledc_grant_t ledc; telemetrix_rmt_grant_t
  rmt; const telemetrix_adc_grant_t *adc; size_t n_adc; const telemetrix_i2c_bus_t *i2c; size_t n_i2c;
  const telemetrix_onewire_bus_t *onewire; size_t n_onewire; } telemetrix_profile_t;`
- `typedef struct { uint8_t family; uint8_t addr[16]; uint8_t prefix; } telemetrix_cidr_t;` (family `AF_INET` or
  `AF_INET6`) and `esp_err_t telemetrix_cidr_parse(const char *text, telemetrix_cidr_t *out);`
- `typedef struct { bool server_enabled; uint16_t port; uint8_t keepalive_idle_s, keepalive_intvl_s, keepalive_count;
  uint16_t send_stall_ms; uint16_t send_queue_bytes; uint16_t i2c_timeout_ms; uint8_t analog_interval_floor_ms;
  uint8_t max_dht, max_sonar, max_servo; const telemetrix_cidr_t *allowed; size_t n_allowed; uint8_t task_priority;
  int8_t task_core; } telemetrix_config_t;` and `void telemetrix_config_default(telemetrix_config_t *out);`
- `typedef struct { bool (*access)(const struct sockaddr *peer, void *ctx); void (*reset)(void *ctx);
  void (*connected)(const char *peer, void *ctx); void (*disconnected)(void *ctx); void *ctx; } telemetrix_hooks_t;`
- Status: `#define TELEMETRIX_PEER_MAX 48`; `typedef struct { uint8_t gpio; const char *use; } telemetrix_pin_use_t;`
  (`use`: `"input"`, `"input_pullup"`, `"input_pulldown"`, `"output"`, `"analog"`, `"pwm"`, `"servo"`, `"i2c"`,
  `"one_wire"`, `"sonar"`, `"dht"`); `typedef struct { bool running, listening; char client[TELEMETRIX_PEER_MAX];
  uint32_t refused_connections, dropped_packets, ignored_commands, refused_commands, coalesced_reports,
  stalled_clients; size_t n_pins; telemetrix_pin_use_t pins[64]; } telemetrix_status_t;`
- `esp_err_t telemetrix_start(const telemetrix_profile_t *, const telemetrix_config_t *, const telemetrix_hooks_t *);`
  (copies all three, including the arrays), `esp_err_t telemetrix_stop(void);`, `esp_err_t
  telemetrix_get_status(telemetrix_status_t *out);`, `void telemetrix_kick_client(void);`
- `#define TELEMETRIX_SOCKETS_M1 3`, `#define TELEMETRIX_EVENTFDS 1`
- Includes only `<stdbool.h>`, `<stddef.h>`, `<stdint.h>`, `<sys/socket.h>`, `esp_err.h`.
- Kconfig: `TELEMETRIX_DHT`, `TELEMETRIX_SONAR`, `TELEMETRIX_ONE_WIRE`, `TELEMETRIX_SERVO`, `TELEMETRIX_I2C`,
  `TELEMETRIX_ANALOG`, all bool default y; a disabled feature's commands become `ignored_commands`.

- [ ] **Step 1:** The repository already exists with the spec and this plan; write `STYLEGUIDE.md` (the
  Styleguide section above, as the repository's rules), `LICENSE`, `README.md` stub (what, credit, headings for
  protocol and deviations), `CHANGELOG.md`, `Makefile` with every target in the styleguide (`fuzz`, `host-server`,
  `test-integration`, `build-examples` may print "not yet" until their task), `.pre-commit-config.yaml`,
  `astyle-rules.yml` (copy from `~/dev/esp32-otbr/tools/astyle-rules.yml`), `idf_component.yml` (no `url` yet).
- [ ] **Step 2:** Write `include/telemetrix.h` and a `test_host` project whose `test_main.c` runs zero tests.
- [ ] **Step 3: Verify** — `make test-host` builds and exits 0 with 0 tests; `make lint` passes; `git status` clean.
- [ ] **Step 4: Commit** — `chore: repository, styleguide, public API header`.

### Task 2: Framing and stock command checks, fuzz harness

**Files:** Create `src/proto/tmx_proto.h`, `frame.c`, `stock_cmds.c`; `test_host/main/test_frame.c`,
`test_stock_cmds.c`; `test/fuzz/{CMakeLists.txt,fuzz_frame.c,corpus/}`

**Interfaces — Produces (`tmx_proto.h`):**
- Every upstream id as `TMX_CMD_<NAME>` and `TMX_RPT_<NAME>` (names from `private_constants.py`).
- `#define TMX_STOCK_PAYLOAD_MAX 30`; `typedef void (*tmx_packet_fn)(uint8_t id, const uint8_t *payload, size_t len,
  void *ctx);`
- `typedef struct { uint8_t buf[256]; uint16_t need, have, skip; uint32_t started_ms; uint32_t dropped; uint16_t cap; }
  tmx_frame_t;` `void tmx_frame_init(tmx_frame_t *f, uint16_t payload_cap);` (cap 30 in M1; M2 raises it),
  `void tmx_frame_feed(tmx_frame_t *f, const uint8_t *data, size_t n, uint32_t now_ms, tmx_packet_fn fn, void *ctx);`,
  `void tmx_frame_tick(tmx_frame_t *f, uint32_t now_ms);`
- `typedef enum { TMX_STOCK_OK, TMX_STOCK_SHORT, TMX_STOCK_IGNORED } tmx_stock_verdict_t;`
  `tmx_stock_verdict_t tmx_stock_check(uint8_t id, const uint8_t *payload, size_t len);`

- [ ] **Step 1: Write the failing tests** — one `TEST_CASE` per effect of the `tmx_frame` and `tmx_stock_check`
  branches; a table-driven case listing every in-scope id with its exact payload length; the CRC8 case uses the bytes
  the stock client sends for `onewire_crc8([0x28,0xFF,0x4C,0x61,0x92,0x16,0x05,crc])`.
- [ ] **Step 2: Run, expect failure** — `make test-host` (missing header).
- [ ] **Step 3: Implement** `frame.c` (state machine: need/have/skip) and `stock_cmds.c` (a static table of minimum
  lengths; variable ones from the count byte).
- [ ] **Step 4: Run, expect pass** — `make test-host`.
- [ ] **Step 5: Fuzz harness** — `test/fuzz` builds `fuzz_frame` with clang `-fsanitize=fuzzer,address,undefined`
  over `tmx_frame_feed` + `tmx_stock_check`, random split points and timestamps; `make fuzz` runs each target 60 s with
  the corpus. Verify: `make fuzz` exits 0 with no crash.
- [ ] **Step 6: Commit** — `feat(proto): framing, stock command checks, fuzz harness`.

### Task 3: Stock report encoders

**Files:** Create `src/proto/stock_reports.c`; `test_host/main/test_reports.c`; modify `tmx_proto.h`

**Interfaces — Produces:** `#define TMX_REPORT_MAX 64`; each encoder writes into `uint8_t *out` and returns the byte
count: `tmx_rpt_loop(out, c)`, `tmx_rpt_digital(out, pin, v)`, `tmx_rpt_analog(out, pin, value)`,
`tmx_rpt_firmware(out)`, `tmx_rpt_servo_unavailable(out, pin)`, `tmx_rpt_i2c_read(out, addr, reg, data, n)`,
`tmx_rpt_i2c_short(out, addr)` (`[3,8,1,addr]`), `tmx_rpt_sonar(out, trig, cm)`, `tmx_rpt_dht_data(out, pin, hum,
temp)`, `tmx_rpt_dht_error(out, pin, err)`, `tmx_rpt_onewire(out, subtype, data, n)`.

- [ ] **Steps 1–5** as Task 2 steps 1–4 and 6, asserting full byte arrays for each layout in Global Constraints; the
  float tests assert the little-endian bytes of 65.2f and 35.1f; commit `feat(proto): stock report encoders`.

### Task 4: Profile, claims, safe states, peripheral grants

**Files:** Create `src/core/tmx_core.h`, `profile.c`, `claims.c`; `test_host/main/test_profile.c`, `test_claims.c`;
`src/drivers/tmx_drv.h` (interface only, below); `src/drivers/fake/{drv_fake.c,tmx_drv_fake.h}`

**Interfaces — Produces:**
- `tmx_drv.h`: `typedef struct { esp_err_t (*gpio_input)(uint8_t gpio, bool pull_up, bool pull_down);
  esp_err_t (*gpio_output)(uint8_t gpio, bool level); void (*gpio_set)(uint8_t gpio, bool v); int (*gpio_get)(uint8_t
  gpio); esp_err_t (*ledc_timer)(uint8_t speed_mode, uint8_t timer, uint32_t freq_hz, uint8_t resolution);
  esp_err_t (*ledc_attach)(uint8_t speed_mode, uint8_t channel, uint8_t timer, uint8_t gpio); esp_err_t
  (*ledc_duty)(uint8_t speed_mode, uint8_t channel, uint32_t duty); void (*ledc_stop)(uint8_t speed_mode, uint8_t
  channel); void (*ledc_timer_free)(uint8_t speed_mode, uint8_t timer); esp_err_t (*adc_read)(uint8_t unit, uint8_t
  channel, int *raw); esp_err_t (*i2c_open)(uint8_t bus, const telemetrix_i2c_bus_t *grant, int8_t sda, int8_t scl);
  esp_err_t (*i2c_read)(uint8_t bus, uint8_t addr, uint8_t reg, bool stop, uint8_t *data, size_t n, uint16_t
  timeout_ms); esp_err_t (*i2c_write)(uint8_t bus, uint8_t addr, const uint8_t *data, size_t n, uint16_t timeout_ms);
  void (*i2c_close)(uint8_t bus); esp_err_t (*ow_open)(uint8_t bus, uint8_t gpio); bool (*ow_reset)(uint8_t bus); void
  (*ow_write)(uint8_t bus, uint8_t byte, bool power); uint8_t (*ow_read)(uint8_t bus); bool (*ow_search)(uint8_t bus,
  uint8_t rom[8]); void (*ow_reset_search)(uint8_t bus); void (*ow_close)(uint8_t bus); esp_err_t (*dht_start)(uint8_t
  gpio, uint32_t token); esp_err_t (*sonar_start)(uint8_t trig, uint8_t echo, uint32_t token); esp_err_t
  (*init)(const telemetrix_profile_t *p); void (*deinit)(void); } tmx_drv_t;` Completions of `dht_start` /
  `sonar_start` arrive through `tmx_completion_post` (Task 5) with the token: DHT 5 frame bytes or an error, sonar
  echo µs or `ESP_ERR_TIMEOUT`.
- `tmx_drv_fake.h`: `extern const tmx_drv_t tmx_drv_fake;` plus scripting: `tmx_fake_reset()`, `tmx_fake_set_level(gpio,
  v)`, `tmx_fake_loopback(out_gpio, in_gpio)`, `tmx_fake_set_adc(unit, ch, raw)`, `tmx_fake_i2c_device(bus, addr,
  const uint8_t *regs, size_t n)`, `tmx_fake_dht_frame(gpio, const uint8_t f[5], esp_err_t err)`,
  `tmx_fake_sonar_echo(trig, uint32_t us)`, `tmx_fake_ow_devices(bus, const uint8_t (*roms)[8], size_t n)`, and a call
  log: `tmx_fake_calls(void)` returning the recorded `"name(args)"` lines, `tmx_fake_count(const char *name)`.
- `tmx_core.h`: `esp_err_t tmx_profile_validate(const telemetrix_profile_t *p);`, `const telemetrix_pin_t
  *tmx_profile_pin(const telemetrix_profile_t *p, uint8_t gpio);`, `typedef enum { TMX_USE_NONE, TMX_USE_INPUT,
  TMX_USE_INPUT_PULLUP, TMX_USE_INPUT_PULLDOWN, TMX_USE_OUTPUT, TMX_USE_ANALOG, TMX_USE_PWM, TMX_USE_SERVO,
  TMX_USE_I2C, TMX_USE_ONE_WIRE, TMX_USE_SONAR_TRIG, TMX_USE_SONAR_ECHO, TMX_USE_DHT } tmx_use_t;`,
  `typedef struct tmx_claims tmx_claims_t;` (fixed arrays, no heap), `esp_err_t tmx_claims_init(tmx_claims_t *c,
  const telemetrix_profile_t *p, const tmx_drv_t *drv);` (applies every pin's safe state), `esp_err_t
  tmx_claim_pin(tmx_claims_t *c, uint8_t gpio, tmx_use_t use);`, `void tmx_release_pin(tmx_claims_t *c, uint8_t gpio);`
  (safe state), `tmx_use_t tmx_pin_use(const tmx_claims_t *c, uint8_t gpio);`, `const char *tmx_use_name(tmx_use_t);`,
  `esp_err_t tmx_claim_ledc(tmx_claims_t *c, uint32_t freq_hz, uint8_t resolution, uint8_t *channel, uint8_t *timer);`,
  `void tmx_release_ledc(tmx_claims_t *c, uint8_t channel);`, `esp_err_t tmx_claim_rmt_rx(tmx_claims_t *c);`,
  `void tmx_release_rmt_rx(tmx_claims_t *c);`, `void tmx_claims_release_all(tmx_claims_t *c);`
- Capability per use: INPUT DIN; INPUT_PULLUP DIN+PULL_UP; INPUT_PULLDOWN DIN+PULL_DOWN; OUTPUT DOUT; ANALOG ADC; PWM
  and SERVO PWM; I2C I2C; ONE_WIRE ONE_WIRE; SONAR_TRIG DOUT; SONAR_ECHO DIN; DHT DIN+DOUT.
- Safe state through the driver: FLOAT `gpio_input(g,false,false)`; PULL_UP/DOWN `gpio_input` with that pull;
  DRIVE_LOW/HIGH `gpio_output(g, level)`.

- [ ] **Steps:** failing tests for every effect of `tmx_profile` and `tmx_claims` (the fake's call log asserts the
  safe-state calls and their order); run, fail; implement; run, pass; `make lint`; commit
  `feat(core): profile validation, claims, safe states, peripheral grants`.

### Task 5: Scheduler and completion queue

**Files:** Create `src/core/sched.c`, `src/core/completion.c`; `test_host/main/test_sched.c`; modify `tmx_core.h`

**Interfaces — Produces:**
- `typedef struct { uint32_t due_ms; bool armed; } tmx_deadline_t;` `void tmx_deadline_arm(tmx_deadline_t *d, uint32_t
  now_ms, uint32_t in_ms);`, `bool tmx_deadline_due(const tmx_deadline_t *d, uint32_t now_ms);`, `uint32_t
  tmx_deadline_left(const tmx_deadline_t *d, uint32_t now_ms);` (wrap-safe signed comparison; `UINT32_MAX` unarmed)
- `typedef struct { uint8_t kind; uint32_t token; esp_err_t err; uint32_t value; uint8_t data[5]; } tmx_completion_t;`
  `void tmx_completion_post(const tmx_completion_t *c);` (callable from an ISR, a driver callback or another task;
  fixed ring of 16, overflow counted; calls `tmx_net_wake()`), `bool tmx_completion_take(tmx_completion_t *out);`
  (network task only). Platform locking: a `portMUX` critical section on targets, a pthread mutex on `linux`.

- [ ] **Steps:** failing tests (deadline order, wrap at `0xFFFFFFF0`, completion ring order and overflow count, with
  `tmx_net_wake` stubbed in the test); implement; pass; commit `feat(core): scheduler deadlines and completion queue`.

### Task 6: Adapter: digital, analog, PWM, servo, reporting, loop, firmware, reset

**Files:** Create `src/adapter/tmx_adapter.h`, `adapter.c`, `digital.c`, `analog.c`, `pwm.c`;
`test_host/main/test_adapter_digital.c`, `test_adapter_analog.c`, `test_adapter_pwm.c`

**Interfaces — Consumes:** Tasks 2–5. **Produces (`tmx_adapter.h`):**
- `typedef void (*tmx_emit_fn)(const uint8_t *report, size_t n, bool reply, int8_t coalesce_key_kind, uint16_t
  coalesce_key, void *ctx);` (`reply` true = never drop; coalesce kinds: -1 none, 0 digital pin, 1 analog pin, 2 sonar
  device, 3 DHT device)
- `typedef struct tmx_adapter tmx_adapter_t;` `esp_err_t tmx_adapter_init(tmx_adapter_t *a, const
  telemetrix_profile_t *p, const telemetrix_config_t *cfg, const tmx_drv_t *drv, tmx_emit_fn emit, void *ctx);`,
  `void tmx_adapter_packet(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len, uint32_t now_ms);`
  (the `tmx_packet_fn` target), `uint32_t tmx_adapter_poll(tmx_adapter_t *a, uint32_t now_ms);` (runs due scans and
  completions; returns ms until the next deadline), `void tmx_adapter_release(tmx_adapter_t *a);`, `void
  tmx_adapter_counters(const tmx_adapter_t *a, uint32_t *ignored, uint32_t *refused);`, `size_t
  tmx_adapter_pin_uses(const tmx_adapter_t *a, telemetrix_pin_use_t *out, size_t cap);`, `bool
  tmx_adapter_reset_requested(tmx_adapter_t *a);` (true once after a RESET command, for the net layer's hook call)
- Digital and analog "last value" start at 0 (upstream); the first sample after a mode change is reported.

- [ ] **Steps:** failing tests for every effect of the SET_PIN_MODE, DIGITAL/ANALOG_WRITE, MODIFY_REPORTING, SERVO,
  RESET/release branches and LOOP/firmware replies, time advanced through `tmx_adapter_poll`; the refused-PWM test
  asserts the pin keeps its previous mode and the fake saw no LEDC call; implement; pass; `make lint`; commit
  `feat(adapter): digital, analog, PWM, servo, reporting and reset`.

### Task 7: Adapter: I2C, 1-wire, DHT, sonar; adapter fuzz target

**Files:** Create `src/adapter/i2c.c`, `onewire.c`, `sensors.c`; `test_host/main/test_adapter_i2c.c`,
`test_adapter_onewire.c`, `test_adapter_sensors.c`; `test/fuzz/fuzz_adapter.c`

**Interfaces — Consumes:** Task 6's adapter; the fake's I2C, 1-wire, DHT and sonar scripting; completions (Task 5).

- [ ] **Step 1: Write the failing tests** for every effect of the I2C, SONAR/DHT and ONE_WIRE branches, including
  `i2c_begin()` with no pins on a fixed bus 0, the 59-byte clamp, the CRC8 case with the stock client's extra byte, the
  DHT frame `02 8C 01 5F EE` → 65.2/35.1, and "a sonar ping in progress does not block": `tmx_adapter_poll` returns
  after `sonar_start` without a completion, and the report appears only after the fake posts it.
- [ ] **Step 2–4:** run, fail; implement (CRC-8 Dallas/Maxim computed in the adapter; I2C uses `cfg.i2c_timeout_ms`);
  run, pass.
- [ ] **Step 5: Fuzz** — `fuzz_adapter` feeds random packets and random completion events into an adapter on the fake
  driver with a full profile; `make fuzz` 60 s per target, no crash, no sanitizer report.
- [ ] **Step 6: Commit** — `feat(adapter): I2C, 1-wire, DHT and sonar; adapter fuzz target`.

### Task 8: Network layer and public API

**Files:** Create `src/net/tmx_net.h`, `net.c`, `conn.c`, `sock.c`, `wake.c`, `api.c`, `src/core/cfg.c`
(`telemetrix_config_default`, `telemetrix_cidr_parse`); `test_host/main/test_conn.c`; modify `CMakeLists.txt`

**Interfaces — Consumes:** Tasks 2–7. **Produces:** the public functions of Task 1; internal `void tmx_net_wake(void);`
and the queue API:
- `typedef struct tmx_conn tmx_conn_t;` `esp_err_t tmx_conn_init(tmx_conn_t *c, uint16_t bytes);`, `void
  tmx_conn_queue(tmx_conn_t *c, const uint8_t *r, size_t n, bool reply, int8_t key_kind, uint16_t key);`, `typedef
  enum { TMX_CONN_IDLE, TMX_CONN_PENDING, TMX_CONN_STALLED, TMX_CONN_ERROR } tmx_conn_state_t;` `tmx_conn_state_t
  tmx_conn_flush(tmx_conn_t *c, int fd, uint32_t now_ms, uint16_t stall_ms);`, `void tmx_conn_reset(tmx_conn_t *c);`
  (`flush` takes a send function pointer in tests so `test_conn.c` scripts partial and zero-byte sends)

Queue algorithm (pseudocode; the rest follows from the tests):

```
queue(report, reply, key):
    if key given and an unsent entry with the same key exists: overwrite its bytes in place; coalesced += 1; return
    if room: append
    else if reply: append into the reply reserve (one quarter of the queue held back for replies);
                   if even that is full: mark connection broken (dropped as stalled)
    else if key given: evict the oldest unsent keyed entry of the same kind, append; coalesced += 1
    else: count and drop (never happens for stock reports: every non-reply report has a key)
flush(now):
    send as much as the socket takes (non-blocking)
    if bytes left and none sent since stall_since >= stall_ms: STALLED
```

Network task loop: `select()` over listener, client, eventfd; timeout = `tmx_adapter_poll` result capped at 100 ms;
`POLLOUT` interest only while the queue has bytes; RESET → release then `reset` hook; disconnect → release then
`disconnected` hook. Status is copied under a mutex the task holds only while updating the snapshot. `telemetrix_stop`
waits up to 2 s for the task and does not hold the lock while hooks run.

- [ ] **Step 1: Write the failing tests** for every effect of `tmx_conn` (coalescing replaces in place, replies never
  dropped with a full queue, stall after `stall_ms` with zero-byte sends, partial sends keep order).
- [ ] **Step 2–4:** run, fail; implement `conn.c`; run, pass.
- [ ] **Step 5: Implement** `net.c`, `sock.c` (address match: IPv4-mapped IPv6 normalised; list, then hook; RST with
  `SO_LINGER` {1,0}; a build warning and one runtime log when `CONFIG_LWIP_SO_LINGER` is off), `wake.c`, `api.c`,
  `cfg.c`.
- [ ] **Step 6: Verify** — `make test-host`; `make lint`.
- [ ] **Step 7: Commit** — `feat(net): server task, send queue with coalescing, stall drop, access and RST refusal`.

### Task 9: Host server and integration tests with the stock client

**Files:** Create `host/server/{CMakeLists.txt,main/main.c,main/host_profile.c,sdkconfig.defaults}`,
`tools/tests/{conftest.py,test_network.py,test_stock_client.py}`, `tools/compat/requirements.txt` (`telemetrix-esp32`
pinned to its latest release at the time of the task, the version recorded in the file; `pytest`); modify `Makefile`

**Interfaces:**
- `host/server` runs the component on the `linux` target with `tmx_drv_fake`, a profile using the ESP32-classic GPIO
  numbers the stock client accepts (4, 5, 12, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33), fake loopbacks
  5→4 and 13→12, ADC on 32 and 33, an I2C device at 0x76 on bus 0, one DHT on 25, a sonar 26→27 at 5800 µs, one 1-wire
  ROM on 16. Arguments: `--port N`, `--allow CIDR`, `--stall-ms N`. Prints `listening <port>` when ready.
- `conftest.py` starts it on a free port per test session and stops it.

- [ ] **Step 1: Write the tests** — `test_network.py`: `test_second_connection_reset_and_counted`,
  `test_disallowed_peer_reset_before_read` (server started with `--allow 10.0.0.0/8`),
  `test_stalled_reader_dropped_after_stall_ms`, `test_garbage_then_valid_packet_still_served`,
  `test_disconnect_returns_pins_to_safe_state` (reconnect and read the input wired to the output that was high).
  `test_stock_client.py`: with the stock client only — firmware version `3.2.100`; digital write 5 → callback on 4;
  PWM on 18; analog callback on 32; I2C read of 3 bytes at 0x76; sonar 100 cm; DHT 65.2/35.1; 1-wire reset presence,
  search ROM, crc8 of the ROM.
- [ ] **Step 2: Implement** the host server and `make host-server`, `make test-integration`.
- [ ] **Step 3: Verify** — `make test-integration` passes; `make lint` (ruff included).
- [ ] **Step 4: Commit** — `test: host server and integration tests with the stock client`.

### Task 10: Target drivers: GPIO, LEDC, ADC

**Files:** Create `src/drivers/esp/drv.c`, `drv_gpio.c`, `drv_ledc.c`, `drv_adc.c`, `src/drivers/esp/tmx_drv_esp.h`;
modify `CMakeLists.txt`

**Interfaces — Produces:** `extern const tmx_drv_t tmx_drv_esp;` (fields not yet implemented return
`ESP_ERR_NOT_SUPPORTED` until Task 11).
- GPIO: `gpio_config` once per mode change (input with pulls; output with the level set before the direction).
- LEDC: low-speed or the grant's speed mode; `ledc_timer_config`, `ledc_channel_config`, `ledc_set_duty` +
  `ledc_update_duty`, `ledc_stop` with idle level 0, timer freed with `deconfigure`.
- ADC: `adc_oneshot` per unit: the grant's handle when given, else created at init and deleted at deinit; channel
  configured once per channel with 12 dB attenuation and 12-bit width; never delete a handle the application owns.

- [ ] **Step 1: Implement** the files.
- [ ] **Step 2: Verify** — `make build-examples` builds the minimal `examples/devkit_wifi` (created here with an
  empty profile if Task 12 has not run) for `esp32`, `esp32s3`, `esp32c6`, `esp32p4`; `make lint`.
- [ ] **Step 3: Commit** — `feat(drivers): GPIO, LEDC and ADC on the IDF`.

### Task 11: Target drivers: I2C, 1-wire, pulse capture

**Files:** Create `src/drivers/esp/drv_i2c.c`, `drv_onewire.c`, `drv_pulse.c`; modify `idf_component.yml`
(`espressif/onewire_bus: "^1.0"`), `drv.c`

- I2C: new master driver; the grant's bus handle when given (never deleted), else `i2c_new_master_bus` on its port
  and pins; one device handle per address cached (8 per bus), created at the grant's speed; every transfer with the
  timeout argument; `stop == false` → `i2c_master_transmit_receive`. A NACK → `ESP_FAIL`.
- 1-wire: `onewire_bus` with one RMT TX + RX pair claimed from the grant (`tmx_claim_rmt_rx`); search through
  `onewire_device_iter`; a bad-CRC search result counts as no device and is logged once.
- DHT: one RMT RX channel (from the grant) created on first use, 1 MHz resolution; start pulse 1 ms low by GPIO, then
  receive; the RMT receive-done callback decodes the last 40 high pulses and posts the completion; no frame within
  10 ms → `ESP_ERR_TIMEOUT` completion from a one-shot `esp_timer`.
- Sonar: trigger 10 µs high by GPIO; echo edges by a GPIO any-edge ISR with `esp_timer_get_time` timestamps; the
  falling edge posts the completion; no echo within 25 ms → timeout completion from a one-shot `esp_timer`. The GPIO
  ISR service is installed only if not already installed (`ESP_ERR_INVALID_STATE` accepted), with flags 0.

- [ ] **Step 1: Implement** the files.
- [ ] **Step 2: Verify** — `make build-examples` for all four chips; `make lint`.
- [ ] **Step 3: Commit** — `feat(drivers): I2C with timeout, 1-wire, asynchronous DHT and sonar`.

### Task 12: Board examples and P4 compile check

**Files:** Create `examples/esp32_poe/` (Ethernet LAN8720 example, `sdkconfig.defaults.wifi` variant, `targets` file),
`examples/devkit_wifi/` (S3 and C6), `examples/p4_build_check/`; modify `Makefile` (`build-examples` builds: esp32_poe
eth for `esp32`, esp32_poe wifi for `esp32`, devkit_wifi for `esp32s3` and `esp32c6`, p4_build_check for `esp32p4`)

- ESP32-POE(-ISO): LAN8720 RMII (MDC 23, MDIO 18, clock out GPIO 17, PHY power GPIO 12, RMII 19, 21, 22, 25, 26, 27)
  never in the profile. Profile pins: the free GPIOs on the Olimex ESP32-POE-ISO's EXT1/EXT2/UEXT headers per its
  schematic (revision recorded in a comment), minus flash pins 6–11, input-only 34–39 as DIN only (no pulls); ADC only
  on ADC1; I2C bus 0 "client chooses" pins; 1-wire bus 0 "client chooses"; LEDC grant all low-speed channels and
  timers; RMT grant 1 TX + 2 RX; safe states FLOAT. The Wi-Fi variant gets the same profile. Wi-Fi credentials from
  menuconfig.
- S3 DevKitC: pins 4, 5, 12, 13, 14, 16, 17, 18, 21 (the stock client's ESP32-classic lists accept them); ADC1 on 4,
  5, 12, 13, 14; same grant style. The C6 build reuses the example with a C6 profile (no pin above 23, ADC1 only).
- `p4_build_check`: a profile on a few P4 GPIOs, no network; only proves it builds.

- [ ] **Step 1: Implement** the examples.
- [ ] **Step 2: Verify** — `make build-examples` builds all five configurations; `make lint`.
- [ ] **Step 3: Commit** — `feat: board examples for ESP32-POE, S3, C6 and a P4 build check`.

### Task 13: Recording proxy, compatibility script, README

**Files:** Create `tools/compat/recording_proxy.py`, `known_reports.py`, `compat_test.py`; `tools/tests/
test_recording_proxy.py`; modify `README.md`, `CHANGELOG.md`

**Interfaces:**
- `known_reports.py`: the set of report ids the stock client dispatches and, per id, its valid lengths (from
  `telemetrix_esp32.py` report dispatch and the layouts in Global Constraints).
- `recording_proxy.py --listen PORT --target HOST:PORT --log FILE [--strict]`: forwards both ways, frames both
  directions, writes one JSON line per packet (`dir`, `t`, `id`, `bytes` hex); `--strict` exits non-zero at the end if a
  server→client packet has an id or length not in `known_reports.py`.
- `compat_test.py --host H [--port P] --loopback OUT,IN [--i2c-addr A] [--dht PIN] [--sonar TRIG,ECHO] [--onewire PIN]`:
  drives the stock client through the proxy; `--compare A.log B.log` normalises analog values, DHT/sonar values,
  timestamps and the firmware build byte, then diffs the packet sequences.

- [ ] **Step 1: Write the failing test** `test_recording_proxy.py`: `test_strict_flags_unknown_report_id` (a fake
  server sends id 12), `test_strict_accepts_every_known_layout`, `test_compare_ignores_normalised_fields`.
- [ ] **Step 2–4:** run, fail; implement; run, pass (`make test-integration` runs it too, through the host server).
- [ ] **Step 5: README** — quick start, profile and grant reference with every default from Global Constraints, the
  stock protocol table (in scope, ignored), deviations, upstream bugs found (spec section 8), security note (no
  authentication; use the allowed list and the access hook), socket and memory budget.
- [ ] **Step 6: Verify** — `make lint`; every byte layout in the README matches Global Constraints (read side by side).
- [ ] **Step 7: Commit** — `feat(tools): recording proxy and compatibility script; README`.

## Spare-board pass (batched, after review and fixes; not part of any task)

Board: `olimex-poe-iso/poe-iso-a` in the clerk (lease it first; registration pending the clerk 0.6.2 fix). Loopback
wire between two free EXT pins chosen in Task 12.

1. Wi-Fi variant: flash upstream Telemetrix4Esp32 3.2.0; `compat_test.py --log upstream.log` through the proxy; flash
   ours; `--log ours.log`; `--compare` shows no differences beyond the normalised fields; `--strict` passes on ours.
2. Ethernet example: `compat_test.py` again.
3. DHT22, HC-SR04, DS18B20 and an I2C sensor if wired; otherwise note them as unchecked.
4. Hostile basics: a second connection gets RST; a client that stops reading is dropped after 10 s.
5. Pull the Ethernet cable mid-session with an output high: within 16 s the pin is in its safe state and a new client
   connects.
6. Ask the user whether to create `t3hk0d3/telemetrix-esp-idf` on GitHub and push.
