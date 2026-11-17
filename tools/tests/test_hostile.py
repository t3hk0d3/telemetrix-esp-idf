"""tools/hostile/hostile_client.py against the host server: it survives and leaks no file descriptors."""

import importlib.util
import socket
import subprocess
import sys
import time
from pathlib import Path

from conftest import LOCALHOST, ROOT

SCRIPT = ROOT / "tools" / "hostile" / "hostile_client.py"
STALL_MS = 500


def open_fds(pid):
    return len(list((Path("/proc") / str(pid) / "fd").iterdir()))


def hostile(srv, mode, timeout=120):
    return subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--host",
            LOCALHOST,
            "--port",
            str(srv.port),
            "--mode",
            mode,
        ]
        + ["--seconds", "1", "--stall-limit", "10"],
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def test_mode_all_server_survives_and_fds_return_to_baseline(make_server):
    srv = make_server("--stall-ms", STALL_MS)
    srv.wait_idle()
    before = open_fds(srv.proc.pid)
    result = hostile(srv, "all")
    assert result.returncode == 0, result.stderr
    deadline = time.monotonic() + 5
    while open_fds(srv.proc.pid) != before and time.monotonic() < deadline:
        time.sleep(0.1)
    assert open_fds(srv.proc.pid) == before
    assert srv.proc.poll() is None


def test_stall_drops_the_client_and_counts_it(make_server):
    srv = make_server("--stall-ms", STALL_MS)
    result = hostile(srv, "stall")
    assert result.returncode == 0, result.stderr
    st = srv.status()
    assert st["stalled_clients"] + st["overflowed_clients"] >= 1


def test_unreachable_server_exits_non_zero():
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--host",
            LOCALHOST,
            "--port",
            "1",
            "--mode",
            "storm",
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert result.returncode == 1


def test_stall_without_tcp_info_exits_non_zero_with_a_clear_message(
    make_server, monkeypatch, capsys
):
    spec = importlib.util.spec_from_file_location("hostile_client", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    monkeypatch.delattr(socket, "TCP_INFO", raising=False)
    srv = make_server("--stall-ms", STALL_MS)
    code = module.main(
        ["--host", LOCALHOST, "--port", str(srv.port), "--mode", "stall"]
    )
    assert (code, "stall mode needs Linux TCP_INFO" in capsys.readouterr().err) == (
        1,
        True,
    )


def test_probe_accepts_another_stock_firmware_version(monkeypatch):
    spec = importlib.util.spec_from_file_location("hostile_client", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    class Sock:
        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def settimeout(self, t):
            pass

        def sendall(self, data):
            pass

    replies = iter([[5, 3, 3, 7, 0xA7, 1, 0], [0, 0x42]])
    monkeypatch.setattr(module.socket, "create_connection", lambda *a, **k: Sock())
    monkeypatch.setattr(module, "recv_report", lambda sock: bytes(next(replies)))
    module.probe_once(LOCALHOST, 1)
