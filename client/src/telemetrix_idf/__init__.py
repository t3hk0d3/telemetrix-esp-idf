"""Asyncio client for the telemetrix-esp-idf server."""

from .board import Board, Stats
from .chain import Chain, ChainResult, CommandResult
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
from .events import AnalogEvent, DhtEvent, DigitalEvent, EventStream, PluginEvent, SonarEvent
from .plugins import PluginHalf
from .wire import Reason

__all__ = [
    "AnalogEvent",
    "Board",
    "BoardConnector",
    "BusInfo",
    "Cap",
    "Chain",
    "ChainResult",
    "CommandResult",
    "DhtEvent",
    "DigitalEvent",
    "EventStream",
    "ChipInfo",
    "PinInfo",
    "PluginEvent",
    "PluginHalf",
    "PluginInfo",
    "Reason",
    "SafeState",
    "SonarEvent",
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
