"""Our client in stock mode against the host server started with --stock-only."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
from typing import TypeVar

import pytest
from conftest import LOCALHOST, Server

from telemetrix_idf import BoardConnector, EventStream, NotSupported, connect

ROM = bytes([0x28, 1, 2, 3, 4, 5, 6])

E = TypeVar("E")


def board_of(srv: Server) -> BoardConnector:
    return connect(LOCALHOST, srv.port, timeout=2.0)


async def first(events: EventStream[E], match: Callable[[E], bool] = lambda e: True) -> E:
    async for event in events:
        if match(event):
            return event
    raise AssertionError("the event stream ended")


@pytest.mark.asyncio
async def test_handshake_stock_mode(stock_server: Server) -> None:
    async with board_of(stock_server) as board:
        assert (board.mode, board.firmware[:2]) == ("stock", (3, 2))


@pytest.mark.asyncio
async def test_digital_loopback_5_to_4_event(stock_server: Server) -> None:
    async with board_of(stock_server) as board:
        changes = board.digital_changes(4)
        await board.pin(4).input().pin(5).output().write(1)
        event = await asyncio.wait_for(first(changes, lambda e: e.value == 1), 2)
        assert event.pin == 4


@pytest.mark.asyncio
async def test_pwm_duty_reaches_the_driver(stock_server: Server) -> None:
    async with board_of(stock_server) as board:
        await board.pin(18).pwm(1000.0, 8).duty(128)
        status = await asyncio.to_thread(stock_server.status)
        assert (status["ledc_attach"], status["ledc_duty"]) == (1, 1)


@pytest.mark.asyncio
async def test_analog_report_of_pin_32(stock_server: Server) -> None:
    async with board_of(stock_server) as board:
        changes = board.analog_changes(32)
        await board.pin(32).analog()
        event = await asyncio.wait_for(first(changes), 2)
        assert event.value == 2048


@pytest.mark.asyncio
async def test_i2c_read_bus_0(stock_server: Server) -> None:
    async with board_of(stock_server) as board:
        assert await board.i2c().begin(21, 22).read(0x76, 0x10, 2) == bytes([0x10 ^ 0xA5, 0x11 ^ 0xA5])


@pytest.mark.asyncio
async def test_dht_reading_of_pin_25(stock_server: Server) -> None:
    async with board_of(stock_server) as board:
        changes = board.dht_changes(25)
        await board.chain().dht(25)
        event = await asyncio.wait_for(first(changes), 3)
        assert (event.humidity, event.temperature) == (pytest.approx(65.2, abs=0.01), pytest.approx(35.1, abs=0.01))


@pytest.mark.asyncio
async def test_sonar_distance_100_cm(stock_server: Server) -> None:
    async with board_of(stock_server) as board:
        changes = board.sonar_changes(26)
        await board.chain().sonar(26, 27)
        event = await asyncio.wait_for(first(changes), 2)
        assert event.cm == 100


@pytest.mark.asyncio
async def test_onewire_search_bus_0_finds_the_rom(stock_server: Server) -> None:
    async with board_of(stock_server) as board:
        await board.onewire().init(16)
        roms = await board.onewire().search_all()
        assert [rom[:7] for rom in roms] == [ROM]


@pytest.mark.asyncio
async def test_reset_raises_not_supported(stock_server: Server) -> None:
    async with board_of(stock_server) as board:
        with pytest.raises(NotSupported):
            board.chain().reset()


@pytest.mark.asyncio
async def test_session_sends_nothing_extended_server_side(stock_server: Server) -> None:
    async with board_of(stock_server) as board:
        await board.pin(18).pwm(1000.0, 8).duty(128)
        with pytest.raises(NotSupported):
            board.chain().reset()
        status = await asyncio.to_thread(stock_server.status)
        counters = {k: status[k] for k in ("extended", "protocol_violations", "ignored_commands", "error_reports")}
        assert counters == {"extended": 0, "protocol_violations": 0, "ignored_commands": 0, "error_reports": 0}
