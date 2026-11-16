"""client: handshake (connection.py), the connection's reader, and the stock-mode firmware/pin rules."""

from __future__ import annotations

import asyncio
import inspect
from collections.abc import Callable

import pytest
from fake_ext_server import FakeExtServer, pin_rec
from fake_stock_server import FakeStockServer

from telemetrix_idf import ConnectionLost, HandshakeTimeout, IncompatibleFirmware, ProtocolError
from telemetrix_idf.board import Board
from telemetrix_idf.connection import connect
from telemetrix_idf.discovery import Cap
from telemetrix_idf.stock import STOCK_PINS, check_stock_firmware

PROBE = (5, bytes([0xA7, 1, 0]))


# --- the probe ---------------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_probe_is_firmware_request_with_magic_and_version() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port):
        assert srv.received[0] == PROBE


# --- long answer, extension major 1 ------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_long_answer_mode_extended() -> None:
    async with FakeExtServer() as srv, connect("127.0.0.1", srv.port) as board:
        assert board.mode == "extended"


@pytest.mark.asyncio
async def test_long_answer_firmware_is_stock_three_bytes() -> None:
    async with FakeExtServer() as srv, connect("127.0.0.1", srv.port) as board:
        assert board.firmware == (3, 2, 100)


@pytest.mark.asyncio
async def test_long_answer_protocol_is_lower_minor() -> None:
    async with FakeExtServer(ext=(1, 4)) as srv, connect("127.0.0.1", srv.port) as board:
        assert board.protocol == (1, 0)


@pytest.mark.asyncio
async def test_long_answer_discovery_fetched() -> None:
    async with FakeExtServer(pages=[pin_rec(4, 0x03)]) as srv, connect("127.0.0.1", srv.port) as board:
        assert srv.received[1:] == [(59, bytes([0]))]
        assert list(board.pins) == [4]


@pytest.mark.asyncio
async def test_long_answer_no_enable_all_reports() -> None:
    async with FakeExtServer() as srv, connect("127.0.0.1", srv.port):
        assert all(cmd != 16 for cmd, _ in srv.received)


@pytest.mark.asyncio
async def test_long_answer_discovery_commands_numbered_from_zero() -> None:
    async with FakeExtServer(pages=[b"", b"", b""]) as srv, connect("127.0.0.1", srv.port) as board:
        assert board.connection.seq == 3


# --- short answer [3, 2, x] --------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_short_answer_mode_stock() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        assert board.mode == "stock"


@pytest.mark.asyncio
async def test_short_answer_firmware_and_no_protocol() -> None:
    async with FakeStockServer(firmware=bytes([3, 2, 7])) as srv, connect("127.0.0.1", srv.port) as board:
        assert (board.firmware, board.protocol) == ((3, 2, 7), None)


@pytest.mark.asyncio
async def test_short_answer_enable_all_reports_sent_once_after_probe() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        await board.close()
        await asyncio.wait_for(srv.eof.wait(), 1)
        assert srv.received == [PROBE, (16, b"")]


@pytest.mark.asyncio
async def test_short_answer_pins_from_upstream_lists() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        expected = set(STOCK_PINS["input"]) | set(STOCK_PINS["output"]) | set(STOCK_PINS["analog"])
        assert set(board.pins) == expected


@pytest.mark.asyncio
async def test_short_answer_pin_caps_inferred_from_lists() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        assert board.pins[2].caps == Cap.DOUT | Cap.PWM
        assert board.pins[36].caps == Cap.ADC
        assert board.pins[32].caps == (
            Cap.DIN | Cap.PULL_UP | Cap.PULL_DOWN | Cap.DOUT | Cap.PWM | Cap.ADC | Cap.I2C | Cap.ONE_WIRE
        )


@pytest.mark.asyncio
async def test_short_answer_buses_client_chosen_on_bus_0() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        assert [(b.kind, b.bus, b.pins) for b in board.buses] == [("i2c", 0, None), ("one_wire", 0, None)]


@pytest.mark.asyncio
async def test_short_answer_no_chip_limits_or_plugins() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        assert (board.chip, dict(board.limits), dict(board.plugins_available)) == (None, {}, {})


@pytest.mark.asyncio
async def test_short_answer_from_our_server_without_extension_is_stock() -> None:
    # our server with extended mode off answers [4,5,3,2,100] to the probe
    async with FakeStockServer(firmware=bytes([3, 2, 100])) as srv, connect("127.0.0.1", srv.port) as board:
        assert (board.mode, board.firmware) == ("stock", (3, 2, 100))


# --- incompatible firmware ---------------------------------------------------------------------------------------


@pytest.mark.parametrize("fw", [bytes([3, 0, 0]), bytes([3, 1, 0]), bytes([2, 9, 0]), bytes([4, 2, 0])])
@pytest.mark.asyncio
async def test_old_or_other_major_firmware_raises_incompatible(fw: bytes) -> None:
    async with FakeStockServer(firmware=fw) as srv:
        with pytest.raises(IncompatibleFirmware):
            await connect("127.0.0.1", srv.port)


@pytest.mark.asyncio
async def test_old_firmware_connection_closed() -> None:
    async with FakeStockServer(firmware=bytes([3, 1, 0])) as srv:
        with pytest.raises(IncompatibleFirmware):
            await connect("127.0.0.1", srv.port)
        await asyncio.wait_for(srv.eof.wait(), 1)
        assert srv.received == [PROBE]


@pytest.mark.asyncio
async def test_extension_major_2_raises_incompatible() -> None:
    async with FakeExtServer(ext=(2, 0)) as srv:
        with pytest.raises(IncompatibleFirmware):
            await connect("127.0.0.1", srv.port)


@pytest.mark.asyncio
async def test_extension_major_2_connection_closed_after_probe_only() -> None:
    async with FakeExtServer(ext=(2, 0)) as srv:
        with pytest.raises(IncompatibleFirmware):
            await connect("127.0.0.1", srv.port)
        await asyncio.wait_for(srv.eof.wait(), 1)
        assert srv.received == [PROBE]


@pytest.mark.parametrize(("major", "minor"), [(3, 2), (3, 3), (3, 9)])
def test_check_stock_firmware_accepts_3_2_and_later(major: int, minor: int) -> None:
    check_stock_firmware(major, minor)


@pytest.mark.parametrize(("major", "minor"), [(3, 0), (3, 1), (2, 5), (4, 0), (0, 0)])
def test_check_stock_firmware_refuses_others(major: int, minor: int) -> None:
    with pytest.raises(IncompatibleFirmware):
        check_stock_firmware(major, minor)


def test_stock_pins_are_upstream_2_1_1_lists() -> None:
    assert STOCK_PINS["analog"] == (32, 33, 34, 35, 36, 39)
    assert STOCK_PINS["touch"] == (4, 12, 13, 14, 15, 27, 32, 33)
    assert STOCK_PINS["output"][0] == 2 and 2 not in STOCK_PINS["input"]
    assert STOCK_PINS["servo"] == STOCK_PINS["input"]


# --- reports before the answer -----------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_reports_before_answer_discarded() -> None:
    # a digital report of pin 4, an analog report of pin 32, and a stray firmware-looking LOOP reply
    stale = bytes([3, 2, 4, 1]) + bytes([4, 3, 32, 1, 2]) + bytes([2, 0, 9])
    async with FakeStockServer(pre_handshake=stale) as srv, connect("127.0.0.1", srv.port) as board:
        assert (board.mode, board.firmware, board.stats.protocol_errors) == ("stock", (3, 2, 0), 0)


@pytest.mark.asyncio
async def test_reports_before_long_answer_discarded() -> None:
    stale = bytes([3, 2, 4, 1]) * 5
    async with FakeExtServer(pre_handshake=stale) as srv, connect("127.0.0.1", srv.port) as board:
        assert board.mode == "extended"


# --- silence -----------------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_handshake_silent_server_times_out_after_probe_only() -> None:
    async with FakeStockServer(silent=True) as srv:
        with pytest.raises(HandshakeTimeout):
            await connect("127.0.0.1", srv.port, handshake_timeout=0.05)
        await asyncio.wait_for(srv.eof.wait(), 1)
        assert bytes(srv.raw) == bytes([4, 5, 0xA7, 1, 0])


def test_handshake_timeout_default_is_3_s() -> None:
    assert inspect.signature(connect).parameters["handshake_timeout"].default == 3.0


def test_await_timeout_default_is_5_s() -> None:
    assert inspect.signature(connect).parameters["timeout"].default == 5.0


@pytest.mark.asyncio
async def test_discovery_silence_raises_timeout_and_closes() -> None:
    class NoPages(FakeExtServer):
        def handle(self, cmd: int, payload: bytes) -> bytes | None:
            return super().handle(cmd, payload) if cmd == 5 else b""

    async with NoPages() as srv:
        with pytest.raises(asyncio.TimeoutError):
            await connect("127.0.0.1", srv.port, timeout=0.05)
        await asyncio.wait_for(srv.eof.wait(), 1)


# --- connect forms -----------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_connect_awaited_returns_board() -> None:
    async with FakeStockServer() as srv:
        board = await connect("127.0.0.1", srv.port)
        try:
            assert isinstance(board, Board)
        finally:
            await board.close()


@pytest.mark.asyncio
async def test_connect_context_manager_closes_on_exit() -> None:
    async with FakeStockServer() as srv:
        async with connect("127.0.0.1", srv.port) as board:
            pass
        assert board.closed
        await asyncio.wait_for(srv.eof.wait(), 1)


@pytest.mark.asyncio
async def test_connect_refused_raises_connection_lost() -> None:
    async with FakeStockServer() as srv:
        port = srv.port
    with pytest.raises(ConnectionLost):
        await connect("127.0.0.1", port)


# --- connection: reader, router, loss ----------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_router_receives_reports_after_connect() -> None:
    got: list[tuple[int, bytes]] = []
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        board.connection.set_router(lambda rid, payload: got.append((rid, payload)))
        srv.push(bytes([3, 2, 4, 1]))
        await _until(lambda: got)
        assert got == [(2, bytes([4, 1]))]


@pytest.mark.asyncio
async def test_router_protocol_error_counted_reader_survives() -> None:
    got: list[int] = []

    def router(rid: int, payload: bytes) -> None:
        if rid == 3:
            raise ProtocolError("bad")
        got.append(rid)

    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        board.connection.set_router(router)
        srv.push(bytes([2, 3, 0]) + bytes([3, 2, 4, 1]))
        await _until(lambda: got)
        assert (board.stats.protocol_errors, got) == (1, [2])


@pytest.mark.asyncio
async def test_eof_fails_pending_waiter_with_connection_lost() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        fut = board.connection.expect(lambda rid, payload: None)
        srv.drop()
        with pytest.raises(ConnectionLost):
            await asyncio.wait_for(fut, 1)


@pytest.mark.asyncio
async def test_eof_marks_board_closed() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        srv.drop()
        await _until(lambda: board.closed)
        assert board.closed


@pytest.mark.asyncio
async def test_eof_runs_close_callbacks() -> None:
    called: list[bool] = []
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        board.connection.add_close_callback(lambda: called.append(True))
        srv.drop()
        await _until(lambda: called)
        assert called == [True]


@pytest.mark.asyncio
async def test_expect_after_close_raises_connection_lost() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        await board.close()
        with pytest.raises(ConnectionLost):
            await board.connection.expect(lambda rid, payload: None)


@pytest.mark.asyncio
async def test_write_after_close_raises_connection_lost() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        await board.close()
        with pytest.raises(ConnectionLost):
            board.connection.write(bytes([2, 0, 1]))


@pytest.mark.asyncio
async def test_close_fails_pending_waiter_with_connection_lost() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        fut = board.connection.expect(lambda rid, payload: None)
        await board.close()
        with pytest.raises(ConnectionLost):
            await fut


@pytest.mark.asyncio
async def test_close_twice_is_harmless() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        await board.close()
        await board.close()
        assert board.closed


@pytest.mark.asyncio
async def test_stock_mode_writes_are_not_numbered() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        board.connection.write(bytes([2, 0, 1, 2, 0, 2]))
        assert board.connection.seq == 0


@pytest.mark.asyncio
async def test_extended_write_counts_every_packet() -> None:
    async with FakeExtServer() as srv, connect("127.0.0.1", srv.port) as board:
        board.connection.write(bytes([4, 57, 0, 1, 1, 2, 0, 9]))
        assert board.connection.seq == 3  # 1 discovery page + 2 packets


async def _until(cond: Callable[[], object], timeout: float = 1.0) -> None:
    for _ in range(int(timeout / 0.005)):
        if cond():
            return
        await asyncio.sleep(0.005)
    raise AssertionError("condition not met in time")


# --- malformed firmware answer and refused discovery -------------------------------------------------------------


@pytest.mark.parametrize(
    "answer", [bytes([3, 5, 3, 2]), bytes([6, 5, 3, 2, 100, 0x00, 1]), bytes([7, 5, 3, 2, 100, 0x55, 1, 0])]
)
@pytest.mark.asyncio
async def test_malformed_firmware_answer_raises_protocol_error_promptly(answer: bytes) -> None:
    class Odd(FakeStockServer):
        def handle(self, cmd: int, payload: bytes) -> bytes | None:
            return answer if cmd == 5 else b""

    async with Odd() as srv:
        with pytest.raises(ProtocolError):
            await connect("127.0.0.1", srv.port, handshake_timeout=2.0, timeout=0.5)
        await asyncio.wait_for(srv.eof.wait(), 1)


@pytest.mark.asyncio
async def test_refused_discovery_page_raises_protocol_error() -> None:
    async with FakeExtServer(pages=[b"", b""], refuse_pages={1}) as srv:
        with pytest.raises(ProtocolError):
            await connect("127.0.0.1", srv.port)


@pytest.mark.asyncio
async def test_refused_discovery_page_server_sees_eof() -> None:
    async with FakeExtServer(pages=[b"", b""], refuse_pages={1}) as srv:
        with pytest.raises(ProtocolError):
            await connect("127.0.0.1", srv.port)
        await asyncio.wait_for(srv.eof.wait(), 1)
        assert srv.received[-1] == (59, bytes([1]))


# --- reader robustness -------------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_bad_matcher_does_not_hide_report_from_later_waiter() -> None:
    def bad(rid: int, payload: bytes) -> None:
        raise ProtocolError("bad")

    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        board.connection.expect(bad)
        good = board.connection.expect(lambda rid, payload: payload if rid == 2 else None)
        srv.push(bytes([3, 2, 4, 1]))
        assert await asyncio.wait_for(good, 1) == bytes([4, 1])


@pytest.mark.asyncio
async def test_bad_matcher_does_not_hide_report_from_router() -> None:
    got: list[int] = []

    def bad(rid: int, payload: bytes) -> None:
        raise ProtocolError("bad")

    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:
        board.connection.expect(bad)
        board.connection.set_router(lambda rid, payload: got.append(rid))
        srv.push(bytes([3, 2, 4, 1]))
        await _until(lambda: got)
        assert (got, board.stats.protocol_errors) == ([2], 1)


@pytest.mark.asyncio
async def test_reader_crash_fails_waiters_with_connection_lost() -> None:
    async with FakeStockServer() as srv, connect("127.0.0.1", srv.port) as board:

        def boom(data: bytes) -> list[tuple[int, bytes]]:
            raise RuntimeError("parser bug")

        board.connection._parser.feed = boom  # type: ignore[method-assign]
        fut = board.connection.expect(lambda rid, payload: None)
        srv.push(bytes([3, 2, 4, 1]))
        with pytest.raises(ConnectionLost):
            await asyncio.wait_for(fut, 1)
