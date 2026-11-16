"""The recording proxy, its strict check, the log comparison, and the compatibility script through the host server."""

import json
import signal
import socket
import subprocess
import sys
import threading
import time

import pytest

from conftest import LOCALHOST, ROOT

COMPAT = ROOT / "tools" / "compat"
sys.path.insert(0, str(COMPAT))

import compat_test  # noqa: E402
import known_reports  # noqa: E402
from recording_proxy import RecordingProxy  # noqa: E402


class FakeServer:
    """Accepts one connection, sends what it is given and keeps what it receives."""

    def __init__(self, send):
        self.listener = socket.socket()
        self.listener.bind((LOCALHOST, 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.received = b""
        self.thread = threading.Thread(target=self._serve, args=(send,), daemon=True)
        self.thread.start()

    def _serve(self, send):
        conn, _ = self.listener.accept()
        conn.sendall(send)
        while chunk := conn.recv(4096):
            self.received += chunk
        conn.close()

    def close(self):
        self.listener.close()


def through_proxy(tmp_path, server_bytes, strict=True, client_bytes=b""):
    """Send server_bytes from a fake server to a client through the proxy; return (log entries, violations, client data)."""
    fake = FakeServer(server_bytes)
    log = tmp_path / "proxy.log"
    proxy = RecordingProxy(0, (LOCALHOST, fake.port), log, strict=strict).start()
    try:
        with socket.create_connection((LOCALHOST, proxy.port), timeout=5) as client:
            if client_bytes:
                client.sendall(client_bytes)
            data = b""
            while len(data) < len(server_bytes):
                chunk = client.recv(4096)
                assert chunk, "proxy closed the connection early"
                data += chunk
            time.sleep(0.1)
    finally:
        violations = proxy.stop()
        fake.close()
    return [json.loads(line) for line in log.read_text().splitlines()], violations, data


def frame(report_id, *payload):
    return bytes([len(payload) + 1, report_id, *payload])


# one valid report per layout the server sends, the stock client's dispatch table permitting it
KNOWN_LAYOUTS = [
    frame(0, 0x42),
    frame(2, 4, 1),
    frame(3, 32, 0x08, 0x00),
    frame(5, 3, 2, 100),
    frame(6, 18),
    frame(8, 1, 0x76),
    frame(9, 3, 0x76, 0x10, 1, 2, 3),
    frame(9, 59, 0x76, 0, *range(59)),
    frame(10, 26, 0, 100),
    frame(11, 0, 25, 0, 0, 0x82, 0x42, 0, 0, 0x0C, 0x42),
    frame(11, 1, 25, 2),
    frame(14, 28, 1),
    frame(14, 32, 7),
    frame(14, 34, 0x28, 1, 2, 3, 4, 5, 6, 0x4F),
    frame(14, 35, 0x4F),
]


def test_strict_flags_unknown_report_id(tmp_path):
    entries, violations, _ = through_proxy(tmp_path, frame(12, 0))
    assert [e["id"] for e in entries if e["dir"] == "s2c"] == [12]
    assert len(violations) == 1
    assert "12" in violations[0]


def test_strict_flags_a_wrong_length_for_a_known_id(tmp_path):
    _, violations, _ = through_proxy(tmp_path, frame(2, 4))
    assert len(violations) == 1


def test_strict_accepts_every_known_layout(tmp_path):
    data = b"".join(KNOWN_LAYOUTS)
    entries, violations, received = through_proxy(tmp_path, data)
    assert violations == []
    assert received == data, "the proxy must forward the bytes unchanged"
    assert [e["id"] for e in entries] == [pkt[1] for pkt in KNOWN_LAYOUTS]


def test_known_layouts_cover_exactly_the_ids_the_client_dispatches_that_the_server_sends():
    assert known_reports.KNOWN_IDS <= known_reports.dispatched_ids()
    assert {p[1] for p in KNOWN_LAYOUTS} == known_reports.KNOWN_IDS


def test_no_strict_records_but_does_not_flag(tmp_path):
    entries, violations, _ = through_proxy(tmp_path, frame(12, 0), strict=False)
    assert violations == []
    assert len(entries) == 1


def test_proxy_records_client_packets_and_frames_split_reads(tmp_path):
    fake = FakeServer(b"")
    log = tmp_path / "split.log"
    proxy = RecordingProxy(0, (LOCALHOST, fake.port), log).start()
    try:
        with socket.create_connection((LOCALHOST, proxy.port), timeout=5) as client:
            for byte in frame(2, 5, 1) + frame(5)[:1] + frame(5)[1:]:  # one byte at a time
                client.sendall(bytes([byte]))
                time.sleep(0.01)
            time.sleep(0.2)
    finally:
        proxy.stop()
        fake.close()
    entries = [json.loads(line) for line in log.read_text().splitlines()]
    assert [(e["dir"], e["id"], e["bytes"]) for e in entries] == [("c2s", 2, "0501"), ("c2s", 5, "")]
    assert fake.received == frame(2, 5, 1) + frame(5)


def write_log(path, packets):
    path.write_text("".join(json.dumps(p) + "\n" for p in packets))
    return path


def entry(direction, report_id, *payload, t=0.0):
    return {"dir": direction, "t": t, "len": len(payload) + 1, "id": report_id, "bytes": bytes(payload).hex()}


def session(tmp_path, name, build=0, analog=100, sonar=50, dht=(0x42, 0x82), stamp=0.0):
    return write_log(
        tmp_path / name,
        [
            entry("c2s", 5, t=stamp),
            entry("s2c", 5, 3, 2, build, t=stamp + 0.1),
            entry("s2c", 3, 33, analog >> 8, analog & 255, t=stamp + 0.2),
            entry("s2c", 2, 33, 1, t=stamp + 0.3),
            entry("s2c", 10, 26, 0, sonar, t=stamp + 0.4),
            entry("s2c", 11, 0, 25, 0, 0, dht[0], dht[1], 0, 0, 0, 0, t=stamp + 0.5),
            entry("s2c", 3, 33, analog >> 8, (analog + 1) & 255, t=stamp + 0.6),
        ],
    )


def test_compare_ignores_normalised_fields(tmp_path):
    a = session(tmp_path, "a.log", build=0, analog=100, sonar=50, dht=(0x42, 0x82), stamp=0.0)
    b = session(tmp_path, "b.log", build=100, analog=2000, sonar=77, dht=(0x41, 0x80), stamp=9.5)
    assert compat_test.compare(a, b) == []
    assert compat_test.main(["--compare", str(a), str(b)]) == 0


def test_compare_reports_a_real_difference_with_context(tmp_path, capsys):
    a = session(tmp_path, "a.log")
    packets = [json.loads(line) for line in a.read_text().splitlines()]
    packets[3] = entry("s2c", 2, 33, 0)  # a digital report with another value
    b = write_log(tmp_path / "b.log", packets)
    diff = compat_test.compare(a, b)
    assert any(line.startswith("-s2c") and "id=2 2101" in line for line in diff)
    assert any(line.startswith("+s2c") and "id=2 2100" in line for line in diff)
    assert any(line.startswith(" ") and "firmware" in line for line in diff), "context lines are part of the diff"
    assert compat_test.main(["--compare", str(a), str(b)]) == 1
    assert "id=2" in capsys.readouterr().out


def test_compare_reports_a_periodic_report_missing_on_one_side(tmp_path):
    a = session(tmp_path, "a.log")
    packets = [json.loads(line) for line in a.read_text().splitlines() if '"id": 10' not in line]
    b = write_log(tmp_path / "b.log", packets)
    assert any("sonar" in line for line in compat_test.compare(a, b))


def test_compat_script_through_the_proxy_against_the_host_server_strict(make_server, tmp_path):
    srv = make_server()
    log = tmp_path / "ours.log"
    result = subprocess.run(
        [
            sys.executable,
            str(COMPAT / "compat_test.py"),
            "--host", LOCALHOST,
            "--port", str(srv.port),
            "--loopback", "5,4",
            "--i2c-addr", "0x76",
            "--dht", "25",
            "--sonar", "26,27",
            "--onewire", "16",
            "--log", str(log),
            "--strict",
        ],
        capture_output=True,
        text=True,
        timeout=50,
    )  # fmt: skip
    assert result.returncode == 0, result.stdout + result.stderr
    ids = {json.loads(line)["id"] for line in log.read_text().splitlines()}
    assert {0, 2, 5, 9, 10, 11, 14} <= ids


@pytest.mark.parametrize(("server_bytes", "status"), [(frame(12, 0), 1), (frame(2, 4, 1), 0)])
def test_cli_strict_exit_status_after_sigterm(tmp_path, server_bytes, status):
    fake = FakeServer(server_bytes)
    proc = subprocess.Popen(
        [sys.executable, str(COMPAT / "recording_proxy.py"), "--listen", "0", "--target", f"{LOCALHOST}:{fake.port}",
         "--log", str(tmp_path / "cli.log"), "--strict"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )  # fmt: skip
    try:
        port = int(proc.stdout.readline().split()[1])
        with socket.create_connection((LOCALHOST, port), timeout=5) as client:
            assert client.recv(16) == server_bytes
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(10) == status
    finally:
        proc.kill()
        proc.wait()
        fake.close()
