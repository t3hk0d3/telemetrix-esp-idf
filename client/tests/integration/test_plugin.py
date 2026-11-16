"""Our client's plugin half against the host server's test plugin test.echo 1.0 (--test-plugin)."""

from __future__ import annotations

import asyncio

import pytest
from conftest import LOCALHOST, Server
from echo_half import ASYNC, ASYNC_MARK, ECHO, LED, REFUSE, REFUSE_CODE, Echo

from telemetrix_idf import BoardConnector, ChainError, connect
from telemetrix_idf.wire import Reason

LED_PIN = 36
LED_READBACK = 35


def board_of(srv: Server) -> BoardConnector:
    return connect(LOCALHOST, srv.port, timeout=2.0, plugins=(Echo,))


@pytest.mark.asyncio
async def test_echo_command_report_on_on_report(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        half = board.plugin("test.echo")
        assert isinstance(half, Echo)
        await half.command(ECHO, b"hello")
        assert await asyncio.wait_for(half.reports.get(), 2) == (ECHO, b"hello")


@pytest.mark.asyncio
async def test_echo_request_returns_the_echoed_args(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        half = board.plugin("test.echo")
        assert await half.request(ECHO, b"abc") == b"abc"
        assert await half.request(ECHO, b"de") == b"de"  # a second request: the id is not the first one's


@pytest.mark.asyncio
async def test_refusal_chain_error_reason_plugin_code_7(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        with pytest.raises(ChainError) as err:
            await board.plugin("test.echo").command(REFUSE)
        result = err.value.results[0]
        assert (result.status, result.reason, result.code) == ("refused", Reason.PLUGIN, REFUSE_CODE)


@pytest.mark.asyncio
async def test_async_request_answered_from_a_task(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        assert await board.plugin("test.echo").request(ASYNC) == bytes([ASYNC_MARK])


@pytest.mark.asyncio
async def test_named_pin_claim_on_test_led_pin_driven_high(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        # the host server loops pin 36 back to input pin 35
        await board.pin(LED_READBACK).input()
        changes = board.digital_changes(LED_READBACK)
        await board.plugin("test.echo").command(LED, bytes([LED_PIN]))

        async def high() -> int:
            async for event in changes:
                if event.value == 1:
                    return event.pin
            raise AssertionError("the event stream ended")

        assert await asyncio.wait_for(high(), 2) == LED_READBACK


@pytest.mark.asyncio
async def test_named_pin_claimed_pin_busy_for_other_uses(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        await board.plugin("test.echo").command(LED, bytes([LED_PIN]))
        with pytest.raises(ChainError) as err:
            await board.pin(LED_PIN).output()
        assert err.value.results[0].reason == Reason.PIN_BUSY


@pytest.mark.asyncio
async def test_named_pin_claim_on_pin_without_the_name_refused(ext_server: Server) -> None:
    async with board_of(ext_server) as board:
        with pytest.raises(ChainError) as err:
            await board.plugin("test.echo").command(LED, bytes([17]))
        assert err.value.results[0].reason == Reason.NO_CAPABILITY
