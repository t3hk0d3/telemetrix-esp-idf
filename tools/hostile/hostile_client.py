#!/usr/bin/env python3
"""A hostile client for the telemetrix server: floods, garbage, a stalled reader and a connection storm.

Standalone (standard library only, raw bytes) so it runs against the host server and against a board. After each mode
a fresh connection must still get the handshake answer and a LOOP reply; otherwise the script exits 1.

    hostile_client.py --host H --port P --mode {flood,garbage,ext-garbage,stall,storm,all} [--seconds N]

`stall` waits up to --stall-limit seconds (default 20) for the server to drop the client that does not read; the
server's default is 10 s (start the host server with --stall-ms to shorten it). `stall` needs Linux: it reads the
socket's TCP state from TCP_INFO, because a client that does not read cannot see the peer's close through recv (a
peek returns the buffered reports, not EOF) and a send may keep succeeding into the buffer. Without TCP_INFO (macOS,
Windows) the mode exits 1 with "stall mode needs Linux TCP_INFO"; the other modes run anywhere.

WARNING: garbage and ext-garbage send arbitrary commands and may drive pins of the target board.
"""

from __future__ import annotations

import argparse
import random
import socket
import struct
import sys
import threading
import time

MAGIC = 0xA7
CMD_LOOP = 0
CMD_GET_FIRMWARE_VERSION = 5
STORM_CONNECTIONS = 150
STORM_SECONDS = 3.0  # at most this long opening connections
STORM_CONNECT_SECONDS = 0.2  # a full accept backlog must not stall the storm
PROBE_SECONDS = 8.0  # how long a fresh connection is retried: the server serves one client and must first see the last gone
IO_TIMEOUT = 3.0
MODES = ("flood", "garbage", "ext-garbage", "stall", "storm")


class ProbeFailed(Exception):
    """A fresh connection did not get its answers."""


def packet(cmd: int, payload: bytes = b"") -> bytes:
    return bytes([len(payload) + 1, cmd]) + payload


def recv_exact(sock: socket.socket, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("closed by the server")
        data += chunk
    return data


def recv_report(sock: socket.socket) -> bytes:
    """One report as [id, payload...]."""
    return recv_exact(sock, recv_exact(sock, 1)[0])


def probe_once(host: str, port: int) -> None:
    """Handshake and LOOP on a fresh connection; raises OSError or ProbeFailed."""
    with socket.create_connection((host, port), timeout=IO_TIMEOUT) as sock:
        sock.settimeout(IO_TIMEOUT)
        sock.sendall(packet(CMD_GET_FIRMWARE_VERSION, bytes([MAGIC, 1, 0])))
        answer = recv_report(sock)
        # [5, major, minor, build] (stock) or that plus [MAGIC, ext major, ext minor] (extended): any firmware version
        if (
            len(answer) not in (4, 7)
            or answer[0] != CMD_GET_FIRMWARE_VERSION
            or (len(answer) == 7 and answer[4] != MAGIC)
        ):
            raise ProbeFailed(f"handshake answer {list(answer)}")
        sock.sendall(packet(CMD_LOOP, b"\x42"))
        if list(recv_report(sock)) != [CMD_LOOP, 0x42]:
            raise ProbeFailed("LOOP reply is not [0, 0x42]")


def probe(host: str, port: int) -> None:
    """probe_once, retried while the server still serves (or is closing) the previous client."""
    deadline = time.monotonic() + PROBE_SECONDS
    while True:
        try:
            probe_once(host, port)
            return
        except (OSError, ProbeFailed) as exc:
            if time.monotonic() >= deadline:
                raise ProbeFailed(
                    f"no clean answer on a fresh connection: {exc!r}"
                ) from exc
            time.sleep(0.1)


def connect(host: str, port: int, timeout: float = IO_TIMEOUT) -> socket.socket | None:
    try:
        sock = socket.create_connection((host, port), timeout=timeout)
    except OSError:
        return None
    sock.settimeout(IO_TIMEOUT)
    return sock


def drain(sock: socket.socket, stop: threading.Event) -> None:
    """Read and drop whatever the server sends until it closes or `stop` is set."""
    sock.settimeout(0.2)
    while not stop.is_set():
        try:
            if not sock.recv(65536):
                return
        except TimeoutError:
            continue
        except OSError:
            return


# --- modes -------------------------------------------------------------------------------------------------------


def flood(host: str, port: int, seconds: float) -> None:
    """Valid LOOP commands as fast as the socket takes them, the replies drained on a thread."""
    deadline = time.monotonic() + seconds
    burst = packet(CMD_LOOP, b"\x01") * 512
    while time.monotonic() < deadline:
        sock = connect(host, port)
        if sock is None:
            time.sleep(0.05)
            continue
        stop = threading.Event()
        reader = threading.Thread(target=drain, args=(sock, stop), daemon=True)
        reader.start()
        try:
            while time.monotonic() < deadline:
                sock.sendall(burst)
        except OSError:
            pass
        finally:
            stop.set()
            reader.join(1)
            sock.close()


def garbage_bytes(rng: random.Random) -> bytes:
    """Random bytes, now and then framed to look like packets: lengths of 0, 255, and ids anywhere."""
    out = bytearray()
    for _ in range(rng.randrange(1, 64)):
        kind = rng.randrange(4)
        if kind == 0:
            out += rng.randbytes(rng.randrange(1, 256))
        elif kind == 1:
            out += bytes([0])
        elif kind == 2:
            out += bytes([255, rng.randrange(256)]) + rng.randbytes(
                rng.randrange(0, 300)
            )
        else:
            n = rng.randrange(1, 40)
            out += bytes([n, rng.randrange(256)]) + rng.randbytes(n - 1)
    return bytes(out)


def hostile_sessions(host: str, port: int, seconds: float, extended: bool) -> None:
    """Connect, optionally handshake into extended mode, send garbage until the server drops us; repeat."""
    rng = random.Random(1234)
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        sock = connect(host, port)
        if sock is None:
            time.sleep(0.05)
            continue
        stop = threading.Event()
        reader = threading.Thread(target=drain, args=(sock, stop), daemon=True)
        reader.start()
        try:
            if extended:
                sock.sendall(packet(CMD_GET_FIRMWARE_VERSION, bytes([MAGIC, 1, 0])))
            for _ in range(200):
                if time.monotonic() >= deadline:
                    break
                sock.sendall(
                    garbage_bytes(rng) if not extended else extended_garbage(rng)
                )
        except OSError:
            pass
        finally:
            stop.set()
            reader.join(1)
            sock.close()


def extended_garbage(rng: random.Random) -> bytes:
    """Well-framed packets with hostile content: every command id (0..255), payload lengths from 0 to the 254 cap, and
    plain random bytes in between."""
    out = bytearray()
    for _ in range(rng.randrange(1, 32)):
        if rng.randrange(8) == 0:
            out += rng.randbytes(rng.randrange(1, 64))
            continue
        payload = rng.randbytes(rng.choice((0, 1, 2, 3, 8, 64, 254)))
        out += packet(rng.randrange(256), payload)
    return bytes(out)


def garbage(host: str, port: int, seconds: float) -> None:
    hostile_sessions(host, port, seconds, extended=False)


def ext_garbage(host: str, port: int, seconds: float) -> None:
    hostile_sessions(host, port, seconds, extended=True)


def dropped(sock: socket.socket) -> bool:
    """True once the peer closed or reset. Linux: the TCP state from TCP_INFO (the buffered reports stay unread)."""
    try:
        info = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 8)
    except OSError:
        return True  # the socket is no longer valid
    return struct.unpack("B", info[:1])[0] != 1  # 1 = ESTABLISHED


def stall(host: str, port: int, limit: float) -> None:
    """Send LOOPs and never read: the server's send buffer fills, then it must drop us within `limit` seconds."""
    if not hasattr(socket, "TCP_INFO"):
        raise ProbeFailed("stall mode needs Linux TCP_INFO")
    sock = connect(host, port)
    if sock is None:
        raise ProbeFailed("the server accepted no connection for the stalled client")
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
        sock.settimeout(0.3)
        burst = packet(CMD_LOOP, b"\x02") * 1000
        started = time.monotonic()
        while time.monotonic() - started < limit:
            try:
                sock.sendall(burst)
            except TimeoutError:
                pass  # our send buffer is full too: the server is not reading either, or it has stopped replying
            except OSError:
                return  # reset: dropped
            if dropped(sock):
                return
            if time.monotonic() - started > 2.0:
                time.sleep(0.1)
        raise ProbeFailed(f"the stalled client was not dropped within {limit:.0f} s")
    finally:
        sock.close()


def storm(host: str, port: int) -> None:
    """Many connections at once: some close at once, some send a byte and hold, the rest only hold."""
    socks = []
    deadline = time.monotonic() + STORM_SECONDS
    for i in range(STORM_CONNECTIONS):
        if time.monotonic() >= deadline:
            break
        sock = connect(host, port, STORM_CONNECT_SECONDS)
        if sock is None:
            continue
        if i % 3 == 0:
            sock.close()
            continue
        if i % 3 == 1:
            try:
                sock.sendall(b"\x01")
            except OSError:
                pass
        socks.append(sock)
    time.sleep(0.5)
    for sock in socks:
        sock.close()


# --- main --------------------------------------------------------------------------------------------------------


def run_mode(mode: str, args: argparse.Namespace) -> None:
    if mode == "flood":
        flood(args.host, args.port, args.seconds)
    elif mode == "garbage":
        garbage(args.host, args.port, args.seconds)
    elif mode == "ext-garbage":
        ext_garbage(args.host, args.port, args.seconds)
    elif mode == "stall":
        stall(args.host, args.port, args.stall_limit)
    elif mode == "storm":
        storm(args.host, args.port)
    probe(args.host, args.port)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--mode", choices=(*MODES, "all"), required=True)
    parser.add_argument(
        "--seconds",
        type=float,
        default=3.0,
        help="how long flood, garbage and ext-garbage run",
    )
    parser.add_argument(
        "--stall-limit",
        type=float,
        default=20.0,
        help="how long stall waits for the drop (seconds)",
    )
    args = parser.parse_args(argv)

    try:
        probe(args.host, args.port)
    except ProbeFailed as exc:
        print(f"FAIL before any mode: {exc}", file=sys.stderr)
        return 1
    for mode in MODES if args.mode == "all" else (args.mode,):
        started = time.monotonic()
        try:
            run_mode(mode, args)
        except ProbeFailed as exc:
            print(f"FAIL {mode}: {exc}", file=sys.stderr)
            return 1
        print(f"ok {mode} ({time.monotonic() - started:.1f} s)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
