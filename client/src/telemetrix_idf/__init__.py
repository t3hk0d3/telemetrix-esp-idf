"""Asyncio client for the telemetrix-esp-idf server."""

from .board import Board, Stats
from .connection import BoardConnector, connect
from .discovery import BusInfo, Cap, ChipInfo, PinInfo, PluginInfo, SafeState
from .errors import (
    ChainError,
    ConnectionLost,
    HandshakeTimeout,
    IncompatibleFirmware,
    IncompatiblePlugin,
    NotSupported,
    ProtocolError,
    TelemetrixError,
)

__all__ = [
    "Board",
    "BoardConnector",
    "BusInfo",
    "Cap",
    "ChipInfo",
    "PinInfo",
    "PluginInfo",
    "SafeState",
    "Stats",
    "connect",
    "ChainError",
    "ConnectionLost",
    "HandshakeTimeout",
    "IncompatibleFirmware",
    "IncompatiblePlugin",
    "NotSupported",
    "ProtocolError",
    "TelemetrixError",
]
