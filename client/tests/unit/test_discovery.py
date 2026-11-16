"""client: discovery (discovery.py) through the connection, and the record parser on its own."""

from __future__ import annotations

import pytest
import pytest_asyncio
from fake_ext_server import (
    FakeExtServer,
    bus_i2c_rec,
    bus_one_wire_rec,
    chip_rec,
    limit_rec,
    named_cap_rec,
    paginate,
    pin_rec,
    plugin_rec,
    rec,
)

from telemetrix_idf import ProtocolError
from telemetrix_idf.connection import connect
from telemetrix_idf.discovery import BusInfo, Cap, ChipInfo, PinInfo, PluginInfo, SafeState, parse_discovery
from telemetrix_idf.wire import CapabilitiesPage

# Three pages, with an unknown record type (9) on the middle one.
THREE_PAGES = [
    chip_rec(18, 301, "v5.5.1") + named_cap_rec(0, "test.led") + pin_rec(4, 0x0F, 2) + pin_rec(5, 0x13, 3, named=[0]),
    rec(9, b"\x01\x02\x03") + pin_rec(34, 0x21, 0, 0, 6) + bus_i2c_rec(0, 21, 22, 400_000) + bus_i2c_rec(1, -1, -1, 0),
    bus_one_wire_rec(0, -1)
    + bus_one_wire_rec(1, 35)
    + limit_rec(1, 8)
    + limit_rec(2, 8)
    + limit_rec(3, 4)
    + limit_rec(4, 2)
    + limit_rec(5, 4)
    + limit_rec(6, 10)
    + limit_rec(42, 7)
    + plugin_rec(0, 1, 0, "test.echo"),
]


@pytest_asyncio.fixture
async def three_page_board():
    async with FakeExtServer(pages=THREE_PAGES) as srv, connect("127.0.0.1", srv.port) as board:
        yield srv, board


@pytest.mark.asyncio
async def test_three_pages_every_page_requested(three_page_board) -> None:
    srv, _ = three_page_board
    assert [p for c, p in srv.received if c == 59] == [b"\x00", b"\x01", b"\x02"]


@pytest.mark.asyncio
async def test_three_pages_chip(three_page_board) -> None:
    _, board = three_page_board
    assert board.chip == ChipInfo(model=18, revision=(3, 1), idf_version="v5.5.1")


@pytest.mark.asyncio
async def test_three_pages_every_pin(three_page_board) -> None:
    _, board = three_page_board
    assert dict(board.pins) == {
        4: PinInfo(4, Cap.DIN | Cap.DOUT | Cap.PULL_UP | Cap.PULL_DOWN, SafeState.PULL_DOWN, None, None, ()),
        5: PinInfo(5, Cap.DIN | Cap.DOUT | Cap.PWM, SafeState.DRIVE_LOW, None, None, ("test.led",)),
        34: PinInfo(34, Cap.DIN | Cap.ADC, SafeState.FLOAT, 0, 6, ()),
    }


@pytest.mark.asyncio
async def test_three_pages_every_bus(three_page_board) -> None:
    _, board = three_page_board
    assert board.buses == [
        BusInfo("i2c", 0, (21, 22), 400_000),
        BusInfo("i2c", 1, None, 0),
        BusInfo("one_wire", 0, None, None),
        BusInfo("one_wire", 1, (35,), None),
    ]


@pytest.mark.asyncio
async def test_three_pages_every_limit_unknown_kind_named(three_page_board) -> None:
    _, board = three_page_board
    assert dict(board.limits) == {
        "servos": 8,
        "pwm_channels": 8,
        "pwm_timers": 4,
        "dht": 2,
        "sonar": 4,
        "analog_floor_ms": 10,
        "kind_42": 7,
    }


@pytest.mark.asyncio
async def test_three_pages_every_plugin(three_page_board) -> None:
    _, board = three_page_board
    assert dict(board.plugins_available) == {"test.echo": PluginInfo(0, 1, 0, "test.echo")}


@pytest.mark.asyncio
async def test_three_pages_unknown_type_skipped(three_page_board) -> None:
    _, board = three_page_board
    assert board.stats.unknown_records == 1


@pytest.mark.asyncio
async def test_discovery_multi_page_rebuilds_every_pin() -> None:
    names = ["test.led", "test.motor", "acme.relay"]
    records = [chip_rec(18, 300, "v5.5.1")] + [named_cap_rec(i, n) for i, n in enumerate(names)]
    for gpio in range(64):
        named = [gpio % 3] if gpio % 4 == 0 else []
        adc = (gpio % 2, gpio % 10) if gpio >= 40 else (-1, -1)
        records.append(pin_rec(gpio, 0x00FF if gpio % 2 else 0x0003, gpio % 5, adc[0], adc[1], named))
    pages = paginate(records)
    assert len(pages) >= 3
    async with FakeExtServer(pages=pages) as srv, connect("127.0.0.1", srv.port) as board:
        assert len([c for c, _ in srv.received if c == 59]) == len(pages)
        assert list(board.pins) == list(range(64))
        for gpio, info in board.pins.items():
            assert info.caps == (Cap(0xFF) if gpio % 2 else Cap.DIN | Cap.DOUT)
            assert info.safe == SafeState(gpio % 5)
            assert (info.adc_unit, info.adc_channel) == ((gpio % 2, gpio % 10) if gpio >= 40 else (None, None))
            assert info.named == ((names[gpio % 3],) if gpio % 4 == 0 else ())


# --- parse_discovery on its own ----------------------------------------------------------------------------------


def _page(records: bytes, page: int = 0, last: int = 0) -> CapabilitiesPage:
    from telemetrix_idf.wire import decode_capabilities

    return decode_capabilities(bytes([page, last]) + records)


def test_parse_named_cap_after_pin_still_resolved() -> None:
    d = parse_discovery([_page(pin_rec(4, 1, named=[2]) + named_cap_rec(2, "x.y"))])
    assert d.pins[4].named == ("x.y",)


def test_parse_undeclared_named_cap_id_named_cap_n() -> None:
    d = parse_discovery([_page(pin_rec(4, 1, named=[7]))])
    assert d.pins[4].named == ("cap_7",)


def test_parse_unknown_safe_state_kept_as_int() -> None:
    d = parse_discovery([_page(pin_rec(4, 1, safe=9))])
    assert d.pins[4].safe == 9


def test_parse_unknown_bus_kind_skipped() -> None:
    d = parse_discovery([_page(rec(3, bytes([7, 0, 1])) + bus_one_wire_rec(0, 4))])
    assert d.buses == [BusInfo("one_wire", 0, (4,), None)]


def test_parse_unknown_record_type_counted() -> None:
    d = parse_discovery([_page(rec(200, b"") + rec(0, b"zz"))])
    assert d.unknown_records == 2


@pytest.mark.parametrize(
    "record",
    [rec(1, b"\x00\x12\x01"), rec(2, b"\x04\x00\x01\x00\xff"), rec(3, b"\x00\x00\x15\x16"), rec(3, b"\x01\x00"),
     rec(4, b"\x01\x00"), rec(5, b"\x00\x01"), rec(6, b"")],
)  # fmt: skip
def test_parse_short_record_raises_protocol_error(record: bytes) -> None:
    with pytest.raises(ProtocolError):
        parse_discovery([_page(record)])


@pytest.mark.asyncio
async def test_truncated_page_fails_connect_with_protocol_error() -> None:
    async with FakeExtServer(pages=[b"\x02\x09\x04"]) as srv:
        with pytest.raises(ProtocolError):
            await connect("127.0.0.1", srv.port)
        await srv.eof.wait()
