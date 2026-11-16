"""The board model a connection produces: mode, firmware, discovered pins, buses, limits and plugins."""

from __future__ import annotations

import asyncio
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from types import MappingProxyType
from typing import TYPE_CHECKING, Any, Literal

from . import wire
from .chain import PWM_CHANNELS, Chain, ChainEngine
from .discovery import BusInfo, ChipInfo, PinInfo, PluginInfo
from .errors import ConnectionLost, IncompatiblePlugin, NotSupported, TelemetrixError
from .events import AnalogEvent, DhtEvent, DigitalEvent, EventHub, EventStream, SonarEvent
from .plugins import PluginHalf, check_halves
from .stock import STOCK_SENSOR_MAX

if TYPE_CHECKING:
    from .connection import Connection

__all__ = ["Board", "Stats"]


@dataclass
class Stats:
    """Counters of one connection."""

    protocol_errors: int = 0  # reports whose payload did not decode (logged, then dropped)
    unknown_records: int = 0  # discovery records of an unknown type (skipped)
    skipped_bytes: int = 0  # zero length bytes the packet parser skipped
    dropped_events: int = 0  # events dropped from a full dispatch queue (events.py)
    unmatched_replies: int = 0  # replies, errors and barrier replies no pending chain expected (dropped)


class Board:
    """One connected server. Built by `connect()`; `close()` ends the connection.

    In stock mode `chip` is None, `pins` come from the upstream client's lists (stock.py), `buses` are bus 0 I2C and
    1-wire with client-chosen pins, and `limits` and `plugins_available` are empty.

    Commands: `chain()`, `pin(gpio)`, `i2c(bus)` and `onewire(bus)` start a `Chain` (chain.py); await it to send it.
    Events: `on_digital(pin, cb)` and friends return an unsubscribe callable; `digital_changes(pin)` and friends are
    async iterators (events.py). `timeout` is the default of every chain's await.
    Plugins: `plugin(name)` returns the bound client half registered with `connect(plugins=[...])` (plugins.py).
    """

    def __init__(
        self,
        connection: Connection,
        *,
        mode: Literal["extended", "stock"],
        firmware: tuple[int, int, int],
        protocol: tuple[int, int] | None,
        chip: ChipInfo | None,
        pins: Mapping[int, PinInfo],
        buses: Sequence[BusInfo],
        limits: Mapping[str, int],
        plugins_available: Mapping[str, PluginInfo],
        stats: Stats,
        plugin_halves: Sequence[type[Any]] = (),
        timeout: float = 5.0,
        stock_sensors: bool = False,
    ) -> None:
        self.connection = connection
        self.mode: Literal["extended", "stock"] = mode
        self.firmware = firmware
        self.protocol = protocol
        self.chip = chip
        self.pins: Mapping[int, PinInfo] = MappingProxyType(dict(pins))
        self.buses: list[BusInfo] = list(buses)
        self.limits: Mapping[str, int] = MappingProxyType(dict(limits))
        self.plugins_available: Mapping[str, PluginInfo] = MappingProxyType(dict(plugins_available))
        self.stats = stats
        self._half_classes = check_halves(plugin_halves)
        self.timeout = timeout
        self._pin_kinds: dict[int, str] = {}  # what this client last set each pin up as
        self._pwm_channels: dict[int, int] = {}  # pin -> LEDC channel named in PWM mode
        self._stock_sensors = stock_sensors  # stock: sonar/DHT allowed (connect(stock_sensors=True))
        self._sensors_sent = {"sonar": 0, "dht": 0}  # stock: sensors written this session
        self._ow_init_sent = False  # stock: a 1-wire init written this session
        self._chains = ChainEngine(connection, stats, extended=mode == "extended")
        self._events = EventHub(stats, only_configured=mode == "stock")
        self._halves: dict[str, PluginHalf] = {}
        self._halves_by_slot: dict[int, PluginHalf] = {}
        if mode == "extended":
            self._bind_halves()
        connection.add_close_callback(self._events.close)
        connection.add_close_callback(self._fail_requests)
        connection.set_router(self._route)

    # --- plugins

    def plugin(self, name: str) -> PluginHalf:
        """The client half bound to the board's plugin `name`, created once per board.

        Raises `NotSupported` in stock mode, when the board has no plugin `name`, or when it has the plugin but no half
        of that name was registered; `IncompatiblePlugin` when the registered half's major differs from the board's.
        """
        if self.mode != "extended":
            raise NotSupported("plugins need extended mode")
        info = self.plugins_available.get(name)
        if info is None:
            raise NotSupported(f"the board has no plugin {name!r}")
        cls = self._half_classes.get(name)
        if cls is None:
            raise NotSupported(f"no client half for plugin {name!r}: pass it to connect(plugins=[...])")
        if cls.major != info.major:
            raise IncompatiblePlugin(
                f"plugin {name!r} is {info.major}.{info.minor} on the board, the half is {cls.major}.x"
            )
        return self._halves[name]

    def _bind_halves(self) -> None:
        for name, cls in self._half_classes.items():
            info = self.plugins_available.get(name)
            if info is None or info.major != cls.major:
                continue
            half = cls(self, info.slot)
            self._halves[name] = half
            self._halves_by_slot[info.slot] = half
            self._events.subscribe(("plugin", info.slot), lambda e, h=half: h.on_report(e.sub, e.data))

    def _fail_requests(self) -> None:
        for half in self._halves.values():
            half._fail_requests(ConnectionLost("connection closed while a plugin request was pending"))

    # --- chains

    def chain(self) -> Chain:
        """An empty chain."""
        return Chain(self)

    def pin(self, gpio: int) -> Chain:
        return Chain(self).pin(gpio)

    def i2c(self, bus: int = 0) -> Chain:
        return Chain(self).i2c(bus)

    def onewire(self, bus: int = 0) -> Chain:
        return Chain(self).onewire(bus)

    # --- events

    def on_digital(self, pin: int, callback: Callable[[DigitalEvent], object]) -> Callable[[], None]:
        return self._events.subscribe(("digital", pin), callback)

    def on_analog(self, pin: int, callback: Callable[[AnalogEvent], object]) -> Callable[[], None]:
        return self._events.subscribe(("analog", pin), callback)

    def on_sonar(self, trig: int, callback: Callable[[SonarEvent], object]) -> Callable[[], None]:
        return self._events.subscribe(("sonar", trig), callback)

    def on_dht(self, pin: int, callback: Callable[[DhtEvent], object]) -> Callable[[], None]:
        return self._events.subscribe(("dht", pin), callback)

    def digital_changes(self, pin: int) -> EventStream[DigitalEvent]:
        return self._events.stream(("digital", pin))

    def analog_changes(self, pin: int) -> EventStream[AnalogEvent]:
        return self._events.stream(("analog", pin))

    def sonar_changes(self, trig: int) -> EventStream[SonarEvent]:
        return self._events.stream(("sonar", trig))

    def dht_changes(self, pin: int) -> EventStream[DhtEvent]:
        return self._events.stream(("dht", pin))

    # --- internals used by chains

    def _route(self, rid: int, payload: bytes) -> None:
        if rid == wire.RPT.PLUGIN_REPORT and self._halves_by_slot:
            report = wire.decode_plugin_report(payload)
            half = self._halves_by_slot.get(report.slot)
            if half is not None and half._take(report.sub, report.data):
                return
        if not self._chains.handle(rid, payload):
            self._events.handle(rid, payload, asyncio.get_running_loop().time())

    def _set_pin_kind(self, gpio: int, kind: str | None) -> Callable[[], None]:
        """Record what `gpio` is set up as; returns the undo (puts back the earlier kind unless it changed again)."""
        before = self._pin_kinds.get(gpio)
        if kind is None:
            self._pin_kinds.pop(gpio, None)
        else:
            self._pin_kinds[gpio] = kind

        def undo() -> None:
            if self._pin_kinds.get(gpio) != kind:
                return
            if before is None:
                self._pin_kinds.pop(gpio, None)
            else:
                self._pin_kinds[gpio] = before

        return undo

    def _stock_ow_gate(self, op: Any) -> None:
        """Stock: refuse a 1-wire op before an init was written (upstream reboots on a null OneWire)."""
        if not self._ow_init_sent:
            raise NotSupported(
                f"1-wire {op.name}() before init: stock firmware dereferences a null OneWire and reboots"
            )

    def _stock_ow_init(self, op: Any) -> Callable[[], None]:
        """Stock: record the session's one 1-wire init; a second raises `ValueError` (upstream leaks a OneWire)."""
        if self._ow_init_sent:
            raise ValueError("1-wire init() already sent this session: stock firmware leaks a OneWire on every init")
        self._ow_init_sent = True

        def undo() -> None:
            self._ow_init_sent = False

        return undo

    def _stock_sensor_count(self, op: Any) -> Callable[[], None]:
        """Stock: count a sonar/DHT written this session; past `STOCK_SENSOR_MAX` of a kind raises `ValueError`."""
        kind = op.name
        if self._sensors_sent[kind] >= STOCK_SENSOR_MAX:
            raise ValueError(
                f"at most {STOCK_SENSOR_MAX} {kind} sensors per stock session: the firmware's array overflows"
            )
        self._sensors_sent[kind] += 1

        def undo() -> None:
            self._sensors_sent[kind] -= 1

        return undo

    @property
    def _pwm_limit(self) -> int:
        """Stock firmware takes the client's channel (16 on the ESP32); our server picks its own: only its limit."""
        if self.mode == "stock":
            return PWM_CHANNELS
        return self.limits.get("pwm_channels", 256)

    def _pwm_allocate(self, gpio: int) -> tuple[int, Callable[[], None] | None]:
        """The channel of `gpio`, allocating the lowest free one; the undo frees a new allocation."""
        if gpio in self._pwm_channels:
            return self._pwm_channels[gpio], None
        free = sorted(set(range(self._pwm_limit)) - set(self._pwm_channels.values()))
        if not free:
            raise TelemetrixError(f"no free PWM channel: all {self._pwm_limit} are in use")
        channel = free[0]
        self._pwm_channels[gpio] = channel

        def undo() -> None:
            if self._pwm_channels.get(gpio) == channel:
                del self._pwm_channels[gpio]

        return channel, undo

    def _pwm_release(self, gpio: int) -> Callable[[], None] | None:
        """Free the channel of `gpio`; the undo takes it back when nobody took it meanwhile."""
        channel = self._pwm_channels.pop(gpio, None)
        if channel is None:
            return None

        def undo() -> None:
            if gpio not in self._pwm_channels and channel not in self._pwm_channels.values():
                self._pwm_channels[gpio] = channel

        return undo

    def _forget_pins(self) -> Callable[[], None]:
        """A RESET: no pin kinds, PWM channels or configured pins any more; the undo merges them back."""
        kinds, channels = dict(self._pin_kinds), dict(self._pwm_channels)
        self._pin_kinds.clear()
        self._pwm_channels.clear()
        undo_configured = self._events.forget_configured()

        def undo() -> None:
            for gpio, kind in kinds.items():
                self._pin_kinds.setdefault(gpio, kind)
            for gpio, channel in channels.items():
                if gpio not in self._pwm_channels and channel not in self._pwm_channels.values():
                    self._pwm_channels[gpio] = channel
            undo_configured()

        return undo

    @property
    def closed(self) -> bool:
        return self.connection.closed

    async def close(self) -> None:
        """Stop the reader and the event dispatch, close the socket; pending awaits fail with `ConnectionLost`.

        Idempotent; iterators end."""
        await self.connection.close()
        await self._events.aclose()

    async def __aenter__(self) -> Board:
        return self

    async def __aexit__(self, *exc: object) -> None:
        await self.close()

    def __repr__(self) -> str:
        return f"<Board {self.mode} firmware={self.firmware} pins={len(self.pins)} closed={self.closed}>"
