"""The client half of the host server's test plugin test.echo 1.0 (test_host/main/test_plugin.c)."""

from __future__ import annotations

import asyncio

from telemetrix_idf import Board, PluginHalf

ECHO = 0  # echoes the args as a PLUGIN_REPORT of sub 0
REFUSE = 1  # refused with the plugin's code 7
ASYNC = 2  # [request id u16]: a task emits [id, 0x55] as sub 2
LED = 3  # [gpio]: claims the pin as "test.led" and drives it high
BUSY = 4  # busy for 50 ms
TICK = 5  # a poll hook emits sub 9 every 1000 ms

REFUSE_CODE = 7
ASYNC_MARK = 0x55


class Echo(PluginHalf):
    name = "test.echo"
    major = 1

    def __init__(self, board: Board, slot: int) -> None:
        super().__init__(board, slot)
        self.reports: asyncio.Queue[tuple[int, bytes]] = asyncio.Queue()

    def on_report(self, sub: int, data: bytes) -> None:
        self.reports.put_nowait((sub, data))
