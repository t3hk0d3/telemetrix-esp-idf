"""Stock-mode rules (constraints "Client: stock mode") not covered by test_handshake/test_chain/test_events."""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import AsyncIterator, Callable
from contextlib import asynccontextmanager
from typing import Any

import pytest
from fake_ext_server import crc8
from fake_stock_server import FakeStockServer

from telemetrix_idf import Board, Chain, NotSupported, connect

EXTENDED_ONLY: dict[str, Callable[[Board], Any]] = {
    "i2c on bus 1": lambda board: board.i2c(1),
    "1-wire on bus 1": lambda board: board.onewire(1),
    "board reset": lambda board: board.chain().reset(),
    "plugin command": lambda board: board.chain().plugin(0, 0),
    "plugin half": lambda board: board.plugin("test.echo"),
}


@pytest.mark.asyncio
async def test_session_never_writes_reset() -> None:
    async with FakeStockServer() as srv:
        async with connect("127.0.0.1", srv.port, timeout=1.0) as board:
            await board.pin(4).output().write(1).pin(5).input()
        await srv.eof.wait()
        assert 20 not in [cmd for cmd, _ in srv.received]


@pytest.mark.asyncio
@pytest.mark.parametrize("call", EXTENDED_ONLY.values(), ids=EXTENDED_ONLY.keys())
async def test_extended_only_call_raises_not_supported(call: Callable[[Board], Any]) -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port, timeout=1.0) as board:
        with pytest.raises(NotSupported):
            call(board)


# --- stock firmware hazards: 1-wire before init, the init leak, sensor arrays ------------------------------------


@asynccontextmanager
async def stock(srv: FakeStockServer | None = None, **kw: Any) -> AsyncIterator[tuple[FakeStockServer, Board, int]]:
    """A stock board on `srv` (a fresh fake by default); the third value is where the chain bytes start."""
    async with contextlib.AsyncExitStack() as stack:
        if srv is None:
            srv = await stack.enter_async_context(FakeStockServer())
        start = len(srv.received)
        board = await stack.enter_async_context(connect("127.0.0.1", srv.port, timeout=1.0, **kw))
        for _ in range(200):  # probe, ENABLE_ALL_REPORTS
            if len(srv.received) >= start + 2:
                break
            await asyncio.sleep(0.005)
        yield srv, board, len(srv.raw)


OW_OPS: dict[str, Callable[[Chain], Chain]] = {
    "reset": lambda c: c.reset(),
    "read": lambda c: c.read(),
    "write": lambda c: c.write(0xCC),
    "select": lambda c: c.select(bytes(8)),
    "skip": lambda c: c.skip(),
    "reset_search": lambda c: c.reset_search(),
    "search": lambda c: c.search(),
}


async def _raw_session(srv: FakeStockServer, packets: bytes) -> None:
    reader, writer = await asyncio.open_connection("127.0.0.1", srv.port)
    writer.write(packets)
    await writer.drain()
    with contextlib.suppress(ConnectionError):
        await asyncio.wait_for(reader.read(), 1)
    writer.close()


@pytest.mark.asyncio
async def test_fake_onewire_op_before_init_crashes_the_board() -> None:
    async with FakeStockServer() as srv:
        await _raw_session(srv, bytes([1, 28]))
        assert srv.crashed


@pytest.mark.asyncio
async def test_fake_seventh_sonar_crashes_the_board() -> None:
    async with FakeStockServer() as srv:
        await _raw_session(srv, bytes([3, 12, 26, 27]) * 7)
        assert srv.crashed


@pytest.mark.asyncio
@pytest.mark.parametrize("op", OW_OPS.values(), ids=OW_OPS.keys())
async def test_onewire_op_before_init_raises_not_supported(op: Callable[[Chain], Chain]) -> None:
    async with stock() as (_, board, _):
        with pytest.raises(NotSupported, match="init"):
            op(board.onewire())


@pytest.mark.asyncio
@pytest.mark.parametrize("op", OW_OPS.values(), ids=OW_OPS.keys())
async def test_onewire_op_before_init_nothing_written(op: Callable[[Chain], Chain]) -> None:
    async with stock() as (srv, board, mark):
        with pytest.raises(NotSupported):
            await op(board.pin(4).output().onewire())
        await asyncio.sleep(0.02)
        assert (bytes(srv.raw[mark:]), srv.crashed) == (b"", False)


@pytest.mark.asyncio
async def test_onewire_search_all_before_init_raises_not_supported() -> None:
    async with stock() as (srv, board, mark):
        with pytest.raises(NotSupported):
            await board.onewire().search_all()
        await asyncio.sleep(0.02)
        assert (bytes(srv.raw[mark:]), srv.crashed) == (b"", False)


@pytest.mark.asyncio
async def test_onewire_init_built_not_sent_ops_still_refused() -> None:
    async with stock() as (srv, board, _):
        pending = board.onewire().init(16)
        with pytest.raises(NotSupported):
            board.onewire().reset()
        await pending
        assert not srv.crashed


@pytest.mark.asyncio
async def test_onewire_crc8_before_init_returns_crc() -> None:
    async with stock() as (srv, board, _):
        assert await board.onewire().crc8(bytes([0x28, 1, 2])) == crc8(bytes([0x28, 1, 2]))
        assert not srv.crashed


@pytest.mark.asyncio
async def test_onewire_init_then_op_in_one_chain_runs() -> None:
    async with stock() as (srv, board, _):
        assert await board.onewire().init(16).reset() is True
        assert not srv.crashed


@pytest.mark.asyncio
async def test_onewire_ops_after_sent_init_run() -> None:
    async with stock() as (srv, board, _):
        await board.onewire().init(16)
        assert await board.onewire().search_all() == []
        assert await board.onewire().skip().write(0xBE).read() == 0x5A
        assert not srv.crashed


@pytest.mark.asyncio
async def test_onewire_second_init_raises_value_error_nothing_written() -> None:
    async with stock() as (srv, board, _):
        await board.onewire().init(16)
        mark = len(srv.raw)
        with pytest.raises(ValueError, match="init"):
            await board.onewire().init(16)
        await asyncio.sleep(0.02)
        assert (bytes(srv.raw[mark:]), srv.ow_inits) == (b"", 1)


@pytest.mark.asyncio
async def test_onewire_second_init_in_one_chain_raises_value_error() -> None:
    async with stock() as (_, board, _):
        with pytest.raises(ValueError, match="init"):
            board.onewire().init(16).init(17)


@pytest.mark.asyncio
async def test_onewire_two_built_inits_second_send_raises_value_error() -> None:
    async with stock() as (srv, board, _):
        first, second = board.onewire().init(16), board.onewire().init(16).reset()
        await first
        mark = len(srv.raw)
        with pytest.raises(ValueError, match="init"):
            await second
        await asyncio.sleep(0.02)
        assert (bytes(srv.raw[mark:]), srv.ow_inits, srv.crashed) == (b"", 1, False)


SENSORS: dict[str, Callable[[Chain], Chain]] = {
    "sonar": lambda c: c.sonar(26, 27),
    "dht": lambda c: c.dht(25),
}


@pytest.mark.asyncio
@pytest.mark.parametrize("add", SENSORS.values(), ids=SENSORS.keys())
async def test_sensor_without_opt_in_raises_not_supported(add: Callable[[Chain], Chain]) -> None:
    async with stock() as (srv, board, mark):
        with pytest.raises(NotSupported, match="stock_sensors"):
            await add(board.chain())
        await asyncio.sleep(0.02)
        assert bytes(srv.raw[mark:]) == b""


@pytest.mark.asyncio
@pytest.mark.parametrize("add", SENSORS.values(), ids=SENSORS.keys())
async def test_sensor_opt_in_six_accepted(add: Callable[[Chain], Chain]) -> None:
    async with stock(stock_sensors=True) as (srv, board, _):
        for _ in range(3):
            await add(add(board.chain()))
        assert (srv.sonars + srv.dhts, srv.crashed) == (6, False)


@pytest.mark.asyncio
@pytest.mark.parametrize("add", SENSORS.values(), ids=SENSORS.keys())
async def test_sensor_opt_in_seventh_raises_value_error_nothing_written(add: Callable[[Chain], Chain]) -> None:
    async with stock(stock_sensors=True) as (srv, board, _):
        for _ in range(6):
            await add(board.chain())
        mark = len(srv.raw)
        with pytest.raises(ValueError, match="6"):
            await add(board.chain())
        await asyncio.sleep(0.02)
        assert (bytes(srv.raw[mark:]), srv.crashed) == (b"", False)


@pytest.mark.asyncio
async def test_sensor_opt_in_seventh_in_one_chain_raises_value_error() -> None:
    async with stock(stock_sensors=True) as (_, board, _):
        chain = board.chain()
        for _ in range(6):
            chain.sonar(26, 27)
        with pytest.raises(ValueError, match="6"):
            chain.sonar(26, 27)


@pytest.mark.asyncio
async def test_sensor_opt_in_built_chains_counted_on_send() -> None:
    async with stock(stock_sensors=True) as (srv, board, _):
        chains = [board.chain().dht(25) for _ in range(7)]
        for chain in chains[:6]:
            await chain
        with pytest.raises(ValueError, match="6"):
            await chains[6]
        assert (srv.dhts, srv.crashed) == (6, False)


@pytest.mark.asyncio
async def test_sensor_opt_in_cap_is_per_session() -> None:
    async with FakeStockServer() as srv:
        for _ in range(2):
            async with stock(srv, stock_sensors=True) as (_, board, _):
                await board.chain().sonar(26, 27)
            srv.power_cycle()  # what the README asks between sessions that create sensors
        assert (srv.sonars, srv.crashed) == (0, False)
