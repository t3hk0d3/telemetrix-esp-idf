"""Wire codec of the Telemetrix protocol: pure functions, no I/O.

Framing both ways is `[len][id][payload]`, `len` counting the id and the payload. 16-bit values are big-endian; the PWM
frequency (f64) and the DHT floats (f32) are little-endian.

Every builder returns a complete packet (length byte included) and raises `ValueError` on a bad argument: a bad
argument is a programmer error, not a runtime condition, so no `TelemetrixError` subclass is involved. Decoders take the
payload a `PacketParser` returned (no length byte, no id) and raise `ValueError` when it is malformed.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum
from typing import NamedTuple

__all__ = [
    "CMD",
    "EXT_VERSION",
    "MAGIC",
    "RPT",
    "BusReport",
    "CapabilitiesPage",
    "DhtData",
    "DhtError",
    "ErrorReport",
    "Firmware",
    "I2cRead",
    "OneWireReport",
    "PacketParser",
    "PinValue",
    "PluginReport",
    "Reason",
    "SonarReport",
    "SyncReply",
    "analog_out_attach",
    "analog_out_detach",
    "analog_write",
    "decode_analog",
    "decode_bus_report",
    "decode_capabilities",
    "decode_dht",
    "decode_digital",
    "decode_error",
    "decode_firmware",
    "decode_i2c_read",
    "decode_i2c_short",
    "decode_one_wire",
    "decode_plugin_report",
    "decode_servo_unavailable",
    "decode_sonar",
    "decode_sync_reply",
    "dht_new",
    "digital_write",
    "enable_all_reports",
    "encode",
    "get_capabilities",
    "get_firmware_version",
    "i2c_begin",
    "i2c_read",
    "i2c_write",
    "loop",
    "modify_reporting",
    "on_bus",
    "one_wire_crc8",
    "one_wire_init",
    "one_wire_read",
    "one_wire_reset",
    "one_wire_reset_search",
    "one_wire_search",
    "one_wire_select",
    "one_wire_skip",
    "one_wire_write",
    "plugin",
    "reset",
    "servo_attach",
    "servo_detach",
    "servo_write",
    "set_analog_scanning_interval",
    "set_pin_mode_analog",
    "set_pin_mode_input",
    "set_pin_mode_input_pulldown",
    "set_pin_mode_input_pullup",
    "set_pin_mode_output",
    "set_pin_mode_pwm",
    "sonar_new",
    "stop_all_reports",
    "sync",
]

MAGIC = 0xA7
EXT_VERSION = (1, 0)

STOCK_PAYLOAD_MAX = 30
EXT_PAYLOAD_MAX = 254
I2C_READ_MAX = 59


class CMD(IntEnum):
    """Command ids (src/proto/tmx_proto.h)."""

    LOOP_COMMAND = 0
    SET_PIN_MODE = 1
    DIGITAL_WRITE = 2
    ANALOG_WRITE = 3
    MODIFY_REPORTING = 4
    GET_FIRMWARE_VERSION = 5
    SERVO_ATTACH = 6
    SERVO_WRITE = 7
    SERVO_DETACH = 8
    I2C_BEGIN = 9
    I2C_READ = 10
    I2C_WRITE = 11
    SONAR_NEW = 12
    DHT_NEW = 13
    STOP_ALL_REPORTS = 14
    SET_ANALOG_SCANNING_INTERVAL = 15
    ENABLE_ALL_REPORTS = 16
    ANALOG_OUT_ATTACH = 17
    ANALOG_OUT_DETACH = 18
    DAC_WRITE = 19
    RESET = 20
    DAC_DISABLE = 21
    SPI_INIT = 22
    SPI_WRITE_BLOCKING = 23
    SPI_READ_BLOCKING = 24
    SPI_SET_FORMAT = 25
    SPI_CS_CONTROL = 26
    ONE_WIRE_INIT = 27
    ONE_WIRE_RESET = 28
    ONE_WIRE_SELECT = 29
    ONE_WIRE_SKIP = 30
    ONE_WIRE_WRITE = 31
    ONE_WIRE_READ = 32
    ONE_WIRE_RESET_SEARCH = 33
    ONE_WIRE_SEARCH = 34
    ONE_WIRE_CRC8 = 35
    SET_PIN_MODE_STEPPER = 36
    STEPPER_MOVE_TO = 37
    STEPPER_MOVE = 38
    STEPPER_RUN = 39
    STEPPER_RUN_SPEED = 40
    STEPPER_SET_MAX_SPEED = 41
    STEPPER_SET_ACCELERATION = 42
    STEPPER_SET_SPEED = 43
    STEPPER_SET_CURRENT_POSITION = 44
    STEPPER_RUN_SPEED_TO_POSITION = 45
    STEPPER_STOP = 46
    STEPPER_DISABLE_OUTPUTS = 47
    STEPPER_ENABLE_OUTPUTS = 48
    STEPPER_SET_MINIMUM_PULSE_WIDTH = 49
    STEPPER_SET_ENABLE_PIN = 50
    STEPPER_SET_3_PINS_INVERTED = 51
    STEPPER_SET_4_PINS_INVERTED = 52
    STEPPER_IS_RUNNING = 53
    STEPPER_GET_CURRENT_POSITION = 54
    STEPPER_GET_DISTANCE_TO_GO = 55
    STEPPER_GET_TARGET_POSITION = 56
    SYNC = 57
    ON_BUS = 58
    GET_CAPABILITIES = 59
    PLUGIN = 60


class RPT(IntEnum):
    """Report ids. `LOOP_COMMAND` is the stock barrier reply `[2, 0, tag]`."""

    LOOP_COMMAND = 0
    DIGITAL_REPORT = 2
    ANALOG_REPORT = 3
    FIRMWARE_REPORT = 5
    SERVO_UNAVAILABLE = 6
    I2C_TOO_FEW_BYTES_RCVD = 7
    I2C_TOO_MANY_BYTES_RCVD = 8
    I2C_READ_REPORT = 9
    SONAR_DISTANCE = 10
    DHT_REPORT = 11
    TOUCH_REPORT = 12
    SPI_REPORT = 13
    ONE_WIRE_REPORT = 14
    STEPPER_DISTANCE_TO_GO = 15
    STEPPER_TARGET_POSITION = 16
    STEPPER_CURRENT_POSITION = 17
    STEPPER_RUNNING_REPORT = 18
    STEPPER_RUN_COMPLETE_REPORT = 19
    ERROR = 57
    SYNC_REPLY = 58
    BUS_REPORT = 59
    CAPABILITIES = 60
    PLUGIN_REPORT = 61
    DEBUG_PRINT = 99


class Reason(IntEnum):
    """Why the server refused a command (`telemetrix_reason_t`)."""

    NONE = 0
    NO_SUCH_PIN = 1
    NO_CAPABILITY = 2
    PIN_BUSY = 3
    EXHAUSTED = 4
    BAD_ARGUMENT = 5
    BUS_UNKNOWN = 6
    NOT_IN_SCOPE = 7
    HARDWARE = 8
    NO_ANSWER = 9
    PLUGIN = 10
    SKIPPED = 11
    NO_SUCH_SLOT = 12


# --- encoders ----------------------------------------------------------------------------------------------------


def encode(cmd: int, payload: bytes = b"") -> bytes:
    """Frame one command: `[1 + len(payload)][cmd][payload]`. Stock ids carry at most 30 bytes, extended ids 254."""
    if not 0 <= cmd <= CMD.PLUGIN:
        raise ValueError(f"command id {cmd} outside 0..{int(CMD.PLUGIN)}")
    cap = EXT_PAYLOAD_MAX if cmd >= CMD.SYNC else STOCK_PAYLOAD_MAX
    if len(payload) > cap:
        raise ValueError(f"payload of {len(payload)} bytes exceeds {cap} for command {cmd}")
    return bytes([1 + len(payload), cmd]) + bytes(payload)


def _u8(name: str, value: int, high: int = 255) -> int:
    if not isinstance(value, int) or not 0 <= value <= high:
        raise ValueError(f"{name} {value!r} outside 0..{high}")
    return value


def _u16(name: str, value: int) -> bytes:
    _u8(name, value, 0xFFFF)
    return value.to_bytes(2, "big")


def _flag(name: str, value: bool | int) -> int:
    if value not in (0, 1):
        raise ValueError(f"{name} {value!r} is not 0 or 1")
    return int(value)


def loop(tag: int) -> bytes:
    return encode(CMD.LOOP_COMMAND, bytes([_u8("tag", tag)]))


def _pin_mode(pin: int, mode: int, report: bool) -> bytes:
    return encode(CMD.SET_PIN_MODE, bytes([_u8("pin", pin), mode, _flag("report", report)]))


def set_pin_mode_input(pin: int, report: bool = True) -> bytes:
    return _pin_mode(pin, 0, report)


def set_pin_mode_input_pullup(pin: int, report: bool = True) -> bytes:
    return _pin_mode(pin, 2, report)


def set_pin_mode_input_pulldown(pin: int, report: bool = True) -> bytes:
    return _pin_mode(pin, 9, report)


def set_pin_mode_output(pin: int) -> bytes:
    return encode(CMD.SET_PIN_MODE, bytes([_u8("pin", pin), 1]))


def set_pin_mode_analog(pin: int, differential: int = 0, report: bool = True) -> bytes:
    return encode(
        CMD.SET_PIN_MODE,
        bytes([_u8("pin", pin), 3]) + _u16("differential", differential) + bytes([_flag("report", report)]),
    )


def set_pin_mode_pwm(pin: int, channel: int, resolution: int, frequency: float) -> bytes:
    """PWM mode 8 `[pin, 8, channel, resolution, f64 LE frequency]`; resolution is 1..14 bits."""
    if not 1 <= resolution <= 14:
        raise ValueError(f"resolution {resolution!r} outside 1..14")
    head = bytes([_u8("pin", pin), 8, _u8("channel", channel), resolution])
    return encode(CMD.SET_PIN_MODE, head + struct.pack("<d", frequency))


def digital_write(pin: int, value: int) -> bytes:
    return encode(CMD.DIGITAL_WRITE, bytes([_u8("pin", pin), _flag("value", value)]))


def analog_write(pin: int, value: int) -> bytes:
    return encode(CMD.ANALOG_WRITE, bytes([_u8("pin", pin)]) + _u16("value", value))


def modify_reporting(mode: int, pin: int = 0) -> bytes:
    """Mode 0 disable all, 1 analog enable, 2 digital enable, 3 analog disable, 4 digital disable."""
    return encode(CMD.MODIFY_REPORTING, bytes([_u8("mode", mode, 4), _u8("pin", pin)]))


def get_firmware_version(ext: tuple[int, int] | None = None) -> bytes:
    """The stock probe (no payload), or the extended handshake `[MAGIC, major, minor]` when `ext` is given."""
    if ext is None:
        return encode(CMD.GET_FIRMWARE_VERSION)
    return encode(CMD.GET_FIRMWARE_VERSION, bytes([MAGIC, _u8("major", ext[0]), _u8("minor", ext[1])]))


def servo_attach(pin: int, min_pulse: int, max_pulse: int) -> bytes:
    return encode(
        CMD.SERVO_ATTACH, bytes([_u8("pin", pin)]) + _u16("min_pulse", min_pulse) + _u16("max_pulse", max_pulse)
    )


def servo_write(pin: int, angle: int) -> bytes:
    return encode(CMD.SERVO_WRITE, bytes([_u8("pin", pin), _u8("angle", angle, 180)]))


def servo_detach(pin: int) -> bytes:
    return encode(CMD.SERVO_DETACH, bytes([_u8("pin", pin)]))


def i2c_begin(sda: int | None = None, scl: int | None = None) -> bytes:
    """No arguments sends no pins; both pins send `[sda, scl]` (`(0, 0)` means no pins too)."""
    if sda is None and scl is None:
        return encode(CMD.I2C_BEGIN)
    if sda is None or scl is None:
        raise ValueError("i2c_begin takes both pins or neither")
    return encode(CMD.I2C_BEGIN, bytes([_u8("sda", sda), _u8("scl", scl)]))


def i2c_read(addr: int, reg: int, n: int, stop: bool) -> bytes:
    """`[addr, reg, count, stop]`; the server reads at most 59 bytes, so more is refused here."""
    payload = bytes([_u8("addr", addr, 127), _u8("reg", reg), _u8("count", n, I2C_READ_MAX), _flag("stop", stop)])
    return encode(CMD.I2C_READ, payload)


def i2c_write(addr: int, data: bytes) -> bytes:
    """`[count, addr, data...]`."""
    return encode(CMD.I2C_WRITE, bytes([len(data), _u8("addr", addr, 127)]) + bytes(data))


def sonar_new(trigger: int, echo: int) -> bytes:
    return encode(CMD.SONAR_NEW, bytes([_u8("trigger", trigger), _u8("echo", echo)]))


def dht_new(pin: int) -> bytes:
    return encode(CMD.DHT_NEW, bytes([_u8("pin", pin)]))


def stop_all_reports() -> bytes:
    return encode(CMD.STOP_ALL_REPORTS)


def enable_all_reports() -> bytes:
    return encode(CMD.ENABLE_ALL_REPORTS)


def set_analog_scanning_interval(ms: int) -> bytes:
    return encode(CMD.SET_ANALOG_SCANNING_INTERVAL, bytes([_u8("interval", ms)]))


def analog_out_attach(pin: int, channel: int) -> bytes:
    return encode(CMD.ANALOG_OUT_ATTACH, bytes([_u8("pin", pin), _u8("channel", channel)]))


def analog_out_detach(pin: int) -> bytes:
    return encode(CMD.ANALOG_OUT_DETACH, bytes([_u8("pin", pin)]))


def reset() -> bytes:
    return encode(CMD.RESET)


def one_wire_init(pin: int) -> bytes:
    return encode(CMD.ONE_WIRE_INIT, bytes([_u8("pin", pin)]))


def one_wire_reset() -> bytes:
    return encode(CMD.ONE_WIRE_RESET)


def one_wire_select(rom: bytes) -> bytes:
    if len(rom) != 8:
        raise ValueError(f"a 1-wire ROM is 8 bytes, got {len(rom)}")
    return encode(CMD.ONE_WIRE_SELECT, bytes(rom))


def one_wire_skip() -> bytes:
    return encode(CMD.ONE_WIRE_SKIP)


def one_wire_write(value: int, power: bool = False) -> bytes:
    return encode(CMD.ONE_WIRE_WRITE, bytes([_u8("value", value), _flag("power", power)]))


def one_wire_read() -> bytes:
    return encode(CMD.ONE_WIRE_READ)


def one_wire_reset_search() -> bytes:
    return encode(CMD.ONE_WIRE_RESET_SEARCH)


def one_wire_search() -> bytes:
    return encode(CMD.ONE_WIRE_SEARCH)


def one_wire_crc8(data: bytes) -> bytes:
    """`[n, bytes...]` with exactly n bytes (the stock client sends one more, which the server ignores)."""
    return encode(CMD.ONE_WIRE_CRC8, bytes([len(data)]) + bytes(data))


def sync(tag: int, abort: bool) -> bytes:
    """`[tag_hi, tag_lo, flags]`; flags bit 0 is "abort on error" for the commands after this SYNC."""
    return encode(CMD.SYNC, _u16("tag", tag) + bytes([_flag("abort", abort)]))


def on_bus(bus: int, inner: bytes) -> bytes:
    """Wrap a framed stock command (a builder's output, length byte included) as `[bus, stock_id, payload...]`."""
    if len(inner) < 2 or inner[0] != len(inner) - 1:
        raise ValueError("on_bus takes one framed stock command")
    return encode(CMD.ON_BUS, bytes([_u8("bus", bus)]) + inner[1:])


def get_capabilities(page: int) -> bytes:
    return encode(CMD.GET_CAPABILITIES, bytes([_u8("page", page)]))


def plugin(slot: int, sub: int, args: bytes = b"") -> bytes:
    return encode(CMD.PLUGIN, bytes([_u8("slot", slot), _u8("sub", sub)]) + bytes(args))


# --- parser ------------------------------------------------------------------------------------------------------


class PacketParser:
    """Splits a byte stream of server reports into `(id, payload)` packets, whatever the chunking."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self.dropped = 0  # zero length bytes skipped

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        self._buf += data
        out: list[tuple[int, bytes]] = []
        pos = 0
        while pos < len(self._buf):
            n = self._buf[pos]
            if n == 0:
                self.dropped += 1
                pos += 1
                continue
            if pos + 1 + n > len(self._buf):
                break
            out.append((self._buf[pos + 1], bytes(self._buf[pos + 2 : pos + 1 + n])))
            pos += 1 + n
        del self._buf[:pos]
        return out


# --- decoders ----------------------------------------------------------------------------------------------------


class Firmware(NamedTuple):
    major: int
    minor: int
    build: int
    ext: tuple[int, int] | None


@dataclass(frozen=True)
class PinValue:
    pin: int
    value: int


@dataclass(frozen=True)
class I2cRead:
    addr: int
    reg: int
    data: bytes


@dataclass(frozen=True)
class SonarReport:
    trigger_pin: int
    cm: int


@dataclass(frozen=True)
class DhtData:
    pin: int
    humidity: float
    temperature: float


@dataclass(frozen=True)
class DhtError:
    pin: int
    code: int


@dataclass(frozen=True)
class OneWireReport:
    op: int
    data: bytes


@dataclass(frozen=True)
class ErrorReport:
    seq: int
    cmd: int
    reason: Reason | int
    slot: int | None = None
    sub: int | None = None
    code: int | None = None


@dataclass(frozen=True)
class SyncReply:
    tag: int
    seq: int


@dataclass(frozen=True)
class BusReport:
    bus: int
    report_id: int
    payload: bytes


@dataclass(frozen=True)
class CapabilitiesPage:
    page: int
    last_page: int
    records: list[tuple[int, bytes]]


@dataclass(frozen=True)
class PluginReport:
    slot: int
    sub: int
    data: bytes


def _need(name: str, payload: bytes, n: int) -> None:
    if len(payload) < n:
        raise ValueError(f"{name} payload of {len(payload)} bytes, need {n}")


def decode_digital(payload: bytes) -> PinValue:
    _need("digital", payload, 2)
    return PinValue(payload[0], payload[1])


def decode_analog(payload: bytes) -> PinValue:
    _need("analog", payload, 3)
    return PinValue(payload[0], int.from_bytes(payload[1:3], "big"))


def decode_firmware(payload: bytes) -> Firmware:
    """Stock `[major, minor, build]`, or the long form with `[MAGIC, ext_major, ext_minor]` after it."""
    if len(payload) == 3:
        return Firmware(payload[0], payload[1], payload[2], None)
    if len(payload) == 6 and payload[3] == MAGIC:
        return Firmware(payload[0], payload[1], payload[2], (payload[4], payload[5]))
    raise ValueError(f"firmware payload {payload.hex()} is neither the stock nor the long form")


def decode_servo_unavailable(payload: bytes) -> int:
    _need("servo unavailable", payload, 1)
    return payload[0]


def decode_i2c_read(payload: bytes) -> I2cRead:
    _need("I2C read", payload, 3)
    if payload[0] != len(payload) - 3:
        raise ValueError(f"I2C read count {payload[0]} does not match {len(payload) - 3} data bytes")
    return I2cRead(payload[1], payload[2], bytes(payload[3:]))


def decode_i2c_short(payload: bytes) -> int:
    """The refusal `[1, addr]` of a NACK or short read (stock mode); returns the address."""
    _need("I2C short", payload, 2)
    return payload[1]


def decode_sonar(payload: bytes) -> SonarReport:
    _need("sonar", payload, 3)
    return SonarReport(payload[0], int.from_bytes(payload[1:3], "big"))


def decode_dht(payload: bytes) -> DhtData | DhtError:
    _need("DHT", payload, 1)
    if payload[0] == 0 and len(payload) >= 10:
        humidity, temperature = struct.unpack("<ff", payload[2:10])
        return DhtData(payload[1], humidity, temperature)
    if payload[0] == 1 and len(payload) >= 3:
        return DhtError(payload[1], payload[2])
    raise ValueError(f"DHT payload {payload.hex()} is neither data nor an error")


def decode_one_wire(payload: bytes) -> OneWireReport:
    _need("one-wire", payload, 1)
    return OneWireReport(payload[0], bytes(payload[1:]))


def decode_error(payload: bytes) -> ErrorReport:
    """`[seq_hi, seq_lo, cmd, reason]`, then `[slot, sub]` for cmd 60 when present, then `[code]` for PLUGIN."""
    _need("ERROR", payload, 4)
    seq = int.from_bytes(payload[0:2], "big")
    cmd = payload[2]
    try:
        reason: Reason | int = Reason(payload[3])
    except ValueError:
        reason = payload[3]
    pos = 4
    slot = sub = code = None
    if cmd == CMD.PLUGIN and len(payload) >= pos + 2:
        slot, sub = payload[pos], payload[pos + 1]
        pos += 2
    if reason == Reason.PLUGIN and len(payload) >= pos + 1:
        code = payload[pos]
    return ErrorReport(seq, cmd, reason, slot, sub, code)


def decode_sync_reply(payload: bytes) -> SyncReply:
    _need("SYNC_REPLY", payload, 4)
    return SyncReply(int.from_bytes(payload[0:2], "big"), int.from_bytes(payload[2:4], "big"))


def decode_bus_report(payload: bytes) -> BusReport:
    """`[bus, stock_id, stock payload...]`; `payload` of the result goes to the stock decoder of `report_id`."""
    _need("BUS_REPORT", payload, 2)
    return BusReport(payload[0], payload[1], bytes(payload[2:]))


def decode_capabilities(payload: bytes) -> CapabilitiesPage:
    """`[page, last_page, (type, len, value)...]`; records stay raw, unknown types included."""
    _need("CAPABILITIES", payload, 2)
    records: list[tuple[int, bytes]] = []
    pos = 2
    while pos < len(payload):
        if pos + 2 > len(payload) or pos + 2 + payload[pos + 1] > len(payload):
            raise ValueError("CAPABILITIES record is truncated")
        n = payload[pos + 1]
        records.append((payload[pos], bytes(payload[pos + 2 : pos + 2 + n])))
        pos += 2 + n
    return CapabilitiesPage(payload[0], payload[1], records)


def decode_plugin_report(payload: bytes) -> PluginReport:
    _need("PLUGIN_REPORT", payload, 2)
    return PluginReport(payload[0], payload[1], bytes(payload[2:]))
