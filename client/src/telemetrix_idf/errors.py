"""Exceptions of the client. Every one derives from `TelemetrixError`."""

from __future__ import annotations

from typing import Any

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


class TelemetrixError(Exception):
    """Base of every error the client raises for a runtime condition.

    Programmer errors (a pin of 300, a payload that cannot fit a packet) are plain `ValueError`.
    """


class HandshakeTimeout(TelemetrixError):
    """The server did not answer the firmware probe in time."""


class IncompatibleFirmware(TelemetrixError):
    """The firmware or the extension protocol version is not one this client supports."""


class NotSupported(TelemetrixError):
    """The call needs extended mode, or a feature the connected server does not have."""


class ConnectionLost(TelemetrixError):
    """The connection closed while an operation was pending."""


class IncompatiblePlugin(TelemetrixError):
    """A plugin helper does not match the plugin the server runs in that slot."""


class ProtocolError(TelemetrixError, ValueError):
    """The server sent bytes that do not decode (short or inconsistent payload). Also a `ValueError`."""


class ChainError(TelemetrixError):
    """A chain had a refused command. `results` holds one entry per command, in order (`CommandResult`, chain.py)."""

    def __init__(self, results: list[Any]) -> None:
        super().__init__(f"chain refused: {len(results)} commands, see .results")
        self.results = results
