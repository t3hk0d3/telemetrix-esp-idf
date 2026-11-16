#include <string.h>

#include "tmx_core.h"
#include "unity.h"

#define ALL_DIGITAL (TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT | TELEMETRIX_CAP_PULL_UP | TELEMETRIX_CAP_PULL_DOWN)

static const telemetrix_pin_t pins[] = {
    {
        .gpio = 2, .caps = ALL_DIGITAL | TELEMETRIX_CAP_PWM, .safe = TELEMETRIX_SAFE_DRIVE_LOW, .adc_unit = -1,
        .adc_channel = -1
    },
    {.gpio = 4, .caps = ALL_DIGITAL, .safe = TELEMETRIX_SAFE_PULL_UP, .adc_unit = -1, .adc_channel = -1},
    {
        .gpio = 5, .caps = TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_ADC, .safe = TELEMETRIX_SAFE_FLOAT, .adc_unit = 1,
        .adc_channel = 4
    },
    {
        .gpio = 21, .caps = ALL_DIGITAL | TELEMETRIX_CAP_I2C, .safe = TELEMETRIX_SAFE_FLOAT, .adc_unit = -1,
        .adc_channel = -1
    },
    {
        .gpio = 22, .caps = ALL_DIGITAL | TELEMETRIX_CAP_I2C | TELEMETRIX_CAP_ONE_WIRE, .safe = TELEMETRIX_SAFE_FLOAT,
        .adc_unit = -1, .adc_channel = -1
    },
};
static const telemetrix_adc_grant_t adc[] = {{.unit = 1, .handle = NULL}};
static const telemetrix_i2c_bus_t i2c[] = {{.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000}};
static const telemetrix_onewire_bus_t onewire[] = {{.bus = 0, .gpio = 22}};

static telemetrix_profile_t valid(void)
{
    telemetrix_profile_t p = {
        .pins = pins, .n_pins = sizeof(pins) / sizeof(pins[0]),
        .ledc = {.speed_mode = 0, .timer_mask = 0x0f, .channel_mask = 0x0f},
        .rmt = {.rx_channels = 1, .tx_channels = 1},
        .adc = adc, .n_adc = 1, .i2c = i2c, .n_i2c = 1, .onewire = onewire, .n_onewire = 1,
    };
    return p;
}

TEST_CASE("On a valid profile: ESP_OK", "[profile]")
{
    telemetrix_profile_t p = valid();
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate(&p));
}

TEST_CASE("On a GPIO listed twice: ESP_ERR_INVALID_ARG", "[profile]")
{
    telemetrix_pin_t dup[] = {pins[0], pins[1], pins[0]};
    telemetrix_profile_t p = valid();
    p.pins = dup;
    p.n_pins = 3;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
}

TEST_CASE("On more than 64 pins: ESP_ERR_INVALID_ARG", "[profile]")
{
    static telemetrix_pin_t many[65];
    for (int i = 0; i < 65; i++) {
        many[i] = (telemetrix_pin_t) {
            .gpio = (uint8_t)i, .caps = TELEMETRIX_CAP_DIN, .adc_unit = -1,
            .adc_channel = -1
        };
    }
    telemetrix_profile_t p = {.pins = many, .n_pins = 65};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    p.n_pins = 64;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate(&p));
}

TEST_CASE("On a safe state out of range: ESP_ERR_INVALID_ARG", "[profile]")
{
    telemetrix_pin_t bad[1] = {pins[1]};
    bad[0].safe = (telemetrix_safe_state_t)9;
    telemetrix_profile_t p = {.pins = bad, .n_pins = 1};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
}

TEST_CASE("On caps outside the defined bits: ESP_ERR_INVALID_ARG", "[profile]")
{
    telemetrix_pin_t bad[1] = {pins[1]};
    bad[0].caps |= 0x100;
    telemetrix_profile_t p = {.pins = bad, .n_pins = 1};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
}

TEST_CASE("On an ADC pin without a channel or with an ungranted unit: ESP_ERR_INVALID_ARG", "[profile]")
{
    telemetrix_pin_t bad[1] = {pins[2]};
    telemetrix_profile_t p = valid();
    p.pins = bad;
    p.n_pins = 1;
    bad[0].adc_channel = -1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    bad[0] = pins[2];
    bad[0].adc_unit = 2;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
}

TEST_CASE("On two I2C buses or two 1-wire buses with one number: ESP_ERR_INVALID_ARG", "[profile]")
{
    telemetrix_i2c_bus_t two_i2c[] = {i2c[0], i2c[0]};
    telemetrix_onewire_bus_t two_ow[] = {onewire[0], onewire[0]};
    telemetrix_profile_t p = valid();
    p.i2c = two_i2c;
    p.n_i2c = 2;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    p = valid();
    p.onewire = two_ow;
    p.n_onewire = 2;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
}

TEST_CASE("On an I2C bus with fixed pins that are not profile pins with the I2C capability: ESP_ERR_INVALID_ARG",
          "[profile]")
{
    telemetrix_i2c_bus_t off_profile[] = {{.bus = 0, .sda = 40, .scl = 22}};
    telemetrix_i2c_bus_t no_cap[] = {{.bus = 0, .sda = 21, .scl = 4}};
    telemetrix_i2c_bus_t chooses[] = {{.bus = 0, .sda = -1, .scl = -1}};
    telemetrix_profile_t p = valid();
    p.i2c = off_profile;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    p.i2c = no_cap;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    p.i2c = chooses;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate(&p));
}

static int app_bus;     /* stands in for an application-owned i2c_master_bus_handle_t */

TEST_CASE("On an application-owned I2C bus with sda or scl -1: ESP_ERR_INVALID_ARG", "[profile]")
{
    telemetrix_i2c_bus_t no_sda[] = {{.bus = 0, .sda = -1, .scl = 26, .handle = &app_bus}};
    telemetrix_i2c_bus_t no_scl[] = {{.bus = 0, .sda = 25, .scl = -1, .handle = &app_bus}};
    telemetrix_i2c_bus_t neither[] = {{.bus = 0, .sda = -1, .scl = -1, .handle = &app_bus}};
    telemetrix_profile_t p = valid();
    p.i2c = no_sda;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    p.i2c = no_scl;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    p.i2c = neither;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
}

TEST_CASE("On an application-owned I2C bus whose sda or scl is a profile pin: ESP_ERR_INVALID_ARG", "[profile]")
{
    telemetrix_i2c_bus_t sda_in[] = {{.bus = 0, .sda = 21, .scl = 26, .handle = &app_bus}};
    telemetrix_i2c_bus_t scl_in[] = {{.bus = 0, .sda = 25, .scl = 22, .handle = &app_bus}};
    telemetrix_i2c_bus_t both_in[] = {{.bus = 0, .sda = 21, .scl = 22, .handle = &app_bus}};
    telemetrix_profile_t p = valid();
    p.i2c = sda_in;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    p.i2c = scl_in;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    p.i2c = both_in;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
}

TEST_CASE("On an application-owned I2C bus with the same pin as sda and scl: ESP_ERR_INVALID_ARG", "[profile]")
{
    telemetrix_i2c_bus_t same[] = {{.bus = 0, .sda = 25, .scl = 25, .handle = &app_bus}};
    telemetrix_profile_t p = valid();
    p.i2c = same;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
}

TEST_CASE("On an application-owned I2C bus with both pins outside the profile: ESP_OK", "[profile]")
{
    telemetrix_i2c_bus_t shared[] = {{.bus = 0, .sda = 25, .scl = 26, .handle = &app_bus}};
    telemetrix_profile_t p = valid();
    p.i2c = shared;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate(&p));
}

TEST_CASE("On a fixed 1-wire gpio that is not a profile pin with the 1-wire capability: ESP_ERR_INVALID_ARG",
          "[profile]")
{
    telemetrix_onewire_bus_t no_cap[] = {{.bus = 0, .gpio = 4}};
    telemetrix_onewire_bus_t unknown[] = {{.bus = 0, .gpio = 40}};
    telemetrix_profile_t p = valid();
    p.onewire = no_cap;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    p.onewire = unknown;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
}

TEST_CASE("On an LEDC grant with no channel or no timer while a pin has the PWM capability: ESP_ERR_INVALID_ARG",
          "[profile]")
{
    telemetrix_profile_t p = valid();
    p.ledc.timer_mask = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    p = valid();
    p.ledc.channel_mask = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate(&p));
    p = valid();
    p = (telemetrix_profile_t) {
        .pins = pins + 1, .n_pins = 1
    };
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate(&p));
}

TEST_CASE("On a GPIO in the profile: returns its entry", "[profile]")
{
    telemetrix_profile_t p = valid();
    TEST_ASSERT_EQUAL_PTR(&pins[3], tmx_profile_pin(&p, 21));
}

TEST_CASE("On a GPIO not in the profile (40, 255): returns NULL", "[profile]")
{
    telemetrix_profile_t p = valid();
    TEST_ASSERT_NULL(tmx_profile_pin(&p, 40));
    TEST_ASSERT_NULL(tmx_profile_pin(&p, 255));
}

/* ---- named capabilities ---- */

static const char *const led_only[] = {"x.led"};
static const char *const led_and_horn[] = {"x.led", "x.horn"};

/** pins with gpio 2 naming the given capabilities */
static telemetrix_pin_t named_pins[2];

static telemetrix_profile_t with_named(const char *const *named, uint8_t n)
{
    named_pins[0] = pins[0];
    named_pins[1] = pins[1];
    named_pins[0].named = named;
    named_pins[0].n_named = n;
    return (telemetrix_profile_t) {
        .pins = named_pins, .n_pins = 2, .ledc = {.timer_mask = 0x0f, .channel_mask = 0x0f}
    };
}

TEST_CASE("tmx_profile_validate_named on pins naming declared capabilities: ESP_OK", "[profile]")
{
    telemetrix_profile_t p = with_named(led_and_horn, 2);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate_named(&p, led_and_horn, 2));
}

TEST_CASE("tmx_profile_validate_named on pins naming nothing: ESP_OK with no names", "[profile]")
{
    telemetrix_profile_t p = valid();
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate_named(&p, NULL, 0));
}

TEST_CASE("tmx_profile_validate_named on a pin naming a capability no plugin declares: ESP_ERR_INVALID_ARG",
          "[profile]")
{
    telemetrix_profile_t p = with_named(led_and_horn, 2);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate_named(&p, led_only, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate_named(&p, NULL, 0));
}

TEST_CASE("tmx_profile_validate_named on a pin with n_named but no array: ESP_ERR_INVALID_ARG", "[profile]")
{
    telemetrix_profile_t p = with_named(NULL, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate_named(&p, led_only, 1));
}

TEST_CASE("tmx_profile_validate_named on more than 16 names: ESP_ERR_INVALID_ARG", "[profile]")
{
    static const char *const many[17] = {
        "x.led", "n.1", "n.2", "n.3", "n.4", "n.5", "n.6", "n.7", "n.8", "n.9", "n.10", "n.11", "n.12", "n.13",
        "n.14", "n.15", "n.16",
    };
    telemetrix_profile_t p = with_named(led_only, 1);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate_named(&p, many, 16));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate_named(&p, many, 17));
}

TEST_CASE("tmx_profile_validate_named on a name over 24 bytes: ESP_ERR_INVALID_ARG", "[profile]")
{
    static const char *const names[] = {"x.led", "abcdefghijkl.mnopqrstuvwx"};
    telemetrix_profile_t p = with_named(led_only, 1);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate_named(&p, names, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_profile_validate_named(&p, names, 2));
}
