# telemetrix-idf-client

Asyncio client (import `telemetrix_idf`) for the telemetrix-esp-idf server. It speaks the extended protocol with this
repository's server and falls back to stock mode against upstream Telemetrix4Esp32 3.2.x. Python 3.10 or later,
asyncio only, no automatic reconnect. AGPL-3.0-or-later.

## Install

The package is not on PyPI. From a checkout of this repository:

```sh
pip install ./client            # or: uv pip install ./client
pip install -e "./client[dev]"  # with pytest and ruff, for development
```

## Connect

```python
import asyncio
from telemetrix_idf import connect


async def main() -> None:
    async with connect("192.168.1.50", 3333) as board:
        print(board.mode, board.firmware, board.chip)
        await board.pin(17).output().write(1)


asyncio.run(main())
```

`connect(host, port, *, handshake_timeout=3.0, timeout=5.0, plugins=())` returns a `BoardConnector`: await it for a
`Board`, or use `async with` to close the connection on exit. The handshake asks for the extended protocol. Against
this server the session enters extended mode and the client discovers the board; against upstream firmware it stays in
stock mode.

- `board.mode` is `"extended"` or `"stock"`; `board.firmware` is the stock version triple; `board.protocol` is the
  extension version or `None`.
- Extended mode only: `board.chip`, `board.pins` (per GPIO: capabilities, safe state, ADC channel, named capabilities),
  `board.buses`, `board.limits` and `board.plugins_available`. In stock mode these are empty or `None`.
- `board.stats` counts what the client could not place: `unmatched_replies`, `protocol_errors`, `dropped_events`.
- `await board.close()` ends the connection; `board.closed` tells.

## Chains

A chain is a batch of commands. Nothing is sent until it is awaited; then every command and a closing barrier go out in
one write, and the await completes when the board has answered the barrier.

```python
await board.pin(4).input(pull="up").pin(5).output().write(1)  # pin(...) switches target within one chain
await board.pin(18).pwm(1000.0, 8).duty(128)
await board.pin(23).servo().angle(90)
await board.pin(32).analog()  # analog reports start; read them as events
data = await board.i2c().begin(21, 22).read(0x76, 0x10, 2)  # the value of the last value-yielding command
await board.onewire().init(16)
roms = await board.onewire().search_all()  # runs its own chains: call it on a fresh chain
await board.chain().dht(25)
await board.i2c(1).read(0x48, 0, 2)  # another bus: sent through ON_BUS (extended mode)
```

- Targets: `pin(gpio)`, `i2c(bus)`, `onewire(bus)`. `write`, `read` and `reset` act on the current target.
- `await chain` returns the value of the last command that yields one (an I2C read, a 1-wire read or ROM search), or
  `None`. `await chain.run(timeout)` returns a `ChainResult` with every value and every command's `CommandResult`.
- A chain is single-use. A building call that raises (a bad pin, a call that needs extended mode) discards the whole
  chain: nothing is written and every later call on it raises `ValueError`. A chain built and never awaited warns.
- Extended mode numbers every command, so a refusal is attributed to exactly the command that caused it. Stock mode has
  only two refusal reports upstream (servo attach, I2C read without an answer); see Stock mode below.

## Errors and the abort rule

Every error the client raises for a runtime condition derives from `TelemetrixError`; a programmer error (a pin
of 300, a payload that cannot fit) is a plain `ValueError`.

| Error | When |
|---|---|
| `ChainError` | A command was refused or skipped. `error.results` holds a `CommandResult` per command (`op`, `status` of `done`, `refused` or `skipped`, `reason`, `code`). |
| `ConnectionLost` | The socket could not be opened, or closed while something was pending (chains, plugin requests). |
| `asyncio.TimeoutError` | The board did not confirm in time (`timeout`, 5 s by default), or a discovery page took too long. |
| `HandshakeTimeout` | No answer to the firmware probe within `handshake_timeout` (3 s). |
| `IncompatibleFirmware` | Firmware other than 3.2.x, or an extension major other than 1. |
| `IncompatiblePlugin` | The registered half's major differs from the plugin on the board. |
| `NotSupported` | The call needs extended mode, a feature or plugin the board does not have, or a plugin without a registered half. |
| `ProtocolError` | The server sent bytes that do not decode. Also a `ValueError`. |

`CommandResult.reason` is a `telemetrix_idf.Reason`: `NO_SUCH_PIN`, `NO_CAPABILITY`, `PIN_BUSY`, `EXHAUSTED`, ... `PLUGIN`
(the plugin's own code is in `CommandResult.code`) and `SKIPPED`.

**Abort rule.** By default a chain is atomic: when a command is refused, every later command of the chain is skipped
(not executed on the board; status `skipped`, reason `SKIPPED`), and the await raises `ChainError`. `chain.atomic(False)`
runs every command whatever happens to the earlier ones; refusals still raise `ChainError` at the end, with each
command's own result. The flag governs the commands that follow the SYNC that carries it, up to the next SYNC (Ruling
R1). The session starts with abort on. The client remembers which mode the last SYNC it sent left behind; a chain whose
mode differs sends one extra SYNC in front, so default chains never pay for it, and chains of different modes cannot
influence each other. Stock mode has no skipping: the server executes everything.

## Timeouts and cancellation

- Every await has a timeout: the board's (`connect(timeout=5.0)`) or the one passed to `chain.run(timeout)`.
  `asyncio.TimeoutError` means "no confirmation", not "not executed": the commands were written and the board may have
  run them.
- Cancelling an await (or a timeout) does not unsend anything. The barrier tag stays reserved until its reply arrives
  or the connection drops, and the late reply is consumed and dropped. The client's record of pin states stays as
  if the chain ran, except for commands that end refused or skipped.
- A closed connection fails every pending await with `ConnectionLost`, ends every event stream and fails every plugin
  request. There is no reconnect: connect again, which gives a fresh session.

## Events

Digital, analog, sonar and DHT reports arrive as events, by callback or by async iterator:

```python
unsubscribe = board.on_digital(4, lambda e: print(e.pin, e.value))  # sync or async callbacks
async for e in board.digital_changes(4):  # DigitalEvent(pin, value, timestamp)
    ...
```

`on_analog`, `on_sonar`, `on_dht` and the matching `analog_changes`, `sonar_changes`, `dht_changes` work the same
(`AnalogEvent`, `SonarEvent(trigger, cm)`, `DhtEvent(pin, humidity, temperature, error)` where `error` is 1 for a
checksum failure and 2 for a timeout). Events are dispatched in arrival order on one task per board, never on the
reader: a callback may be a coroutine (awaited before the next event), and one that raises is logged while the others
still run. The queue holds 1000 events; when it is full the oldest is dropped and counted in `board.stats.dropped_events`.
A stream ends when it is closed (`aclose()`) or the connection closes. A stream sees events from its creation on, so
create it before the chain that starts the reports.

## Stock mode

Against upstream Telemetrix4Esp32 3.2.x the client works within what that firmware can do:

- `reset()` raises `NotSupported`: stock firmware reboots on RESET and the client never sends it. Pins and
  reports a session set up are not released when it disconnects; the next client inherits them (the client ignores
  reports of pins it did not configure itself).
- The stock firmware has no way to say no. A refusal is visible only for a servo attach (REFUSED `EXHAUSTED`) and an I2C
  read nobody answers (REFUSED `NO_ANSWER`); every other refusal is silent, so the command shows as `done`.
- No skipping, so `atomic()` has no effect. The barrier is `LOOP` with an 8-bit tag.
- Calls that need extended mode raise `NotSupported`: ON_BUS (I2C and 1-wire bus 1 and up), capability discovery,
  plugins. Pin numbers are checked against the stock client's lists (telemetrix-esp32 2.1.1) and a bad one is a
  `ValueError`.
- PWM channels are numbered by the client (16 of them); the extended server allocates its own.

## Plugins

A server plugin (see the repository README) can have a client half. Subclass `PluginHalf`, give it the plugin's `name`
and `major`, and register the class when connecting:

```python
from telemetrix_idf import PluginHalf, connect


class Echo(PluginHalf):
    name = "test.echo"
    major = 1

    def on_report(self, sub: int, data: bytes) -> None:  # may be a coroutine function
        print("report", sub, data)


async with connect(host, port, plugins=[Echo]) as board:
    echo = board.plugin("test.echo")  # the instance bound to the plugin's slot, created once per board
    await echo.command(0, b"hi")  # a Chain holding PLUGIN [slot, sub, args]; composes with other ops
    reply = await echo.request(0, b"hi")  # a command and the report answering it
```

- `board.plugin(name)` raises `NotSupported` in stock mode, when the board lacks the plugin, or when the board has it
  but no half was registered; `IncompatiblePlugin` when the majors differ.
- A refusal by the plugin raises `ChainError` with reason `PLUGIN` and the plugin's code in `CommandResult.code`.
- `on_report(sub, data)` receives every PLUGIN_REPORT of the slot that no request took, on the board's event task.
- **Request-id convention.** `request(sub, args, timeout=None)` sends `command(sub, id + args)` where `id` is a
  big-endian u16 the half picks (1, 2, 3, ..., wrapping past 65535; 0 is never used). The plugin must answer with a
  PLUGIN_REPORT on the same `sub` whose data starts with the same two bytes. `request` returns the data after the id.
  A report that matches no waiting request (another id, another sub) goes to `on_report`; so does a late answer to a
  request that timed out. A request raises `ChainError` when refused, `asyncio.TimeoutError` when the command and the
  report take longer than `timeout`, and `ConnectionLost` when the connection closes meanwhile.

## Development

```sh
make client-lint          # ruff format --check and ruff check
make client-test          # unit tests against in-process fake servers
make client-integration   # against the host server (host/server), builds it first
```
