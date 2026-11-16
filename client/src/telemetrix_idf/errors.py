"""Exceptions of the client. Every one derives from `TelemetrixError`."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .chain import CommandResult

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
    """A chain had a refused or skipped command. `results` holds every command's `CommandResult` (chain.py)."""

    def __init__(self, results: list[CommandResult]) -> None:
        bad = [r for r in results if r.status != "done"]
        first = f"; first: {bad[0].op} {bad[0].status} {bad[0].reason!r}" if bad else ""
        super().__init__(f"chain refused: {len(bad)} of {len(results)} commands not done{first}")
        self.results = results
