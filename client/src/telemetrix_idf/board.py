"""The board model a connection produces: mode, firmware, discovered pins, buses, limits and plugins."""

from __future__ import annotations

import asyncio
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from types import MappingProxyType
from typing import TYPE_CHECKING, Any, Literal

from .chain import PWM_CHANNELS, Chain, ChainEngine
from .discovery import BusInfo, ChipInfo, PinInfo, PluginInfo
from .errors import TelemetrixError
from .events import AnalogEvent, DhtEvent, DigitalEvent, EventHub, EventStream, SonarEvent

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
        self._plugin_halves = tuple(plugin_halves)  # bound to sessions by plugins.py (Task 12)
        self.timeout = timeout
        self._pin_kinds: dict[int, str] = {}  # what this client last set each pin up as
        self._pwm_channels: dict[int, int] = {}  # pin -> LEDC channel named in PWM mode
        self._chains = ChainEngine(connection, stats, extended=mode == "extended")
        self._events = EventHub(stats, only_configured=mode == "stock")
        connection.add_close_callback(self._events.close)
        connection.set_router(self._route)

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
        if not self._chains.handle(rid, payload):
            self._events.handle(rid, payload, asyncio.get_running_loop().time())

    def _set_pin_kind(self, gpio: int, kind: str | None) -> None:
        if kind is None:
            self._pin_kinds.pop(gpio, None)
        else:
            self._pin_kinds[gpio] = kind

    def _pwm_channel(self, gpio: int) -> int:
        if gpio in self._pwm_channels:
            return self._pwm_channels[gpio]
        free = sorted(set(range(PWM_CHANNELS)) - set(self._pwm_channels.values()))
        if not free:
            raise TelemetrixError(f"no free PWM channel: all {PWM_CHANNELS} are in use")
        self._pwm_channels[gpio] = free[0]
        return free[0]

    def _pwm_release(self, gpio: int) -> None:
        self._pwm_channels.pop(gpio, None)

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
