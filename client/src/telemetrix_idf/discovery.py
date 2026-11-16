"""Discovery: the board model's records, parsed from the server's CAPABILITIES pages.

Record layouts are in the README (protocol, Discovery). Unknown record types, bus kinds and LIMIT kinds never fail
discovery: unknown types are skipped and counted, unknown bus kinds skipped, unknown LIMIT kinds stored as
`"kind_<n>"`. A known record too short for its layout raises `ProtocolError`.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass, field
from enum import IntEnum, IntFlag
from typing import Literal

from .errors import ProtocolError
from .wire import CapabilitiesPage

__all__ = [
    "LIMIT_NAMES",
    "BusInfo",
    "Cap",
    "ChipInfo",
    "Discovery",
    "PinInfo",
    "PluginInfo",
    "SafeState",
    "parse_discovery",
]

REC_CHIP = 1
REC_PIN = 2
REC_BUS = 3
REC_LIMIT = 4
REC_PLUGIN = 5
REC_NAMED_CAP = 6

LIMIT_NAMES = {1: "servos", 2: "pwm_channels", 3: "pwm_timers", 4: "dht", 5: "sonar", 6: "analog_floor_ms"}


class Cap(IntFlag):
    """What a pin may be used for (`telemetrix_cap_t`). Bits 8-15 are reserved and kept when set."""

    DIN = 1 << 0
    DOUT = 1 << 1
    PULL_UP = 1 << 2
    PULL_DOWN = 1 << 3
    PWM = 1 << 4
    ADC = 1 << 5
    I2C = 1 << 6
    ONE_WIRE = 1 << 7


class SafeState(IntEnum):
    """The state a pin returns to on release (`telemetrix_safe_state_t`)."""

    FLOAT = 0
    PULL_UP = 1
    PULL_DOWN = 2
    DRIVE_LOW = 3
    DRIVE_HIGH = 4


@dataclass(frozen=True)
class ChipInfo:
    model: int  # esp_chip_model_t
    revision: tuple[int, int]  # (major, minor)
    idf_version: str


@dataclass(frozen=True)
class PinInfo:
    gpio: int
    caps: Cap
    safe: SafeState | int | None  # None in stock mode (unknown); an unknown value stays an int
    adc_unit: int | None
    adc_channel: int | None
    named: tuple[str, ...] = ()  # named capabilities granted to the pin


@dataclass(frozen=True)
class BusInfo:
    kind: Literal["i2c", "one_wire"]
    bus: int
    pins: tuple[int, ...] | None  # (sda, scl) or (gpio,); None = the client chooses
    speed_hz: int | None  # I2C only


@dataclass(frozen=True)
class PluginInfo:
    slot: int
    major: int
    minor: int
    name: str


@dataclass
class Discovery:
    chip: ChipInfo | None = None
    pins: dict[int, PinInfo] = field(default_factory=dict)
    buses: list[BusInfo] = field(default_factory=list)
    limits: dict[str, int] = field(default_factory=dict)
    plugins: dict[str, PluginInfo] = field(default_factory=dict)
    unknown_records: int = 0


def _need(name: str, value: bytes, n: int) -> None:
    if len(value) < n:
        raise ProtocolError(f"{name} record of {len(value)} bytes, need {n}")


def _i8(b: int) -> int:
    return b - 256 if b >= 128 else b


def _ascii(value: bytes) -> str:
    return value.decode("ascii", errors="replace")


def parse_discovery(pages: Sequence[CapabilitiesPage]) -> Discovery:
    """Build the board model from every page, in page order. Named capability ids are resolved after all records."""
    d = Discovery()
    cap_names: dict[int, str] = {}
    pin_named_ids: dict[int, bytes] = {}
    for page in pages:
        for rtype, value in page.records:
            if rtype == REC_CHIP:
                _need("CHIP", value, 4)
                rev = int.from_bytes(value[2:4], "big")
                d.chip = ChipInfo(int.from_bytes(value[0:2], "big"), divmod(rev, 100), _ascii(value[4:]))
            elif rtype == REC_NAMED_CAP:
                _need("NAMED_CAP", value, 1)
                cap_names[value[0]] = _ascii(value[1:])
            elif rtype == REC_PIN:
                _need("PIN", value, 6)
                gpio = value[0]
                unit, channel = _i8(value[4]), _i8(value[5])
                adc = (None, None) if unit < 0 or channel < 0 else (unit, channel)
                try:
                    safe: SafeState | int = SafeState(value[3])
                except ValueError:
                    safe = value[3]
                d.pins[gpio] = PinInfo(gpio, Cap(int.from_bytes(value[1:3], "big")), safe, adc[0], adc[1])
                pin_named_ids[gpio] = value[6:]
            elif rtype == REC_BUS:
                _need("BUS", value, 2)
                bus = _parse_bus(value)
                if bus is not None:
                    d.buses.append(bus)
            elif rtype == REC_LIMIT:
                _need("LIMIT", value, 3)
                d.limits[LIMIT_NAMES.get(value[0], f"kind_{value[0]}")] = int.from_bytes(value[1:3], "big")
            elif rtype == REC_PLUGIN:
                _need("PLUGIN", value, 3)
                name = _ascii(value[3:])
                d.plugins[name] = PluginInfo(value[0], value[1], value[2], name)
            else:
                d.unknown_records += 1
    for gpio, ids in pin_named_ids.items():
        if ids:
            named = tuple(cap_names.get(i, f"cap_{i}") for i in ids)
            p = d.pins[gpio]
            d.pins[gpio] = PinInfo(p.gpio, p.caps, p.safe, p.adc_unit, p.adc_channel, named)
    return d


def _parse_bus(value: bytes) -> BusInfo | None:
    kind, bus = value[0], value[1]
    if kind == 0:
        _need("I2C BUS", value, 8)
        sda, scl = _i8(value[2]), _i8(value[3])
        pins = None if sda < 0 or scl < 0 else (sda, scl)
        return BusInfo("i2c", bus, pins, int.from_bytes(value[4:8], "big"))
    if kind == 1:
        _need("1-wire BUS", value, 3)
        gpio = _i8(value[2])
        return BusInfo("one_wire", bus, None if gpio < 0 else (gpio,), None)
    return None
