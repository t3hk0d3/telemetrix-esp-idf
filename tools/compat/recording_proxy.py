"""A recording TCP proxy for the stock Telemetrix protocol.

    recording_proxy.py --listen PORT --target HOST:PORT --log FILE [--strict] [--bind ADDRESS]

Bytes are forwarded unchanged in both directions as they arrive. A copy is framed with the stock rule
`[len][id][payload]` (`len` counts the id and the payload) and written as one JSON line per packet:

    {"dir": "c2s"|"s2c", "t": seconds since the proxy started, "len": length byte, "id": id or null, "bytes": payload hex}

A packet with `len` 0 has no id (`id` null, no bytes). With --strict the exit status is 1 when a server-to-client
packet has an id the stock client does not dispatch or a length that id does not allow (known_reports.py), or when the
server sends `len` 0, which makes the stock client raise. A violation is also written to stderr when it happens.
SIGINT and SIGTERM shut the proxy down cleanly.
"""

import argparse
import json
import signal
import socket
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import known_reports  # noqa: E402

C2S = "c2s"
S2C = "s2c"
CHUNK = 4096


class Framer:
    """Cuts a byte stream into `[len][id][payload]` packets. Yields (len, id or None, payload bytes)."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        while self.buf:
            n = self.buf[0]
            if n == 0:
                del self.buf[0]
                yield 0, None, b""
            elif len(self.buf) > n:
                packet = bytes(self.buf[1 : 1 + n])
                del self.buf[: 1 + n]
                yield n, packet[0], packet[1:]
            else:
                return


class RecordingProxy:
    """Listens, forwards every connection to the target and records both directions."""

    def __init__(self, listen_port, target, log_path, strict=False, bind="127.0.0.1"):
        self.target = target
        self.strict = strict
        self.violations = []
        self.stop_event = threading.Event()
        self.start_time = time.monotonic()
        self.log_lock = threading.Lock()
        self.log = open(log_path, "w", buffering=1)  # noqa: SIM115 - closed by stop()
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind((bind, listen_port))
        self.listener.listen(4)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.threads = []
        self.conns = []
        self.accept_thread = threading.Thread(target=self._accept_loop, daemon=True)

    def start(self):
        self.accept_thread.start()
        return self

    def stop(self):
        """Close everything and return the strict violations (a list of text lines)."""
        self.stop_event.set()
        self.accept_thread.join(5)
        for conn in list(self.conns):
            for sock in conn:
                _close(sock)
        for thread in self.threads:
            thread.join(5)
        self.listener.close()
        self.log.close()
        return list(self.violations)

    def _accept_loop(self):
        while not self.stop_event.is_set():
            try:
                client, _ = self.listener.accept()
            except TimeoutError:
                continue
            except OSError:
                return
            try:
                server = socket.create_connection(self.target, timeout=5)
            except OSError as exc:
                sys.stderr.write(f"proxy: cannot reach {self.target}: {exc}\n")
                _close(client)
                continue
            server.settimeout(None)
            client.settimeout(None)
            for sock in (client, server):
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.conns.append((client, server))
            for src, dst, direction in ((client, server, C2S), (server, client, S2C)):
                thread = threading.Thread(target=self._pump, args=(src, dst, direction), daemon=True)
                self.threads.append(thread)
                thread.start()

    def _pump(self, src, dst, direction):
        framer = Framer()
        try:
            while True:
                data = src.recv(CHUNK)
                if not data:
                    break
                dst.sendall(data)
                for length, report_id, payload in framer.feed(data):
                    self._record(direction, length, report_id, payload)
        except OSError:
            _close(src)
            _close(dst)  # a reset on one side is a reset on the other
            return
        try:
            dst.shutdown(socket.SHUT_WR)  # pass the end of stream on, keep the other direction open
        except OSError:
            pass

    def _record(self, direction, length, report_id, payload):
        entry = {
            "dir": direction,
            "t": round(time.monotonic() - self.start_time, 6),
            "len": length,
            "id": report_id,
            "bytes": payload.hex(),
        }
        with self.log_lock:
            if not self.log.closed:
                self.log.write(json.dumps(entry) + "\n")
            if direction == S2C and self.strict:
                reason = "length 0 (the stock client raises on it)" if report_id is None else None
                if report_id is not None:
                    reason = known_reports.check_report(report_id, payload)
                if reason:
                    line = f"strict: {reason}: {length:02x}{(bytes([report_id]) + payload).hex()}"
                    self.violations.append(line)
                    sys.stderr.write(line + "\n")


def _close(sock):
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


def parse_target(text):
    host, _, port = text.rpartition(":")
    if not host or not port.isdigit():
        raise argparse.ArgumentTypeError("target must be HOST:PORT")
    return host, int(port)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--listen", type=int, required=True, metavar="PORT")
    parser.add_argument("--target", type=parse_target, required=True, metavar="HOST:PORT")
    parser.add_argument("--log", required=True, metavar="FILE")
    parser.add_argument("--strict", action="store_true", help="exit 1 on a server report the stock client does not know")
    parser.add_argument("--bind", default="127.0.0.1", metavar="ADDRESS")
    args = parser.parse_args(argv)

    proxy = RecordingProxy(args.listen, args.target, args.log, args.strict, args.bind).start()
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    print(f"listening {proxy.port}", flush=True)
    while not stop.wait(0.2):
        pass
    violations = proxy.stop()
    if args.strict and violations:
        print(f"{len(violations)} strict violation(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
