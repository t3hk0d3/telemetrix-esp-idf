"""A scripted in-process fake of this repo's server in extended mode, and discovery record builders.

Records and replies are built here from the README's layouts with plain byte arithmetic, not with
`telemetrix_idf.wire`.
"""

from __future__ import annotations

from collections.abc import Sequence

from fake_stock_server import FakeServerBase

__all__ = [
    "PAGE_RECORD_BYTES",
    "FakeExtServer",
    "bus_i2c_rec",
    "bus_one_wire_rec",
    "chip_rec",
    "limit_rec",
    "named_cap_rec",
    "paginate",
    "pin_rec",
    "plugin_rec",
    "rec",
]

PAGE_RECORD_BYTES = 251


def rec(rtype: int, value: bytes) -> bytes:
    return bytes([rtype, len(value)]) + value


def _i8(v: int) -> int:
    return v & 0xFF


def chip_rec(model: int, revision: int, idf: str) -> bytes:
    return rec(1, model.to_bytes(2, "big") + revision.to_bytes(2, "big") + idf.encode())


def pin_rec(
    gpio: int, caps: int, safe: int = 0, adc_unit: int = -1, adc_channel: int = -1, named: Sequence[int] = ()
) -> bytes:
    return rec(2, bytes([gpio]) + caps.to_bytes(2, "big") + bytes([safe, _i8(adc_unit), _i8(adc_channel), *named]))


def bus_i2c_rec(bus: int, sda: int, scl: int, speed_hz: int) -> bytes:
    return rec(3, bytes([0, bus, _i8(sda), _i8(scl)]) + speed_hz.to_bytes(4, "big"))


def bus_one_wire_rec(bus: int, gpio: int) -> bytes:
    return rec(3, bytes([1, bus, _i8(gpio)]))


def limit_rec(kind: int, count: int) -> bytes:
    return rec(4, bytes([kind]) + count.to_bytes(2, "big"))


def plugin_rec(slot: int, major: int, minor: int, name: str) -> bytes:
    return rec(5, bytes([slot, major, minor]) + name.encode())


def named_cap_rec(cap_id: int, name: str) -> bytes:
    return rec(6, bytes([cap_id]) + name.encode())


def paginate(records: Sequence[bytes]) -> list[bytes]:
    """Pack whole records into pages of at most 251 record bytes, as the server does."""
    pages: list[bytes] = [b""]
    for r in records:
        if len(pages[-1]) + len(r) > PAGE_RECORD_BYTES:
            pages.append(b"")
        pages[-1] += r
    return pages


class FakeExtServer(FakeServerBase):
    """Answers the magic probe with the long answer `[7,5,3,2,100,0xA7,major,minor]` and GET_CAPABILITIES from
    `pages` (record bytes per page). Any other probe payload gets the stock `[4,5,3,2,100]`."""

    def __init__(
        self,
        *,
        pages: Sequence[bytes] = (b"",),
        ext: tuple[int, int] = (1, 0),
        silent: bool = False,
        pre_handshake: bytes = b"",
    ) -> None:
        super().__init__(pre_handshake=pre_handshake, silent=silent)
        self.pages = list(pages)
        self.ext = ext

    def handle(self, cmd: int, payload: bytes) -> bytes | None:
        if cmd == 5:
            if len(payload) == 3 and payload[0] == 0xA7:
                return bytes([7, 5, 3, 2, 100, 0xA7, self.ext[0], self.ext[1]])
            return bytes([4, 5, 3, 2, 100])
        if cmd == 59 and payload:
            page = payload[0]
            last = len(self.pages) - 1
            if page > last:
                return b""  # the real server sends ERROR BAD_ARGUMENT; the client never asks past last_page
            body = bytes([page, last]) + self.pages[page]
            return bytes([1 + len(body), 60]) + body
        return b""
