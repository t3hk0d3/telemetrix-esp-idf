#include "board_profile.h"

#include "sdkconfig.h"

/*
 * ESP32-S3-DevKitC-1 and ESP32-C6-DevKitC-1, selected by CONFIG_IDF_TARGET (the host check builds the S3 one).
 * Pin choice follows the DevKitC-1 user guides (docs.espressif.com); not checked against the board schematics.
 * S3, left out: GPIO 0 (boot), 3, 45, 46 (strapping), 19 and 20 (USB), 26-37 (flash and PSRAM), 48 (the RGB LED).
 * S3, ADC: only GPIO 4 and 5 are ADC1 (channels 3 and 4); GPIO 12, 13 and 14 are ADC2 and ADC2 is unusable while
 *   Wi-Fi runs, so they are digital and PWM only.
 * C6, left out: GPIO 4, 5, 8, 9, 15 (strapping), 8 also the RGB LED, 12 and 13 (USB), 16 and 17 (UART0 console),
 *   24-30 (flash). ADC1 is GPIO0-6 (channels 0-6); GPIO 4 and 5 are strapping, so ADC is on 0, 1, 2, 3 and 6.
 *
 * Suggested loopback jumper if these boards are ever used for a pass: S3 GPIO16 to GPIO17, C6 GPIO18 to GPIO19.
 */

#define BIDIR (TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT | TELEMETRIX_CAP_PULL_UP | TELEMETRIX_CAP_PULL_DOWN | \
               TELEMETRIX_CAP_PWM | TELEMETRIX_CAP_I2C | TELEMETRIX_CAP_ONE_WIRE)

#define PIN(n, caps_, unit, channel) \
    {.gpio = (n), .caps = (caps_), .safe = TELEMETRIX_SAFE_FLOAT, .adc_unit = (unit), .adc_channel = (channel)}

#if CONFIG_IDF_TARGET_ESP32C6

/* adc_unit 0 is ADC1; its channel number is the GPIO number. */
static const telemetrix_pin_t pins[] = {
    PIN(0, BIDIR | TELEMETRIX_CAP_ADC, 0, 0),
    PIN(1, BIDIR | TELEMETRIX_CAP_ADC, 0, 1),
    PIN(2, BIDIR | TELEMETRIX_CAP_ADC, 0, 2),
    PIN(3, BIDIR | TELEMETRIX_CAP_ADC, 0, 3),
    PIN(6, BIDIR | TELEMETRIX_CAP_ADC, 0, 6),
    PIN(7, BIDIR, -1, -1),
    PIN(10, BIDIR, -1, -1),
    PIN(11, BIDIR, -1, -1),
    PIN(18, BIDIR, -1, -1),
    PIN(19, BIDIR, -1, -1),
    PIN(20, BIDIR, -1, -1),
    PIN(21, BIDIR, -1, -1),
    PIN(22, BIDIR, -1, -1),
    PIN(23, BIDIR, -1, -1),
};
#define LEDC_CHANNELS 0x3f /* 6 channels */
/* I2C bus 0: SDA GPIO23, SCL GPIO22 (plain GPIO, both in the stock client's pin lists). */
#define I2C_SDA 23
#define I2C_SCL 22

#else

/* adc_unit 0 is ADC1; GPIO4 is channel 3, GPIO5 channel 4. */
static const telemetrix_pin_t pins[] = {
    PIN(4, BIDIR | TELEMETRIX_CAP_ADC, 0, 3),
    PIN(5, BIDIR | TELEMETRIX_CAP_ADC, 0, 4),
    PIN(12, BIDIR, -1, -1),
    PIN(13, BIDIR, -1, -1),
    PIN(14, BIDIR, -1, -1),
    PIN(16, BIDIR, -1, -1),
    PIN(17, BIDIR, -1, -1),
    PIN(18, BIDIR, -1, -1),
    PIN(21, BIDIR, -1, -1),
};
#define LEDC_CHANNELS 0xff /* 8 channels */
/* I2C bus 0: SDA GPIO13, SCL GPIO14 (digital-only ADC2 pins, both in the stock client's pin lists; the Arduino default
 * pair 8/9 is not in them). */
#define I2C_SDA 13
#define I2C_SCL 14

#endif

static const telemetrix_adc_grant_t adc[] = {{.unit = 0, .handle = NULL}};
/* Fixed pins: the stock client's I2C_BEGIN names none. They stay usable as plain GPIO until a client sends I2C_BEGIN. */
static const telemetrix_i2c_bus_t i2c[] = {{.bus = 0, .port = 0, .sda = I2C_SDA, .scl = I2C_SCL, .speed_hz = 100000}};
static const telemetrix_onewire_bus_t onewire[] = {{.bus = 0, .gpio = -1}};

const telemetrix_profile_t board_profile = {
    .pins = pins,
    .n_pins = sizeof(pins) / sizeof(pins[0]),
    /* Low-speed mode is 0 (ledc_mode_t) on these chips; 4 timers. */
    .ledc = {.speed_mode = 0, .timer_mask = 0x0f, .channel_mask = LEDC_CHANNELS},
    .rmt = {.rx_channels = 2, .tx_channels = 1},
    .adc = adc,
    .n_adc = 1,
    .i2c = i2c,
    .n_i2c = 1,
    .onewire = onewire,
    .n_onewire = 1,
};
