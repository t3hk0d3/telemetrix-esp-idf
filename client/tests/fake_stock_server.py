"""In-process fakes of a Telemetrix server, for the client's unit tests.

`FakeStockServer` speaks upstream Telemetrix4Esp32 3.2.0 stock mode. Its replies are literal bytes, not built with
`telemetrix_idf.wire`, so the tests do not just mirror the codec. Upstream facts it reproduces:

- `GET_FIRMWARE_VERSION` gets `[4, 5, 3, 2, 0]` whatever its payload (the sketch reads and ignores the payload);
- reports of pins configured by an earlier session keep coming, before any command (`pre_handshake`);
- `SERVO_ATTACH` past the servo limit gets `SERVO_UNAVAILABLE [2, 6, pin]`;
- an `I2C_READ` that is not answered gets `[3, 8, 1, addr]`; an `I2C_WRITE` NACK reports nothing;
- `RESET` reboots: the server drops the connection;
- `LOOP [tag]` gets `[2, 0, tag]`;
- 1-wire: `OneWire *ow = NULL` until `ONE_WIRE_INIT`, and every 1-wire op but init and CRC8 dereferences it unchecked,
  so one before any init crashes the board (`crashed` is set, the connection drops). Each init leaks a `new OneWire`
  (counted in `ow_inits`). The OneWire object outlives the session, like on the board;
- `SONAR_NEW` and `DHT_NEW` write `sonars[]`/`dhts[]` (6 each) unbounded and the counts outlive the session: a 7th
  crashes the board (`crashed`, the connection drops). `power_cycle()` clears what a reboot clears.

Scriptable: `firmware` (the 3 version bytes), `silent` (answer nothing), `pre_handshake` (raw bytes sent on accept),
`servo_limit` (attaches that succeed), `i2c_nack` (addresses that do not answer). `received` lists every packet
`(id, payload)` the server got, over all connections; `push(data)` sends raw bytes to every open connection.
"""

from __future__ import annotations

import asyncio
import contextlib

__all__ = ["FakeServerBase", "FakeStockServer"]


class FakeServerBase:
    """A TCP server on 127.0.0.1 with a free port; subclasses answer packets in `handle`."""

    def __init__(self, *, pre_handshake: bytes = b"", silent: bool = False) -> None:
        self.pre_handshake = pre_handshake
        self.silent = silent
        self.received: list[tuple[int, bytes]] = []
        self.raw = bytearray()
        self.connections = 0
        self.eof = asyncio.Event()  # set when a client closed its side
        self._writers: list[asyncio.StreamWriter] = []
        self._server: asyncio.Server | None = None
        self.port = 0

    async def __aenter__(self) -> FakeServerBase:
        self._server = await asyncio.start_server(self._serve, "127.0.0.1", 0)
        self.port = self._server.sockets[0].getsockname()[1]
        return self

    async def __aexit__(self, *exc: object) -> None:
        assert self._server is not None
        self._server.close()
        for w in self._writers:
            w.close()
        with contextlib.suppress(Exception):
            await self._server.wait_closed()

    def push(self, data: bytes) -> None:
        for w in self._writers:
            w.write(data)

    def drop(self) -> None:
        """Close every open connection (the client sees EOF)."""
        for w in self._writers:
            w.close()

    def handle(self, cmd: int, payload: bytes) -> bytes | None:
        """Return the reply bytes for one packet; `None` closes the connection."""
        return b""

    async def _serve(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.connections += 1
        self._writers.append(writer)
        if self.pre_handshake:
            writer.write(self.pre_handshake)
        buf = bytearray()
        try:
            while True:
                data = await reader.read(4096)
                if not data:
                    self.eof.set()
                    return
                self.raw += data
                buf += data
                while buf and len(buf) >= 1 + buf[0]:
                    n = buf[0]
                    if n == 0:
                        del buf[:1]
                        continue
                    cmd, payload = buf[1], bytes(buf[2 : 1 + n])
                    del buf[: 1 + n]
                    self.received.append((cmd, payload))
                    if self.silent:
                        continue
                    reply = self.handle(cmd, payload)
                    if reply is None:
                        writer.close()
                        return
                    if reply:
                        writer.write(reply)
                await writer.drain()
        except (ConnectionError, OSError):
            self.eof.set()
        finally:
            if writer in self._writers:
                self._writers.remove(writer)
            writer.close()


class FakeStockServer(FakeServerBase):
    def __init__(
        self,
        *,
        firmware: bytes = bytes([3, 2, 0]),
        silent: bool = False,
        pre_handshake: bytes = b"",
        servo_limit: int | None = None,
        i2c_nack: frozenset[int] | set[int] = frozenset(),
    ) -> None:
        super().__init__(pre_handshake=pre_handshake, silent=silent)
        self.firmware = firmware
        self.servo_limit = servo_limit
        self.i2c_nack = set(i2c_nack)
        self.servos = 0
        self.crashed = False
        self.power_cycle()

    MAX_SONARS = 6  # Telemetrix4Esp32 3.2.0 MAX_SONARS
    MAX_DHTS = 6  # Telemetrix4Esp32 3.2.0 MAX_DHTS

    def power_cycle(self) -> None:
        """What a reboot clears: the OneWire object and the sensor counts."""
        self.ow_inits = 0
        self.sonars = 0
        self.dhts = 0

    def _crash(self) -> None:
        self.crashed = True
        self.power_cycle()

    def handle(self, cmd: int, payload: bytes) -> bytes | None:
        if cmd == 0 and payload:  # LOOP_COMMAND [tag]
            return bytes([2, 0, payload[0]])
        if cmd == 5:  # GET_FIRMWARE_VERSION, payload ignored
            return bytes([1 + len(self.firmware), 5]) + self.firmware
        if cmd == 6 and payload:  # SERVO_ATTACH [pin, min, max]
            if self.servo_limit is not None and self.servos >= self.servo_limit:
                return bytes([2, 6, payload[0]])
            self.servos += 1
            return b""
        if cmd == 10 and len(payload) >= 3:  # I2C_READ [addr, reg, count, stop]
            addr, reg, count = payload[0], payload[1], payload[2]
            if addr in self.i2c_nack:
                return bytes([3, 8, 1, addr])
            data = bytes(range(count))
            return bytes([4 + count, 9, count, addr, reg]) + data
        if cmd == 20:  # RESET reboots the board
            return None
        if cmd == 12 or cmd == 13:  # SONAR_NEW / DHT_NEW: unbounded writes into 6-entry arrays
            if cmd == 12:
                self.sonars += 1
                over = self.sonars > self.MAX_SONARS
            else:
                self.dhts += 1
                over = self.dhts > self.MAX_DHTS
            if over:
                self._crash()
                return None
            return b""
        if cmd == 27:  # ONE_WIRE_INIT: ow = new OneWire(pin), the old one leaks
            self.ow_inits += 1
            return b""
        if cmd == 35:  # ONE_WIRE_CRC8: static, safe without init
            return bytes([3, 14, 35, _crc8(payload[1 : 1 + payload[0]] if payload else b"")])
        if 28 <= cmd <= 34:  # every other 1-wire op dereferences ow unchecked
            if self.ow_inits == 0:
                self._crash()
                return None
            if cmd == 28:  # reset: presence
                return bytes([3, 14, 28, 1])
            if cmd == 32:  # read
                return bytes([3, 14, 32, 0x5A])
            if cmd == 34:  # search: no device
                return bytes([10, 14, 34]) + b"\xff" * 8
            return b""
        return b""


def _crc8(data: bytes) -> int:
    """Dallas/Maxim CRC-8, as OneWire::crc8."""
    crc = 0
    for byte in data:
        for _ in range(8):
            mix = (crc ^ byte) & 1
            crc >>= 1
            if mix:
                crc ^= 0x8C
            byte >>= 1
    return crc
