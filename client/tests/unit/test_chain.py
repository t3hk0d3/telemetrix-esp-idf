"""client: chains (chain.py) against the scripted extended fake and the stock fake."""

from __future__ import annotations

import asyncio
import gc
from collections.abc import AsyncIterator, Callable
from contextlib import asynccontextmanager
from typing import Any

import pytest
from fake_ext_server import FakeExtServer, crc8
from fake_stock_server import FakeStockServer

from telemetrix_idf import Board, ChainError, ConnectionLost, NotSupported, TelemetrixError
from telemetrix_idf.chain import ChainResult, CommandResult, TagPool
from telemetrix_idf.connection import connect
from telemetrix_idf.wire import Reason

ROM_A = bytes([0x28, 1, 2, 3, 4, 5, 6, 0x10])
ROM_B = bytes([0x28, 9, 8, 7, 6, 5, 4, 0x20])


def refuse_pin(gpio: int, reason: int = Reason.NO_SUCH_PIN) -> Callable[[int, bytes], int | None]:
    """A `refuse` script for the extended fake: every pin command on `gpio` is refused."""

    def refuse(cmd: int, payload: bytes) -> int | None:
        return reason if cmd in (1, 2, 3, 6, 7, 8) and payload[:1] == bytes([gpio]) else None

    return refuse


@asynccontextmanager
async def ext(timeout: float = 1.0, **kw: Any) -> AsyncIterator[tuple[FakeExtServer, Board, int]]:
    """An extended board; the third value is where the chain bytes start in `srv.raw`."""
    async with FakeExtServer(**kw) as srv, connect("127.0.0.1", srv.port, timeout=timeout) as board:
        yield srv, board, len(srv.raw)


@asynccontextmanager
async def stock(**kw: Any) -> AsyncIterator[tuple[FakeStockServer, Board, int]]:
    async with FakeStockServer(**kw) as srv, connect("127.0.0.1", srv.port, timeout=1.0) as board:
        await _until(lambda: len(srv.received) == 2)  # probe, ENABLE_ALL_REPORTS
        yield srv, board, len(srv.raw)


def spy_writes(board: Board) -> list[bytes]:
    writes: list[bytes] = []
    writer = board.connection._writer
    real = writer.write

    def write(data: bytes) -> None:
        writes.append(bytes(data))
        real(data)

    writer.write = write  # type: ignore[method-assign]
    return writes


async def _until(cond: Callable[[], object], timeout: float = 1.0) -> None:
    for _ in range(int(timeout / 0.005)):
        if cond():
            return
        await asyncio.sleep(0.005)
    raise AssertionError("condition not met in time")


# --- building ----------------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_building_nothing_sent_until_awaited() -> None:
    async with ext() as (srv, board, mark):
        chain = board.pin(17).output().write(1)
        await asyncio.sleep(0.02)
        assert bytes(srv.raw[mark:]) == b""
        await chain


@pytest.mark.asyncio
async def test_building_unawaited_chain_warns_never_awaited() -> None:
    async with ext() as (_, board, _):
        with pytest.warns(RuntimeWarning, match="chain never awaited"):
            board.pin(17).output().write(1)
            gc.collect()


@pytest.mark.asyncio
async def test_building_empty_chain_does_not_warn(recwarn: pytest.WarningsRecorder) -> None:
    async with ext() as (_, board, _):
        board.chain()
        gc.collect()
        assert not [w for w in recwarn if issubclass(w.category, RuntimeWarning)]


@pytest.mark.asyncio
async def test_building_pin_op_without_pin_raises_value_error() -> None:
    async with ext() as (_, board, _):
        with pytest.raises(ValueError):
            board.chain().output()


@pytest.mark.asyncio
async def test_building_awaiting_twice_raises_value_error() -> None:
    async with ext() as (_, board, _):
        chain = board.pin(17).output()
        await chain
        with pytest.raises(ValueError):
            await chain


# --- await (extended): success -----------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_ext_success_commands_then_sync_bytes() -> None:
    async with ext() as (srv, board, mark):
        await board.pin(17).output().write(1)
        assert bytes(srv.raw[mark:]) == bytes([3, 1, 17, 1, 3, 2, 17, 1, 4, 57, 0, 0, 1])


@pytest.mark.asyncio
async def test_ext_success_one_write() -> None:
    async with ext() as (_, board, _):
        writes = spy_writes(board)
        await board.pin(17).output().write(1)
        assert len(writes) == 1


@pytest.mark.asyncio
async def test_ext_success_returns_none_without_value_op() -> None:
    async with ext() as (_, board, _):
        assert await board.pin(17).output().write(1) is None


@pytest.mark.asyncio
async def test_ext_success_returns_last_value() -> None:
    async with ext() as (_, board, _):
        value = await board.i2c(0).read(0x48, 0, 2).onewire(0).read().pin(4).output()
        assert value == 0x5A


@pytest.mark.asyncio
async def test_ext_run_returns_chain_result() -> None:
    async with ext() as (_, board, _):
        result = await board.pin(17).output().i2c(0).read(0x48, 3, 2).run()
        assert result == ChainResult(
            values=[bytes([0, 1])],
            results=[CommandResult("output", "done", None), CommandResult("read", "done", None)],
        )


@pytest.mark.asyncio
async def test_ext_seq_mirrors_server_counter() -> None:
    async with ext() as (srv, board, _):
        await board.pin(17).output().write(1)
        await board.pin(17).write(0)
        assert board.connection.seq == srv.seq


# --- await (extended): refusal with abort ------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_ext_refusal_raises_chain_error_with_results_by_seq() -> None:
    async with ext(refuse=refuse_pin(99)) as (_, board, _):
        with pytest.raises(ChainError) as err:
            await board.pin(4).output().pin(99).output().pin(4).write(1).write(0)
        assert err.value.results == [
            CommandResult("output", "done", None),
            CommandResult("output", "refused", Reason.NO_SUCH_PIN),
            CommandResult("write", "skipped", Reason.SKIPPED),
            CommandResult("write", "skipped", Reason.SKIPPED),
        ]


@pytest.mark.asyncio
async def test_ext_refusal_run_raises_chain_error() -> None:
    async with ext(refuse=refuse_pin(99)) as (_, board, _):
        with pytest.raises(ChainError):
            await board.pin(99).output().run()


@pytest.mark.asyncio
async def test_ext_refusal_next_chain_executes() -> None:
    async with ext(refuse=refuse_pin(99)) as (_, board, _):
        with pytest.raises(ChainError):
            await board.pin(99).output()
        assert (await board.pin(4).output().run()).results == [CommandResult("output", "done", None)]


# --- await (extended): atomic(False) -----------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_ext_non_atomic_after_atomic_prefixed_sync_flags_0() -> None:
    async with ext(refuse=refuse_pin(99)) as (srv, board, mark):
        with pytest.raises(ChainError):
            await board.pin(99).output().pin(4).output().atomic(False)
        assert bytes(srv.raw[mark:]) == bytes([4, 57, 0, 0, 0, 3, 1, 99, 1, 3, 1, 4, 1, 4, 57, 0, 1, 0])


@pytest.mark.asyncio
async def test_ext_non_atomic_later_commands_executed() -> None:
    async with ext(refuse=refuse_pin(99)) as (_, board, _):
        with pytest.raises(ChainError) as err:
            await board.pin(99).output().pin(4).output().atomic(False)
        assert [r.status for r in err.value.results] == ["refused", "done"]


@pytest.mark.asyncio
async def test_ext_non_atomic_twice_no_second_prefix() -> None:
    async with ext() as (srv, board, _):
        await board.pin(4).output().atomic(False)
        mark = len(srv.raw)
        await board.pin(4).write(1).atomic(False)
        assert bytes(srv.raw[mark:]) == bytes([3, 2, 4, 1, 4, 57, 0, 2, 0])


@pytest.mark.asyncio
async def test_ext_atomic_after_non_atomic_prefixed_sync_flags_1() -> None:
    async with ext() as (srv, board, _):
        await board.pin(4).output().atomic(False)
        mark = len(srv.raw)
        await board.pin(4).write(1)
        assert bytes(srv.raw[mark:]) == bytes([4, 57, 0, 2, 1, 3, 2, 4, 1, 4, 57, 0, 3, 1])


@pytest.mark.asyncio
async def test_ext_prefix_sync_reply_consumed() -> None:
    async with ext() as (_, board, _):
        await board.pin(4).output().atomic(False)
        assert (board.stats.unmatched_replies, board._chains.tags.reserved) == (0, set())


# --- await (extended): concurrency, timeout, cancellation --------------------------------------------------------


@pytest.mark.asyncio
async def test_concurrent_chains_error_lands_on_owner() -> None:
    async with ext(refuse=refuse_pin(99)) as (_, board, _):
        first, second = await asyncio.gather(
            board.pin(99).output().write(1), board.pin(4).output().write(1), return_exceptions=True
        )
        assert isinstance(first, ChainError)
        assert [r.status for r in first.results] == ["refused", "skipped"]


@pytest.mark.asyncio
async def test_concurrent_chains_second_returns_normally() -> None:
    async with ext(refuse=refuse_pin(99)) as (_, board, _):
        _, second = await asyncio.gather(
            board.pin(99).output().write(1), board.pin(4).output().write(1), return_exceptions=True
        )
        assert second is None


@pytest.mark.asyncio
async def test_ext_late_reply_raises_timeout() -> None:
    async with ext(timeout=0.05) as (srv, board, _):
        srv.hold = True
        with pytest.raises(asyncio.TimeoutError):
            await board.pin(4).output()


@pytest.mark.asyncio
async def test_ext_late_reply_tag_stays_reserved() -> None:
    async with ext(timeout=0.05) as (srv, board, _):
        srv.hold = True
        with pytest.raises(asyncio.TimeoutError):
            await board.pin(4).output()
        assert board._chains.tags.reserved == {0}


@pytest.mark.asyncio
async def test_ext_late_reply_next_chain_gets_another_tag() -> None:
    async with ext(timeout=0.05) as (srv, board, _):
        srv.hold = True
        with pytest.raises(asyncio.TimeoutError):
            await board.pin(4).output()
        mark = len(srv.raw)
        later = asyncio.ensure_future(board.pin(4).write(1).run(timeout=1.0))
        await _until(lambda: len(srv.raw) > mark)
        srv.release()
        await later
        assert bytes(srv.raw[mark:]) == bytes([3, 2, 4, 1, 4, 57, 0, 1, 1])


@pytest.mark.asyncio
async def test_ext_late_reply_discarded_later_chain_gets_its_own() -> None:
    async with ext(timeout=0.2) as (srv, board, _):
        srv.hold = True
        with pytest.raises(asyncio.TimeoutError):
            await asyncio.wait_for(board.i2c(0).read(0x48, 1, 2), 0.05)
        later = asyncio.ensure_future(board.i2c(0).read(0x48, 7, 3).run(timeout=1.0))
        await _until(lambda: len(srv.received) == 6)  # probe, page, read+sync, read+sync
        srv.release()
        assert (await later).values == [bytes([0, 1, 2])]


@pytest.mark.asyncio
async def test_ext_late_reply_released_tag_freed() -> None:
    async with ext(timeout=0.05) as (srv, board, _):
        srv.hold = True
        with pytest.raises(asyncio.TimeoutError):
            await board.pin(4).output()
        srv.release()
        await _until(lambda: not board._chains.tags.reserved)
        assert board.stats.unmatched_replies == 0


@pytest.mark.asyncio
async def test_ext_cancel_raises_cancelled_error() -> None:
    async with ext() as (srv, board, _):
        srv.hold = True
        task = asyncio.ensure_future(board.pin(4).output())
        await _until(lambda: board._chains.tags.reserved)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task


@pytest.mark.asyncio
async def test_ext_cancel_tag_reserved_until_reply() -> None:
    async with ext() as (srv, board, _):
        srv.hold = True
        task = asyncio.ensure_future(board.pin(4).output())
        await _until(lambda: board._chains.tags.reserved)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert board._chains.tags.reserved == {0}
        srv.release()
        await _until(lambda: not board._chains.tags.reserved)


@pytest.mark.asyncio
async def test_ext_connection_lost_fails_pending_chain() -> None:
    async with ext() as (srv, board, _):
        srv.hold = True
        task = asyncio.ensure_future(board.pin(4).output())
        await _until(lambda: board._chains.tags.reserved)
        srv.drop()
        with pytest.raises(ConnectionLost):
            await task


@pytest.mark.asyncio
async def test_ext_unexpected_reply_counted_and_dropped() -> None:
    async with ext() as (srv, board, _):
        srv.push(bytes([5, 9, 1, 0x48, 0, 7]) + bytes([5, 58, 0x12, 0x34, 0, 0]) + bytes([5, 57, 0x40, 0, 1, 1]))
        await _until(lambda: board.stats.unmatched_replies == 3)
        assert await board.pin(4).output() is None


# --- barrier tags ------------------------------------------------------------------------------------------------


def test_tag_pool_allocates_in_order() -> None:
    pool = TagPool(4)
    assert [pool.allocate(), pool.allocate()] == [0, 1]


def test_tag_pool_skips_reserved_tags() -> None:
    pool = TagPool(3)
    first = pool.allocate()
    pool.allocate()
    pool.allocate()
    pool.release(1)
    pool.release(2)
    assert (pool.allocate(), pool.allocate()) == (1, 2)
    assert first in pool.reserved


def test_tag_pool_released_tag_reused() -> None:
    pool = TagPool(1)
    pool.release(pool.allocate())
    assert pool.allocate() == 0


def test_tag_pool_all_reserved_raises_no_free_tag() -> None:
    pool = TagPool(2)
    pool.allocate()
    pool.allocate()
    with pytest.raises(TelemetrixError, match="no free barrier tag"):
        pool.allocate()


@pytest.mark.asyncio
async def test_ext_all_tags_reserved_nothing_written() -> None:
    async with ext() as (srv, board, mark):
        board._chains.tags = TagPool(1)
        board._chains.tags.allocate()
        with pytest.raises(TelemetrixError, match="no free barrier tag"):
            await board.pin(4).output()
        await asyncio.sleep(0.02)
        assert bytes(srv.raw[mark:]) == b""


# --- I2C and 1-wire in a chain -----------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_ext_i2c_read_returns_bytes() -> None:
    async with ext() as (_, board, _):
        assert await board.i2c().begin().read(0x48, 0, 4) == bytes([0, 1, 2, 3])


@pytest.mark.asyncio
async def test_ext_i2c_on_bus_1_sent_through_on_bus() -> None:
    async with ext() as (srv, board, mark):
        await board.i2c(1).write(0x48, b"\x01").read(0x48, 2, 1)
        assert bytes(srv.raw[mark:]) == bytes(
            [6, 58, 1, 11, 1, 0x48, 1] + [7, 58, 1, 10, 0x48, 2, 1, 1] + [4, 57, 0, 0, 1]
        )


@pytest.mark.asyncio
async def test_ext_i2c_on_bus_1_bus_report_matched() -> None:
    async with ext() as (_, board, _):
        result = await board.i2c(1).read(0x48, 2, 3).i2c(0).read(0x50, 0, 1).run()
        assert result.values == [bytes([0, 1, 2]), bytes([0])]


@pytest.mark.asyncio
async def test_ext_i2c_nack_refused_no_answer() -> None:
    async with ext(i2c_nack={0x50}) as (_, board, _):
        with pytest.raises(ChainError) as err:
            await board.i2c(1).read(0x50, 0, 1)
        assert err.value.results == [CommandResult("read", "refused", Reason.NO_ANSWER)]


@pytest.mark.asyncio
async def test_ext_unknown_bus_refused_bus_unknown() -> None:
    async with ext() as (_, board, _):
        with pytest.raises(ChainError) as err:
            await board.onewire(5).reset()
        assert err.value.results == [CommandResult("reset", "refused", Reason.BUS_UNKNOWN)]


@pytest.mark.asyncio
async def test_ext_onewire_reset_yields_presence() -> None:
    async with ext(roms=[ROM_A]) as (_, board, _):
        assert await board.onewire(0).init(4).reset() is True


@pytest.mark.asyncio
async def test_ext_onewire_read_yields_int() -> None:
    async with ext() as (_, board, _):
        assert await board.onewire(1).skip().write(0xBE, power=True).read() == 0x5A


@pytest.mark.asyncio
async def test_ext_onewire_search_yields_rom_or_none() -> None:
    async with ext(roms=[ROM_A]) as (_, board, _):
        result = await board.onewire(0).reset_search().search().search().run()
        assert result.values == [ROM_A, None]


@pytest.mark.asyncio
async def test_ext_onewire_crc8_yields_int() -> None:
    async with ext() as (_, board, _):
        assert await board.onewire(0).crc8(ROM_A[:7]) == crc8(ROM_A[:7])


@pytest.mark.asyncio
async def test_ext_onewire_search_all_lists_roms() -> None:
    async with ext(roms=[ROM_A, ROM_B]) as (_, board, _):
        assert await board.onewire(1).search_all() == [ROM_A, ROM_B]


@pytest.mark.asyncio
async def test_ext_onewire_select_bytes() -> None:
    async with ext() as (srv, board, mark):
        await board.onewire(0).select(ROM_A)
        assert bytes(srv.raw[mark:]) == bytes([9, 29, *ROM_A, 4, 57, 0, 0, 1])


@pytest.mark.asyncio
async def test_ext_reset_sends_reset() -> None:
    async with ext() as (srv, board, mark):
        await board.chain().reset()
        assert bytes(srv.raw[mark:]) == bytes([1, 20, 4, 57, 0, 0, 1])


@pytest.mark.asyncio
async def test_ext_pin_ops_bytes() -> None:
    async with ext() as (srv, board, mark):
        await (
            board.pin(4)
            .input("up")
            .pin(5)
            .input("down", report=False)
            .pin(6)
            .input()
            .pin(34)
            .analog()
            .report(False)
            .pin(5)
            .report(True)
            .pin(12)
            .pwm(1000.0, 10)
            .duty(512)
            .detach()
            .pin(13)
            .servo()
            .angle(90)
            .detach()
            .sonar(18, 19)
            .dht(21)
            .reporting(False)
            .analog_interval(20)
        )
        assert bytes(srv.raw[mark:]) == (
            bytes([4, 1, 4, 2, 1, 4, 1, 5, 9, 0, 4, 1, 6, 0, 1])
            + bytes([6, 1, 34, 3, 0, 0, 1, 3, 4, 3, 34, 3, 4, 2, 5])
            + bytes([13, 1, 12, 8, 0, 10])
            + bytes.fromhex("0000000000408f40")
            + bytes([4, 3, 12, 2, 0, 2, 18, 12])
            + bytes([6, 6, 13, 3, 0xE8, 7, 0xD0, 3, 7, 13, 90, 2, 8, 13])
            + bytes([3, 12, 18, 19, 2, 13, 21, 1, 14, 2, 15, 20])
            + bytes([4, 57, 0, 0, 1])
        )


# --- await (stock) -----------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_stock_success_commands_then_loop_tag() -> None:
    async with stock() as (srv, board, mark):
        await board.pin(17).output().write(1)
        assert bytes(srv.raw[mark:]) == bytes([3, 1, 17, 1, 3, 2, 17, 1, 2, 0, 0])


@pytest.mark.asyncio
async def test_stock_success_completes_on_loop_reply() -> None:
    async with stock() as (_, board, _):
        assert await asyncio.wait_for(board.pin(17).output().write(1), 1) is None


@pytest.mark.asyncio
async def test_stock_servo_unavailable_refused_exhausted() -> None:
    async with stock(servo_limit=1) as (_, board, _):
        with pytest.raises(ChainError) as err:
            await board.pin(4).servo().pin(5).servo().angle(10)
        assert err.value.results == [
            CommandResult("servo", "done", None),
            CommandResult("servo", "refused", Reason.EXHAUSTED),
            CommandResult("angle", "done", None),
        ]


@pytest.mark.asyncio
async def test_stock_i2c_read_returns_bytes() -> None:
    async with stock() as (_, board, _):
        assert await board.i2c().begin(21, 22).read(0x48, 0, 3) == bytes([0, 1, 2])


@pytest.mark.asyncio
async def test_stock_i2c_nack_refused_no_answer() -> None:
    async with stock(i2c_nack={0x50}) as (_, board, _):
        with pytest.raises(ChainError) as err:
            await board.i2c().read(0x48, 0, 1).read(0x50, 0, 1).run()
        assert [r.status for r in err.value.results] == ["done", "refused"]
        assert err.value.results[1].reason == Reason.NO_ANSWER


@pytest.mark.asyncio
async def test_stock_tags_are_8_bit() -> None:
    async with stock() as (_, board, _):
        assert board._chains.tags.size == 256


@pytest.mark.asyncio
async def test_stock_call_needing_extended_raises_not_supported() -> None:
    async with stock() as (_, board, _):
        with pytest.raises(NotSupported):
            await board.i2c(1).read(0x48, 0, 1)


@pytest.mark.asyncio
async def test_stock_call_needing_extended_nothing_written() -> None:
    async with stock() as (srv, board, mark):
        with pytest.raises(NotSupported):
            await board.pin(4).output().onewire(1).reset()
        await asyncio.sleep(0.02)
        assert bytes(srv.raw[mark:]) == b""


@pytest.mark.asyncio
async def test_stock_reset_raises_not_supported() -> None:
    async with stock() as (srv, board, mark):
        with pytest.raises(NotSupported):
            await board.chain().reset()
        await asyncio.sleep(0.02)
        assert bytes(srv.raw[mark:]) == b""


@pytest.mark.asyncio
async def test_stock_invalid_pin_raises_value_error_nothing_written() -> None:
    async with stock() as (srv, board, mark):
        with pytest.raises(ValueError):
            await board.pin(4).output().pin(34).output()  # 34 is input-only (analog list)
        await asyncio.sleep(0.02)
        assert bytes(srv.raw[mark:]) == b""


@pytest.mark.asyncio
async def test_stock_pwm_channels_allocated_per_pin() -> None:
    async with stock() as (srv, board, mark):
        await board.pin(12).pwm(1000.0, 10).pin(13).pwm(1000.0, 10).pin(12).pwm(500.0, 8)
        assert [p[2] for c, p in srv.received[2:] if c == 1] == [0, 1, 0]


@pytest.mark.asyncio
async def test_await_timeout_defaults_to_connect_timeout() -> None:
    async with ext(timeout=0.25) as (_, board, _):
        assert board.timeout == 0.25
