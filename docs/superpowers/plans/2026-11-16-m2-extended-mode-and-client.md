# M2: extended mode and our client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The server speaks an extended mode, entered through a safe handshake: numbered commands, error reports tied
to the command that caused them, a sync barrier with abort, bus selection, discovery and a plugin envelope with a
public plugin API. Our asyncio Python client drives it with an async chaining API and falls back to a stock mode for
upstream Telemetrix4Esp32 3.2 firmware.

**Architecture:** The M1 layers stay. A new adapter module, `session.c`, owns the session mode, the handshake, the
command counter, error reports, `SYNC` with abort and skip, `ON_BUS` and the `PLUGIN` dispatch. It sits in front of
the existing stock dispatch. Feature functions return a refusal reason instead of `bool`, so the stock layer keeps its
silent refusals while extended mode reports them. Discovery records are encoded in `proto` and built from the
profile, claims and plugin registry in the adapter. The plugin registry lives in `core`. The public plugin API
(`include/telemetrix_plugin.h`) is implemented in the adapter, plus a thread-safe emit ring in `net`. The net layer
switches the frame assembler to the extended cap and the strict policy, and closes violators with RST. The client is a
pure-asyncio package in `client/`: wire codec, connection with reader and dispatch tasks, handshake, discovery,
chains, stock mode and plugin halves.

**Tech Stack:**
- Server: ESP-IDF v6.1 (`esp32`, `esp32s3`, `esp32c6`, `esp32p4`, `linux`), Unity host tests, clang libFuzzer.
- Client: Python ≥ 3.10, no runtime dependencies. Packaged with hatchling; tested with pytest and pytest-asyncio; linted
  with ruff.
- Stock-mode fixtures: `telemetrix-esp32` 2.1.1 (already pinned in `tools/compat/requirements.txt`).

**Spec:** `docs/superpowers/specs/2026-11-12-telemetrix-esp-idf-design.md`, revision 3 plus M4 (approved
2026-11-16). This plan implements:
- section 4.4 (extended mode and handshake);
- 4.2 (named capabilities, hooks for plugin sockets excepted);
- 4.3 (extended framing policy, plugin time budget);
- 4.6 (plugin API without side-port sockets);
- 4.7 (tests for all of the above);
- section 5 (the client), and section 6 (M2).

Out of M2 and left to M3: streams, side-port sockets in the plugin API, and the client stream API.

**Where:** `t3hk0d3/telemetrix-esp-idf`, branch `feat/m2-extended-mode` cut from `main` (0230f25). Merged by PR at
the end, with the user's yes.

## Styleguide

`STYLEGUIDE.md` (written in M1) governs every task. Task 1 extends it for `client/`, in the same file:

- **Python layout:** `client/` is its own package: `client/pyproject.toml`, sources in
  `client/src/telemetrix_idf/`, tests in `client/tests/unit/` and `client/tests/integration/`.
- **Python style:** ruff format and ruff check with `select = ["E", "F", "I", "B", "UP", "ASYNC"]`, line length 120.
  Type hints on every public function. Public names are listed in `__all__`.
- **Python test names:** files are `test_<topic>.py` and functions are `test_<scenario>_<effect>`, one test per
  effect of the BDD skeleton. Async tests use `pytest.mark.asyncio`.
- **Errors:** every exception the client raises derives from `telemetrix_idf.TelemetrixError`; never a bare
  `Exception`.
- **Make targets** (each with a `## ` help line): `make client-lint`, `make client-test` (unit),
  `make client-integration` (against the host server). `make lint` also runs the client's ruff through pre-commit.

## Global Constraints

Everything in M1's Global Constraints still holds for stock mode. Stock-mode bytes do not change. On top of that:

**Wire values.** They are fixed here, listed in the README, and confirmed in the courtesy check.

- **Handshake (spec 4.4).**
  - Magic byte `0xA7`. Extension-protocol version 1.0 (`TMX_EXT_MAJOR 1`, `TMX_EXT_MINOR 0`).
  - `GET_FIRMWARE_VERSION` with a payload of exactly `[0xA7, major, minor]` gets the long answer
    `[7,5,3,2,100,0xA7,1,0]`: the stock three version bytes, the magic, and the server's own version.
  - Any other payload (empty, another length, another first byte) gets the stock `[4,5,3,2,100]`.
  - Same major: the session enters extended mode at the lower minor. The client computes the lower minor; the server
    stores it.
  - Different major: the long answer, and the session stays in stock mode.
  - In extended mode, `GET_FIRMWARE_VERSION` always gets the long answer.
  - With `cfg.extended` false or `CONFIG_TELEMETRIX_EXTENDED` off, the stock answer is sent always.
- **Command numbering.**
  - A 16-bit counter (`seq`) starts at 0 with the first command after the handshake reply and wraps at 65536.
  - The handshake's own `GET_FIRMWARE_VERSION` is not numbered.
  - Every later packet is numbered, `SYNC` included.
- **New command ids:**
  - `57` SYNC `[tag_hi, tag_lo, flags]`
  - `58` ON_BUS `[bus, stock_id, stock payload…]`
  - `59` GET_CAPABILITIES `[page]`
  - `60` PLUGIN `[slot, sub, args…]`

  Extended-mode payload cap is 254.
- **New report ids:**
  - `57` ERROR `[seq_hi, seq_lo, cmd, reason]`. It is followed by `[slot, sub]` when `cmd` is 60, then by `[code]` when
    `reason` is PLUGIN.
  - `58` SYNC_REPLY `[tag_hi, tag_lo, seq_hi, seq_lo]`, where `seq` is the SYNC's own number.
  - `59` BUS_REPORT `[bus, stock_id, stock payload…]`: the stock report minus its length byte.
  - `60` CAPABILITIES `[page, last_page, records…]`
  - `61` PLUGIN_REPORT `[slot, sub, data…]`

  Reports are at most 255 bytes on the wire, length byte included.
- **Reasons** (public as `telemetrix_reason_t`):

  | Code | Reason |
  |---|---|
  | `0` | NONE |
  | `1` | NO_SUCH_PIN |
  | `2` | NO_CAPABILITY |
  | `3` | PIN_BUSY |
  | `4` | EXHAUSTED |
  | `5` | BAD_ARGUMENT |
  | `6` | BUS_UNKNOWN |
  | `7` | NOT_IN_SCOPE |
  | `8` | HARDWARE |
  | `9` | NO_ANSWER |
  | `10` | PLUGIN |
  | `11` | SKIPPED |
  | `12` | NO_SUCH_SLOT |

**Extended-mode behaviour.**

- **Framing policy.**
  - These are violations: `len` 0; a packet unfinished after 2000 ms; an id outside 0–60.
  - A violation closes the client with RST (`SO_LINGER` {1,0}), increments `protocol_violations`, and runs the usual
    release.
  - Ids 0–56 are known: the out-of-scope ones get an error with `NOT_IN_SCOPE`.
  - A short payload gets an error with `BAD_ARGUMENT`; the connection stays.
- **Errors.**
  - Every refused command produces exactly one ERROR report, queued as a reply (never dropped) before the next command
    is handled.
  - In extended mode the ERROR replaces upstream's refusal reports: servo unavailable becomes ERROR `EXHAUSTED`, and
    the I2C NACK or short read becomes ERROR `NO_ANSWER`. Stock mode keeps upstream's reports.
  - DHT error reports and sonar timeouts are not command refusals and stay stock reports in both modes.
- **SYNC flags** (spec clarification, Ruling R1 below).
  - Bit 0, "abort on error", applies to the commands after that SYNC, up to the next SYNC. The session starts with
    abort on.
  - While abort is on and a command is refused, every later command up to the next SYNC is skipped: not executed, and
    ERROR `SKIPPED` with its own `seq`. A SYNC is never skipped.
  - SYNC_REPLY is queued after every reply and error of earlier commands.
- **ON_BUS.**
  - `bus` is the profile bus number. Inner ids allowed: 9–11 (I2C) and 27–35 (1-wire); any other inner id gets
    `BAD_ARGUMENT`. A bus number not in the profile gets `BUS_UNKNOWN`.
  - The inner command's reply comes back as BUS_REPORT; its refusal as ERROR with `cmd` 58.
  - Stock commands sent directly act on bus 0 in both modes. Stock clamps stay: I2C read at most 59 bytes.
- **Discovery.**
  - Records are `[type][len][value]`. They are never split across pages, and each page carries at most 251 bytes of
    records.
  - Order: CHIP, NAMED_CAP…, PIN… (profile order), BUS… (I2C then 1-wire, profile order), LIMIT…, PLUGIN… (slot
    order).
  - A page past `last_page` gets ERROR `BAD_ARGUMENT`.
  - Signed bytes are two's complement; multi-byte values are big-endian. Values per type:

    | Type | Record | Value |
    |---|---|---|
    | 1 | CHIP | `[model u16][revision u16 (major×100+minor)][IDF version ASCII ≤ 32]` |
    | 2 | PIN | `[gpio][caps u16][safe][adc_unit i8][adc_channel i8][named cap ids…]` |
    | 3 | BUS | I2C `[0][bus][sda i8][scl i8][speed_hz u32]`; 1-wire `[1][bus][gpio i8]`; −1 = client chooses |
    | 4 | LIMIT | `[kind][count u16]` |
    | 5 | PLUGIN | `[slot][major][minor][name ASCII]` |
    | 6 | NAMED_CAP | `[id][name ASCII]` |

  - LIMIT kinds, computed from the grants:

    | Kind | Limit | Count |
    |---|---|---|
    | 1 | servos | min(`max_servo`, granted LEDC channels) |
    | 2 | PWM channels | granted LEDC channels |
    | 3 | PWM timers | granted LEDC timers |
    | 4 | DHT | `max_dht` when an RMT RX channel is granted, else 0 |
    | 5 | sonar | `max_sonar` |
    | 6 | analog interval floor, ms | `analog_interval_floor_ms` |
- **Plugins.**
  - Names match `^[a-z0-9_]+\.[a-z0-9_.]+$` and are at most 32 bytes. The `telemetrix.` prefix is reserved for
    built-ins.
  - At most `TELEMETRIX_PLUGINS_MAX` (8) plugins. A slot is the registration index (built-ins first; there are none in
    M2).
  - Named capabilities: at most 16 per server, names at most 24 bytes, ids in order of first declaration.
  - A profile pin naming a capability no registered plugin declares makes `telemetrix_start` return
    `ESP_ERR_INVALID_ARG`. So do a duplicate plugin name and a bad name.
  - Handler and poll calls are timed against `cfg.plugin_budget_us` (default 5000). An overrun is counted and never
    aborted.
  - Plugins can emit only while a client is in extended mode; otherwise `ESP_ERR_INVALID_STATE`.
  - Poll hooks run only during an extended session. Release hooks run on disconnect, RESET and stop, in slot order.
  - Async emit ring: 8 entries of up to 251 data bytes. When the ring is full the emit fails with `ESP_ERR_NO_MEM` and
    is counted. Entries from an earlier session are dropped.

**Client (spec 5).**

- **Package.** Distribution `telemetrix-idf-client`, import `telemetrix_idf`, AGPL-3.0-or-later, Python ≥ 3.10,
  asyncio only, no automatic reconnect.
- **Timeouts.** Handshake 3 s; every await 5 s by default.
- **Supported firmware:** this server (extended mode) and upstream Telemetrix4Esp32 3.2.x (stock mode).
  - Firmware major ≠ 3, or 3.0 and 3.1, raise `IncompatibleFirmware`. Checked against upstream tags: 3.0.0 and 3.1.0
    ignore `I2C_BEGIN` pins, fall through on pull-down mode 9, do not clamp I2C reads, and have no framing bounds.
  - An extension major ≠ 1 raises `IncompatibleFirmware`.
- **Stock mode:**
  - Never sends RESET, and `reset()` raises `NotSupported`.
  - Sends `ENABLE_ALL_REPORTS` on connect, and ignores reports for pins it has not configured.
  - Uses `LOOP [tag u8]` as the barrier.
  - Pin lists come from `telemetrix-esp32` 2.1.1's `valid_*_pins`, with the version recorded in the source.
  - Calls that need extended mode raise `NotSupported`.
  - Stock firmware hazards (Telemetrix4Esp32 3.2.0; state persists across sessions until a power cycle, since the
    client never sends RESET): a 1-wire op other than init and CRC-8 before an init null-derefs `OneWire *ow` and
    reboots, so it raises `NotSupported` until an init was sent this session; each init leaks a `new OneWire`, so a
    second init in a session raises `ValueError`; `sonars[]`/`dhts[]` (6 each) are written unbounded and their counts
    persist, so sonar/DHT raise `NotSupported` unless `connect(..., stock_sensors=True)`, with at most 6 of each per
    session (`ValueError` beyond, before writing), and the user power-cycles between sessions that create sensors.
- **Barrier tags.** 16-bit in extended mode, 8-bit in stock mode. A tag whose wait timed out or was cancelled stays
  reserved until its reply arrives or the connection drops.

## Rulings made while planning (open to the user's correction)

- **R1 — SYNC flag scope.** The spec says the abort flag on a SYNC governs the commands since the previous sync. A
  server cannot skip commands it has already executed, so the flag instead governs the commands after that SYNC, and
  the session starts with abort on.
  - The client tracks the mode the last SYNC it wrote left behind. A chain whose mode differs from it is prefixed with
    a SYNC carrying its mode; that SYNC's reply is consumed. The chain's closing SYNC repeats its mode.
  - Default chains (abort on) never pay for the extra SYNC.
- **R2 — Stock-mode firmware range.** Only 3.2.x is supported; the 3.x layouts are not identical (Global Constraints).
- **R3 — Side-port sockets.** The plugin API's side-port sockets (spec 4.6) move to M3 with streams, their only
  consumer.
- **R4 — Refusal reports in extended mode.** ERROR replaces upstream's refusal reports, so a refusal is reported
  exactly once.
- **R5 — Runtime stock-only switch.** A runtime `cfg.extended` (default true) sits next to `CONFIG_TELEMETRIX_EXTENDED`.
  An application can serve stock only, and the integration tests drive our client's stock mode against the host
  server.

## Review Focus

1. Two coroutines await chains at the same moment and the first chain's second command is refused. The error, and the
   skips after it, land on the first chain; the second chain completes. Task 12 test
   `test_concurrent_chains_error_lands_on_owner`.
2. Our client against stock firmware that is busy with another client: the TCP connect succeeds and nothing is
   answered. After 3 s it raises `HandshakeTimeout` and has sent nothing but the probe. Task 10 test
   `test_handshake_silent_server_times_out_after_probe_only`.
3. Mid extended session a client sends `len` 0, or stops halfway through a packet for over 2 s. The connection is
   reset, pins go to their safe states, `protocol_violations` is +1, and the next client is served. Tasks 2 and 8
   tests, and the hostile script in Task 13.
4. A plugin whose handler takes 50 ms: the overrun is counted in status, and the command still completes. A following
   `LOOP` gets its reply. Task 6 test `overrun: counted, command completes`.
5. A 64-pin profile with named capabilities spans several discovery pages. No record is split, and the client rebuilds
   all 64 pins with their names. Task 7 test and Task 10 test `test_discovery_multi_page_rebuilds_every_pin`.

## File map

- **`include/telemetrix.h`** (modify):
  - `telemetrix_reason_t`;
  - `telemetrix_pin_t` gains `const char *const *named; uint8_t n_named;`;
  - `telemetrix_config_t` gains `bool extended; uint32_t plugin_budget_us; const struct telemetrix_plugin *const *plugins;
    size_t n_plugins;`;
  - status gains `bool extended; uint32_t protocol_violations, error_reports; size_t n_plugins;
    telemetrix_plugin_status_t plugins[TELEMETRIX_PLUGINS_MAX];`.
- **`include/telemetrix_plugin.h`** (create): the public plugin API (Task 6).
- **`src/proto/`:**
  - `tmx_proto.h` (new ids, reasons, magic, `TMX_EXT_*`);
  - `frame.c` (mode switch, strict policy, halt);
  - `ext_reports.c` (create: ERROR, SYNC_REPLY, BUS_REPORT wrapper, PLUGIN_REPORT, long firmware answer);
  - `caps.c` (create: TLV record encoders and the page packer).
- **`src/core/`:**
  - `tmx_core.h`, `profile.c` (named capabilities, bus count limits);
  - `claims.c` (RMT TX counting; named-capability claims);
  - `plugins.c` (create: registry, name checks, named-capability table, per-plugin counters).
- **`src/adapter/`:**
  - `tmx_adapter.h`; `adapter.c`, `digital.c`, `analog.c`, `pwm.c`, `i2c.c`, `onewire.c`, `sensors.c` (reasons;
    per-bus state);
  - `session.c` (create: mode, handshake, `seq`, ERROR, SYNC/abort, ON_BUS, PLUGIN dispatch);
  - `discovery.c` (create: records from profile, grants and plugins);
  - `plugin_api.c` (create: `telemetrix_plugin_*` functions).
- **`src/drivers/`:** `tmx_drv.h` gains `chip_info` and `micros`; `esp/drv.c` and `fake/drv_fake.c` implement them.
  The fake adds `tmx_fake_set_micros_step`.
- **`src/net/`:**
  - `net.c` (frame mode on handshake, violation close, plugin poll in the select timeout, ring drain, status);
  - `emit_ring.c` (create: thread-safe async emit);
  - `tmx_net.h`; `api.c` (status copy).
- **`Kconfig`:** `TELEMETRIX_EXTENDED` (bool, default y).
- **`test_host/main/`:**
  - `test_frame.c`, `test_ext_reports.c`, `test_caps.c`, `test_plugins.c`, `test_adapter_reasons.c`,
    `test_adapter_buses.c`, `test_session.c`, `test_sync.c`, `test_discovery.c`, `test_plugin_api.c`,
    `test_emit_ring.c`, `test_server.c`;
  - `test_plugin.c`/`.h`: the test plugin `test.echo`, shared with the fuzzer and host server.
- **`test/fuzz/fuzz_session.c`:** extended mode after a scripted handshake.
- **`host/server/`:**
  - `main.c`: flags `--stock-only` and `--test-plugin`;
  - `host_profile.c`: I2C bus 1 on 2/15 with a device at 0x48; 1-wire bus 1 on 35; pin 36 granting `test.led`.
- **`client/`:**
  - `pyproject.toml`, `README.md`;
  - `src/telemetrix_idf/`: `__init__.py`, `errors.py`, `wire.py`, `discovery.py`, `connection.py`, `board.py`,
    `chain.py`, `stock.py`, `events.py`, `plugins.py`;
  - `tests/fake_stock_server.py`, `tests/unit/`, `tests/integration/` (with `conftest.py` starting the host server).
- **`tools/hostile/hostile_client.py`** and `tools/tests/test_hostile.py`.
- **Docs:** `README.md` (extended protocol tables, plugin API section, budget lines), `CHANGELOG.md`, `STYLEGUIDE.md`.

## BDD skeleton

```
tmx_frame (extended)
    tmx_frame_set_mode
        Called from inside the packet callback with cap 254, strict
            - the next packet may carry up to 254 payload bytes
            - bytes of the next packet already in the same feed call are parsed under the new cap
        On len 0 while strict
            - violation set, dropped +1, feed consumes nothing more
        On a packet unfinished after 2000 ms while strict
            - violation set at the tick
    tmx_frame_halt
        Called from the packet callback
            - feed returns without emitting the rest of the chunk; violation set
tmx_ext_reports
    each encoder
        On the example values of Global Constraints
            - exact bytes, ERROR with and without [slot, sub] and [code]
    tmx_rpt_bus_wrap
        On a stock I2C read report on bus 2
            - [len, 59, 2, 9, count, addr, reg, data…]
tmx_caps
    record encoders
        On each record type
            - exact [type][len][value] bytes
    tmx_caps_page
        On records totalling 600 bytes
            - pages 0..2, each ≤ 251 record bytes, no record split, last_page 2 in every page
        On a page past the last
            - returns false
tmx_profile (named capabilities)
    tmx_profile_validate_named
        On a pin naming a capability no plugin declares
            - ESP_ERR_INVALID_ARG
        On more than 4 I2C or 4 1-wire buses
            - ESP_ERR_INVALID_ARG
tmx_claims (M2 additions)
    tmx_claim_named
        On a pin whose profile entry grants the name
            - claimed for that plugin
        On a pin without the name, or claimed by another use
            - ESP_ERR_NOT_SUPPORTED / ESP_ERR_INVALID_STATE
    tmx_claim_rmt_tx
        On more TX claims than tx_channels
            - ESP_ERR_NOT_FOUND
tmx_plugins
    tmx_plugins_init
        On two plugins
            - slots 0 and 1 in registration order
        On a duplicate name, a bad name, a reserved telemetrix. name, more than 8 plugins
            - ESP_ERR_INVALID_ARG
        On named capabilities declared by two plugins
            - one id per distinct name, in first-declaration order
tmx_adapter (refusal reasons; stock behaviour unchanged)
    each feature function
        On a pin not in the profile / without the capability / busy / grant exhausted / bad value
            - returns NO_SUCH_PIN / NO_CAPABILITY / PIN_BUSY / EXHAUSTED / BAD_ARGUMENT
        On an I2C NACK
            - returns NO_ANSWER
    stock mode
        On every M1 refusal case
            - refused_commands +1 and the same reports as M1 (M1 tests unchanged and passing)
tmx_adapter (buses)
    I2C on bus n
        On begin, write, read on bus 1 while bus 0 is open
            - driver calls carry bus 1; bus 0 untouched
        On release
            - every open bus closed, shared handles untouched
    1-wire on bus n
        On search on bus 1 after a search on bus 0
            - each bus keeps its own search state
session (src/adapter/session.c)
    handshake
        On GET_FIRMWARE_VERSION [0xA7,1,0]
            - long answer [7,5,3,2,100,0xA7,1,0]; mode extended; next command seq 0
        On [0xA7,1,5] from a newer-minor client
            - extended at minor 0
        On [0xA7,2,0]
            - long answer; mode stays stock
        On an empty or other payload
            - stock answer [4,5,3,2,100]; mode stays stock
        On cfg.extended false
            - stock answer for every payload
        On a second GET_FIRMWARE_VERSION in extended mode
            - long answer, numbered
    numbering and errors
        On three commands, the second refused
            - ERROR [0,1,cmd,reason] once, queued as reply before the third command is handled
        On seq 65535 then one more command
            - the next command is seq 0
        On an out-of-scope id (touch mode, DAC, SPI, stepper)
            - ERROR NOT_IN_SCOPE, ignored_commands unchanged
        On a short payload
            - ERROR BAD_ARGUMENT, connection stays
        On id 61..255
            - violation requested, nothing executed
        On servo exhaustion / I2C NACK in extended mode
            - ERROR EXHAUSTED / NO_ANSWER and no [2,6,pin] / [3,8,1,addr]
    SYNC
        On SYNC [tag,0x01] with no refusal since the last SYNC
            - SYNC_REPLY [tag, own seq] after every earlier reply
        On abort on and command 2 of 4 refused
            - commands 3 and 4 not executed, ERROR SKIPPED for each with their seq, SYNC never skipped
        On SYNC flags 0 then a refused command
            - later commands still execute
        On a session just entered
            - abort is on
    ON_BUS
        On [1, 10, addr, reg, count, stop] with a device on bus 1
            - BUS_REPORT [1, 9, count, addr, reg, data…]
        On a bus not in the profile
            - ERROR BUS_UNKNOWN with cmd 58
        On an inner id outside 9-11, 27-35
            - ERROR BAD_ARGUMENT
        On ONE_WIRE_CRC8 from the stock client sent directly on bus 0
            - stock reply unchanged
    GET_CAPABILITIES
        On page 0
            - CAPABILITIES [0, last, records…] as a reply
        On a page past the last
            - ERROR BAD_ARGUMENT
    PLUGIN
        On a valid slot
            - the handler gets (sub, args); its NONE completes the command
        On an unknown slot
            - ERROR NO_SUCH_SLOT with [slot, sub]
        On a handler returning PLUGIN with code 7
            - ERROR [seq, 60, 10, slot, sub, 7]
    release (disconnect, RESET, stop)
        On release
            - plugin release hooks in slot order, mode back to stock, seq 0, abort on, buses closed
discovery (src/adapter/discovery.c)
    tmx_discovery_build
        On the host profile with the test plugin
            - CHIP from drv chip_info, NAMED_CAP "test.led", one PIN per profile pin, BUS for i2c 0/1 and 1-wire 0/1,
              LIMIT kinds 1-6 with grant-derived counts, PLUGIN slot 0 "test.echo" 1.0
        On a 64-pin profile with 4 named capabilities
            - more than one page, all 64 PIN records present once
plugin API (include/telemetrix_plugin.h)
    telemetrix_plugin_claim_pin
        On a granted named pin
            - ESP_OK; released to its safe state on session release
    telemetrix_plugin_emit
        On extended session
            - PLUGIN_REPORT [slot, sub, data] queued as reply
        In stock mode / no client
            - ESP_ERR_INVALID_STATE, nothing queued
        On data over 251 bytes
            - ESP_ERR_INVALID_ARG
    telemetrix_plugin_emit_async
        From another task during an extended session
            - delivered as PLUGIN_REPORT on the network task, network task woken
        With the ring full
            - ESP_ERR_NO_MEM, async_dropped +1
        Queued before a disconnect
            - dropped at drain (earlier session)
    budget
        On a handler taking 50 ms (fake micros)
            - overruns +1, max_us 50000, the command completes, the next LOOP is answered
    poll hook
        On a hook returning 20 ms
            - called again after 20 ms; select timeout never longer
        In stock mode
            - not called
tmx_emit_ring (src/net/emit_ring.c)
    put / take
        On 9 puts without a take
            - 8 stored in order, the 9th ESP_ERR_NO_MEM
        On a generation bump
            - older entries skipped at take
net (extended)
    On a handshake then 200 bytes of a 254-byte command in one recv
        - the command is assembled whole
    On a violation
        - client reset with RST, protocol_violations +1, pins to safe states, plugins released, next client served
    status
        - extended, protocol_violations, error_reports, per-plugin name, slot, commands, refusals, overruns, max_us,
          async_dropped
client: wire (telemetrix_idf.wire)
    encoders
        On each command
            - exact bytes of Global Constraints / M1 constraints
    PacketParser.feed
        On packets split at every byte boundary
            - the same packets out
    decode_error / decode_sync_reply / decode_capabilities / decode_bus_report
        On server fixtures
            - typed values
client: handshake (connection.py)
    On the long answer with major 1
        - mode "extended", protocol (1, min(client, server minor)), discovery fetched
    On the short answer [3,2,x]
        - mode "stock"; ENABLE_ALL_REPORTS sent
    On firmware 3.0/3.1 or major ≠ 3
        - IncompatibleFirmware, connection closed
    On extension major 2
        - IncompatibleFirmware
    On reports arriving before the answer
        - discarded
    On silence for 3 s
        - HandshakeTimeout; only the probe was written
client: discovery
    On three pages with an unknown record type
        - Board with every pin, bus, limit, plugin; unknown type skipped
client: chains (chain.py)
    building
        On board.pin(17).output().write(1)
            - nothing sent until awaited
        On a chain garbage-collected unawaited with commands
            - RuntimeWarning "chain never awaited"
    await (extended)
        On success
            - one write: commands then SYNC; returns the last value-returning op's value or None
        On a refusal with abort
            - ChainError with results [DONE, REFUSED(reason), SKIPPED…] by seq
        On chain.atomic(False) after an atomic chain
            - prefixed SYNC flags 0; later commands executed
        On two concurrent chains, the first refused
            - error on the first only; the second returns normally
        On a reply later than the timeout
            - asyncio.TimeoutError at 5 s; the late reply discarded; tag reused only after it
        On cancellation
            - CancelledError; tag reserved until the reply
        On I2C read in a chain
            - returns the bytes; with ON_BUS on bus 1 the BUS_REPORT is matched
    await (stock)
        On success
            - commands then LOOP [tag]; completes on [2,0,tag]
        On SERVO_ATTACH answered by servo unavailable
            - ChainError with that op REFUSED EXHAUSTED
        On a call needing extended mode
            - NotSupported, nothing written
        On reset()
            - NotSupported
client: events (events.py)
    On a digital report for a configured pin
        - callback and async iterator get (pin, value, timestamp)
    On a report for an unconfigured pin in stock mode
        - ignored
    On a callback that sleeps 1 s
        - barrier replies still complete; events delivered in order
    On a full dispatch queue (1000)
        - oldest event dropped, board.stats.dropped_events +1
client: plugins (plugins.py)
    On a half registered with the board's plugin of the same major
        - board.plugin(name) bound to the session slot
    On a major mismatch or a plugin the board lacks
        - board.plugin(name) raises IncompatiblePlugin / NotSupported
    On a PLUGIN_REPORT
        - the half's on_report(sub, data) called on the dispatch task
hostile client (tools/hostile)
    flood, garbage (stock and extended), stalled reader, connection storm
        - server keeps answering LOOP and handshakes; stalled client dropped; open fds of the server back to baseline
```

## Tasks

### Task 1: Branch, styleguide additions, extended wire constants, frame mode switch

**Files:**
- Modify: `STYLEGUIDE.md`, `src/proto/tmx_proto.h`, `src/proto/frame.c`, `test_host/main/test_frame.c`, `Makefile`
  (client targets print "not yet" until Task 9).
- Create: `src/proto/ext_reports.c`, `test_host/main/test_ext_reports.c`.

**Interfaces — Produces (`tmx_proto.h`):**
- Command ids `TMX_CMD_SYNC 57`, `TMX_CMD_ON_BUS 58`, `TMX_CMD_GET_CAPABILITIES 59`, `TMX_CMD_PLUGIN 60`;
  `TMX_CMD_EXT_LAST 60`.
- Report ids `TMX_RPT_ERROR 57`, `TMX_RPT_SYNC_REPLY 58`, `TMX_RPT_BUS_REPORT 59`, `TMX_RPT_CAPABILITIES 60`,
  `TMX_RPT_PLUGIN_REPORT 61`.
- `TMX_EXT_MAGIC 0xA7`, `TMX_EXT_MAJOR 1`, `TMX_EXT_MINOR 0`, `TMX_EXT_PAYLOAD_MAX 254`, `TMX_EXT_REPORT_MAX 256`.
- Frame:
  - `void tmx_frame_set_mode(tmx_frame_t *f, uint16_t payload_cap, bool strict);`: takes effect at the next length
    byte, and is callable from inside the packet callback.
  - `void tmx_frame_halt(tmx_frame_t *f);`
  - `tmx_frame_t` gains `bool strict, violation;`.
- Encoders (`out` holds `TMX_EXT_REPORT_MAX` bytes; each returns bytes written):
  - `size_t tmx_rpt_firmware_long(uint8_t *out, uint8_t major, uint8_t minor);`
  - `size_t tmx_rpt_error(uint8_t *out, uint16_t seq, uint8_t cmd, uint8_t reason, int16_t slot, uint8_t sub, int16_t
    code);` (`slot`/`code` −1 = absent)
  - `size_t tmx_rpt_sync_reply(uint8_t *out, uint16_t tag, uint16_t seq);`
  - `size_t tmx_rpt_bus_wrap(uint8_t *out, uint8_t bus, const uint8_t *stock_report, size_t n);` (input: a full stock
    report including its length byte)
  - `size_t tmx_rpt_plugin(uint8_t *out, uint8_t slot, uint8_t sub, const uint8_t *data, size_t n);` (0 when `n` >
    251)

**Reasons:** `telemetrix_reason_t` lands in Task 2 (public header). Here the encoders take `uint8_t reason`.

- [ ] **Step 1: Branch** — `git switch -c feat/m2-extended-mode`.
- [ ] **Step 2: Styleguide** — append the client rules of this plan's Styleguide section to `STYLEGUIDE.md`.
- [ ] **Step 3: Write the failing tests:**
  - every `tmx_frame` (extended) and `tmx_ext_reports` effect of the skeleton;
  - the strict-mode `len` 0 test asserts that a valid packet after the zero in the same chunk is not emitted.
- [ ] **Step 4: Run** `make test-host` — the new tests fail.
- [ ] **Step 5: Implement** the frame mode switch and the encoders.
- [ ] **Step 6: Run** `make test-host` and `make lint` — pass.
- [ ] **Step 7: Commit** — `feat(proto): extended ids, ext report encoders, frame mode switch`.

### Task 2: Refusal reasons through the adapter

**Files:**
- Modify: `include/telemetrix.h` (`telemetrix_reason_t`); `src/adapter/*.c`; `tmx_adapter.h`.
- Create: `test_host/main/test_adapter_reasons.c`.

**Interfaces — Produces:**
- `typedef enum { TELEMETRIX_REASON_NONE = 0, …_NO_SUCH_PIN = 1, …_NO_CAPABILITY, …_PIN_BUSY, …_EXHAUSTED,
  …_BAD_ARGUMENT, …_BUS_UNKNOWN, …_NOT_IN_SCOPE, …_HARDWARE, …_NO_ANSWER, …_PLUGIN, …_SKIPPED, …_NO_SUCH_SLOT = 12 }
  telemetrix_reason_t;` (exact values per Global Constraints).
- Every feature function of `tmx_adapter.h` that returned `bool` or `tmx_res_t` returns `telemetrix_reason_t`
  (NONE = done). `tmx_res_t` is removed: "servo unavailable" is `EXHAUSTED`.
- `telemetrix_reason_t tmx_adapter_command(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len, uint32_t
  now_ms);` is the stock dispatch, now returning the reason.
  - It does not emit upstream's refusal reports.
  - Stock mode's `tmx_adapter_packet` keeps its contract: it calls `tmx_adapter_command`, counts refusals, and emits
    `[2,6,pin]` and `[3,8,1,addr]` for `EXHAUSTED` on servo attach and `NO_ANSWER` on I2C read.
- Mapping rules:
  - Short payload: `BAD_ARGUMENT`.
  - Ignored ids (out of scope, switched off in Kconfig): `NOT_IN_SCOPE`, which stock mode counts as ignored.
  - Driver failure: `HARDWARE`.

- [ ] **Step 1: Write the failing tests** — one test per reason per feature, in `test_adapter_reasons.c`, calling
  `tmx_adapter_command`.
- [ ] **Step 2: Run** — fail to compile or fail.
- [ ] **Step 3: Implement**, a mechanical return-type change. Every M1 adapter test must pass unmodified (it proves
  stock behaviour is unchanged).
- [ ] **Step 4: Run** `make test-host` and `make fuzz` (60 s per target) — pass.
- [ ] **Step 5: Commit** — `refactor(adapter): refusal reasons instead of booleans`.

### Task 3: More than one I2C and 1-wire bus in the adapter

**Files:**
- Modify: `src/adapter/i2c.c`, `onewire.c`, `tmx_adapter.h`, `src/core/profile.c`, `src/drivers/fake/drv_fake.c` (only
  if per-bus 1-wire search state is missing).
- Create: `test_host/main/test_adapter_buses.c`.

**Interfaces — Produces:**
- `#define TMX_MAX_BUSES 4` per kind; profile validation refuses more.
- `tmx_i2c_t` and `tmx_onewire_t` become arrays of per-bus state indexed by the profile bus index.
- `telemetrix_reason_t tmx_i2c_command(tmx_adapter_t *a, uint8_t bus, uint8_t id, const uint8_t *p, size_t len);` and
  `telemetrix_reason_t tmx_onewire_command(tmx_adapter_t *a, uint8_t bus, uint8_t id, const uint8_t *p, size_t len);`
  - `bus` is the profile bus number; a bus not in the profile gets `BUS_UNKNOWN`.
  - Stock dispatch passes 0.
- `int16_t on_bus;` in `tmx_adapter_t`: the bus of the ON_BUS command being handled, −1 otherwise.
  `tmx_adapter_emit` wraps a reply with `tmx_rpt_bus_wrap` while `on_bus >= 0`.
- The `I2C_BEGIN` pin rules of M1 apply per bus, using that bus's grant.

- [ ] **Step 1: Write the failing tests** for the "tmx_adapter (buses)" branch and the `TMX_MAX_BUSES` validation.
- [ ] **Step 2–4:** run and see them fail; implement; run `make test-host` and see them pass. The M1 I2C and 1-wire
  tests stay unchanged.
- [ ] **Step 5: Commit** — `feat(adapter): I2C and 1-wire on any profile bus`.

### Task 4: Session: handshake, numbering, error reports, extended framing policy

**Files:**
- Create: `src/adapter/session.c`, `test_host/main/test_session.c`.
- Modify: `tmx_adapter.h`, `adapter.c`, `include/telemetrix.h` (`cfg.extended`), `src/core/cfg.c` (default true),
  `Kconfig` (`TELEMETRIX_EXTENDED`).

**Interfaces — Produces (`tmx_adapter.h`):**
- `tmx_adapter_t` gains `tmx_session_t session` with these fields:
  - `bool extended`
  - `uint8_t minor`
  - `uint16_t seq`
  - `bool abort`
  - `bool skipping`
  - `bool violation`
  - `uint32_t error_reports`
- `tmx_adapter_packet` becomes the session entry:
  - In stock mode it is the M1 path, except that `GET_FIRMWARE_VERSION` goes through the handshake rule.
  - In extended mode it numbers the packet, checks it, dispatches, and emits ERROR on refusal.
- `bool tmx_adapter_extended(const tmx_adapter_t *a);`
- `bool tmx_adapter_violation(const tmx_adapter_t *a);`: sticky until release.
- `bool tmx_adapter_mode_changed(tmx_adapter_t *a);`: true once after entering extended mode, for the net layer's
  `tmx_frame_set_mode`.
- `tmx_adapter_release` also resets the session (stock, seq 0, abort on, no violation).

- [ ] **Step 1: Write the failing tests** for the "handshake" and "numbering and errors" branches of `session`, and for
  `cfg.extended` false. Every M1 stock-mode test must still pass.
- [ ] **Step 2–4:** run, fail; implement; run `make test-host`, pass; also build `test_host/features_off` with
  `TELEMETRIX_EXTENDED` off and assert the stock answer to the probe there.
- [ ] **Step 5: Commit** — `feat(adapter): extended-mode handshake, command numbering and error reports`.

### Task 5: SYNC with abort, ON_BUS

**Files:**
- Modify: `src/adapter/session.c`.
- Create: `test_host/main/test_sync.c`.

**Interfaces:**
- Consumes: Task 3's per-bus functions and `on_bus`; Task 4's session.
- Produces: the SYNC and ON_BUS behaviour of Global Constraints.

Skip algorithm (pseudocode, network task, per numbered packet):

```
if id == SYNC:
    emit SYNC_REPLY(tag, seq); abort = flags & 1; skipping = false
elif skipping:
    emit ERROR(seq, id, SKIPPED [+ slot, sub when id == PLUGIN and payload has them])
else:
    reason = dispatch(id, payload)
    if reason != NONE:
        emit ERROR(seq, id, reason, …)
        if abort: skipping = true
seq += 1
```

- [ ] **Step 1: Write the failing tests** for the "SYNC" and "ON_BUS" branches, including SYNC_REPLY ordering after an
  I2C read reply in the same chunk, and the stock client's direct `ONE_WIRE_CRC8` in extended mode.
- [ ] **Step 2–4:** run, fail; implement; run `make test-host`, pass.
- [ ] **Step 5: Commit** — `feat(adapter): SYNC barrier with abort and skip; ON_BUS with BUS_REPORT`.

### Task 6: Plugin registry, public plugin API, test plugin

**Files:**
- Create: `include/telemetrix_plugin.h`, `src/core/plugins.c`, `src/adapter/plugin_api.c`, `src/net/emit_ring.c`,
  `test_host/main/test_plugins.c`, `test_plugin_api.c`, `test_emit_ring.c`, `test_plugin.c`, `test_plugin.h`.
- Modify: `include/telemetrix.h` (config and status fields; `telemetrix_pin_t.named`, `n_named`), `src/core/profile.c`, `src/core/claims.c` (named claims, RMT TX),
  `src/drivers/tmx_drv.h` (`micros`), the fake (`tmx_fake_set_micros_step`), `session.c` (PLUGIN dispatch, release
  hooks, poll).

**Interfaces — Produces (`include/telemetrix_plugin.h`; includes only `telemetrix.h`):**
- `typedef struct telemetrix_plugin_ctx telemetrix_plugin_ctx_t;` (opaque)
- The plugin descriptor:

  ```
  typedef struct telemetrix_plugin {
      const char *name;
      uint8_t major, minor;
      const char *const *named_caps;
      size_t n_named_caps;
      telemetrix_reason_t (*command)(telemetrix_plugin_ctx_t *ctx, uint8_t sub, const uint8_t *args, size_t len,
                                     uint8_t *code);
      uint32_t (*poll)(telemetrix_plugin_ctx_t *ctx, uint32_t now_ms);
      void (*release)(telemetrix_plugin_ctx_t *ctx);
      void *user;
  } telemetrix_plugin_t;
  ```

  - `poll`: NULL means none. It returns the ms until the next call; `UINT32_MAX` means none.
  - `code` is written when `command` returns `TELEMETRIX_REASON_PLUGIN`.
  - The plugin structs must outlive the server.
- `void *telemetrix_plugin_user(telemetrix_plugin_ctx_t *ctx);`
- `esp_err_t telemetrix_plugin_claim_pin(telemetrix_plugin_ctx_t *ctx, uint8_t gpio, const char *named_cap);`:
  `ESP_ERR_NOT_SUPPORTED` when not granted, `ESP_ERR_INVALID_STATE` when busy.
- `void telemetrix_plugin_release_pin(telemetrix_plugin_ctx_t *ctx, uint8_t gpio);`: puts the pin in its safe state.
- `esp_err_t telemetrix_plugin_claim_rmt(telemetrix_plugin_ctx_t *ctx, bool tx);` and `void
  telemetrix_plugin_release_rmt(telemetrix_plugin_ctx_t *ctx, bool tx);`
- `esp_err_t telemetrix_plugin_emit(telemetrix_plugin_ctx_t *ctx, uint8_t sub, const uint8_t *data, size_t n);`:
  network task only.
- `esp_err_t telemetrix_plugin_emit_async(telemetrix_plugin_ctx_t *ctx, uint8_t sub, const uint8_t *data, size_t n);`:
  any task, not an ISR. It waits at most 10 ms for the ring lock (`ESP_ERR_TIMEOUT`), and wakes the network task.
- `void telemetrix_plugin_wake(telemetrix_plugin_ctx_t *ctx);`: poll soon.
- `void telemetrix_plugin_set_status(telemetrix_plugin_ctx_t *ctx, const char *text);`: at most 47 characters, copied.
- `#define TELEMETRIX_PLUGINS_MAX 8`.
- `typedef struct { char name[33]; uint8_t slot; uint32_t commands, refusals, overruns, max_us, async_dropped; char
  text[48]; } telemetrix_plugin_status_t;`
- Config: `bool extended; uint32_t plugin_budget_us; const telemetrix_plugin_t *const *plugins; size_t n_plugins;`.
  Defaults: true, 5000, NULL, 0. `telemetrix_start` copies the pointer array.
- Internal:
  - `esp_err_t tmx_plugins_init(tmx_plugins_t *r, const telemetrix_plugin_t *const *list, size_t n);`
  - `tmx_emit_ring_put` / `tmx_emit_ring_take` / `tmx_emit_ring_bump`, with a session generation.
- `esp_err_t tmx_profile_validate_named(const telemetrix_profile_t *p, const char *const *names, size_t n);` checks
  that every pin's named capability is in `names`. An unknown name is `ESP_ERR_INVALID_ARG`, as is a name over 24
  bytes or more than 16 names.
- `uint64_t (*micros)(void);` in `tmx_drv_t`: `esp_timer_get_time` on chips, a controllable counter in the fake.
- The test plugin `test.echo` 1.0 declares `test.led`. Sub-commands:
  - `0`: echo args as PLUGIN_REPORT sub 0.
  - `1`: refuse with PLUGIN code 7.
  - `2 [req u16]`: emit_async `[req, 0x55]` from a FreeRTOS task.
  - `3 [gpio]`: claim the pin as `test.led` and drive it high.
  - `4`: busy 50 ms. On the fake, this advances micros by 50000.

  Its poll hook emits sub 9 every 1000 ms once sub 5 enables it.

- [ ] **Step 1: Write the failing tests** for `tmx_plugins`, `tmx_profile_validate_named`, the plugin API, `tmx_emit_ring` and the PLUGIN branch of
  `session`. The test plugin is the API's end-to-end user.
- [ ] **Step 2–4:** run, fail; implement; run `make test-host`, pass.
- [ ] **Step 5: Commit** — `feat: plugin registry, public plugin API and envelope`.

### Task 7: Discovery

**Files:**
- Create: `src/proto/caps.c`, `src/adapter/discovery.c`, `test_host/main/test_caps.c`, `test_discovery.c`.
- Modify: `src/drivers/tmx_drv.h` (`chip_info`), `src/drivers/esp/drv.c`, `src/drivers/fake/drv_fake.c`, `session.c` (GET_CAPABILITIES).

**Interfaces — Produces:**
- `void (*chip_info)(uint16_t *model, uint16_t *revision, char *idf, size_t idf_cap);` in `tmx_drv_t`:
  - On chips: `esp_chip_info` and `esp_get_idf_version`.
  - In the fake: model 0xFFFF, revision 0, `"linux"`.
- Record encoders in `tmx_proto.h`, each `size_t tmx_caps_<type>(uint8_t *out, …)` returning the record length:
  `tmx_caps_chip`, `tmx_caps_pin`, `tmx_caps_bus_i2c`, `tmx_caps_bus_onewire`, `tmx_caps_limit`, `tmx_caps_plugin`,
  `tmx_caps_named`.
- `typedef size_t (*tmx_caps_source_fn)(size_t i, uint8_t *rec, void *ctx);` yields record `i`, or 0 after the last.
  `bool tmx_caps_page(tmx_caps_source_fn src, void *ctx, uint8_t page, uint8_t *out, size_t *n);` writes the whole
  CAPABILITIES report for `page`; false past the last page.
  - Pages are computed by walking records from the start each call. The discovery buffer is static, 256 bytes.
- `size_t tmx_discovery_record(size_t i, uint8_t *rec, void *adapter);` is the source over profile, grants, named
  capabilities and plugins.
- Consumes: Task 6's plugin registry (slots, named-capability ids) and `telemetrix_pin_t.named`.

- [ ] **Step 1: Write the failing tests** for `tmx_caps` and `discovery`. Include a
  64-pin profile with 4 named capabilities; the test reassembles every page and checks each PIN record exactly once.
- [ ] **Step 2–4:** run, fail; implement; run `make test-host`, pass.
- [ ] **Step 5: Commit** — `feat: discovery records and pages`.

### Task 8: Network layer, status, fuzzing, host server

**Files:**
- Modify: `src/net/net.c`, `api.c`, `tmx_net.h`, `test_host/main/test_server.c`, `host/server/main/main.c`,
  `host_profile.c`, `test/fuzz/CMakeLists.txt`, `test/fuzz/fuzz_adapter.c`, `Makefile` (`FUZZ_TARGETS` adds
  `fuzz_session`), `README.md` (wire tables).
- Create: `test/fuzz/fuzz_session.c`.

**Interfaces:**
- Consumes: Tasks 1–7.
- Produces: the "net (extended)" behaviour and the status fields of the file map.
- The network task:
  - after each packet, calls `tmx_frame_set_mode(&frame, TMX_EXT_PAYLOAD_MAX, true)` when
    `tmx_adapter_mode_changed`;
  - after each packet, calls `tmx_frame_halt` when `tmx_adapter_violation`;
  - after the feed, refuses the client when `frame.violation` or the adapter violation is set;
  - drains the emit ring each loop;
  - takes the minimum of the plugin poll deadlines into the select timeout;
  - resets the frame to stock mode on release.
- Host server:
  - `--stock-only` sets `cfg.extended` false;
  - `--test-plugin` registers `test.echo`;
  - the profile adds I2C bus 1 (sda 2, scl 15, a device at 0x48 with the same register pattern), 1-wire bus 1 on gpio
    35 with one ROM, and gpio 36 with `named = {"test.led"}`.
- `fuzz_session` replays the handshake, then feeds fuzzer bytes and random completions into an adapter with the test
  plugin.

- [ ] **Step 1: Write the failing tests** in `test_server.c` for the "net (extended)" branch, including a scripted
  `len` 0 in extended mode and the 254-byte command split across recv calls.
- [ ] **Step 2–4:** run, fail; implement; run `make test-host`, pass.
- [ ] **Step 5: Fuzz** — `make fuzz` (60 s per target, three targets): no crash, no sanitizer report.
- [ ] **Step 6: Verify** — `make test-integration` (M1's stock-client suite) still passes; `make build-examples`
  builds all five with no new warnings; `make lint`.
- [ ] **Step 7: README** — new ids, reasons, record layouts, SYNC flag semantics (R1), the extended framing policy,
  and the plugin API with the budget and emit rules.
- [ ] **Step 8: Commit** — `feat(net): extended sessions, violation close, plugin polling and status`.

### Task 9: Client package, wire codec

**Files:**
- Create: `client/pyproject.toml`, `client/README.md` (stub), `client/src/telemetrix_idf/{__init__.py,errors.py,
  wire.py}`, `client/tests/unit/test_wire.py`.
- Modify: `Makefile` (client targets with uv venv `client/.venv`), `.pre-commit-config.yaml` (ruff on `client/`).

**Interfaces — Produces:**
- `errors.py`:
  - `TelemetrixError`
  - `HandshakeTimeout`
  - `IncompatibleFirmware`
  - `NotSupported`
  - `ConnectionLost`
  - `ChainError(results: list[CommandResult])`
  - `IncompatiblePlugin`
- `wire.py`:
  - `CMD` and `RPT` int enums with every id;
  - `Reason` int enum;
  - `MAGIC = 0xA7`, `EXT_VERSION = (1, 0)`;
  - `encode(cmd: int, payload: bytes) -> bytes`, plus one typed builder per command (`set_pin_mode_output(pin)`,
    `i2c_read(addr, reg, n, stop)`, `sync(tag, abort)`, `on_bus(bus, inner: bytes)`, `plugin(slot, sub, args)` and
    the rest);
  - `class PacketParser: def feed(self, data: bytes) -> list[tuple[int, bytes]]`;
  - decoders `decode_error`, `decode_sync_reply`, `decode_bus_report`, `decode_capabilities`, `decode_firmware`
    (returns `(major, minor, build, ext: tuple[int, int] | None)`), returning dataclasses.
- Fixtures: the byte strings of this plan's Global Constraints and M1's.

- [ ] **Step 1: Write the failing tests** for "client: wire".
- [ ] **Step 2–4:** run `make client-test`, fail; implement; pass.
- [ ] **Step 5:** run `make client-lint`.
- [ ] **Step 6: Commit** — `feat(client): package skeleton and wire codec`.

### Task 10: Client connection, handshake, discovery, board model

**Files:**
- Create: `client/src/telemetrix_idf/{connection.py,discovery.py,board.py,stock.py}`,
  `client/tests/fake_stock_server.py`, `client/tests/unit/test_handshake.py`, `test_discovery.py`.

**Interfaces — Produces:**
- `async def connect(host: str, port: int = 31336, *, handshake_timeout: float = 3.0, timeout: float = 5.0, plugins:
  Sequence[type[PluginHalf]] = ()) -> Board` (also usable as `async with connect(...) as board`).
- `Board`:
  - `mode: Literal["extended", "stock"]`, `firmware: tuple[int, int, int]`, `protocol: tuple[int, int] | None`;
  - `chip: ChipInfo | None`, `pins: Mapping[int, PinInfo]`, `buses: list[BusInfo]`, `limits: Mapping[str, int]`,
    `plugins_available: Mapping[str, PluginInfo]`;
  - `stats: Stats`, `async close()`.
- `Connection`:
  - owns the `StreamReader` and `StreamWriter`, a reader task, and `write(data: bytes) -> None` (synchronous, a single
    `writer.write`);
  - routes every report to the chain matcher (Task 11) or the event dispatcher (Task 11);
  - on EOF or error, every pending await fails with `ConnectionLost`.
- `stock.py`:
  - `STOCK_PINS` (input, output, analog, servo, touch lists from `telemetrix-esp32` 2.1.1, with the version in a
    comment);
  - `check_stock_firmware(major, minor)`, which raises `IncompatibleFirmware` unless major 3 and minor ≥ 2.
- `fake_stock_server.py`: an asyncio server that speaks upstream 3.2.0 stock mode (from the sketch and the client's
  constants). Scriptable:
  - firmware bytes;
  - silence;
  - pre-handshake reports;
  - servo unavailable after N attaches;
  - I2C NACK addresses.

- [ ] **Step 1: Write the failing tests** for "client: handshake" and "client: discovery": against the fake stock
  server, and against canned extended-mode byte scripts for the long answer and discovery pages. Include
  `test_handshake_silent_server_times_out_after_probe_only` and `test_discovery_multi_page_rebuilds_every_pin`.
- [ ] **Step 2–4:** run, fail; implement; pass.
- [ ] **Step 5: Commit** — `feat(client): connection, handshake, discovery and board model`.

### Task 11: Client chains and events

**Files:**
- Create: `client/src/telemetrix_idf/{chain.py,events.py}`, `client/tests/unit/test_chain.py`, `test_events.py`.
- Modify: `board.py`.

**Interfaces — Produces:**
- `board.chain() -> Chain`, plus shortcuts that start a chain: `board.pin(gpio)`, `board.i2c(bus=0)`,
  `board.onewire(bus=0)`.
- `Chain` methods, each returning the same `Chain`:
  - pins: `pin(gpio)`, then `input(pull: Literal["up", "down"] | None = None, report: bool = True)`, `output()`,
    `write(level)`, `analog(differential: int = 0, report: bool = True)`, `pwm(freq_hz: float, resolution: int)`,
    `duty(value)`, `servo(min_us=1000, max_us=2000)`, `angle(deg)`, `detach()`, `report(on: bool)`;
  - I2C: `i2c(bus)`, then `begin(sda=None, scl=None)`, `write(addr, data)`, `read(addr, reg, n, stop=True)`, which
    yields bytes;
  - 1-wire: `onewire(bus)`, then `init(pin=None)`, `reset()` (yields bool), `select(rom)`, `skip()`,
    `write(byte, power=False)`, `read()` (yields int), `reset_search()`, `search()` (yields `bytes | None`),
    `crc8(data)` (yields int);
  - sensors and reporting: `sonar(trig, echo)`, `dht(pin)`, `reporting(enabled)`, `analog_interval(ms)`;
  - board RESET: `reset()` on a chain without a target (`board.chain().reset()`); after `pin()` or `i2c()` it raises
    `ValueError`;
  - `atomic(flag: bool = True)`.
  - On a bus other than 0, I2C and 1-wire ops are sent through ON_BUS.
- Awaiting the chain sends it:
  - It returns the value of the last op that yields one, or None.
  - `await chain.run()` returns `ChainResult(values: list, results: list[CommandResult])`.
  - Each `CommandResult` is `(op: str, status: Literal["done", "refused", "skipped"], reason: Reason | None)`.
- `board.onewire(bus).search_all() -> list[bytes]` loops `search` chains until no ROM is left.

Matching (pseudocode):

```
send(chain):
    if extended and chain.atomic != last_sync_mode: prepend SYNC(tag_x, chain.atomic), mark tag_x "consume"
    assign seq numbers to the chain's commands in write order (client-side counter mirrors the server's)
    append SYNC(tag, chain.atomic); last_sync_mode = chain.atomic
    register expectations: per reply kind (I2C read on bus b, 1-wire op on bus b, capabilities page) a FIFO of ops
    write all bytes in one call
on ERROR(seq): find the op by seq; mark refused/skipped; drop its reply expectation
on reply of kind k: pop the head of FIFO k, store the value
on SYNC_REPLY(tag) / LOOP reply(tag): complete the chain owning tag; raise ChainError if any op refused or skipped
```

- Events (`events.py`):
  - `board.on_digital(pin, cb)`, `on_analog`, `on_sonar(trig, cb)`, `on_dht(pin, cb)`, each returning an unsubscribe
    callable;
  - `board.digital_changes(pin)` and the other three, async iterators;
  - callbacks may be sync or async and run on a dispatch task with its own queue (1000 events; when full, the oldest
    is dropped and counted).
- The never-awaited warning is a `RuntimeWarning` emitted from `Chain.__del__` when unsent ops exist.

- [ ] **Step 1: Write the failing tests** for "client: chains" (extended cases use a scripted in-process fake of our
  server built from `wire.py`; stock cases use the fake stock server) and "client: events". Include
  `test_concurrent_chains_error_lands_on_owner`.
- [ ] **Step 2–4:** run, fail; implement; pass.
- [ ] **Step 5: Commit** — `feat(client): async chaining API and events`.

### Task 12: Client plugin halves, stock-mode rules, integration tests against the host server

**Files:**
- Create: `client/src/telemetrix_idf/plugins.py`, `client/tests/unit/test_plugins.py`, `test_stock.py`,
  `client/tests/integration/{conftest.py,test_extended.py,test_stock_mode.py,test_plugin.py}`.

**Interfaces — Produces:**
- `class PluginHalf`:
  - class attributes `name: str` and `major: int`;
  - the constructor takes `(board, slot)`;
  - `command(sub: int, args: bytes = b"") -> Chain`;
  - `on_report(sub: int, data: bytes) -> None` (override);
  - `request(sub: int, args: bytes) -> Awaitable[bytes]`: by the request-id convention, the first two bytes of args
    and of the matching report's data are a u16 request id the half chooses.
- `board.plugin(name) -> PluginHalf`. It raises `NotSupported` when the board lacks the plugin or is in stock mode,
  and `IncompatiblePlugin` on a major mismatch.
- `conftest.py` builds the host server once (`make host-server`) and starts one per test on a free port: with
  `--test-plugin` for extended tests, `--stock-only` for stock tests.

- [ ] **Step 1: Write the tests.**
  - Unit tests for "client: plugins" and the stock rules: no RESET written, `ENABLE_ALL_REPORTS` on connect,
    extended-only calls raise, unconfigured-pin reports ignored.
  - Integration tests against the host server:
    - handshake and discovery: every host profile pin, buses 0 and 1, `test.echo` 1.0;
    - a digital loopback on 5→4 through a chain and an event;
    - I2C on bus 0 and bus 1 (`read` from 0x48 on bus 1 via ON_BUS);
    - 1-wire search on bus 1;
    - `test_concurrent_chains_error_lands_on_owner`;
    - abort and `atomic(False)`;
    - a timeout with a late reply (test plugin sub 4 with the server's real clock: a 50 ms busy handler and a client
      timeout of 0.01 s);
    - cancellation;
    - a slow callback that does not delay barriers;
    - plugin echo, refusal code 7 surfaced in `ChainError`, the async request sub 2, the named-pin claim sub 3;
    - our client in stock mode against `--stock-only`: digital, PWM, analog, I2C, DHT, sonar, 1-wire, `reset()`
      raising.
- [ ] **Step 2–4:** run `make client-test client-integration`, fail; implement; pass.
- [ ] **Step 5: Commit** — `feat(client): plugin halves, stock-mode rules, integration tests`.

### Task 13: Hostile-client script, docs, final verification

**Files:**
- Create: `tools/hostile/hostile_client.py`, `tools/tests/test_hostile.py`.
- Modify: `client/README.md`, `README.md`, `CHANGELOG.md`.

**Interfaces:**
- `hostile_client.py --host H --port P --mode {flood,garbage,ext-garbage,stall,storm,all} [--seconds N]`. After each
  mode it checks that a fresh connection still gets the handshake answer and a LOOP reply. It exits non-zero
  otherwise.
- `test_hostile.py` runs `--mode all` against the host server and compares `/proc/<pid>/fd` counts before and after:
  they must be equal after a 1 s settle.

- [ ] **Step 1: Write** `test_hostile.py` (it fails without the script); write the script; run
  `make test-integration`, pass.
- [ ] **Step 2: Docs.**
  - `client/README.md`: install; connect; chains; errors and the abort rule; timeouts and cancellation semantics;
    events; stock mode limits (no RESET, pins not released by stock firmware, silent refusals except servo and I2C);
    plugins.
  - `CHANGELOG.md`: an M2 entry.
- [ ] **Step 3: Verify everything.** `make lint`, `make test-host`, `make fuzz`, `make test-integration`,
  `make client-test`, `make client-integration` and `make build-examples` all pass, with every output kept in
  `$SCRATCH/*.log`.
- [ ] **Step 4: Commit** — `test: hostile-client script; docs for extended mode and the client`.

## Spare-board pass (batched, after review and fixes; not part of any task)

Board: `olimex-poe-iso/poe-iso-a` (register it first; the OliZig's hub port is still needed to repoint `olizig-a`).
Lease it through the clerk. The S3 pass waits until an S3 is registered.

1. Ethernet example, our client in extended mode:
   - handshake and discovery match the profile;
   - digital loopback 32→33;
   - PWM;
   - I2C at SDA 13 / SCL 16 if a sensor is wired;
   - a refused chain shows the right per-command results.
2. `hostile_client.py --mode all` against the board: it stays up, and the stall drop takes about 10 s.
3. Wi-Fi variant: upstream Telemetrix4Esp32 3.2.0 flashed, and our client in stock mode does digital, PWM, analog and
   `reset()` raising NotSupported. Then our firmware again.
4. The M1 pass items still unchecked (listed in the M1 ledger archive) are run in the same session.
