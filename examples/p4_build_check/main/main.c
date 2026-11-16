#include "esp_log.h"
#include "telemetrix.h"

/* Only proves the component builds for the ESP32-P4: a few GPIOs, no network bring-up, not meant to be flashed. */

#define BIDIR (TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT | TELEMETRIX_CAP_PULL_UP | TELEMETRIX_CAP_PULL_DOWN | \
               TELEMETRIX_CAP_PWM | TELEMETRIX_CAP_I2C | TELEMETRIX_CAP_ONE_WIRE)

#define PIN(n, caps_, unit, channel) \
    {.gpio = (n), .caps = (caps_), .safe = TELEMETRIX_SAFE_FLOAT, .adc_unit = (unit), .adc_channel = (channel)}

/* ESP32-P4 ADC1: GPIO16-23 are channels 0-7. GPIO7 and 8 serve as the fixed I2C pair. */
static const telemetrix_pin_t pins[] = {
    PIN(2, BIDIR, -1, -1),
    PIN(3, BIDIR, -1, -1),
    PIN(7, BIDIR, -1, -1),
    PIN(8, BIDIR, -1, -1),
    PIN(20, BIDIR | TELEMETRIX_CAP_ADC, 0, 4),
    PIN(21, BIDIR | TELEMETRIX_CAP_ADC, 0, 5),
};
static const telemetrix_adc_grant_t adc[] = {{.unit = 0, .handle = NULL}};
/* Fixed pins: the stock client's I2C_BEGIN names none. */
static const telemetrix_i2c_bus_t i2c[] = {{.bus = 0, .port = 0, .sda = 7, .scl = 8, .speed_hz = 100000}};
static const telemetrix_onewire_bus_t onewire[] = {{.bus = 0, .gpio = -1}};

static const telemetrix_profile_t profile = {
    .pins = pins,
    .n_pins = sizeof(pins) / sizeof(pins[0]),
    .ledc = {.speed_mode = 0, .timer_mask = 0x0f, .channel_mask = 0xff},
    .rmt = {.rx_channels = 2, .tx_channels = 1},
    .adc = adc,
    .n_adc = 1,
    .i2c = i2c,
    .n_i2c = 1,
    .onewire = onewire,
    .n_onewire = 1,
};

void app_main(void)
{
    telemetrix_config_t config;
    telemetrix_config_default(&config);
    const esp_err_t err = telemetrix_start(&profile, &config, NULL);
    ESP_LOGI("p4_build_check", "telemetrix_start: %s", esp_err_to_name(err));
}
