"""The board model a connection produces: mode, firmware, discovered pins, buses, limits and plugins."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from types import MappingProxyType
from typing import TYPE_CHECKING, Any, Literal

from .discovery import BusInfo, ChipInfo, PinInfo, PluginInfo

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


class Board:
    """One connected server. Built by `connect()`; `close()` ends the connection.

    In stock mode `chip` is None, `pins` come from the upstream client's lists (stock.py), `buses` are bus 0 I2C and
    1-wire with client-chosen pins, and `limits` and `plugins_available` are empty.
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

    @property
    def closed(self) -> bool:
        return self.connection.closed

    async def close(self) -> None:
        """Stop the reader and close the socket; pending awaits fail with `ConnectionLost`. Idempotent."""
        await self.connection.close()

    async def __aenter__(self) -> Board:
        return self

    async def __aexit__(self, *exc: object) -> None:
        await self.close()

    def __repr__(self) -> str:
        return f"<Board {self.mode} firmware={self.firmware} pins={len(self.pins)} closed={self.closed}>"
