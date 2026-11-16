"""Client plugin halves: the client side of a server plugin (spec 4.6, 5).

Subclass `PluginHalf`, set the class attributes `name` (the server plugin's name) and `major`, and pass the class to
`connect(plugins=[...])`. In extended mode the board binds each registered half whose plugin it runs with the same
major to that plugin's slot, once per board; `board.plugin(name)` returns the bound instance.

- `command(sub, args)` is a `Chain` holding PLUGIN `[slot, sub, args]`; it composes with other ops, and a refusal
  arrives as `ChainError` with reason PLUGIN and the plugin's code in `CommandResult.code`.
- `on_report(sub, data)` (override) gets every PLUGIN_REPORT of the slot, on the board's event dispatch task (never
  the reader); it may be a coroutine function, awaited before the next event.
- `request(sub, args)`: by the request-id convention the first two bytes of the args and of the answering report's
  data are a big-endian u16 request id. The half picks the id (1, 2, 3, ... wrapping past 65535, 0 never used), sends
  the command and returns the data after the id of the first PLUGIN_REPORT of its slot that starts with it and comes
  on the request's own sub (the report's sub equals the command's). That report goes to the request, not to
  `on_report`. A report that matches no waiting request, such as a late answer to a request that timed out, is
  delivered to `on_report`. When the connection closes, every pending request raises `ConnectionLost`.
"""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Iterable
from typing import TYPE_CHECKING, ClassVar

from .errors import TelemetrixError

if TYPE_CHECKING:
    from .board import Board
    from .chain import Chain

__all__ = ["PluginHalf", "check_halves"]

REQUEST_IDS = 1 << 16


class PluginHalf:
    """The client half of one server plugin, bound to a board and the plugin's slot."""

    name: ClassVar[str]
    major: ClassVar[int]

    def __init__(self, board: Board, slot: int) -> None:
        self.board = board
        self.slot = slot
        self._requests: dict[int, tuple[int, asyncio.Future[bytes]]] = {}  # id -> (sub, future)
        self._next_id = 1

    def command(self, sub: int, args: bytes = b"") -> Chain:
        """A chain holding PLUGIN `[slot, sub, args]`; await it to send it."""
        return self.board.chain().plugin(self.slot, sub, args)

    def on_report(self, sub: int, data: bytes) -> Awaitable[None] | None:
        """Called for each PLUGIN_REPORT of this slot no request took. Override; the default ignores it."""
        return None

    async def request(self, sub: int, args: bytes = b"", timeout: float | None = None) -> bytes:
        """Send `command(sub, [id] + args)` and return the data after the id of the report answering it.

        Raises `ChainError` when the command is refused, `asyncio.TimeoutError` when the command and its report take
        longer than `timeout` (default: the board's), `ConnectionLost` when the connection closes.
        """
        rid = self._allocate_id()
        future: asyncio.Future[bytes] = asyncio.get_running_loop().create_future()
        self._requests[rid] = (sub, future)
        wait = self.board.timeout if timeout is None else timeout

        async def exchange() -> bytes:
            await self.command(sub, rid.to_bytes(2, "big") + bytes(args)).run(wait)
            return await future

        try:
            return await asyncio.wait_for(exchange(), wait)
        finally:
            self._requests.pop(rid, None)
            if not future.done():
                future.cancel()

    def _allocate_id(self) -> int:
        for _ in range(REQUEST_IDS):
            rid = self._next_id
            self._next_id = (self._next_id + 1) % REQUEST_IDS
            if rid and rid not in self._requests:
                return rid
        raise TelemetrixError("no free request id")

    def _take(self, sub: int, data: bytes) -> bool:
        """Complete the request whose id `data` starts with, when the report came on the request's own sub (called by
        the reader); False when none waits for it."""
        if len(data) < 2:
            return False
        entry = self._requests.get(int.from_bytes(data[:2], "big"))
        if entry is None or entry[0] != sub or entry[1].done():
            return False
        future = entry[1]
        future.set_result(bytes(data[2:]))
        return True

    def _fail_requests(self, error: BaseException) -> None:
        for _, future in self._requests.values():
            if not future.done():
                future.set_exception(error)
                future.exception()  # mark it retrieved: a request that is no longer awaited must not log at GC

    def __repr__(self) -> str:
        return f"<{type(self).__name__} {self.name!r} {self.major}.x slot={self.slot}>"


def check_halves(halves: Iterable[object]) -> dict[str, type[PluginHalf]]:
    """The registered halves by name. Raises `TypeError` for anything but a `PluginHalf` subclass with a str `name`
    and an int `major`, `ValueError` for two halves of the same name."""
    found: dict[str, type[PluginHalf]] = {}
    for half in halves:
        if not (isinstance(half, type) and issubclass(half, PluginHalf)):
            raise TypeError(f"{half!r} is not a PluginHalf subclass")
        name, major = getattr(half, "name", None), getattr(half, "major", None)
        if not isinstance(name, str) or not isinstance(major, int) or isinstance(major, bool):
            raise TypeError(f"{half.__name__} needs class attributes name (str) and major (int)")
        if name in found:
            raise ValueError(f"two plugin halves named {name!r}")
        found[name] = half
    return found
