"""Input events: digital, analog, sonar and DHT reports turned into events for callbacks and async iterators.

The reader task only decodes a report and appends the event to the dispatch queue; it never runs or awaits a
callback. One dispatch task per board takes events from the queue in arrival order and hands each to the callbacks
(sync or async; an async one is awaited before the next event) and the iterators subscribed to its pin. The queue
holds `max_queue` events (1000): when it is full the oldest event is dropped and counted in
`board.stats.dropped_events`. Each iterator has a queue of the same size with the same rule. A callback that raises
is logged and the others still run.

In stock mode the board passes only reports for pins this session configured (`configure`), since upstream keeps
reporting pins an earlier session set up. Every event carries `timestamp`, the loop's `time()` when the reader got the
report. PLUGIN_REPORTs become `PluginEvent`s keyed by slot; the board subscribes each bound plugin half (plugins.py).
"""

from __future__ import annotations

import asyncio
import contextlib
import inspect
import logging
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Generic, Literal, TypeVar

from . import wire

if TYPE_CHECKING:
    from .board import Stats

__all__ = ["AnalogEvent", "DhtEvent", "DigitalEvent", "Event", "EventHub", "EventStream", "PluginEvent", "SonarEvent"]

log = logging.getLogger("telemetrix_idf")

MAX_QUEUE = 1000

Kind = Literal["digital", "analog", "sonar", "dht", "plugin"]
Key = tuple[Kind, int]


@dataclass(frozen=True)
class DigitalEvent:
    pin: int
    value: int
    timestamp: float


@dataclass(frozen=True)
class AnalogEvent:
    pin: int
    value: int
    timestamp: float


@dataclass(frozen=True)
class SonarEvent:
    trigger: int
    cm: int
    timestamp: float


@dataclass(frozen=True)
class DhtEvent:
    """A reading (`humidity`, `temperature`, `error` None) or a failed read (`error` 1 checksum, 2 timeout)."""

    pin: int
    humidity: float | None
    temperature: float | None
    error: int | None
    timestamp: float


@dataclass(frozen=True)
class PluginEvent:
    slot: int
    sub: int
    data: bytes
    timestamp: float


Event = DigitalEvent | AnalogEvent | SonarEvent | DhtEvent | PluginEvent
E = TypeVar("E", DigitalEvent, AnalogEvent, SonarEvent, DhtEvent, PluginEvent)
Callback = Callable[[Any], object]


class EventStream(Generic[E]):
    """Async iterator over one pin's events, from its creation on. Ends when closed or when the connection closes."""

    def __init__(self, hub: EventHub, key: Key) -> None:
        self._hub = hub
        self._key = key
        self._queue: deque[E] = deque()
        self._wake = asyncio.Event()
        self._closed = False

    def __aiter__(self) -> EventStream[E]:
        return self

    async def __anext__(self) -> E:
        while not self._queue:
            if self._closed:
                raise StopAsyncIteration
            self._wake.clear()
            await self._wake.wait()
        return self._queue.popleft()

    async def aclose(self) -> None:
        """Unsubscribe; events already queued can still be read, then iteration ends."""
        self._end()

    async def __aenter__(self) -> EventStream[E]:
        return self

    async def __aexit__(self, *exc: object) -> None:
        await self.aclose()

    def _put(self, event: E) -> None:
        if len(self._queue) >= self._hub.max_queue:
            self._queue.popleft()
            self._hub.stats.dropped_events += 1
        self._queue.append(event)
        self._wake.set()

    def _end(self) -> None:
        if not self._closed:
            self._closed = True
            self._hub._streams.get(self._key, set()).discard(self)
            self._wake.set()


class EventHub:
    """Subscriptions and the dispatch task of one board. Built by `Board`."""

    def __init__(self, stats: Stats, *, only_configured: bool, max_queue: int = MAX_QUEUE) -> None:
        self.stats = stats
        self.max_queue = max_queue
        self._only_configured = only_configured
        self._configured: set[Key] = set()
        self._callbacks: dict[Key, list[Callback]] = {}
        self._streams: dict[Key, set[EventStream[Any]]] = {}
        self._queue: deque[tuple[Key, Event]] = deque()
        self._wake = asyncio.Event()
        self._closed = False
        self._task = asyncio.get_running_loop().create_task(self._run(), name="telemetrix-events")

    def configure(self, key: Key) -> Callable[[], None] | None:
        """Mark a pin as set up in this session (stock mode passes only these pins' reports); returns the undo."""
        if key in self._configured:
            return None
        self._configured.add(key)
        return lambda: self._configured.discard(key)

    def forget_configured(self) -> Callable[[], None]:
        """No pin is set up any more (a RESET); returns the undo, which adds the forgotten pins back."""
        forgotten = set(self._configured)
        self._configured.clear()
        return lambda: self._configured.update(forgotten)

    def subscribe(self, key: Key, callback: Callback) -> Callable[[], None]:
        """Call `callback(event)` for each event of `key`; returns the unsubscribe callable (idempotent)."""
        callbacks = self._callbacks.setdefault(key, [])
        callbacks.append(callback)
        done = False

        def unsubscribe() -> None:
            nonlocal done
            if not done:
                done = True
                callbacks.remove(callback)

        return unsubscribe

    def stream(self, key: Key) -> EventStream[Any]:
        """A stream of `key`'s events; one created after the connection closed ends at once."""
        s: EventStream[Any] = EventStream(self, key)
        if self._closed:
            s._closed = True
            return s
        self._streams.setdefault(key, set()).add(s)
        return s

    def handle(self, rid: int, payload: bytes, now: float) -> bool:
        """Turn an event report into an event; False when `rid` is not an event report."""
        event: Event
        if rid == wire.RPT.DIGITAL_REPORT:
            d = wire.decode_digital(payload)
            key: Key = ("digital", d.pin)
            event = DigitalEvent(d.pin, d.value, now)
        elif rid == wire.RPT.ANALOG_REPORT:
            a = wire.decode_analog(payload)
            key, event = ("analog", a.pin), AnalogEvent(a.pin, a.value, now)
        elif rid == wire.RPT.SONAR_DISTANCE:
            s = wire.decode_sonar(payload)
            key, event = ("sonar", s.trigger_pin), SonarEvent(s.trigger_pin, s.cm, now)
        elif rid == wire.RPT.DHT_REPORT:
            r = wire.decode_dht(payload)
            if isinstance(r, wire.DhtData):
                event = DhtEvent(r.pin, r.humidity, r.temperature, None, now)
            else:
                event = DhtEvent(r.pin, None, None, r.code, now)
            key = ("dht", r.pin)
        elif rid == wire.RPT.PLUGIN_REPORT:
            p = wire.decode_plugin_report(payload)
            key, event = ("plugin", p.slot), PluginEvent(p.slot, p.sub, p.data, now)
        else:
            return False
        if self._only_configured and key[0] != "plugin" and key not in self._configured:
            return True
        if not self._callbacks.get(key) and not self._streams.get(key):
            return True
        if len(self._queue) >= self.max_queue:
            self._queue.popleft()
            self.stats.dropped_events += 1
        self._queue.append((key, event))
        self._wake.set()
        return True

    async def _run(self) -> None:
        while True:
            while not self._queue:
                self._wake.clear()
                await self._wake.wait()
            key, event = self._queue.popleft()
            for s in list(self._streams.get(key, ())):
                s._put(event)
            for cb in list(self._callbacks.get(key, ())):
                try:
                    result = cb(event)
                    if inspect.isawaitable(result):
                        await result
                except Exception:
                    log.exception("event callback failed")

    def close(self) -> None:
        """Stop the dispatch task and end every iterator (connection closed)."""
        self._closed = True
        self._task.cancel()
        for streams in list(self._streams.values()):
            for s in list(streams):
                s._end()

    async def aclose(self) -> None:
        self.close()
        if self._task is not asyncio.current_task():
            with contextlib.suppress(asyncio.CancelledError):
                await self._task
