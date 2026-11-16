"""Wire codec: byte-exact encoders, the packet parser and the report decoders (BDD: client: wire)."""

import struct

import pytest

from telemetrix_idf import wire
from telemetrix_idf.wire import CMD, RPT, Reason

# --- constants ---------------------------------------------------------------------------------------------------


def test_constants_match_the_server_ids():
    assert wire.MAGIC == 0xA7
    assert wire.EXT_VERSION == (1, 0)
    assert (CMD.SYNC, CMD.ON_BUS, CMD.GET_CAPABILITIES, CMD.PLUGIN) == (57, 58, 59, 60)
    assert (RPT.ERROR, RPT.SYNC_REPLY, RPT.BUS_REPORT, RPT.CAPABILITIES, RPT.PLUGIN_REPORT) == (57, 58, 59, 60, 61)
    assert CMD.ONE_WIRE_CRC8 == 35 and CMD.RESET == 20 and CMD.STEPPER_GET_TARGET_POSITION == 56
    assert RPT.DIGITAL_REPORT == 2 and RPT.ONE_WIRE_REPORT == 14 and RPT.DEBUG_PRINT == 99


def test_reasons_match_the_server_codes():
    assert [r.value for r in Reason] == list(range(13))
    assert Reason.NONE == 0 and Reason.PLUGIN == 10 and Reason.SKIPPED == 11 and Reason.NO_SUCH_SLOT == 12


# --- encoders ----------------------------------------------------------------------------------------------------

EXACT = [
    ("loop", wire.loop(7), [2, 0, 7]),
    ("input", wire.set_pin_mode_input(4), [4, 1, 4, 0, 1]),
    ("input without reports", wire.set_pin_mode_input(4, report=False), [4, 1, 4, 0, 0]),
    ("pull-up", wire.set_pin_mode_input_pullup(4), [4, 1, 4, 2, 1]),
    ("pull-down", wire.set_pin_mode_input_pulldown(4, report=False), [4, 1, 4, 9, 0]),
    ("output", wire.set_pin_mode_output(17), [3, 1, 17, 1]),
    ("analog", wire.set_pin_mode_analog(3, differential=0x0102), [6, 1, 3, 3, 1, 2, 1]),
    ("digital write", wire.digital_write(17, 1), [3, 2, 17, 1]),
    ("analog write", wire.analog_write(17, 0x0123), [4, 3, 17, 1, 0x23]),
    ("modify reporting", wire.modify_reporting(3, 9), [3, 4, 3, 9]),
    ("firmware stock probe", wire.get_firmware_version(), [1, 5]),
    ("firmware handshake", wire.get_firmware_version((1, 0)), [4, 5, 0xA7, 1, 0]),
    ("servo attach", wire.servo_attach(5, 544, 2400), [6, 6, 5, 2, 0x20, 9, 0x60]),
    ("servo write", wire.servo_write(5, 90), [3, 7, 5, 90]),
    ("servo detach", wire.servo_detach(5), [2, 8, 5]),
    ("i2c begin default", wire.i2c_begin(), [1, 9]),
    ("i2c begin pins", wire.i2c_begin(21, 22), [3, 9, 21, 22]),
    ("i2c begin no pins", wire.i2c_begin(0, 0), [3, 9, 0, 0]),
    ("i2c read", wire.i2c_read(0x44, 0x10, 6, True), [5, 10, 0x44, 0x10, 6, 1]),
    ("i2c read no stop", wire.i2c_read(0x44, 0, 59, False), [5, 10, 0x44, 0, 59, 0]),
    ("i2c write", wire.i2c_write(0x44, b"\x01\x02"), [5, 11, 2, 0x44, 1, 2]),
    ("sonar new", wire.sonar_new(12, 13), [3, 12, 12, 13]),
    ("dht new", wire.dht_new(15), [2, 13, 15]),
    ("stop all reports", wire.stop_all_reports(), [1, 14]),
    ("analog interval", wire.set_analog_scanning_interval(20), [2, 15, 20]),
    ("enable all reports", wire.enable_all_reports(), [1, 16]),
    ("analog out attach", wire.analog_out_attach(2, 1), [3, 17, 2, 1]),
    ("analog out detach", wire.analog_out_detach(2), [2, 18, 2]),
    ("reset", wire.reset(), [1, 20]),
    ("one-wire init", wire.one_wire_init(4), [2, 27, 4]),
    ("one-wire reset", wire.one_wire_reset(), [1, 28]),
    ("one-wire select", wire.one_wire_select(bytes(range(1, 9))), [9, 29, 1, 2, 3, 4, 5, 6, 7, 8]),
    ("one-wire skip", wire.one_wire_skip(), [1, 30]),
    ("one-wire write", wire.one_wire_write(0x44, power=True), [3, 31, 0x44, 1]),
    ("one-wire write unpowered", wire.one_wire_write(0x44), [3, 31, 0x44, 0]),
    ("one-wire read", wire.one_wire_read(), [1, 32]),
    ("one-wire reset search", wire.one_wire_reset_search(), [1, 33]),
    ("one-wire search", wire.one_wire_search(), [1, 34]),
    ("one-wire crc8 sends exactly n bytes", wire.one_wire_crc8(b"\xaa\xbb\xcc"), [5, 35, 3, 0xAA, 0xBB, 0xCC]),
    ("sync", wire.sync(0x0102, abort=True), [4, 57, 1, 2, 1]),
    ("sync no abort", wire.sync(0xFFFF, abort=False), [4, 57, 0xFF, 0xFF, 0]),
    ("get capabilities", wire.get_capabilities(2), [2, 59, 2]),
    ("plugin", wire.plugin(1, 3, b"\x09\x08"), [5, 60, 1, 3, 9, 8]),
    ("plugin without args", wire.plugin(0, 0), [3, 60, 0, 0]),
    ("on bus", wire.on_bus(1, wire.i2c_read(0x44, 0, 2, True)), [7, 58, 1, 10, 0x44, 0, 2, 1]),
]


@pytest.mark.parametrize(("name", "got", "want"), EXACT, ids=[e[0] for e in EXACT])
def test_encoder_produces_the_exact_bytes(name, got, want):
    assert got == bytes(want)


def test_pwm_mode_carries_the_little_endian_f64_frequency():
    got = wire.set_pin_mode_pwm(9, channel=2, resolution=10, frequency=1000.0)
    assert got == bytes([13, 1, 9, 8, 2, 10]) + struct.pack("<d", 1000.0)
    assert got[6:] == bytes([0, 0, 0, 0, 0, 0x40, 0x8F, 0x40])


def test_encode_frames_length_id_and_payload():
    assert wire.encode(CMD.DIGITAL_WRITE, b"\x01\x02") == b"\x03\x02\x01\x02"


def test_encode_accepts_a_stock_payload_of_30_bytes():
    assert len(wire.encode(CMD.I2C_WRITE, bytes(30))) == 32


def test_encode_rejects_a_stock_payload_of_31_bytes():
    with pytest.raises(ValueError, match="payload"):
        wire.encode(CMD.I2C_WRITE, bytes(31))


def test_encode_accepts_an_extended_payload_of_254_bytes():
    assert len(wire.encode(CMD.PLUGIN, bytes(254))) == 256


def test_encode_rejects_an_extended_payload_of_255_bytes():
    with pytest.raises(ValueError, match="payload"):
        wire.encode(CMD.PLUGIN, bytes(255))


@pytest.mark.parametrize("cmd", [-1, 61, 300])
def test_encode_rejects_an_id_outside_0_to_60(cmd):
    with pytest.raises(ValueError, match="command id"):
        wire.encode(cmd, b"")


@pytest.mark.parametrize(
    "call",
    [
        lambda: wire.set_pin_mode_output(256),
        lambda: wire.set_pin_mode_output(-1),
        lambda: wire.digital_write(1, 2),
        lambda: wire.analog_write(1, 65536),
        lambda: wire.modify_reporting(5, 1),
        lambda: wire.servo_write(1, 181),
        lambda: wire.i2c_read(128, 0, 1, True),
        lambda: wire.i2c_read(0x44, 0, 60, True),
        lambda: wire.i2c_begin(21),
        lambda: wire.i2c_write(0x44, bytes(29)),
        lambda: wire.one_wire_select(b"\x01\x02"),
        lambda: wire.one_wire_crc8(bytes(30)),
        lambda: wire.set_analog_scanning_interval(256),
        lambda: wire.sync(65536, abort=True),
        lambda: wire.get_capabilities(256),
        lambda: wire.plugin(256, 0),
        lambda: wire.plugin(0, 0, bytes(253)),
        lambda: wire.on_bus(256, wire.i2c_read(0x44, 0, 1, True)),
        lambda: wire.on_bus(1, b"\x05\x0a"),
        lambda: wire.set_pin_mode_pwm(1, 0, 15, 100.0),
        lambda: wire.get_firmware_version((256, 0)),
    ],
)
def test_builder_rejects_a_bad_argument_with_value_error(call):
    with pytest.raises(ValueError):
        call()


# --- PacketParser ------------------------------------------------------------------------------------------------

STREAM = bytes([2, 0, 7]) + bytes([4, 3, 5, 1, 2]) + bytes([1, 14]) + bytes([5, 58, 0, 9, 0, 3])
PACKETS = [(0, b"\x07"), (3, b"\x05\x01\x02"), (14, b""), (58, b"\x00\x09\x00\x03")]


def test_parser_on_one_chunk_returns_every_packet():
    assert wire.PacketParser().feed(STREAM) == PACKETS


def test_parser_split_at_every_byte_boundary_returns_the_same_packets():
    for cut in range(len(STREAM) + 1):
        parser = wire.PacketParser()
        assert parser.feed(STREAM[:cut]) + parser.feed(STREAM[cut:]) == PACKETS


def test_parser_fed_one_byte_at_a_time_returns_the_same_packets():
    parser = wire.PacketParser()
    got = [p for b in STREAM for p in parser.feed(bytes([b]))]
    assert got == PACKETS


def test_parser_keeps_a_partial_packet_until_the_rest_arrives():
    parser = wire.PacketParser()
    assert parser.feed(b"\x04\x03\x05") == []
    assert parser.feed(b"\x01\x02") == [(3, b"\x05\x01\x02")]


def test_parser_skips_a_zero_length_byte_and_counts_it():
    parser = wire.PacketParser()
    assert parser.feed(b"\x00\x02\x00\x07") == [(0, b"\x07")]
    assert parser.dropped == 1


def test_parser_returns_a_255_byte_report():
    packet = bytes([255, 61]) + bytes(254)
    assert wire.PacketParser().feed(packet) == [(61, bytes(254))]


# --- decoders: stock ---------------------------------------------------------------------------------------------


def test_decode_digital_returns_pin_and_value():
    assert wire.decode_digital(b"\x11\x01") == wire.PinValue(pin=17, value=1)


def test_decode_analog_returns_pin_and_big_endian_value():
    assert wire.decode_analog(b"\x03\x0f\xff") == wire.PinValue(pin=3, value=0x0FFF)


def test_decode_firmware_stock_has_no_extension():
    assert wire.decode_firmware(bytes([3, 2, 100])) == (3, 2, 100, None)


def test_decode_firmware_long_form_returns_the_extension_version():
    assert wire.decode_firmware(bytes([3, 2, 100, 0xA7, 1, 0])) == (3, 2, 100, (1, 0))


def test_decode_firmware_long_form_with_a_wrong_magic_is_rejected():
    with pytest.raises(ValueError, match="firmware"):
        wire.decode_firmware(bytes([3, 2, 100, 0xA8, 1, 0]))


def test_decode_firmware_of_a_wrong_length_is_rejected():
    with pytest.raises(ValueError, match="firmware"):
        wire.decode_firmware(bytes([3, 2]))


def test_decode_servo_unavailable_returns_the_pin():
    assert wire.decode_servo_unavailable(b"\x05") == 5


def test_decode_i2c_read_returns_address_register_and_data():
    assert wire.decode_i2c_read(bytes([2, 0x44, 0x10, 0xAA, 0xBB])) == wire.I2cRead(
        addr=0x44, reg=0x10, data=b"\xaa\xbb"
    )


def test_decode_i2c_read_with_a_count_mismatch_is_rejected():
    with pytest.raises(ValueError, match="I2C"):
        wire.decode_i2c_read(bytes([3, 0x44, 0x10, 0xAA]))


def test_decode_i2c_short_returns_the_address():
    assert wire.decode_i2c_short(bytes([1, 0x44])) == 0x44


def test_decode_sonar_returns_trigger_and_centimetres():
    assert wire.decode_sonar(bytes([12, 0x01, 0x2C])) == wire.SonarReport(trigger_pin=12, cm=300)


def test_decode_dht_data_returns_little_endian_floats():
    payload = bytes([0, 15]) + struct.pack("<ff", 45.5, 21.25)
    assert wire.decode_dht(payload) == wire.DhtData(pin=15, humidity=45.5, temperature=21.25)


def test_decode_dht_error_returns_pin_and_code():
    assert wire.decode_dht(bytes([1, 15, 2])) == wire.DhtError(pin=15, code=2)


def test_decode_dht_of_an_unknown_kind_is_rejected():
    with pytest.raises(ValueError, match="DHT"):
        wire.decode_dht(bytes([2, 15, 2]))


def test_decode_one_wire_returns_the_operation_and_data():
    assert wire.decode_one_wire(bytes([28, 1])) == wire.OneWireReport(op=28, data=b"\x01")


def test_decode_one_wire_search_returns_eight_bytes():
    assert wire.decode_one_wire(bytes([34]) + bytes(range(8))).data == bytes(range(8))


# --- decoders: extended ------------------------------------------------------------------------------------------


def test_decode_error_returns_seq_cmd_and_reason():
    got = wire.decode_error(bytes([0x01, 0x02, 2, 3]))
    assert got == wire.ErrorReport(seq=0x0102, cmd=2, reason=Reason.PIN_BUSY, slot=None, sub=None, code=None)


def test_decode_error_for_a_plugin_command_returns_slot_and_sub():
    got = wire.decode_error(bytes([0, 9, 60, 5, 1, 3]))
    assert (got.cmd, got.reason, got.slot, got.sub, got.code) == (60, Reason.BAD_ARGUMENT, 1, 3, None)


def test_decode_error_with_the_plugin_reason_returns_the_code():
    got = wire.decode_error(bytes([0, 9, 60, 10, 1, 3, 0x2A]))
    assert (got.reason, got.slot, got.sub, got.code) == (Reason.PLUGIN, 1, 3, 42)


def test_decode_error_for_a_missing_slot_has_no_slot_and_sub():
    got = wire.decode_error(bytes([0, 9, 60, 12]))
    assert (got.reason, got.slot, got.sub, got.code) == (Reason.NO_SUCH_SLOT, None, None, None)


def test_decode_error_with_an_unknown_reason_keeps_the_number():
    assert wire.decode_error(bytes([0, 1, 2, 99])).reason == 99


def test_decode_error_that_is_too_short_is_rejected():
    with pytest.raises(ValueError, match="ERROR"):
        wire.decode_error(bytes([0, 1, 2]))


def test_decode_sync_reply_returns_tag_and_seq():
    assert wire.decode_sync_reply(bytes([0xAB, 0xCD, 0x00, 0x09])) == wire.SyncReply(tag=0xABCD, seq=9)


def test_decode_sync_reply_that_is_too_short_is_rejected():
    with pytest.raises(ValueError, match="SYNC_REPLY"):
        wire.decode_sync_reply(b"\x01\x02\x03")


def test_decode_bus_report_returns_the_bus_and_the_inner_stock_report():
    got = wire.decode_bus_report(bytes([1, 9, 2, 0x44, 0x10, 0xAA, 0xBB]))
    assert got == wire.BusReport(bus=1, report_id=RPT.I2C_READ_REPORT, payload=bytes([2, 0x44, 0x10, 0xAA, 0xBB]))


def test_decode_bus_report_payload_feeds_the_stock_decoder():
    got = wire.decode_bus_report(bytes([1, 9, 2, 0x44, 0x10, 0xAA, 0xBB]))
    assert wire.decode_i2c_read(got.payload).data == b"\xaa\xbb"


def test_decode_bus_report_that_is_too_short_is_rejected():
    with pytest.raises(ValueError, match="BUS_REPORT"):
        wire.decode_bus_report(b"\x01")


def test_decode_capabilities_returns_page_numbers_and_raw_records():
    payload = bytes([0, 2]) + bytes([1, 3, 9, 8, 7]) + bytes([4, 0]) + bytes([2, 1, 5])
    got = wire.decode_capabilities(payload)
    assert (got.page, got.last_page) == (0, 2)
    assert got.records == [(1, b"\x09\x08\x07"), (4, b""), (2, b"\x05")]


def test_decode_capabilities_of_an_empty_page_has_no_records():
    assert wire.decode_capabilities(bytes([1, 1])).records == []


def test_decode_capabilities_with_a_truncated_record_is_rejected():
    with pytest.raises(ValueError, match="CAPABILITIES"):
        wire.decode_capabilities(bytes([0, 0, 1, 5, 1, 2]))


def test_decode_plugin_report_returns_slot_sub_and_data():
    assert wire.decode_plugin_report(bytes([2, 7, 1, 2, 3])) == wire.PluginReport(slot=2, sub=7, data=b"\x01\x02\x03")
