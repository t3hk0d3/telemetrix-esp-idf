"""The connection: socket, reader task, report routing, and `connect()` with the handshake and discovery.

The reader task feeds a `PacketParser` and offers each report first to the pending "expect" waiters, in registration
order (the first whose matcher returns a value consumes the report), then to the router (`set_router`, default: drop).
Chains and events (chain.py, events.py) install the router. A `ProtocolError` raised by a matcher or the router is
logged and counted in `stats.protocol_errors`; the reader keeps going. On EOF or a socket error every pending waiter
fails with `ConnectionLost` and the connection is closed.

Handshake: exactly one packet, `GET_FIRMWARE_VERSION [0xA7, 1, 0]`; every report before the FIRMWARE report is
dropped (no router is installed yet). Discovery reads every CAPABILITIES page with direct GET_CAPABILITIES requests
before `connect()` returns.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
from collections.abc import Callable, Generator, Sequence
from typing import Any, TypeVar

from . import wire
from .board import Board, Stats
from .discovery import parse_discovery
from .errors import ConnectionLost, HandshakeTimeout, IncompatibleFirmware, ProtocolError
from .stock import STOCK_BUSES, check_stock_firmware, stock_pins

__all__ = ["BoardConnector", "Connection", "Router", "connect"]

log = logging.getLogger("telemetrix_idf")

T = TypeVar("T")
Router = Callable[[int, bytes], None]
Matcher = Callable[[int, bytes], Any]

READ_CHUNK = 4096


def _drop(rid: int, payload: bytes) -> None:
    return None


class Connection:
    """Owns the stream pair and the reader task. Built by `connect()`."""

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, stats: Stats) -> None:
        self._reader = reader
        self._writer = writer
        self._stats = stats
        self._parser = wire.PacketParser()
        self._router: Router = _drop
        self._waiters: list[tuple[Matcher, asyncio.Future[Any]]] = []
        self._close_callbacks: list[Callable[[], None]] = []
        self._closed = False
        self._numbered = False
        self._seq = 0
        self._task = asyncio.get_running_loop().create_task(self._read_loop(), name="telemetrix-reader")

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def seq(self) -> int:
        """The number the next written packet gets (extended mode; stays 0 in stock mode)."""
        return self._seq

    def start_numbering(self) -> None:
        """From now on every written packet is numbered, the first 0 (extended mode, after the handshake reply)."""
        self._numbered = True
        self._seq = 0

    def set_router(self, router: Router | None) -> None:
        """Route every report no waiter consumed to `router(id, payload)`; None restores the default (drop)."""
        self._router = router or _drop

    def add_close_callback(self, callback: Callable[[], None]) -> None:
        """Call `callback()` once when the connection closes (EOF, error or `close()`)."""
        self._close_callbacks.append(callback)

    def expect(self, match: Callable[[int, bytes], T | None]) -> asyncio.Future[T]:
        """Register a waiter now; the future gets the first non-None `match(id, payload)` of a later report.

        Register before writing the request. Cancelling the future (or a `wait_for` around it) unregisters it.
        """
        fut: asyncio.Future[T] = asyncio.get_running_loop().create_future()
        if self._closed:
            fut.set_exception(ConnectionLost("connection closed"))
            return fut
        entry = (match, fut)
        self._waiters.append(entry)
        fut.add_done_callback(lambda _: self._waiters.remove(entry) if entry in self._waiters else None)
        return fut

    def write(self, data: bytes) -> None:
        """Write one or more framed packets in a single `writer.write`; numbered packets advance `seq`."""
        if self._closed:
            raise ConnectionLost("connection closed")
        if self._numbered:
            pos = 0
            while pos < len(data):
                pos += 1 + data[pos]
                self._seq = (self._seq + 1) & 0xFFFF
        self._writer.write(data)

    async def close(self) -> None:
        """Cancel the reader, close the writer, fail pending waiters with `ConnectionLost`. Idempotent."""
        if not self._task.done() and self._task is not asyncio.current_task():
            self._task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._task
        self._shutdown("connection closed")
        with contextlib.suppress(OSError, ConnectionError):
            await self._writer.wait_closed()

    def _shutdown(self, why: str) -> None:
        if self._closed:
            return
        self._closed = True
        for _, fut in list(self._waiters):
            if not fut.done():
                fut.set_exception(ConnectionLost(why))
        self._waiters.clear()
        self._writer.close()
        for cb in self._close_callbacks:
            try:
                cb()
            except Exception:
                log.exception("close callback failed")

    async def _read_loop(self) -> None:
        why = "connection closed by the server"
        try:
            while True:
                data = await self._reader.read(READ_CHUNK)
                if not data:
                    break
                for rid, payload in self._parser.feed(data):
                    self._dispatch(rid, payload)
                self._stats.skipped_bytes = self._parser.dropped
        except (OSError, ConnectionError) as e:
            why = f"connection lost: {e}"
        self._shutdown(why)

    def _dispatch(self, rid: int, payload: bytes) -> None:
        try:
            for match, fut in self._waiters:
                if fut.done():
                    continue
                result = match(rid, payload)
                if result is not None:
                    fut.set_result(result)
                    return
            self._router(rid, payload)
        except ProtocolError as e:
            self._stats.protocol_errors += 1
            log.warning("dropped report %d %s: %s", rid, payload.hex(), e)
        except Exception:
            log.exception("report %d handler failed", rid)


class BoardConnector:
    """What `connect()` returns: await it for a `Board`, or use it as `async with connect(...) as board`."""

    def __init__(self, opener: Callable[[], Any]) -> None:
        self._opener = opener
        self._board: Board | None = None

    def __await__(self) -> Generator[Any, None, Board]:
        return self._opener().__await__()

    async def __aenter__(self) -> Board:
        self._board = await self._opener()
        return self._board

    async def __aexit__(self, *exc: object) -> None:
        if self._board is not None:
            await self._board.close()


def connect(
    host: str,
    port: int = 31336,
    *,
    handshake_timeout: float = 3.0,
    timeout: float = 5.0,
    plugins: Sequence[type[Any]] = (),
) -> BoardConnector:
    """Connect, handshake and (extended mode) discover the board.

    Raises `HandshakeTimeout` when the probe gets no FIRMWARE report within `handshake_timeout` seconds,
    `IncompatibleFirmware` for an extension major other than 1 or stock firmware other than 3.x with x >= 2,
    `ConnectionLost` when the socket cannot be opened or closes, `asyncio.TimeoutError` when opening the socket or a
    discovery page takes longer than `timeout`, and `ProtocolError` for a malformed or refused discovery page. The
    socket is closed on every failure.
    `plugins` are the client plugin halves (plugins.py) bound to the board's plugins.
    """

    async def opener() -> Board:
        return await _open(host, port, handshake_timeout, timeout, plugins)

    return BoardConnector(opener)


async def _open(host: str, port: int, handshake_timeout: float, timeout: float, plugins: Sequence[type[Any]]) -> Board:
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout)
    except OSError as e:
        raise ConnectionLost(f"cannot connect to {host}:{port}: {e}") from e
    stats = Stats()
    conn = Connection(reader, writer, stats)
    try:
        return await _handshake(conn, stats, handshake_timeout, timeout, plugins)
    except BaseException:
        await conn.close()
        raise


def _match_firmware(rid: int, payload: bytes) -> wire.Firmware | None:
    return wire.decode_firmware(payload) if rid == wire.RPT.FIRMWARE_REPORT else None


async def _handshake(
    conn: Connection, stats: Stats, handshake_timeout: float, timeout: float, plugins: Sequence[type[Any]]
) -> Board:
    answer = conn.expect(_match_firmware)
    conn.write(wire.get_firmware_version(wire.EXT_VERSION))
    try:
        fw = await asyncio.wait_for(answer, handshake_timeout)
    except asyncio.TimeoutError:
        raise HandshakeTimeout(f"no firmware report within {handshake_timeout} s") from None
    firmware = (fw.major, fw.minor, fw.build)

    if fw.ext is None:
        check_stock_firmware(fw.major, fw.minor)
        conn.write(wire.enable_all_reports())
        return Board(
            conn,
            mode="stock",
            firmware=firmware,
            protocol=None,
            chip=None,
            pins=stock_pins(),
            buses=STOCK_BUSES,
            limits={},
            plugins_available={},
            stats=stats,
            plugin_halves=plugins,
        )

    major, minor = fw.ext
    if major != wire.EXT_VERSION[0]:
        raise IncompatibleFirmware(f"extension protocol {major}.{minor}, this client speaks {wire.EXT_VERSION[0]}.x")
    conn.start_numbering()
    found = parse_discovery(await _discover(conn, timeout))
    stats.unknown_records += found.unknown_records
    return Board(
        conn,
        mode="extended",
        firmware=firmware,
        protocol=(major, min(wire.EXT_VERSION[1], minor)),
        chip=found.chip,
        pins=found.pins,
        buses=found.buses,
        limits=found.limits,
        plugins_available=found.plugins,
        stats=stats,
        plugin_halves=plugins,
    )


def _page_matcher(page: int) -> Callable[[int, bytes], tuple[int, bytes] | None]:
    def match(rid: int, payload: bytes) -> tuple[int, bytes] | None:
        if rid == wire.RPT.CAPABILITIES and payload[:1] == bytes([page]):
            return rid, payload
        if rid == wire.RPT.ERROR and len(payload) >= 3 and payload[2] == wire.CMD.GET_CAPABILITIES:
            return rid, payload
        return None

    return match


async def _discover(conn: Connection, timeout: float) -> list[wire.CapabilitiesPage]:
    pages: list[wire.CapabilitiesPage] = []
    page, last = 0, 0
    while page <= last:
        answer = conn.expect(_page_matcher(page))
        conn.write(wire.get_capabilities(page))
        rid, payload = await asyncio.wait_for(answer, timeout)
        if rid == wire.RPT.ERROR:
            raise ProtocolError(f"GET_CAPABILITIES {page} refused: {wire.decode_error(payload)}")
        cp = wire.decode_capabilities(payload)
        if page == 0:
            last = cp.last_page
        pages.append(cp)
        page += 1
    return pages
