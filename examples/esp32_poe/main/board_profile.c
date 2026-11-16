#include "board_profile.h"

/*
 * Olimex ESP32-POE-ISO (also covers ESP32-POE, same LAN8720 wiring), read from the schematic
 * https://github.com/OLIMEX/ESP32-POE-ISO/blob/master/HARDWARE/ESP32-PoE-ISO-Rev.O/ESP32-PoE-ISO_Rev_O.kicad_sch
 * (hardware revision O, the newest in that repository). Header nets: EXT1 GPIO0-5 (1, 3 are the USB-UART console),
 * EXT2 GPIO13 14 15 16 32 33 34 35 36 39; UEXT repeats GPIO2 5 13 36.
 *
 * Left out of the profile:
 *   GPIO 17, 18, 19, 21, 22, 23, 25, 26, 27  LAN8720 RMII, clock, MDC and MDIO
 *   GPIO 12                                  PHY power enable
 *   GPIO 6-11                                the module's flash
 *   GPIO 0, 2, 15                            strapping pins (boot mode, boot log, SD timing)
 *   GPIO 1, 3                                UART0, the console and the programming port
 *   GPIO 34                                  the user button BUT1
 * The 2.2k series resistor arrays RM1/RM2 sit between the module and some header pins; fine for logic, I2C and 1-wire
 * at 100 kHz, not for fast edges. GPIO16 is unusable on a WROVER module (the PSRAM uses it); this board has a WROOM.
 *
 * ADC only on ADC1 (ADC2 is unusable while Wi-Fi runs; the Ethernet build keeps the same rule). Input-only GPIO
 * 35, 36, 39 are digital inputs and ADC only, without pulls (they have none).
 *
 * Board pass loopback: a jumper from GPIO32 (output) to GPIO33 (input), EXT2 pins 6 and 5.
 */

#define BIDIR (TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT | TELEMETRIX_CAP_PULL_UP | TELEMETRIX_CAP_PULL_DOWN | \
               TELEMETRIX_CAP_PWM | TELEMETRIX_CAP_I2C | TELEMETRIX_CAP_ONE_WIRE)
#define INPUT_ONLY (TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_ADC)

#define PIN(n, caps_, unit, channel) \
    {.gpio = (n), .caps = (caps_), .safe = TELEMETRIX_SAFE_FLOAT, .adc_unit = (unit), .adc_channel = (channel)}

/* ESP32 ADC1 channel numbers: GPIO36 0, GPIO39 3, GPIO32 4, GPIO33 5, GPIO35 7. adc_unit 0 is ADC1. */
static const telemetrix_pin_t pins[] = {
    PIN(4, BIDIR, -1, -1),
    PIN(5, BIDIR, -1, -1),
    PIN(13, BIDIR, -1, -1),
    PIN(14, BIDIR, -1, -1),
    PIN(16, BIDIR, -1, -1),
    PIN(32, BIDIR | TELEMETRIX_CAP_ADC, 0, 4),
    PIN(33, BIDIR | TELEMETRIX_CAP_ADC, 0, 5),
    PIN(35, INPUT_ONLY, 0, 7),
    PIN(36, INPUT_ONLY, 0, 0),
    PIN(39, INPUT_ONLY, 0, 3),
};

static const telemetrix_adc_grant_t adc[] = {{.unit = 0, .handle = NULL}};
/* The client chooses the pins; GPIO13 and GPIO16 carry the board's I2C labels. */
static const telemetrix_i2c_bus_t i2c[] = {{.bus = 0, .port = 0, .sda = -1, .scl = -1, .speed_hz = 100000}};
static const telemetrix_onewire_bus_t onewire[] = {{.bus = 0, .gpio = -1}};

const telemetrix_profile_t board_profile = {
    .pins = pins,
    .n_pins = sizeof(pins) / sizeof(pins[0]),
    /* ESP32 low-speed mode is 1 (ledc_mode_t); 8 channels, 4 timers. */
    .ledc = {.speed_mode = 1, .timer_mask = 0x0f, .channel_mask = 0xff},
    .rmt = {.rx_channels = 2, .tx_channels = 1},
    .adc = adc,
    .n_adc = 1,
    .i2c = i2c,
    .n_i2c = 1,
    .onewire = onewire,
    .n_onewire = 1,
};
