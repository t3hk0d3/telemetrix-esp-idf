"""Fixtures: the host server as a subprocess on a free port, raw sockets and the stock telemetrix-esp32 client."""

import re
import signal
import socket
import subprocess
import threading
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SERVER_ELF = ROOT / "host" / "server" / "build" / "telemetrix_host_server.elf"
LOCALHOST = "127.0.0.1"
IO_TIMEOUT = 5.0


def free_port():
    """A port nobody listens on right now: bind 0, read it back, close."""
    with socket.socket() as s:
        s.bind((LOCALHOST, 0))
        return s.getsockname()[1]


class Server:
    """The host server process. Its stdout is read by a thread; SIGUSR2 makes it print one `status` line."""

    def __init__(self, *args):
        self.port = free_port()
        self.proc = subprocess.Popen(
            [str(SERVER_ELF), "--port", str(self.port), *map(str, args)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.lines = []
        self.cond = threading.Condition()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self._wait_line(lambda line: line == f"listening {self.port}", 10)

    def _read(self):
        for line in self.proc.stdout:
            with self.cond:
                self.lines.append(line.rstrip("\n"))
                self.cond.notify_all()

    def _wait_line(self, match, timeout, start=0):
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                for line in self.lines[start:]:
                    if match(line):
                        return line
                left = deadline - time.monotonic()
                if (
                    left <= 0
                    or self.proc.poll() is not None
                    and not self.reader.is_alive()
                ):
                    raise AssertionError(
                        f"no matching server output; got {self.lines[start:]}"
                    )
                self.cond.wait(min(left, 0.1))

    def status(self):
        """The server's counters (and the client it serves, '-' for none) as a dict."""
        with self.cond:
            start = len(self.lines)
        self.proc.send_signal(signal.SIGUSR2)
        line = self._wait_line(lambda x: x.startswith("status "), IO_TIMEOUT, start)
        return {
            k: (v if k == "client" else int(v))
            for k, v in re.findall(r"(\w+)=(\S+)", line)
        }

    def wait_idle(self, timeout=IO_TIMEOUT):
        """Wait until the server serves nobody, so the next connection is not refused."""
        deadline = time.monotonic() + timeout
        while self.status()["client"] != "-":
            assert time.monotonic() < deadline, "server still serves a client"
            time.sleep(0.02)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        try:
            code = self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise
        finally:
            self.reader.join(2)
        return code


class Raw:
    """A plain TCP client speaking the stock framing: [len][id][payload]."""

    def __init__(self, port, rcvbuf=None):
        self.sock = socket.socket()
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.settimeout(IO_TIMEOUT)
        self.sock.connect((LOCALHOST, port))

    def send(self, *packet):
        """Send one packet given as id and payload bytes."""
        self.sock.sendall(bytes([len(packet), *packet]))

    def recv_exact(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            assert chunk, "connection closed"
            data += chunk
        return data

    def recv_report(self):
        """One report as [id, payload...]."""
        n = self.recv_exact(1)[0]
        return list(self.recv_exact(n))

    def loop(self, char=0x42):
        """LOOP round trip: the reply must echo the character."""
        self.send(0, char)
        assert self.recv_report() == [0, char]

    def close(self):
        self.sock.close()


@pytest.fixture(scope="session")
def server():
    srv = Server()
    yield srv
    assert srv.stop() == 0, "the server did not exit cleanly on SIGTERM"


@pytest.fixture
def make_server():
    """Start extra server processes with their own arguments; all are stopped after the test."""
    started = []

    def make(*args):
        srv = Server(*args)
        started.append(srv)
        return srv

    yield make
    for srv in started:
        assert srv.stop() == 0


@pytest.fixture
def raw(server):
    """Raw connections to the shared server; each is closed and the server idle again after the test."""
    server.wait_idle()
    conns = []

    def connect(srv=server, **kw):
        conns.append(Raw(srv.port, **kw))
        return conns[-1]

    yield connect
    for conn in conns:
        conn.close()
    server.wait_idle()


@pytest.fixture
def stock(server):
    """The stock client on the shared server, shut down (STOP_ALL_REPORTS, RESET) after the test."""
    from telemetrix_esp32 import telemetrix_esp32

    server.wait_idle()
    clients = []

    def connect(srv=server):
        client = telemetrix_esp32.TelemetrixEsp32(LOCALHOST, ip_port=srv.port)
        clients.append(client)
        return client

    yield connect
    for client in clients:
        client.shutdown()
        # the client's receiver thread sits in recv(1) and keeps a closed socket alive: end the stream first
        client.sock.shutdown(socket.SHUT_RDWR)
        client.sock.close()
    server.wait_idle()
