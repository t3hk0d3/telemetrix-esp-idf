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
  stock refusal is silent: the command shows as done. Nothing is skipped. Stock hazards are refused before writing:
  a 1-wire op other than init and CRC-8 before an init was sent this session (upstream dereferences a null OneWire
  and reboots), a second init (each leaks a OneWire), and sonar/DHT without `connect(..., stock_sensors=True)` or
  past 6 of a kind this session (upstream writes its 6-entry arrays unbounded).

Replies are matched per kind (I2C read on a bus, 1-wire op on a bus and subtype) by a FIFO of the ops that expect
them; an ERROR removes its op's expectation. A reply nothing expects is counted in `board.stats.unmatched_replies`
and dropped. A reply that does not decode marks its op REFUSED HARDWARE (counted in `board.stats.protocol_errors`).
Barrier tags are 16-bit (extended) or 8-bit (stock); a tag whose await timed out or was cancelled stays reserved until
its reply arrives or the connection drops, and its late replies are consumed and discarded.

The client's own pin state (what each pin was set up as, the stock-mode pins whose reports are passed on, PWM
channels) changes when a chain is written and is put back for every op that ends refused or skipped. PWM channels are
allocated when the chain is written: 16 in stock mode, `limits["pwm_channels"]` (or 256) in extended mode, where the
server picks its own channel. A RESET clears that state (put back too when the RESET is refused).
"""

from __future__ import annotations

import asyncio
import functools
import logging
import warnings
from collections import deque
from collections.abc import Callable, Generator, Iterable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Literal, NamedTuple, TypeVar, cast

from . import wire
from .errors import ChainError, ConnectionLost, NotSupported, ProtocolError, TelemetrixError
from .stock import STOCK_PINS, STOCK_SENSOR_MAX
from .wire import Reason

if TYPE_CHECKING:
    from .board import Board, Stats
    from .connection import Connection

__all__ = ["Chain", "ChainEngine", "ChainResult", "CommandResult", "CommandStatus", "TagPool"]

log = logging.getLogger("telemetrix_idf")

CommandStatus = Literal["done", "refused", "skipped"]
PWM_CHANNELS = 16  # LEDC channels of the stock firmware's chip (ESP32)

Undo = Callable[[], None]


class CommandResult(NamedTuple):
    op: str  # the chain method that added the command, e.g. "output"
    status: CommandStatus
    reason: Reason | int | None
    code: int | None = None  # a plugin's own refusal code (reason PLUGIN)


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
    prepare: Callable[[_Op], Undo | None] | None = None  # before writing; may set `packet`, may raise
    on_send: list[Callable[[], Undo | None]] = field(default_factory=list)  # after writing; each may return an undo
    undo: list[Undo] = field(default_factory=list)  # run (last first) when the op ends refused or skipped
    seq: int | None = None
    status: CommandStatus = "done"
    reason: Reason | int | None = None
    code: int | None = None
    value: Any = None

    def settle(self) -> None:
        """At the barrier: put back the client state this op changed unless it was done."""
        if self.status == "done":
            return
        for undo in reversed(self.undo):
            try:
                undo()
            except Exception:
                log.exception("undo of %s failed", self.name)
        self.undo.clear()


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
        op.code = e.code
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
        try:
            op.value = op.decode(payload)
        except ProtocolError as e:
            self._stats.protocol_errors += 1
            log.warning("reply to %s does not decode: %s", op.name, e)
            op.status, op.reason = "refused", Reason.HARDWARE

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
            op.settle()
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

_OW_BEFORE_INIT = (
    "1-wire {}() before init: stock firmware dereferences a null OneWire and reboots; send onewire().init(pin) first"
)
_OW_SECOND_INIT = "1-wire init() already sent this session: stock firmware leaks a OneWire on every init"
_SENSOR_OPT_IN = (
    "{}() in stock mode needs connect(..., stock_sensors=True): the firmware's sensor count persists until power-cycle"
)
_SENSOR_CAP = f"at most {STOCK_SENSOR_MAX} {{}} sensors per stock session: the firmware's array overflows past that"

F = TypeVar("F", bound=Callable[..., Any])


def _building(method: F) -> F:
    """A chain-building method: refused on a discarded chain; any error it raises discards the chain."""

    @functools.wraps(method)
    def wrapper(self: Chain, *args: Any, **kwargs: Any) -> Any:
        self._check_usable()
        try:
            return method(self, *args, **kwargs)
        except Exception as e:
            self._broken = e
            self._ops.clear()
            raise

    return cast(F, wrapper)


class Chain:
    """A batch of commands for one board. Every method adds a command (or picks a target) and returns the chain.

    Targets: `pin(gpio)` for the pin methods, `i2c(bus)` for the I2C methods, `onewire(bus)` for the 1-wire methods.
    `write`, `read` and `reset` act on the current target: `write(level)` on a pin, `write(addr, data)` on I2C,
    `write(byte, power=...)` on 1-wire; `reset()` on a 1-wire bus is a bus reset, and on a chain without a target
    (`board.chain().reset()`) the board's RESET.
    On a bus other than 0 the I2C and 1-wire commands go through ON_BUS (extended mode only).

    `await chain` returns the value of the last value-yielding op, or None. `await chain.run()` returns a
    `ChainResult`. Both raise `ChainError` (every op's `CommandResult`) when an op was refused or skipped,
    `asyncio.TimeoutError` when the board does not confirm in time, `ConnectionLost` when the connection closes.
    A call that needs extended mode raises `NotSupported` in stock mode, and an invalid stock pin `ValueError`.
    Any error a building call raises discards the chain: nothing of it is written, it does not warn as never awaited,
    and every later call on it, await included, raises `ValueError`.
    """

    def __init__(self, board: Board) -> None:
        self._board = board
        self._stock = board.mode == "stock"
        self._ops: list[_Op] = []
        self._atomic = True
        self._target: tuple[str, int] | None = None
        self._kinds: dict[int, str] = {}  # pin kinds set earlier in this chain
        self._ow_init_here = False  # stock: a 1-wire init earlier in this chain
        self._sensors_here = {"sonar": 0, "dht": 0}  # stock: sensors added earlier in this chain
        self._sent = False
        self._broken: Exception | None = None  # the error that discarded the chain

    # --- targets and flags

    @_building
    def pin(self, gpio: int) -> Chain:
        self._target = ("pin", gpio)
        return self

    @_building
    def i2c(self, bus: int = 0) -> Chain:
        self._need_bus(bus)
        self._target = ("i2c", bus)
        return self

    @_building
    def onewire(self, bus: int = 0) -> Chain:
        self._need_bus(bus)
        self._target = ("onewire", bus)
        return self

    @_building
    def atomic(self, flag: bool = True) -> Chain:
        """Abort on the first refusal (default): later commands are skipped. False runs every command."""
        self._atomic = bool(flag)
        return self

    # --- pins

    @_building
    def input(self, pull: Literal["up", "down"] | None = None, report: bool = True) -> Chain:
        gpio = self._pin("input", "input")
        builders = {None: wire.set_pin_mode_input, "up": wire.set_pin_mode_input_pullup}
        builders["down"] = wire.set_pin_mode_input_pulldown
        if pull not in builders:
            raise ValueError(f"pull {pull!r} is not 'up', 'down' or None")
        return self._add("input", builders[pull](gpio, report), kind=(gpio, "digital"), event=("digital", gpio))

    @_building
    def output(self) -> Chain:
        gpio = self._pin("output", "output")
        return self._add("output", wire.set_pin_mode_output(gpio), kind=(gpio, "output"))

    @_building
    def analog(self, differential: int = 0, report: bool = True) -> Chain:
        gpio = self._pin("analog", "analog")
        packet = wire.set_pin_mode_analog(gpio, differential, report)
        return self._add("analog", packet, kind=(gpio, "analog"), event=("analog", gpio))

    @_building
    def pwm(self, freq_hz: float, resolution: int) -> Chain:
        """PWM mode. The channel is the client's, allocated when the chain is written (stock firmware uses it; our
        server allocates its own). No free channel then raises `TelemetrixError` and nothing is written."""
        gpio = self._pin("pwm", "output")
        board = self._board
        packet = wire.set_pin_mode_pwm(gpio, 0, resolution, freq_hz)  # validates; the channel byte is set on send

        def allocate(op: _Op) -> Undo | None:
            channel, undo = board._pwm_allocate(gpio)
            op.packet = packet[:4] + bytes([channel]) + packet[5:]
            return undo

        return self._add_op(_Op("pwm", packet, prepare=allocate), kind=(gpio, "pwm"))

    @_building
    def duty(self, value: int) -> Chain:
        gpio = self._pin("duty", "output")
        return self._add("duty", wire.analog_write(gpio, value))

    @_building
    def servo(self, min_us: int = 1000, max_us: int = 2000) -> Chain:
        gpio = self._pin("servo", "servo")
        op = _Op("servo", wire.servo_attach(gpio, min_us, max_us), refusal=("servo", gpio))
        return self._add_op(op, kind=(gpio, "servo"))

    @_building
    def angle(self, deg: int) -> Chain:
        gpio = self._pin("angle", "servo")
        return self._add("angle", wire.servo_write(gpio, deg))

    @_building
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

    @_building
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

    @_building
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
        return self._add_op(self._stock_ow(_Op("write", self._on_bus(n, wire.one_wire_write(value, power)))))

    @_building
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

    @_building
    def reset(self) -> Chain:
        """On a 1-wire bus: a bus reset, yields presence (bool). On a chain without a target (`board.chain().reset()`):
        the board's RESET (extended mode only). After `pin()` or `i2c()` it raises `ValueError`."""
        if self._target is not None and self._target[0] == "onewire":
            bus = self._target[1]
            return self._ow("reset", wire.one_wire_reset(), bus, wire.CMD.ONE_WIRE_RESET, _ow_presence)
        if self._target is not None:
            raise ValueError(
                f"reset() on a {self._target[0]} target is ambiguous; the board's RESET is board.chain().reset()"
            )
        if self._stock:
            self._discard(NotSupported("reset() needs extended mode; upstream reboots on RESET"))
        return self._add_op(_Op("reset", wire.reset(), on_send=[self._board._forget_pins]))

    # --- I2C

    @_building
    def begin(self, sda: int | None = None, scl: int | None = None) -> Chain:
        bus = self._bus("begin", "i2c")
        for gpio in (sda, scl):
            if gpio is not None:
                self._stock_pin(gpio, "input", "output")
        return self._add("begin", self._on_bus(bus, wire.i2c_begin(sda, scl)))

    # --- 1-wire

    @_building
    def init(self, pin: int | None = None) -> Chain:
        """Start the bus; `pin` defaults to the bus's pin from discovery when the board fixes it."""
        bus = self._bus("init", "onewire")
        if pin is None:
            fixed = [b.pins[0] for b in self._board.buses if b.kind == "one_wire" and b.bus == bus and b.pins]
            if not fixed:
                raise ValueError(f"init() needs a pin: 1-wire bus {bus} takes the client's pin")
            pin = fixed[0]
        self._stock_pin(pin, "input", "output")
        op = _Op("init", self._on_bus(bus, wire.one_wire_init(pin)))
        if self._stock:
            if self._board._ow_init_sent or self._ow_init_here:
                raise ValueError(_OW_SECOND_INIT)
            self._ow_init_here = True
            op.prepare = self._board._stock_ow_init
        return self._add_op(op)

    @_building
    def select(self, rom: bytes) -> Chain:
        bus = self._bus("select", "onewire")
        return self._add_op(self._stock_ow(_Op("select", self._on_bus(bus, wire.one_wire_select(rom)))))

    @_building
    def skip(self) -> Chain:
        bus = self._bus("skip", "onewire")
        return self._add_op(self._stock_ow(_Op("skip", self._on_bus(bus, wire.one_wire_skip()))))

    @_building
    def reset_search(self) -> Chain:
        bus = self._bus("reset_search", "onewire")
        return self._add_op(self._stock_ow(_Op("reset_search", self._on_bus(bus, wire.one_wire_reset_search()))))

    @_building
    def search(self) -> Chain:
        """Yields the next ROM (8 bytes), or None when no device is left."""
        bus = self._bus("search", "onewire")
        return self._ow("search", wire.one_wire_search(), bus, wire.CMD.ONE_WIRE_SEARCH, _ow_rom)

    @_building
    def crc8(self, data: bytes) -> Chain:
        """Yields the board's CRC-8 of `data` (needs no init, also in stock mode)."""
        bus = self._bus("crc8", "onewire")
        op = _Op("crc8", self._on_bus(bus, wire.one_wire_crc8(data)), reply=("ow", bus, wire.CMD.ONE_WIRE_CRC8))
        op.decode = _ow_byte
        return self._add_op(op)

    async def search_all(self, timeout: float | None = None) -> list[bytes]:
        """Every ROM on this chain's 1-wire bus: a search reset, then one search chain per device.

        In stock mode it raises `NotSupported` before writing anything until an init was sent this session."""
        self._check_usable()
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

    @_building
    def sonar(self, trig: int, echo: int) -> Chain:
        """An HC-SR04. Stock mode: needs `connect(..., stock_sensors=True)`, at most 6 per session (see README)."""
        self._stock_sensor("sonar")
        self._stock_pin(trig, "output")
        self._stock_pin(echo, "input")
        op = self._stock_sensor_op(_Op("sonar", wire.sonar_new(trig, echo)))
        return self._add_op(op, event=("sonar", trig))

    @_building
    def dht(self, pin: int) -> Chain:
        """A DHT sensor. Stock mode: needs `connect(..., stock_sensors=True)`, at most 6 per session (see README)."""
        self._stock_sensor("dht")
        self._stock_pin(pin, "input", "output")
        return self._add_op(self._stock_sensor_op(_Op("dht", wire.dht_new(pin))), event=("dht", pin))

    @_building
    def reporting(self, enabled: bool) -> Chain:
        """Resume (ENABLE_ALL_REPORTS) or stop (STOP_ALL_REPORTS) every report."""
        return self._add("reporting", wire.enable_all_reports() if enabled else wire.stop_all_reports())

    @_building
    def analog_interval(self, ms: int) -> Chain:
        return self._add("analog_interval", wire.set_analog_scanning_interval(ms))

    # --- plugins

    @_building
    def plugin(self, slot: int, sub: int, args: bytes = b"") -> Chain:
        """PLUGIN `[slot, sub, args]` (extended mode only). A refusal shows as reason PLUGIN with the plugin's code
        in `CommandResult.code`. `board.plugin(name).command(sub, args)` names the slot for you (plugins.py)."""
        if self._stock:
            self._discard(NotSupported("plugins need extended mode"))
        return self._add("plugin", wire.plugin(slot, sub, args))

    # --- sending

    def __await__(self) -> Generator[Any, None, Any]:
        return self._value().__await__()

    async def run(self, timeout: float | None = None) -> ChainResult:
        """Send the chain; `timeout` defaults to the board's (5 s)."""
        self._check_usable()
        self._sent = True
        ops = self._ops
        prepared: list[tuple[_Op, Undo]] = []
        try:
            for op in ops:
                undo = op.prepare(op) if op.prepare is not None else None
                if undo is not None:
                    prepared.append((op, undo))
            future = self._board._chains.send(ops, self._atomic)
        except BaseException:
            for _, undo in reversed(prepared):
                undo()
            raise
        for op, undo in prepared:
            op.undo.append(undo)
        for op in ops:
            for effect in op.on_send:
                undo = effect()
                if undo is not None:
                    op.undo.append(undo)
        await asyncio.wait_for(future, self._board.timeout if timeout is None else timeout)
        results = [CommandResult(op.name, op.status, op.reason, op.code) for op in ops]
        if any(op.status != "done" for op in ops):
            raise ChainError(results)
        return ChainResult([op.value for op in ops if op.decode is not None], results)

    async def _value(self) -> Any:
        values = (await self.run()).values
        return values[-1] if values else None

    def __del__(self) -> None:
        if self._ops and not self._sent and self._broken is None:
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
        return self._add_op(self._stock_ow(_Op(name, self._on_bus(bus, inner), reply=("ow", bus, sub), decode=decode)))

    def _stock_ow(self, op: _Op) -> _Op:
        """Stock mode: a 1-wire op other than init and CRC-8 needs an init sent this session, or earlier in this
        chain (upstream calls `ow->...` on a null pointer and reboots). Checked now and again when written."""
        if self._stock:
            if not (self._board._ow_init_sent or self._ow_init_here):
                self._discard(NotSupported(_OW_BEFORE_INIT.format(op.name)))
            op.prepare = self._board._stock_ow_gate
        return op

    def _stock_sensor(self, kind: Literal["sonar", "dht"]) -> None:
        """Stock mode: sensors need the opt-in, and at most `STOCK_SENSOR_MAX` of a kind per session."""
        if not self._stock:
            return
        if not self._board._stock_sensors:
            self._discard(NotSupported(_SENSOR_OPT_IN.format(kind)))
        if self._board._sensors_sent[kind] + self._sensors_here[kind] >= STOCK_SENSOR_MAX:
            raise ValueError(_SENSOR_CAP.format(kind))
        self._sensors_here[kind] += 1

    def _stock_sensor_op(self, op: _Op) -> _Op:
        if self._stock:
            op.prepare = self._board._stock_sensor_count
        return op

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

    def _check_usable(self) -> None:
        if self._broken is not None:
            raise ValueError(f"chain discarded: an earlier call on it raised {self._broken!r}")
        if self._sent:
            raise ValueError("chain already sent")

    def _discard(self, error: Exception) -> None:
        self._ops.clear()
        raise error
