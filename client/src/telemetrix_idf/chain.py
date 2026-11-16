"""Chains: build a batch of commands, await it, and get the board's confirmation.

`board.pin(17).output().write(1)` builds a `Chain`; nothing is sent until it is awaited. Awaiting writes every command
and a closing barrier in one write and completes when the board answers the barrier:

- extended mode: SYNC `[tag, abort]`, answered by SYNC_REPLY `[tag, seq]`. Every command is numbered (the client
  counts like the server, SYNC included), so an ERROR lands on the command, and the chain, that caused it. The abort
  flag is the chain's `atomic` (default True). The engine tracks the flag the last SYNC it wrote left behind (the
  session starts with abort on); a chain whose flag differs is prefixed with a SYNC carrying its flag, whose reply is
  consumed (Ruling R1). With abort on, a refused command makes the server skip every later one of the chain.
- stock mode: `LOOP [tag]`, answered by `[2, 0, tag]`. Upstream reports only two refusals, which are matched:
  `SERVO_ATTACH` by `[6, pin]` (REFUSED EXHAUSTED) and an I2C read by `[8, 1, addr]` (REFUSED NO_ANSWER). Every other
  stock refusal is silent: the command shows as done. Nothing is skipped.

Replies are matched per kind (I2C read on a bus, 1-wire op on a bus and subtype) by a FIFO of the ops that expect
them; an ERROR removes its op's expectation. A reply nothing expects is counted in `board.stats.unmatched_replies`
and dropped. Barrier tags are 16-bit (extended) or 8-bit (stock); a tag whose await timed out or was cancelled stays
reserved until its reply arrives or the connection drops, and its late replies are consumed and discarded.
"""

from __future__ import annotations

import asyncio
import warnings
from collections import deque
from collections.abc import Callable, Generator, Iterable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Literal, NamedTuple

from . import wire
from .errors import ChainError, ConnectionLost, NotSupported, ProtocolError, TelemetrixError
from .stock import STOCK_PINS
from .wire import Reason

if TYPE_CHECKING:
    from .board import Board, Stats
    from .connection import Connection

__all__ = ["Chain", "ChainEngine", "ChainResult", "CommandResult", "CommandStatus", "TagPool"]

CommandStatus = Literal["done", "refused", "skipped"]
PWM_CHANNELS = 16  # LEDC channels of the stock firmware's chip (ESP32)


class CommandResult(NamedTuple):
    op: str  # the chain method that added the command, e.g. "output"
    status: CommandStatus
    reason: Reason | int | None


class ChainResult(NamedTuple):
    values: list[Any]  # one per value-yielding op, in chain order
    results: list[CommandResult]  # one per command, in chain order


class TagPool:
    """Barrier tags `0..size-1`, handed out by a rolling counter that skips reserved tags."""

    def __init__(self, size: int) -> None:
        self.size = size
        self.reserved: set[int] = set()
        self._next = 0

    def allocate(self) -> int:
        for _ in range(self.size):
            tag = self._next
            self._next = (self._next + 1) % self.size
            if tag not in self.reserved:
                self.reserved.add(tag)
                return tag
        raise TelemetrixError("no free barrier tag")

    def release(self, tag: int) -> None:
        self.reserved.discard(tag)


ReplyKey = tuple[Any, ...]


@dataclass(eq=False)
class _Op:
    name: str
    packet: bytes
    reply: ReplyKey | None = None  # the reply FIFO this op waits in
    decode: Callable[[bytes], Any] | None = None  # set for value-yielding ops
    refusal: ReplyKey | None = None  # stock: the refusal report FIFO (servo by pin)
    addr: int | None = None  # stock I2C read: matched against `[8, 1, addr]`
    on_send: list[Callable[[], None]] = field(default_factory=list)
    seq: int | None = None
    status: CommandStatus = "done"
    reason: Reason | int | None = None
    value: Any = None


@dataclass(eq=False)
class _Pending:
    ops: list[_Op]
    future: asyncio.Future[None]


class ChainEngine:
    """Sends chains and matches the board's replies to them. One per board; `handle` is called by the router."""

    def __init__(self, connection: Connection, stats: Stats, *, extended: bool) -> None:
        self._conn = connection
        self._stats = stats
        self.extended = extended
        self.tags = TagPool(1 << 16 if extended else 1 << 8)
        self._abort = True  # the flag the last SYNC left behind; a session starts with abort on
        self._barriers: dict[int, _Pending | None] = {}  # None: a prefix SYNC whose reply is consumed
        self._replies: dict[ReplyKey, deque[_Op]] = {}
        self._refusals: dict[ReplyKey, deque[_Op]] = {}
        self._by_seq: dict[int, _Op] = {}
        connection.add_close_callback(self._on_close)

    def send(self, ops: list[_Op], atomic: bool) -> asyncio.Future[None]:
        """Write the ops and the barrier in one write; the future completes on the barrier's reply."""
        if self._conn.closed:
            raise ConnectionLost("connection closed")
        prefix = self.extended and atomic != self._abort
        prefix_tag = self.tags.allocate() if prefix else None
        try:
            tag = self.tags.allocate()
        except TelemetrixError:
            if prefix_tag is not None:
                self.tags.release(prefix_tag)
            raise
        packets: list[bytes] = []
        seq = self._conn.seq
        if prefix_tag is not None:
            packets.append(wire.sync(prefix_tag, atomic))
            seq += 1
        for op in ops:
            if self.extended:
                op.seq = seq & 0xFFFF
                seq += 1
            packets.append(op.packet)
        packets.append(wire.sync(tag, atomic) if self.extended else wire.loop(tag))
        try:
            self._conn.write(b"".join(packets))
        except BaseException:
            for t in (prefix_tag, tag):
                if t is not None:
                    self.tags.release(t)
            raise
        if prefix_tag is not None:
            self._barriers[prefix_tag] = None
        if self.extended:
            self._abort = atomic
        for op in ops:
            if op.seq is not None:
                self._by_seq[op.seq] = op
            if op.reply is not None:
                self._replies.setdefault(op.reply, deque()).append(op)
            if op.refusal is not None and not self.extended:
                self._refusals.setdefault(op.refusal, deque()).append(op)
        future: asyncio.Future[None] = asyncio.get_running_loop().create_future()
        self._barriers[tag] = _Pending(ops, future)
        return future

    def handle(self, rid: int, payload: bytes) -> bool:
        """Take a reply report; False when `rid` is not one the engine handles."""
        if self.extended:
            if rid == wire.RPT.ERROR:
                self._error(wire.decode_error(payload))
            elif rid == wire.RPT.SYNC_REPLY:
                self._barrier(wire.decode_sync_reply(payload).tag)
            elif rid == wire.RPT.BUS_REPORT:
                b = wire.decode_bus_report(payload)
                if b.report_id == wire.RPT.I2C_READ_REPORT:
                    self._reply(("i2c", b.bus), b.payload)
                elif b.report_id == wire.RPT.ONE_WIRE_REPORT:
                    self._reply(("ow", b.bus, wire.decode_one_wire(b.payload).op), b.payload)
                else:
                    self._stats.unmatched_replies += 1
            elif rid not in (wire.RPT.I2C_READ_REPORT, wire.RPT.ONE_WIRE_REPORT):
                return False
        elif rid == wire.RPT.LOOP_COMMAND:
            if not payload:
                raise ProtocolError("LOOP reply without a tag")
            self._barrier(payload[0])
        elif rid == wire.RPT.SERVO_UNAVAILABLE:
            self._refused(self._refusals.get(("servo", wire.decode_servo_unavailable(payload))), Reason.EXHAUSTED)
        elif rid == wire.RPT.I2C_TOO_MANY_BYTES_RCVD:
            addr = wire.decode_i2c_short(payload)
            fifo = self._replies.get(("i2c", 0), deque())
            self._refused(deque(op for op in fifo if op.addr == addr), Reason.NO_ANSWER)
        elif rid not in (wire.RPT.I2C_READ_REPORT, wire.RPT.ONE_WIRE_REPORT):
            return False
        if rid == wire.RPT.I2C_READ_REPORT:
            self._reply(("i2c", 0), payload)
        elif rid == wire.RPT.ONE_WIRE_REPORT:
            self._reply(("ow", 0, wire.decode_one_wire(payload).op), payload)
        return True

    def _error(self, e: wire.ErrorReport) -> None:
        op = self._by_seq.pop(e.seq, None)
        if op is None:
            self._stats.unmatched_replies += 1
            return
        op.status = "skipped" if e.reason == Reason.SKIPPED else "refused"
        op.reason = e.reason
        self._forget(op)

    def _refused(self, candidates: Iterable[_Op] | None, reason: Reason) -> None:
        op = next(iter(candidates or ()), None)
        if op is None:
            self._stats.unmatched_replies += 1
            return
        op.status, op.reason = "refused", reason
        self._forget(op)

    def _reply(self, key: ReplyKey, payload: bytes) -> None:
        fifo = self._replies.get(key)
        if not fifo:
            self._stats.unmatched_replies += 1
            return
        op = fifo.popleft()
        if op.refusal is not None:
            self._remove(self._refusals, op.refusal, op)
        assert op.decode is not None
        op.value = op.decode(payload)

    def _barrier(self, tag: int) -> None:
        if tag not in self._barriers:
            self._stats.unmatched_replies += 1
            return
        pending = self._barriers.pop(tag)
        self.tags.release(tag)
        if pending is None:
            return
        for op in pending.ops:
            if op.seq is not None and self._by_seq.get(op.seq) is op:
                del self._by_seq[op.seq]
            self._forget(op)
        if not pending.future.done():
            pending.future.set_result(None)

    def _forget(self, op: _Op) -> None:
        if op.reply is not None:
            self._remove(self._replies, op.reply, op)
        if op.refusal is not None:
            self._remove(self._refusals, op.refusal, op)

    @staticmethod
    def _remove(table: dict[ReplyKey, deque[_Op]], key: ReplyKey, op: _Op) -> None:
        fifo = table.get(key)
        if fifo is not None and op in fifo:
            fifo.remove(op)

    def _on_close(self) -> None:
        for pending in self._barriers.values():
            if pending is not None and not pending.future.done():
                pending.future.set_exception(ConnectionLost("connection closed while a chain was pending"))
        self._barriers.clear()
        self._replies.clear()
        self._refusals.clear()
        self._by_seq.clear()
        self.tags.reserved.clear()


# --- decoders of value-yielding ops ------------------------------------------------------------------------------


def _i2c_data(payload: bytes) -> bytes:
    return wire.decode_i2c_read(payload).data


def _ow_byte(payload: bytes) -> int:
    data = wire.decode_one_wire(payload).data
    if not data:
        raise ProtocolError("1-wire report without a value")
    return data[0]


def _ow_presence(payload: bytes) -> bool:
    return _ow_byte(payload) != 0


def _ow_rom(payload: bytes) -> bytes | None:
    data = wire.decode_one_wire(payload).data
    if len(data) < 8:
        raise ProtocolError(f"1-wire search report with {len(data)} ROM bytes, need 8")
    rom = bytes(data[:8])
    return None if rom == b"\xff" * 8 else rom


# --- the chain ---------------------------------------------------------------------------------------------------


class Chain:
    """A batch of commands for one board. Every method adds a command (or picks a target) and returns the chain.

    Targets: `pin(gpio)` for the pin methods, `i2c(bus)` for the I2C methods, `onewire(bus)` for the 1-wire methods.
    `write`, `read` and `reset` act on the current target: `write(level)` on a pin, `write(addr, data)` on I2C,
    `write(byte, power=...)` on 1-wire; `reset()` on a 1-wire bus is a bus reset, otherwise the board's RESET.
    On a bus other than 0 the I2C and 1-wire commands go through ON_BUS (extended mode only).

    `await chain` returns the value of the last value-yielding op, or None. `await chain.run()` returns a
    `ChainResult`. Both raise `ChainError` (every op's `CommandResult`) when an op was refused or skipped,
    `asyncio.TimeoutError` when the board does not confirm in time, `ConnectionLost` when the connection closes.
    A call that needs extended mode raises `NotSupported` in stock mode, and an invalid stock pin `ValueError`;
    either discards the chain before anything is written.
    """

    def __init__(self, board: Board) -> None:
        self._board = board
        self._stock = board.mode == "stock"
        self._ops: list[_Op] = []
        self._atomic = True
        self._target: tuple[str, int] | None = None
        self._kinds: dict[int, str] = {}  # pin kinds set earlier in this chain
        self._sent = False

    # --- targets and flags

    def pin(self, gpio: int) -> Chain:
        self._target = ("pin", gpio)
        return self

    def i2c(self, bus: int = 0) -> Chain:
        self._need_bus(bus)
        self._target = ("i2c", bus)
        return self

    def onewire(self, bus: int = 0) -> Chain:
        self._need_bus(bus)
        self._target = ("onewire", bus)
        return self

    def atomic(self, flag: bool = True) -> Chain:
        """Abort on the first refusal (default): later commands are skipped. False runs every command."""
        self._atomic = bool(flag)
        return self

    # --- pins

    def input(self, pull: Literal["up", "down"] | None = None, report: bool = True) -> Chain:
        gpio = self._pin("input", "input")
        builders = {None: wire.set_pin_mode_input, "up": wire.set_pin_mode_input_pullup}
        builders["down"] = wire.set_pin_mode_input_pulldown
        if pull not in builders:
            raise ValueError(f"pull {pull!r} is not 'up', 'down' or None")
        return self._add("input", builders[pull](gpio, report), kind=(gpio, "digital"), event=("digital", gpio))

    def output(self) -> Chain:
        gpio = self._pin("output", "output")
        return self._add("output", wire.set_pin_mode_output(gpio), kind=(gpio, "output"))

    def analog(self, differential: int = 0, report: bool = True) -> Chain:
        gpio = self._pin("analog", "analog")
        packet = wire.set_pin_mode_analog(gpio, differential, report)
        return self._add("analog", packet, kind=(gpio, "analog"), event=("analog", gpio))

    def pwm(self, freq_hz: float, resolution: int) -> Chain:
        """PWM mode. The channel is the client's (stock firmware uses it; our server allocates its own)."""
        gpio = self._pin("pwm", "output")
        channel = self._board._pwm_channel(gpio)
        packet = wire.set_pin_mode_pwm(gpio, channel, resolution, freq_hz)
        return self._add("pwm", packet, kind=(gpio, "pwm"))

    def duty(self, value: int) -> Chain:
        gpio = self._pin("duty", "output")
        return self._add("duty", wire.analog_write(gpio, value))

    def servo(self, min_us: int = 1000, max_us: int = 2000) -> Chain:
        gpio = self._pin("servo", "servo")
        op = _Op("servo", wire.servo_attach(gpio, min_us, max_us), refusal=("servo", gpio))
        return self._add_op(op, kind=(gpio, "servo"))

    def angle(self, deg: int) -> Chain:
        gpio = self._pin("angle", "servo")
        return self._add("angle", wire.servo_write(gpio, deg))

    def detach(self) -> Chain:
        """Detach a servo or PWM pin (as last set up by this client)."""
        gpio = self._pin("detach")
        kind = self._kind(gpio)
        if kind == "servo":
            return self._add("detach", wire.servo_detach(gpio), kind=(gpio, None))
        if kind == "pwm":
            board = self._board
            op = _Op("detach", wire.analog_out_detach(gpio), on_send=[lambda: board._pwm_release(gpio)])
            return self._add_op(op, kind=(gpio, None))
        raise ValueError(f"detach() needs a servo or PWM pin; pin {gpio} is {kind or 'not set up'}")

    def report(self, on: bool) -> Chain:
        """Turn reporting of a digital input or analog pin on or off."""
        gpio = self._pin("report")
        kind = self._kind(gpio)
        if kind == "digital":
            return self._add("report", wire.modify_reporting(2 if on else 4, gpio))
        if kind == "analog":
            return self._add("report", wire.modify_reporting(1 if on else 3, gpio))
        raise ValueError(f"report() needs a digital input or analog pin; pin {gpio} is {kind or 'not set up'}")

    # --- context-dependent: pin, I2C, 1-wire

    def write(self, value: int, data: bytes | None = None, *, power: bool = False) -> Chain:
        """`write(level)` on a pin, `write(addr, data)` on I2C, `write(byte, power=False)` on 1-wire."""
        kind, n = self._need_target("write")
        if kind == "pin":
            if data is not None:
                raise ValueError("write(level) on a pin takes one value")
            self._stock_pin(n, "output")
            return self._add("write", wire.digital_write(n, value))
        if kind == "i2c":
            if data is None:
                raise ValueError("write(addr, data) on I2C needs the data")
            return self._add("write", self._on_bus(n, wire.i2c_write(value, data)))
        if data is not None:
            raise ValueError("write(byte, power=False) on 1-wire takes one byte")
        return self._add("write", self._on_bus(n, wire.one_wire_write(value, power)))

    def read(self, addr: int | None = None, reg: int = 0, n: int = 1, stop: bool = True) -> Chain:
        """`read(addr, reg, n, stop=True)` on I2C yields bytes; `read()` on 1-wire yields an int."""
        kind, bus = self._need_target("read")
        if kind == "i2c":
            if addr is None:
                raise ValueError("read(addr, reg, n) on I2C needs the address")
            packet = self._on_bus(bus, wire.i2c_read(addr, reg, n, stop))
            return self._add_op(_Op("read", packet, reply=("i2c", bus), decode=_i2c_data, addr=addr))
        if kind == "onewire":
            if addr is not None:
                raise ValueError("read() on 1-wire takes no arguments")
            return self._ow("read", wire.one_wire_read(), bus, wire.CMD.ONE_WIRE_READ, _ow_byte)
        raise ValueError("read() needs an I2C or 1-wire target; start with .i2c(bus) or .onewire(bus)")

    def reset(self) -> Chain:
        """On a 1-wire bus: a bus reset, yields presence (bool). Otherwise the board's RESET (extended mode only)."""
        if self._target is not None and self._target[0] == "onewire":
            bus = self._target[1]
            return self._ow("reset", wire.one_wire_reset(), bus, wire.CMD.ONE_WIRE_RESET, _ow_presence)
        if self._stock:
            self._discard(NotSupported("reset() needs extended mode; upstream reboots on RESET"))
        return self._add("reset", wire.reset())

    # --- I2C

    def begin(self, sda: int | None = None, scl: int | None = None) -> Chain:
        bus = self._bus("begin", "i2c")
        for gpio in (sda, scl):
            if gpio is not None:
                self._stock_pin(gpio, "input", "output")
        return self._add("begin", self._on_bus(bus, wire.i2c_begin(sda, scl)))

    # --- 1-wire

    def init(self, pin: int | None = None) -> Chain:
        """Start the bus; `pin` defaults to the bus's pin from discovery when the board fixes it."""
        bus = self._bus("init", "onewire")
        if pin is None:
            fixed = [b.pins[0] for b in self._board.buses if b.kind == "one_wire" and b.bus == bus and b.pins]
            if not fixed:
                raise ValueError(f"init() needs a pin: 1-wire bus {bus} takes the client's pin")
            pin = fixed[0]
        self._stock_pin(pin, "input", "output")
        return self._add("init", self._on_bus(bus, wire.one_wire_init(pin)))

    def select(self, rom: bytes) -> Chain:
        bus = self._bus("select", "onewire")
        return self._add("select", self._on_bus(bus, wire.one_wire_select(rom)))

    def skip(self) -> Chain:
        bus = self._bus("skip", "onewire")
        return self._add("skip", self._on_bus(bus, wire.one_wire_skip()))

    def reset_search(self) -> Chain:
        bus = self._bus("reset_search", "onewire")
        return self._add("reset_search", self._on_bus(bus, wire.one_wire_reset_search()))

    def search(self) -> Chain:
        """Yields the next ROM (8 bytes), or None when no device is left."""
        bus = self._bus("search", "onewire")
        return self._ow("search", wire.one_wire_search(), bus, wire.CMD.ONE_WIRE_SEARCH, _ow_rom)

    def crc8(self, data: bytes) -> Chain:
        """Yields the board's CRC-8 of `data`."""
        bus = self._bus("crc8", "onewire")
        return self._ow("crc8", wire.one_wire_crc8(data), bus, wire.CMD.ONE_WIRE_CRC8, _ow_byte)

    async def search_all(self, timeout: float | None = None) -> list[bytes]:
        """Every ROM on this chain's 1-wire bus: a search reset, then one search chain per device."""
        bus = self._bus("search_all", "onewire")
        if self._ops:
            raise ValueError("search_all() runs its own chains; call it on a chain without commands")
        self._sent = True
        await self._board.onewire(bus).reset_search().run(timeout)
        roms: list[bytes] = []
        while True:
            rom = (await self._board.onewire(bus).search().run(timeout)).values[0]
            if rom is None or rom in roms:
                return roms
            roms.append(rom)

    # --- sensors and reporting

    def sonar(self, trig: int, echo: int) -> Chain:
        self._stock_pin(trig, "output")
        self._stock_pin(echo, "input")
        return self._add("sonar", wire.sonar_new(trig, echo), event=("sonar", trig))

    def dht(self, pin: int) -> Chain:
        self._stock_pin(pin, "input", "output")
        return self._add("dht", wire.dht_new(pin), event=("dht", pin))

    def reporting(self, enabled: bool) -> Chain:
        """Resume (ENABLE_ALL_REPORTS) or stop (STOP_ALL_REPORTS) every report."""
        return self._add("reporting", wire.enable_all_reports() if enabled else wire.stop_all_reports())

    def analog_interval(self, ms: int) -> Chain:
        return self._add("analog_interval", wire.set_analog_scanning_interval(ms))

    # --- sending

    def __await__(self) -> Generator[Any, None, Any]:
        return self._value().__await__()

    async def run(self, timeout: float | None = None) -> ChainResult:
        """Send the chain; `timeout` defaults to the board's (5 s)."""
        if self._sent:
            raise ValueError("chain already sent")
        self._sent = True
        ops = self._ops
        future = self._board._chains.send(ops, self._atomic)
        for op in ops:
            for effect in op.on_send:
                effect()
        await asyncio.wait_for(future, self._board.timeout if timeout is None else timeout)
        results = [CommandResult(op.name, op.status, op.reason) for op in ops]
        if any(op.status != "done" for op in ops):
            raise ChainError(results)
        return ChainResult([op.value for op in ops if op.decode is not None], results)

    async def _value(self) -> Any:
        values = (await self.run()).values
        return values[-1] if values else None

    def __del__(self) -> None:
        if self._ops and not self._sent:
            warnings.warn(f"chain never awaited ({len(self._ops)} commands)", RuntimeWarning, stacklevel=2)

    def __repr__(self) -> str:
        return f"<Chain {[op.name for op in self._ops]} atomic={self._atomic} sent={self._sent}>"

    # --- helpers

    def _add(self, name: str, packet: bytes, *, kind: tuple[int, str | None] | None = None, event: Any = None) -> Chain:
        return self._add_op(_Op(name, packet), kind=kind, event=event)

    def _add_op(self, op: _Op, *, kind: tuple[int, str | None] | None = None, event: Any = None) -> Chain:
        board = self._board
        if kind is not None:
            gpio, k = kind
            self._kinds[gpio] = k or ""
            op.on_send.append(lambda: board._set_pin_kind(gpio, k))
        if event is not None:
            op.on_send.append(lambda: board._events.configure(event))
        self._ops.append(op)
        return self

    def _ow(self, name: str, inner: bytes, bus: int, sub: int, decode: Callable[[bytes], Any]) -> Chain:
        return self._add_op(_Op(name, self._on_bus(bus, inner), reply=("ow", bus, sub), decode=decode))

    def _on_bus(self, bus: int, inner: bytes) -> bytes:
        return inner if bus == 0 else wire.on_bus(bus, inner)

    def _need_target(self, name: str) -> tuple[str, int]:
        if self._target is None:
            raise ValueError(f"{name}() needs a target; start with .pin(gpio), .i2c(bus) or .onewire(bus)")
        return self._target

    def _pin(self, name: str, *stock_lists: str) -> int:
        kind, gpio = self._need_target(name)
        if kind != "pin":
            raise ValueError(f"{name}() needs a pin; start with .pin(gpio)")
        self._stock_pin(gpio, *stock_lists)
        return gpio

    def _bus(self, name: str, kind: str) -> int:
        target = self._need_target(name)
        if target[0] != kind:
            raise ValueError(f"{name}() needs a {kind} bus; start with .{kind}(bus)")
        return target[1]

    def _kind(self, gpio: int) -> str | None:
        return self._kinds[gpio] or None if gpio in self._kinds else self._board._pin_kinds.get(gpio)

    def _need_bus(self, bus: int) -> None:
        if self._stock and bus != 0:
            self._discard(NotSupported(f"bus {bus} needs extended mode; stock mode has bus 0 only"))

    def _stock_pin(self, gpio: int, *lists: str) -> None:
        """Stock mode: the pin must be in every named upstream list (the firmware indexes unchecked arrays)."""
        if self._stock and not all(gpio in STOCK_PINS[name] for name in lists):
            self._discard(ValueError(f"pin {gpio} is not a stock {'/'.join(lists)} pin"))

    def _discard(self, error: Exception) -> None:
        self._ops.clear()
        raise error
