"""Stock-mode rules: the supported upstream firmware range, and the pin lists of the upstream client.

Upstream Telemetrix4Esp32 has no discovery, so a stock-mode board model comes from the pin lists of the upstream
Python client, `telemetrix-esp32` 2.1.1 (`telemetrix_esp32.py`, the `valid_*_pins` attributes of `TelemetrixEsp32`).
"""

from __future__ import annotations

from collections.abc import Mapping
from types import MappingProxyType

from .discovery import BusInfo, Cap, PinInfo
from .errors import IncompatibleFirmware

__all__ = ["STOCK_BUSES", "STOCK_PINS", "check_stock_firmware", "stock_pins"]

# From telemetrix-esp32 2.1.1 (valid_gpio_input_pins, valid_gpio_output_pins, valid_analog_in_pins,
# valid_servo_pins, valid_touch_pins), copied verbatim.
STOCK_PINS: Mapping[str, tuple[int, ...]] = MappingProxyType(
    {
        "input": (4, 5, 12, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33),
        "output": (2, 4, 5, 12, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33),
        "analog": (32, 33, 34, 35, 36, 39),
        "servo": (4, 5, 12, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33),
        "touch": (4, 12, 13, 14, 15, 27, 32, 33),
    }
)

# Upstream takes the I2C pins and the 1-wire pin from the client, on its only bus.
STOCK_BUSES: tuple[BusInfo, ...] = (BusInfo("i2c", 0, None, None), BusInfo("one_wire", 0, None, None))


def check_stock_firmware(major: int, minor: int) -> None:
    """Raise `IncompatibleFirmware` unless the firmware is 3.2 or a later 3.x (Ruling R2)."""
    if major != 3 or minor < 2:
        raise IncompatibleFirmware(f"firmware {major}.{minor} is not supported in stock mode (need 3.x, x >= 2)")


def stock_pins() -> dict[int, PinInfo]:
    """The stock board's pins, capabilities inferred from list membership, in GPIO order.

    input: DIN, PULL_UP, PULL_DOWN; output: DOUT, PWM (upstream's PWM uses the output list); servo: PWM; analog: ADC;
    input and output both: I2C and ONE_WIRE. Touch has no capability bit, so a touch-only pin (15) is left out. The
    safe state and the ADC unit and channel are unknown (None).
    """
    caps: dict[int, Cap] = {}

    def grant(names: tuple[int, ...], cap: Cap) -> None:
        for gpio in names:
            caps[gpio] = caps.get(gpio, Cap(0)) | cap

    grant(STOCK_PINS["input"], Cap.DIN | Cap.PULL_UP | Cap.PULL_DOWN)
    grant(STOCK_PINS["output"], Cap.DOUT | Cap.PWM)
    grant(STOCK_PINS["servo"], Cap.PWM)
    grant(STOCK_PINS["analog"], Cap.ADC)
    grant(tuple(set(STOCK_PINS["input"]) & set(STOCK_PINS["output"])), Cap.I2C | Cap.ONE_WIRE)
    return {gpio: PinInfo(gpio, caps[gpio], None, None, None) for gpio in sorted(caps)}
