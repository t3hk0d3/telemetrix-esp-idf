"""Asyncio client for the telemetrix-esp-idf server."""

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
    "ChainError",
    "ConnectionLost",
    "HandshakeTimeout",
    "IncompatibleFirmware",
    "IncompatiblePlugin",
    "NotSupported",
    "ProtocolError",
    "TelemetrixError",
]
