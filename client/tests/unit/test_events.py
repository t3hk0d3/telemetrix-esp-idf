"""client: events (events.py) through the board's dispatch task."""

from __future__ import annotations

import asyncio
import struct
from collections.abc import AsyncIterator, Callable
from contextlib import asynccontextmanager
from typing import Any

import pytest
from fake_ext_server import FakeExtServer
from fake_stock_server import FakeStockServer

from telemetrix_idf import Board
from telemetrix_idf.connection import connect
from telemetrix_idf.events import AnalogEvent, DhtEvent, DigitalEvent, SonarEvent


@asynccontextmanager
async def ext() -> AsyncIterator[tuple[FakeExtServer, Board]]:
    async with FakeExtServer() as srv, connect("127.0.0.1", srv.port, timeout=1.0) as board:
        yield srv, board


@asynccontextmanager
async def stock(stock_sensors: bool = False, **kw: Any) -> AsyncIterator[tuple[FakeStockServer, Board]]:
    async with (
        FakeStockServer(**kw) as srv,
        connect("127.0.0.1", srv.port, timeout=1.0, stock_sensors=stock_sensors) as board,
    ):
        yield srv, board


def digital(pin: int, value: int) -> bytes:
    return bytes([3, 2, pin, value])


def analog(pin: int, value: int) -> bytes:
    return bytes([4, 3, pin, value >> 8, value & 0xFF])


async def _until(cond: Callable[[], object], timeout: float = 1.0) -> None:
    for _ in range(int(timeout / 0.005)):
        if cond():
            return
        await asyncio.sleep(0.005)
    raise AssertionError("condition not met in time")


# --- a digital report for a configured pin -----------------------------------------------------------------------


@pytest.mark.asyncio
async def test_digital_report_callback_gets_pin_value_timestamp() -> None:
    got: list[DigitalEvent] = []
    async with stock() as (srv, board):
        await board.pin(4).input()
        board.on_digital(4, got.append)
        before = asyncio.get_running_loop().time()
        srv.push(digital(4, 1))
        await _until(lambda: got)
        assert (got[0].pin, got[0].value) == (4, 1)
        assert before <= got[0].timestamp <= asyncio.get_running_loop().time()


@pytest.mark.asyncio
async def test_digital_report_iterator_gets_event() -> None:
    async with stock() as (srv, board):
        await board.pin(4).input()
        changes = board.digital_changes(4)
        srv.push(digital(4, 1) + digital(4, 0))
        first = await asyncio.wait_for(changes.__anext__(), 1)
        second = await asyncio.wait_for(changes.__anext__(), 1)
        assert [(first.pin, first.value), (second.pin, second.value)] == [(4, 1), (4, 0)]
        await changes.aclose()


@pytest.mark.asyncio
async def test_digital_report_other_pin_not_delivered() -> None:
    got: list[DigitalEvent] = []
    async with ext() as (srv, board):
        board.on_digital(4, got.append)
        srv.push(digital(5, 1) + digital(4, 1))
        await _until(lambda: got)
        assert [e.pin for e in got] == [4]


@pytest.mark.asyncio
async def test_unsubscribe_stops_callback() -> None:
    got: list[DigitalEvent] = []
    async with ext() as (srv, board):
        unsubscribe = board.on_digital(4, got.append)
        srv.push(digital(4, 1))
        await _until(lambda: got)
        unsubscribe()
        srv.push(digital(4, 0))
        await asyncio.sleep(0.02)
        assert len(got) == 1


@pytest.mark.asyncio
async def test_async_callback_awaited() -> None:
    got: list[int] = []

    async def cb(event: DigitalEvent) -> None:
        await asyncio.sleep(0)
        got.append(event.value)

    async with ext() as (srv, board):
        board.on_digital(4, cb)
        srv.push(digital(4, 1))
        await _until(lambda: got)
        assert got == [1]


@pytest.mark.asyncio
async def test_failing_callback_does_not_stop_dispatch() -> None:
    got: list[int] = []

    def bad(event: DigitalEvent) -> None:
        raise RuntimeError("callback bug")

    async with ext() as (srv, board):
        board.on_digital(4, bad)
        board.on_digital(4, lambda e: got.append(e.value))
        srv.push(digital(4, 1) + digital(4, 0))
        await _until(lambda: len(got) == 2)
        assert got == [1, 0]


@pytest.mark.asyncio
async def test_iterator_ends_on_close() -> None:
    async with ext() as (_, board):
        changes = board.digital_changes(4)
        await board.close()
        with pytest.raises(StopAsyncIteration):
            await asyncio.wait_for(changes.__anext__(), 1)


@pytest.mark.asyncio
async def test_iterator_created_after_close_ends_at_once() -> None:
    async with ext() as (_, board):
        await board.close()
        changes = board.digital_changes(4)
        with pytest.raises(StopAsyncIteration):
            await asyncio.wait_for(changes.__anext__(), 1)


@pytest.mark.asyncio
async def test_iterator_created_after_eof_ends_at_once() -> None:
    async with ext() as (srv, board):
        srv.drop()
        await _until(lambda: board.closed)
        changes = board.analog_changes(34)
        with pytest.raises(StopAsyncIteration):
            await asyncio.wait_for(changes.__anext__(), 1)


# --- the other report kinds --------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_analog_report_delivered() -> None:
    async with ext() as (srv, board):
        changes = board.analog_changes(34)
        srv.push(analog(34, 4095))
        event = await asyncio.wait_for(changes.__anext__(), 1)
        assert (type(event), event.pin, event.value) == (AnalogEvent, 34, 4095)


@pytest.mark.asyncio
async def test_sonar_report_delivered() -> None:
    got: list[SonarEvent] = []
    async with ext() as (srv, board):
        board.on_sonar(18, got.append)
        srv.push(bytes([4, 10, 18, 0x01, 0x02]))
        await _until(lambda: got)
        assert (got[0].trigger, got[0].cm) == (18, 0x102)


@pytest.mark.asyncio
async def test_dht_report_delivered() -> None:
    async with ext() as (srv, board):
        changes = board.dht_changes(21)
        srv.push(bytes([11, 11, 0, 21]) + struct.pack("<ff", 40.5, 21.25))
        event = await asyncio.wait_for(changes.__anext__(), 1)
        assert (event.pin, event.humidity, event.temperature, event.error) == (21, 40.5, 21.25, None)


@pytest.mark.asyncio
async def test_dht_error_delivered() -> None:
    got: list[DhtEvent] = []
    async with ext() as (srv, board):
        board.on_dht(21, got.append)
        srv.push(bytes([4, 11, 1, 21, 2]))
        await _until(lambda: got)
        assert (got[0].humidity, got[0].temperature, got[0].error) == (None, None, 2)


# --- stock mode: unconfigured pins -------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_stock_unconfigured_pin_report_ignored() -> None:
    got: list[DigitalEvent] = []
    async with stock() as (srv, board):
        await board.pin(5).input()
        board.on_digital(4, got.append)
        board.on_digital(5, got.append)
        srv.push(digital(4, 1) + digital(5, 1))
        await _until(lambda: got)
        await asyncio.sleep(0.02)
        assert [e.pin for e in got] == [5]


@pytest.mark.asyncio
async def test_stock_sonar_and_dht_configured_by_their_chains() -> None:
    got: list[object] = []
    async with stock(stock_sensors=True) as (srv, board):
        board.on_sonar(18, got.append)
        board.on_dht(21, got.append)
        srv.push(bytes([4, 10, 18, 0, 5]) + bytes([4, 11, 1, 21, 2]))
        await asyncio.sleep(0.02)
        await board.chain().sonar(18, 19).dht(21)
        srv.push(bytes([4, 10, 18, 0, 6]) + bytes([4, 11, 1, 21, 1]))
        await _until(lambda: len(got) == 2)
        assert [type(e) for e in got] == [SonarEvent, DhtEvent]


# --- a slow callback ---------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_slow_callback_barrier_still_completes() -> None:
    gate = asyncio.Event()

    async def slow(event: DigitalEvent) -> None:
        await gate.wait()

    async with ext() as (srv, board):
        board.on_digital(4, slow)
        srv.push(digital(4, 1))
        await asyncio.sleep(0.01)
        assert await asyncio.wait_for(board.pin(4).output(), 1) is None
        gate.set()


@pytest.mark.asyncio
async def test_slow_callback_events_delivered_in_order() -> None:
    gate = asyncio.Event()
    got: list[int] = []

    async def slow(event: AnalogEvent) -> None:
        await gate.wait()
        got.append(event.value)

    async with ext() as (srv, board):
        board.on_analog(34, slow)
        srv.push(b"".join(analog(34, v) for v in range(10)))
        await asyncio.sleep(0.02)
        gate.set()
        await _until(lambda: len(got) == 10)
        assert got == list(range(10))


# --- a full dispatch queue ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_full_queue_oldest_event_dropped() -> None:
    gate = asyncio.Event()
    got: list[int] = []

    async def slow(event: AnalogEvent) -> None:
        await gate.wait()
        got.append(event.value)

    async with ext() as (srv, board):
        board.on_analog(34, slow)
        srv.push(analog(34, 0))
        await asyncio.sleep(0.02)  # event 0 taken by the dispatch task, which now waits on the gate
        srv.push(b"".join(analog(34, v) for v in range(1, 1002)))
        await _until(lambda: board.stats.dropped_events == 1)
        gate.set()
        await _until(lambda: len(got) == 1001)
        assert got == [0, *range(2, 1002)]


@pytest.mark.asyncio
async def test_full_queue_dropped_counted() -> None:
    gate = asyncio.Event()

    async def slow(event: AnalogEvent) -> None:
        await gate.wait()

    async with ext() as (srv, board):
        board.on_analog(34, slow)
        srv.push(analog(34, 0))
        await asyncio.sleep(0.02)
        srv.push(b"".join(analog(34, v) for v in range(1, 1004)))
        await _until(lambda: board.stats.dropped_events == 3)
        await asyncio.sleep(0.02)
        gate.set()
        assert board.stats.dropped_events == 3
