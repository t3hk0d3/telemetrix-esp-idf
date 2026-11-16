"""client: plugins (plugins.py) against the scripted extended fake and the stock fake."""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator, Callable
from contextlib import asynccontextmanager
from typing import Any

import pytest
from fake_ext_server import FakeExtServer, plugin_rec
from fake_stock_server import FakeStockServer

from telemetrix_idf import (
    Board,
    ChainError,
    IncompatiblePlugin,
    NotSupported,
    PluginHalf,
    connect,
)
from telemetrix_idf.wire import Reason

SLOT = 2


class Echo(PluginHalf):
    name = "test.echo"
    major = 1

    def __init__(self, board: Board, slot: int) -> None:
        super().__init__(board, slot)
        self.reports: list[tuple[int, bytes, str]] = []

    def on_report(self, sub: int, data: bytes) -> None:
        task = asyncio.current_task()
        self.reports.append((sub, data, task.get_name() if task else ""))


class EchoV2(Echo):
    major = 2


class Other(PluginHalf):
    name = "test.other"
    major = 1


def echo_plugin(slot: int, sub: int, args: bytes) -> list[tuple[int, bytes]] | int:
    """sub 0 echoes the args as a report of sub 0; sub 1 refuses with code 7; sub 2 answers later (pushed)."""
    if sub == 0:
        return [(0, args)]
    if sub == 1:
        return 7
    return []


@asynccontextmanager
async def ext(
    halves: tuple[type[PluginHalf], ...] = (Echo,), plugin: Callable[..., Any] = echo_plugin, timeout: float = 1.0
) -> AsyncIterator[tuple[FakeExtServer, Board]]:
    pages = [plugin_rec(SLOT, 1, 0, "test.echo")]
    async with (
        FakeExtServer(pages=pages, plugin=plugin) as srv,
        connect("127.0.0.1", srv.port, timeout=timeout, plugins=halves) as board,
    ):
        yield srv, board


async def _until(cond: Callable[[], object], timeout: float = 1.0) -> None:
    for _ in range(int(timeout / 0.005)):
        if cond():
            return
        await asyncio.sleep(0.005)
    raise AssertionError("condition not met in time")


def plugin_report(slot: int, sub: int, data: bytes) -> bytes:
    return bytes([3 + len(data), 61, slot, sub]) + data


# --- binding -----------------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_same_major_plugin_bound_to_session_slot() -> None:
    async with ext() as (_, board):
        half = board.plugin("test.echo")
        assert (type(half), half.slot, half.board) == (Echo, SLOT, board)


@pytest.mark.asyncio
async def test_same_major_plugin_created_once_per_board() -> None:
    async with ext() as (_, board):
        assert board.plugin("test.echo") is board.plugin("test.echo")


@pytest.mark.asyncio
async def test_major_mismatch_raises_incompatible_plugin() -> None:
    async with ext(halves=(EchoV2,)) as (_, board):
        with pytest.raises(IncompatiblePlugin):
            board.plugin("test.echo")


@pytest.mark.asyncio
async def test_plugin_board_lacks_raises_not_supported() -> None:
    async with ext(halves=(Echo, Other)) as (_, board):
        with pytest.raises(NotSupported):
            board.plugin("test.other")


@pytest.mark.asyncio
async def test_unregistered_half_raises_value_error() -> None:
    async with ext(halves=()) as (_, board):
        with pytest.raises(ValueError, match="connect"):
            board.plugin("test.echo")


@pytest.mark.asyncio
async def test_stock_mode_plugin_raises_not_supported() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port, timeout=1.0, plugins=(Echo,)) as board:
        with pytest.raises(NotSupported):
            board.plugin("test.echo")


@pytest.mark.asyncio
async def test_registering_a_non_half_raises_type_error() -> None:
    async with FakeExtServer() as srv:
        with pytest.raises(TypeError):
            await connect("127.0.0.1", srv.port, timeout=1.0, plugins=(object,))  # type: ignore[arg-type]


# --- reports -----------------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_plugin_report_calls_on_report_with_sub_and_data() -> None:
    async with ext() as (srv, board):
        half = board.plugin("test.echo")
        srv.push(plugin_report(SLOT, 9, b"\x01\x02"))
        await _until(lambda: half.reports)
        assert half.reports[0][:2] == (9, b"\x01\x02")


@pytest.mark.asyncio
async def test_plugin_report_on_report_runs_on_dispatch_task() -> None:
    async with ext() as (srv, board):
        half = board.plugin("test.echo")
        srv.push(plugin_report(SLOT, 9, b""))
        await _until(lambda: half.reports)
        assert half.reports[0][2] == "telemetrix-events"


@pytest.mark.asyncio
async def test_plugin_report_of_another_slot_not_delivered() -> None:
    async with ext() as (srv, board):
        half = board.plugin("test.echo")
        srv.push(plugin_report(SLOT + 1, 9, b"") + plugin_report(SLOT, 8, b""))
        await _until(lambda: half.reports)
        await asyncio.sleep(0.02)
        assert [r[0] for r in half.reports] == [8]


# --- command -----------------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_command_sends_plugin_then_sync() -> None:
    async with ext() as (srv, board):
        mark = len(srv.raw)
        await board.plugin("test.echo").command(0, b"\x05")
        assert bytes(srv.raw[mark:]) == bytes([4, 60, SLOT, 0, 5, 4, 57, 0, 0, 1])


@pytest.mark.asyncio
async def test_command_composes_with_other_ops() -> None:
    async with ext() as (srv, board):
        mark = len(srv.raw)
        await board.plugin("test.echo").command(0).pin(17).output()
        assert bytes(srv.raw[mark:]) == bytes([3, 60, SLOT, 0, 3, 1, 17, 1, 4, 57, 0, 0, 1])


@pytest.mark.asyncio
async def test_command_refusal_raises_chain_error_reason_plugin() -> None:
    async with ext() as (_, board):
        with pytest.raises(ChainError) as err:
            await board.plugin("test.echo").command(1)
        assert (err.value.results[0].status, err.value.results[0].reason) == ("refused", Reason.PLUGIN)


@pytest.mark.asyncio
async def test_command_refusal_code_on_command_result() -> None:
    async with ext() as (_, board):
        with pytest.raises(ChainError) as err:
            await board.plugin("test.echo").command(1)
        assert err.value.results[0].code == 7


# --- request -----------------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_request_prefixes_args_with_request_id() -> None:
    async with ext() as (srv, board):
        await board.plugin("test.echo").request(0, b"\xaa")
        cmd, payload = next(p for p in srv.received if p[0] == 60)
        assert payload == bytes([SLOT, 0, 0, 0, 0xAA])


@pytest.mark.asyncio
async def test_request_returns_report_data_after_the_id() -> None:
    async with ext() as (_, board):
        assert await board.plugin("test.echo").request(0, b"\xaa\xbb") == b"\xaa\xbb"


@pytest.mark.asyncio
async def test_request_concurrent_requests_get_distinct_ids() -> None:
    async with ext() as (srv, board):
        half = board.plugin("test.echo")
        await asyncio.gather(half.request(0, b"\x01"), half.request(0, b"\x02"))
        ids = [payload[2:4] for cmd, payload in srv.received if cmd == 60]
        assert len(set(ids)) == 2


@pytest.mark.asyncio
async def test_request_completes_on_report_with_its_id_only() -> None:
    async with ext() as (srv, board):
        task = asyncio.ensure_future(board.plugin("test.echo").request(2, b""))
        await _until(lambda: any(cmd == 60 for cmd, _ in srv.received))
        await asyncio.sleep(0.02)
        srv.push(plugin_report(SLOT, 2, b"\x00\x09\x55"))
        await asyncio.sleep(0.02)
        assert not task.done()
        srv.push(plugin_report(SLOT, 2, b"\x00\x00\x55"))
        assert await asyncio.wait_for(task, 1) == b"\x55"


@pytest.mark.asyncio
async def test_request_matched_report_not_passed_to_on_report() -> None:
    async with ext() as (_, board):
        half = board.plugin("test.echo")
        await half.request(0, b"\x01")
        await asyncio.sleep(0.02)
        assert half.reports == []


@pytest.mark.asyncio
async def test_request_without_report_raises_timeout() -> None:
    async with ext(timeout=0.1) as (_, board):
        with pytest.raises(asyncio.TimeoutError):
            await board.plugin("test.echo").request(2, b"")


@pytest.mark.asyncio
async def test_request_refused_raises_chain_error() -> None:
    async with ext() as (_, board):
        with pytest.raises(ChainError):
            await board.plugin("test.echo").request(1, b"")
