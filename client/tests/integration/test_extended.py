"""Our client in extended mode against the host server started with --test-plugin."""

from __future__ import annotations

import asyncio
import time
from collections.abc import Callable
from typing import TypeVar

import pytest
from conftest import LOCALHOST, Server
from echo_half import BUSY, Echo

from telemetrix_idf import BoardConnector, BusInfo, ChainError, EventStream, PluginInfo, connect
from telemetrix_idf.wire import Reason

PROFILE_PINS = {2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 35, 36}
ROM = bytes([0x28, 1, 2, 3, 4, 5, 6])
NO_PIN = 99

E = TypeVar("E")


def board_of(srv: Server, timeout: float = 2.0) -> BoardConnector:
    return connect(LOCALHOST, srv.port, timeout=timeout, plugins=(Echo,))


async def first(events: EventStream[E], match: Callable[[E], bool]) -> E:
    async for event in events:
        if match(event):
            return event
    raise AssertionError("the event stream ended")


def regs(reg: int, n: int) -> bytes:
    """The fake I2C devices' registers: regs[i] = i ^ 0xa5."""
    return bytes((reg + i) ^ 0xA5 for i in range(n))


# --- handshake and discovery -------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_handshake_extended_protocol_1_0(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        assert (board.mode, board.protocol) == ("extended", (1, 0))


@pytest.mark.asyncio
async def test_discovery_every_profile_pin(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        assert set(board.pins) == PROFILE_PINS


@pytest.mark.asyncio
async def test_discovery_named_pin_test_led(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        assert board.pins[36].named == ("test.led",)


@pytest.mark.asyncio
async def test_discovery_buses_0_and_1(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        assert board.buses == [
            BusInfo("i2c", 0, (21, 22), 100000),
            BusInfo("i2c", 1, (2, 15), 100000),
            BusInfo("one_wire", 0, None, None),
            BusInfo("one_wire", 1, (35,), None),
        ]


@pytest.mark.asyncio
async def test_discovery_test_echo_plugin_1_0(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        assert board.plugins_available == {"test.echo": PluginInfo(0, 1, 0, "test.echo")}


# --- pins, I2C, 1-wire -------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_digital_loopback_5_to_4_event(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        changes = board.digital_changes(4)
        await board.pin(4).input().pin(5).output().write(1)
        event = await asyncio.wait_for(first(changes, lambda e: e.value == 1), 2)
        assert event.pin == 4


@pytest.mark.asyncio
async def test_i2c_read_bus_0(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        assert await board.i2c(0).begin().read(0x76, 0x10, 3) == regs(0x10, 3)


@pytest.mark.asyncio
async def test_i2c_read_bus_1_through_on_bus(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        assert await board.i2c(1).begin().read(0x48, 0x20, 2) == regs(0x20, 2)


@pytest.mark.asyncio
async def test_onewire_search_bus_1_finds_the_rom(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        await board.onewire(1).init()
        roms = await board.onewire(1).search_all()
        assert [rom[:7] for rom in roms] == [ROM]


# --- chains ------------------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_concurrent_chains_error_lands_on_owner(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        bad, good = await asyncio.gather(
            board.pin(NO_PIN).output().run(), board.i2c(0).begin().read(0x76, 1, 1).run(), return_exceptions=True
        )
        assert isinstance(bad, ChainError)
        assert good.values == [regs(1, 1)]


@pytest.mark.asyncio
async def test_abort_skips_the_rest_of_the_chain(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        with pytest.raises(ChainError) as err:
            await board.pin(NO_PIN).output().pin(5).output().write(1)
        assert [(r.status, r.reason) for r in err.value.results] == [
            ("refused", Reason.NO_SUCH_PIN),
            ("skipped", Reason.SKIPPED),
            ("skipped", Reason.SKIPPED),
        ]


@pytest.mark.asyncio
async def test_non_atomic_chain_runs_the_rest(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        with pytest.raises(ChainError) as err:
            await board.pin(NO_PIN).output().i2c(0).begin().read(0x76, 2, 1).atomic(False)
        assert [r.status for r in err.value.results] == ["refused", "done", "done"]


@pytest.mark.asyncio
async def test_timeout_late_reply_not_shifted_onto_next_chain(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        with pytest.raises(asyncio.TimeoutError):
            await board.plugin("test.echo").command(BUSY).run(timeout=0.01)
        assert await board.i2c(0).begin().read(0x76, 3, 2) == regs(3, 2)


@pytest.mark.asyncio
async def test_cancellation_next_chain_correct(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        task = asyncio.ensure_future(board.plugin("test.echo").command(BUSY).run())
        await asyncio.sleep(0.01)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert await board.i2c(0).begin().read(0x76, 4, 1) == regs(4, 1)


@pytest.mark.asyncio
async def test_slow_callback_does_not_delay_barriers(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        called = asyncio.Event()

        async def slow(event: object) -> None:
            called.set()
            await asyncio.sleep(1)

        board.on_digital(4, slow)
        await board.pin(4).input().pin(5).output().write(1).write(0).write(1)
        await asyncio.wait_for(called.wait(), 2)
        start = time.monotonic()
        await board.pin(17).output().write(1)
        assert time.monotonic() - start < 0.5
