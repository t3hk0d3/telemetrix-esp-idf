"""TCP behaviour of the host server, with raw sockets: refusal, access list, stalled readers, garbage, safe states."""

import time

import pytest
from conftest import IO_TIMEOUT, Raw

LOOP_PACKET = bytes([2, 0, 0x42])


def dropped_for_not_reading(status):
    """Clients dropped by the stall timer or by a reply that found no room in the send queue."""
    return status["stalled_clients"] + status["overflowed_clients"]


def expect_reset(conn, send=b""):
    """The server answers with RST and not with a byte; the send may already meet the reset."""
    with pytest.raises(ConnectionResetError):
        if send:
            conn.sock.sendall(send)
        assert conn.sock.recv(16) != b""


def test_second_connection_reset_and_counted(server, raw):
    first = raw()
    first.loop(1)
    before = server.status()["refused_connections"]

    second = raw()
    expect_reset(second)

    assert server.status()["refused_connections"] == before + 1
    first.loop(2)


def test_disallowed_peer_reset_before_read(make_server):
    srv = make_server("--allow", "10.0.0.0/8", "--allow", "192.168.0.0/16")
    conn = Raw(srv.port)
    # bytes that would change a counter if the server read them: three len-0 bytes are three dropped packets
    expect_reset(conn, bytes(3) + LOOP_PACKET)
    conn.close()

    status = srv.status()
    assert status["refused_connections"] == 1
    assert status["client"] == "-"
    assert status["dropped_packets"] == 0
    assert status["ignored_commands"] == 0
    assert status["refused_commands"] == 0


def test_allowed_peer_is_served(make_server):
    srv = make_server("--allow", "10.0.0.0/8", "--allow", "127.0.0.0/8")
    conn = Raw(srv.port)
    conn.loop()
    conn.close()


def test_non_reading_client_dropped_and_counted(make_server):
    srv = make_server("--stall-ms", 500)
    # A tiny receive buffer keeps the server's send buffer small too: the replies pile up in the server.
    conn = Raw(srv.port, rcvbuf=1024)
    conn.sock.setblocking(False)
    chunk = LOOP_PACKET * 4000
    idle_since = None
    sent = 0
    while sent < 64 * 1024 * 1024:
        try:
            sent += conn.sock.send(chunk)
            idle_since = None
        except (BlockingIOError, ConnectionResetError, BrokenPipeError):
            idle_since = idle_since or time.monotonic()
            if time.monotonic() - idle_since > 0.5:
                break
            time.sleep(0.01)
    # never read: the server must give up on this client by itself
    deadline = time.monotonic() + IO_TIMEOUT
    # replies pile up: the send queue overflows (overflowed_clients) or, if it never does, the stall timer fires
    while dropped_for_not_reading(srv.status()) == 0:
        assert time.monotonic() < deadline, "the stalled client was not dropped"
        time.sleep(0.05)
    assert srv.status()["client"] == "-"

    # what is left of the connection ends: data, then EOF or RST
    conn.sock.setblocking(True)
    conn.sock.settimeout(IO_TIMEOUT)
    try:
        while conn.sock.recv(65536):
            pass
    except ConnectionResetError:
        pass
    conn.close()

    fresh = Raw(srv.port)
    fresh.loop()
    fresh.close()


def test_garbage_then_valid_packet_still_served(server, raw):
    conn = raw()
    before = server.status()["dropped_packets"]

    conn.sock.sendall(bytes([0]))  # len 0: one byte consumed
    conn.sock.sendall(bytes([40]) + bytes(40))  # len above the stock cap: skipped
    conn.sock.sendall(LOOP_PACKET)

    assert conn.recv_report() == [0, 0x42]
    assert server.status()["dropped_packets"] >= before + 2


def test_disconnect_returns_pins_to_safe_state(server, raw):
    # gpio 5 (safe: low) is wired to gpio 4, gpio 13 (safe: high) to gpio 12
    first = raw()
    first.send(1, 4, 0, 1)  # gpio 4 input, report
    first.send(1, 12, 0, 1)
    first.send(1, 5, 1)  # gpio 5 and 13 outputs
    first.send(1, 13, 1)
    first.send(2, 5, 1)  # 5 high, 13 low
    first.send(2, 13, 0)
    seen = {}
    deadline = time.monotonic() + IO_TIMEOUT
    while seen.get(4) != 1 or seen.get(12) != 0:
        assert time.monotonic() < deadline, f"inputs never followed the outputs: {seen}"
        report = first.recv_report()
        if report[0] == 2:
            seen[report[1]] = report[2]
    first.close()
    server.wait_idle()

    second = raw()
    second.send(1, 4, 0, 1)
    second.send(1, 12, 0, 1)
    seen = {}
    deadline = time.monotonic() + IO_TIMEOUT
    while 4 not in seen or 12 not in seen:
        assert time.monotonic() < deadline, f"no first reports: {seen}"
        report = second.recv_report()
        if report[0] == 2:
            seen[report[1]] = report[2]
    assert seen == {4: 0, 12: 1}


def test_pulse_written_in_one_send_reports_both_edges(server, raw):
    # gpio 5 is wired to gpio 4: a 1 then a 0 written by two packets of one send, with the socket not backed up
    conn = raw()
    conn.send(1, 5, 1)  # gpio 5 output (low)
    conn.send(1, 4, 0, 1)  # gpio 4 input, report
    assert conn.recv_report() == [2, 4, 0]  # the first sample
    conn.sock.sendall(bytes([3, 2, 5, 1, 3, 2, 5, 0]))
    assert conn.recv_report() == [2, 4, 1]
    assert conn.recv_report() == [2, 4, 0]
    conn.close()
