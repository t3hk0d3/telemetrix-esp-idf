"""Stock-mode rules (constraints "Client: stock mode") not covered by test_handshake/test_chain/test_events."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import pytest
from fake_stock_server import FakeStockServer

from telemetrix_idf import Board, NotSupported, connect

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
