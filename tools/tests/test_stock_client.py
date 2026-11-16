"""The stock telemetrix-esp32 client against the host server and its fake devices."""

import queue
import time

import pytest

TIMEOUT = 5.0


def collect():
    """A callback and the queue it fills."""
    q = queue.Queue()
    return q, q.put


def expect(q, match, timeout=TIMEOUT):
    """The first report on q that match accepts; reports that do not match are skipped."""
    deadline = time.monotonic() + timeout
    seen = []
    while time.monotonic() < deadline:
        try:
            report = q.get(timeout=0.1)
        except queue.Empty:
            continue
        if match(report):
            return report
        seen.append(report)
    pytest.fail(f"no matching report; saw {seen}")


def dallas_crc8(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8C if crc & 1 else crc >> 1
    return crc


ROM_PREFIX = [0x28, 1, 2, 3, 4, 5, 6]


def test_firmware_version_is_3_2_100(stock):
    client = stock()
    assert client.firmware_version == [3, 2, 100]


def test_digital_write_reaches_the_wired_input(stock):
    client = stock()
    q, cb = collect()
    client.set_pin_mode_digital_output(5)
    client.set_pin_mode_digital_input(4, cb)
    expect(q, lambda r: r[1] == 4 and r[2] == 0)
    client.digital_write(5, 1)
    expect(q, lambda r: r[1] == 4 and r[2] == 1)
    client.digital_write(5, 0)
    expect(q, lambda r: r[1] == 4 and r[2] == 0)


def test_pwm_on_gpio_18_attaches_and_sets_duty(make_server, stock):
    srv = (
        make_server()
    )  # the call counts come from the fake's call log, which a long session would fill
    client = stock(srv)
    client.set_pin_mode_analog_output(18, channel=0, frequency=5000.0, resolution=8)
    client.analog_write(18, 128)
    deadline = time.monotonic() + TIMEOUT
    while True:
        status = srv.status()
        if status["ledc_attach"] == 1 and status["ledc_duty"] >= 1:
            break
        assert time.monotonic() < deadline, f"no PWM on the fake: {status}"
        time.sleep(0.05)
    assert status["refused_commands"] == 0


def test_analog_input_reports_the_fake_adc_value(stock):
    client = stock()
    q, cb = collect()
    client.set_pin_mode_analog_input(32, 0, cb)
    report = expect(q, lambda r: r[1] == 32)
    assert report[2] == 2048


def test_i2c_read_three_bytes_from_0x76(stock):
    client = stock()
    q, cb = collect()
    client.set_pin_mode_i2c()
    client.i2c_read(0x76, 0x10, 3, cb)
    report = expect(q, lambda r: r[2] == 0x76)
    # [report type, count, address, register, data x count, time stamp]
    assert report[1:-1] == [3, 0x76, 0x10, 0x10 ^ 0xA5, 0x11 ^ 0xA5, 0x12 ^ 0xA5]


def test_sonar_reports_100_cm(stock):
    client = stock()
    q, cb = collect()
    client.set_pin_mode_sonar(26, 27, cb)
    report = expect(q, lambda r: r[1] == 26)
    assert report[2] == 100


def test_dht_reports_humidity_and_temperature(stock):
    client = stock()
    q, cb = collect()
    client.set_pin_mode_dht(25, cb)
    report = expect(q, lambda r: r[2] == 25)
    # [report type, error flag, pin, humidity, temperature, time stamp]
    assert report[1] == 0
    assert report[3] == pytest.approx(65.2, abs=0.01)
    assert report[4] == pytest.approx(35.1, abs=0.01)


def test_one_wire_reset_search_and_crc8(stock):
    client = stock()
    q, cb = collect()
    client.set_pin_mode_one_wire(16)

    client.onewire_reset(cb)
    reset = expect(q, lambda r: r[1] == 28)
    assert reset[2] == 1  # presence

    client.onewire_reset_search()
    client.onewire_search(cb)
    found = expect(q, lambda r: r[1] == 34)
    rom = found[2:10]
    assert rom[:7] == ROM_PREFIX
    assert rom[7] == dallas_crc8(ROM_PREFIX)

    client.onewire_crc8(
        rom, cb
    )  # the client sends the length minus one and all 8 bytes; the CRC covers 7
    crc = expect(q, lambda r: r[1] == 35)
    assert crc[2] == rom[7]

    client.onewire_search(cb)
    assert expect(q, lambda r: r[1] == 34)[2:10] == [0xFF] * 8
