"""tools/hostile/hostile_client.py against the host server: it survives and leaks no file descriptors."""

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
    assert srv.status()["stalled_clients"] >= 1


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
