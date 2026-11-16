"""A scripted in-process fake of this repo's server in extended mode, and discovery record builders.

Records and replies are built here from the README's layouts with plain byte arithmetic, not with
`telemetrix_idf.wire`.
"""

from __future__ import annotations

from collections.abc import Callable, Sequence

from fake_stock_server import FakeServerBase

__all__ = [
    "PAGE_RECORD_BYTES",
    "FakeExtServer",
    "bus_i2c_rec",
    "bus_one_wire_rec",
    "chip_rec",
    "crc8",
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


def crc8(data: bytes) -> int:
    """Dallas/Maxim CRC-8 (polynomial 0x31 reflected), as the OneWire library computes it."""
    crc = 0
    for byte in data:
        for _ in range(8):
            mix = (crc ^ byte) & 1
            crc >>= 1
            if mix:
                crc ^= 0x8C
            byte >>= 1
    return crc


class FakeExtServer(FakeServerBase):
    """Answers the magic probe with the long answer `[7,5,3,2,100,0xA7,major,minor]` and GET_CAPABILITIES from
    `pages` (record bytes per page). Any other probe payload gets the stock `[4,5,3,2,100]`.

    After the long answer it numbers every packet from 0 and mirrors the server's extended semantics: SYNC gets
    SYNC_REPLY `[tag, seq]` and sets the abort flag (on at the start); a refused command gets ERROR
    `[seq, cmd, reason]`, and with abort on every later command up to the next SYNC gets ERROR SKIPPED.

    Scriptable: `refuse(cmd, payload)` returns a reason to refuse a command (ON_BUS is passed as 58 with its
    payload); `refuse_pages` are discovery pages answered with ERROR BAD_ARGUMENT; `i2c_nack` are addresses whose
    I2C read gets ERROR NO_ANSWER; `buses` are the bus numbers ON_BUS accepts (others: BUS_UNKNOWN); `roms` are the
    1-wire devices ONE_WIRE_SEARCH finds; `hold` keeps every reply in `held` until `release()`;
    `plugin(slot, sub, args)` answers PLUGIN: a list of `(sub, data)` sent as PLUGIN_REPORTs of that slot, or an int,
    the plugin's refusal code (ERROR PLUGIN with `[slot, sub, code]`). Without `plugin` every PLUGIN gets ERROR
    NO_SUCH_SLOT.
    """

    def __init__(
        self,
        *,
        pages: Sequence[bytes] = (b"",),
        ext: tuple[int, int] = (1, 0),
        silent: bool = False,
        pre_handshake: bytes = b"",
        refuse: Callable[[int, bytes], int | None] | None = None,
        refuse_pages: frozenset[int] | set[int] = frozenset(),
        i2c_nack: frozenset[int] | set[int] = frozenset(),
        buses: frozenset[int] | set[int] = frozenset({0, 1}),
        roms: Sequence[bytes] = (),
        plugin: Callable[[int, int, bytes], list[tuple[int, bytes]] | int] | None = None,
    ) -> None:
        super().__init__(pre_handshake=pre_handshake, silent=silent)
        self.pages = list(pages)
        self.ext = ext
        self.refuse = refuse
        self.refuse_pages = set(refuse_pages)
        self.i2c_nack = set(i2c_nack)
        self.buses = set(buses)
        self.roms = list(roms)
        self.plugin = plugin
        self.hold = False
        self.held = bytearray()
        self.numbered = False
        self.seq = 0
        self.abort = True
        self.failed = False
        self._search = 0

    def release(self) -> None:
        """Send the held replies and stop holding."""
        self.hold = False
        data, self.held = bytes(self.held), bytearray()
        self.push(data)

    def handle(self, cmd: int, payload: bytes) -> bytes | None:
        reply = self._handle(cmd, payload)
        if reply and self.hold:
            self.held += reply
            return b""
        return reply

    def _handle(self, cmd: int, payload: bytes) -> bytes | None:
        if cmd == 5:
            if len(payload) == 3 and payload[0] == 0xA7:
                self.numbered, self.seq = True, 0
                return bytes([7, 5, 3, 2, 100, 0xA7, self.ext[0], self.ext[1]])
            return bytes([4, 5, 3, 2, 100])
        if not self.numbered:
            return b""
        seq = self.seq
        self.seq = (self.seq + 1) & 0xFFFF
        if cmd == 57:
            self.abort, self.failed = bool(payload[2] & 1), False
            return bytes([5, 58, payload[0], payload[1], seq >> 8, seq & 0xFF])
        if self.abort and self.failed:
            return self._error(seq, cmd, 11)
        reason = self.refuse(cmd, payload) if self.refuse else None
        if reason:
            return self._error(seq, cmd, reason)
        if cmd == 59:
            page = payload[0]
            if page in self.refuse_pages or page >= len(self.pages):
                return self._error(seq, cmd, 5)
            body = bytes([page, len(self.pages) - 1]) + self.pages[page]
            return bytes([1 + len(body), 60]) + body
        if cmd == 60:
            return self._plugin(seq, payload[0], payload[1], payload[2:])
        if cmd == 58:
            bus = payload[0]
            if bus not in self.buses:
                return self._error(seq, cmd, 6)
            out = self._stock(payload[1], payload[2:])
            if isinstance(out, int):
                return self._error(seq, cmd, out)
            if out is None:
                return b""
            body = bytes([bus]) + out
            return bytes([1 + len(body), 59]) + body
        out = self._stock(cmd, payload)
        if isinstance(out, int):
            return self._error(seq, cmd, out)
        return b"" if out is None else bytes([len(out)]) + out

    def _plugin(self, seq: int, slot: int, sub: int, args: bytes) -> bytes:
        if self.plugin is None:
            self.failed = True
            return bytes([7, 57, seq >> 8, seq & 0xFF, 60, 12, slot, sub])
        out = self.plugin(slot, sub, args)
        if isinstance(out, int):
            self.failed = True
            return bytes([8, 57, seq >> 8, seq & 0xFF, 60, 10, slot, sub, out])
        return b"".join(bytes([3 + len(data), 61, slot, rsub]) + data for rsub, data in out)

    def _error(self, seq: int, cmd: int, reason: int) -> bytes:
        if reason != 11:
            self.failed = True
        return bytes([5, 57, seq >> 8, seq & 0xFF, cmd, reason])

    def _stock(self, cmd: int, payload: bytes) -> bytes | int | None:
        """A stock command's report `[id, payload...]` (no length byte), a refusal reason, or None (no report)."""
        if cmd == 10:  # I2C_READ [addr, reg, count, stop]
            addr, reg, count = payload[0], payload[1], payload[2]
            if addr in self.i2c_nack:
                return 9  # NO_ANSWER
            return bytes([9, count, addr, reg]) + bytes(range(count))
        if cmd == 28:  # ONE_WIRE_RESET: presence
            return bytes([14, 28, 1 if self.roms else 0])
        if cmd == 32:  # ONE_WIRE_READ
            return bytes([14, 32, 0x5A])
        if cmd == 33:  # ONE_WIRE_RESET_SEARCH
            self._search = 0
            return None
        if cmd == 34:  # ONE_WIRE_SEARCH: the next ROM, all 0xff when none is left
            rom = self.roms[self._search] if self._search < len(self.roms) else b"\xff" * 8
            self._search += 1
            return bytes([14, 34]) + rom
        if cmd == 35:  # ONE_WIRE_CRC8 [n, bytes...]
            return bytes([14, 35, crc8(payload[1 : 1 + payload[0]])])
        return None
