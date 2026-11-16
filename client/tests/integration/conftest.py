"""Fixtures: the host server (host/server) built once, then started per test on a free port.

`ext_server` runs it with `--test-plugin` (extended mode, plugin test.echo in slot 0), `stock_server` with
`--stock-only` (our client's stock mode). Mirrors tools/tests/conftest.py. `make client-integration` builds the server
first and sets TMX_HOST_SERVER_BUILT=1; run directly, the session builds it with `make host-server`.
"""

from __future__ import annotations

import os
import re
import signal
import socket
import subprocess
import threading
import time
from collections.abc import Callable, Iterator
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
SERVER_ELF = ROOT / "host" / "server" / "build" / "telemetrix_host_server.elf"
LOCALHOST = "127.0.0.1"
IO_TIMEOUT = 5.0


def free_port() -> int:
    """A port nobody listens on right now: bind 0, read it back, close."""
    with socket.socket() as s:
        s.bind((LOCALHOST, 0))
        return int(s.getsockname()[1])


class Server:
    """The host server process. Its stdout is read by a thread; SIGUSR2 makes it print one `status` line."""

    def __init__(self, *args: str) -> None:
        self.args = args
        self.lines: list[str] = []
        self.cond = threading.Condition()
        # a free port can be taken between picking it and the server binding it: retry once with a new one
        for attempt in (1, 2):
            self.port = free_port()
            self.lines.clear()
            self.proc = subprocess.Popen(
                [str(SERVER_ELF), "--port", str(self.port), *args],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            self.reader = threading.Thread(target=self._read, daemon=True)
            self.reader.start()
            try:
                self._wait_line(lambda line: line == f"listening {self.port}", 10)
                return
            except BaseException:
                self.kill()
                if attempt == 2:
                    raise

    def kill(self) -> None:
        """Leave no process behind."""
        self.proc.kill()
        self.proc.wait()
        self.reader.join(2)

    def _read(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            with self.cond:
                self.lines.append(line.rstrip("\n"))
                self.cond.notify_all()

    def _wait_line(self, match: Callable[[str], bool], timeout: float, start: int = 0) -> str:
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                for line in self.lines[start:]:
                    if match(line):
                        return line
                left = deadline - time.monotonic()
                if left <= 0 or (self.proc.poll() is not None and not self.reader.is_alive()):
                    raise AssertionError(f"no matching server output; got {self.lines[start:]}")
                self.cond.wait(min(left, 0.1))

    def status(self) -> dict[str, int | str]:
        """The server's counters (and the client it serves, '-' for none) as a dict. Blocks: call it in a thread
        from async code (`asyncio.to_thread`)."""
        with self.cond:
            start = len(self.lines)
        self.proc.send_signal(signal.SIGUSR2)
        line = self._wait_line(lambda x: x.startswith("status "), IO_TIMEOUT, start)
        return {k: (v if k == "client" else int(v)) for k, v in re.findall(r"(\w+)=(\S+)", line)}

    def stop(self) -> int:
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        try:
            return self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise
        finally:
            self.reader.join(2)


@pytest.fixture(scope="session")
def server_elf() -> Path:
    """The host server binary, built once per session unless the Makefile already built it."""
    if os.environ.get("TMX_HOST_SERVER_BUILT") != "1":
        subprocess.run(["make", "-C", str(ROOT), "host-server"], check=True, stdout=subprocess.DEVNULL)
    assert SERVER_ELF.exists(), f"{SERVER_ELF} is missing"
    return SERVER_ELF


def _serve(*args: str) -> Iterator[Server]:
    srv = Server(*args)
    yield srv
    try:
        code = srv.stop()
    except (subprocess.TimeoutExpired, OSError):
        srv.kill()
        raise
    assert code == 0, f"the server exited with {code}"


@pytest.fixture
def ext_server(server_elf: Path) -> Iterator[Server]:
    """A host server with the test plugin; our client gets extended mode."""
    yield from _serve("--test-plugin")


@pytest.fixture
def stock_server(server_elf: Path) -> Iterator[Server]:
    """A host server serving stock mode only; our client gets stock mode."""
    yield from _serve("--stock-only")
