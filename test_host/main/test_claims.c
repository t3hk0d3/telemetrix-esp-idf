#include <string.h>

#include "fake/tmx_drv_fake.h"
#include "tmx_core.h"
#include "unity.h"

#define ALL_DIGITAL (TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT | TELEMETRIX_CAP_PULL_UP | TELEMETRIX_CAP_PULL_DOWN)

static const telemetrix_pin_t pins[] = {
    {.gpio = 2, .caps = ALL_DIGITAL | TELEMETRIX_CAP_PWM, .safe = TELEMETRIX_SAFE_DRIVE_LOW, .adc_unit = -1,
     .adc_channel = -1},
    {.gpio = 4, .caps = ALL_DIGITAL, .safe = TELEMETRIX_SAFE_PULL_UP, .adc_unit = -1, .adc_channel = -1},
    {.gpio = 5, .caps = TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_ADC, .safe = TELEMETRIX_SAFE_PULL_DOWN, .adc_unit = 1,
     .adc_channel = 4},
    {.gpio = 21, .caps = ALL_DIGITAL | TELEMETRIX_CAP_I2C, .safe = TELEMETRIX_SAFE_DRIVE_HIGH, .adc_unit = -1,
     .adc_channel = -1},
    {.gpio = 22, .caps = TELEMETRIX_CAP_DIN, .safe = TELEMETRIX_SAFE_FLOAT, .adc_unit = -1, .adc_channel = -1},
};
static int shared_i2c_handle, shared_adc_handle;
static const telemetrix_adc_grant_t adc[] = {{.unit = 1, .handle = &shared_adc_handle}};
static const telemetrix_i2c_bus_t i2c[] = {{.bus = 0, .port = 0, .sda = 21, .scl = -1, .speed_hz = 100000,
                                            .handle = &shared_i2c_handle}};

static telemetrix_profile_t profile(void)
{
    telemetrix_profile_t p = {
        .pins = pins, .n_pins = sizeof(pins) / sizeof(pins[0]),
        .ledc = {.speed_mode = 0, .timer_mask = 0x0f, .channel_mask = 0xff},
        .rmt = {.rx_channels = 1, .tx_channels = 1},
        .adc = adc, .n_adc = 1, .i2c = i2c, .n_i2c = 1,
    };
    return p;
}

static void start(tmx_claims_t *c, const telemetrix_profile_t *p)
{
    tmx_fake_reset();
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claims_init(c, p, &tmx_drv_fake));
    tmx_fake_reset();
}

TEST_CASE("On init: every profile pin gets its safe state through the driver, in profile order", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    tmx_fake_reset();
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claims_init(&c, &p, &tmx_drv_fake));
    TEST_ASSERT_EQUAL_STRING("gpio_output(2,0)\n"
                             "gpio_input(4,1,0)\n"
                             "gpio_input(5,0,1)\n"
                             "gpio_output(21,1)\n"
                             "gpio_input(22,0,0)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&c, 2));
}

TEST_CASE("On a free pin with the capability the use needs: claims it, returns ESP_OK", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    start(&c, &p);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_pin(&c, 2, TMX_USE_OUTPUT));
    TEST_ASSERT_EQUAL(TMX_USE_OUTPUT, tmx_pin_use(&c, 2));
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_pin(&c, 5, TMX_USE_ANALOG));
    TEST_ASSERT_EQUAL(TMX_USE_ANALOG, tmx_pin_use(&c, 5));
}

TEST_CASE("On a pin lacking the capability: ESP_ERR_NOT_SUPPORTED, no claim", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    start(&c, &p);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tmx_claim_pin(&c, 4, TMX_USE_PWM));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&c, 4));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tmx_claim_pin(&c, 22, TMX_USE_OUTPUT));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tmx_claim_pin(&c, 40, TMX_USE_INPUT));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&c, 22));
}

TEST_CASE("On a pin claimed by another use: ESP_ERR_INVALID_STATE", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    start(&c, &p);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_pin(&c, 2, TMX_USE_PWM));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tmx_claim_pin(&c, 2, TMX_USE_OUTPUT));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tmx_claim_pin(&c, 2, TMX_USE_SERVO));
    TEST_ASSERT_EQUAL(TMX_USE_PWM, tmx_pin_use(&c, 2));
}

TEST_CASE("On reclaiming a digital pin for another digital use: ESP_OK", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    start(&c, &p);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_pin(&c, 4, TMX_USE_INPUT_PULLUP));
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_pin(&c, 4, TMX_USE_OUTPUT));
    TEST_ASSERT_EQUAL(TMX_USE_OUTPUT, tmx_pin_use(&c, 4));
}

TEST_CASE("On release_pin: the pin gets its safe state and is free", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    start(&c, &p);
    tmx_claim_pin(&c, 21, TMX_USE_OUTPUT);
    tmx_release_pin(&c, 21);
    TEST_ASSERT_EQUAL_STRING("gpio_output(21,1)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&c, 21));
}

TEST_CASE("On use names: the strings of the status report", "[claims]")
{
    TEST_ASSERT_EQUAL_STRING("input_pullup", tmx_use_name(TMX_USE_INPUT_PULLUP));
    TEST_ASSERT_EQUAL_STRING("one_wire", tmx_use_name(TMX_USE_ONE_WIRE));
    TEST_ASSERT_EQUAL_STRING("sonar", tmx_use_name(TMX_USE_SONAR_ECHO));
    TEST_ASSERT_EQUAL_STRING("dht", tmx_use_name(TMX_USE_DHT));
}

TEST_CASE("On a request for (1000 Hz, 8 bit) twice: two channels, one shared timer", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    uint8_t ch0, t0, ch1, t1;
    start(&c, &p);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_ledc(&c, 1000, 8, &ch0, &t0));
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_ledc(&c, 1000, 8, &ch1, &t1));
    TEST_ASSERT_EQUAL(0, ch0);
    TEST_ASSERT_EQUAL(1, ch1);
    TEST_ASSERT_EQUAL(t0, t1);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("ledc_timer"));
    TEST_ASSERT_EQUAL(0, tmx_fake_count("ledc_attach"));
}

TEST_CASE("On a fifth distinct (frequency, resolution) with 4 timers granted: ESP_ERR_NOT_FOUND, nothing claimed",
          "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    uint8_t ch, t;
    start(&c, &p);
    for (uint32_t f = 1; f <= 4; f++) {
        TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_ledc(&c, f * 100, 8, &ch, &t));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, tmx_claim_ledc(&c, 500, 8, &ch, &t));
    TEST_ASSERT_EQUAL(4, tmx_fake_count("ledc_timer"));
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_ledc(&c, 100, 8, &ch, &t));
    TEST_ASSERT_EQUAL(4, ch);
}

TEST_CASE("On no free channel: ESP_ERR_NOT_FOUND and the driver is not touched", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    uint8_t ch, t;
    p.ledc.channel_mask = 0x01;
    start(&c, &p);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_ledc(&c, 100, 8, &ch, &t));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, tmx_claim_ledc(&c, 200, 8, &ch, &t));
    TEST_ASSERT_EQUAL(1, tmx_fake_count("ledc_timer"));
}

TEST_CASE("On release_ledc of the last channel of a timer: channel stopped, timer freed", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    uint8_t ch0, ch1, t;
    start(&c, &p);
    tmx_claim_ledc(&c, 1000, 8, &ch0, &t);
    tmx_claim_ledc(&c, 1000, 8, &ch1, &t);
    tmx_fake_reset();
    tmx_release_ledc(&c, ch0);
    TEST_ASSERT_EQUAL_STRING("ledc_stop(0,0)\n", tmx_fake_calls());
    tmx_release_ledc(&c, ch1);
    TEST_ASSERT_EQUAL_STRING("ledc_stop(0,0)\nledc_stop(0,1)\nledc_timer_free(0,0)\n", tmx_fake_calls());
}

TEST_CASE("On RMT RX claims beyond the grant: ESP_ERR_NOT_FOUND until one is released", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    start(&c, &p);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_rmt_rx(&c));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, tmx_claim_rmt_rx(&c));
    tmx_release_rmt_rx(&c);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_rmt_rx(&c));
}

TEST_CASE("On release: every claimed pin gets its safe state through the driver, in profile order", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    start(&c, &p);
    tmx_claim_pin(&c, 21, TMX_USE_OUTPUT);
    tmx_claim_pin(&c, 4, TMX_USE_INPUT);
    tmx_claim_pin(&c, 2, TMX_USE_OUTPUT);
    tmx_claims_release_all(&c);
    TEST_ASSERT_EQUAL_STRING("gpio_output(2,0)\ngpio_input(4,1,0)\ngpio_output(21,1)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&c, 21));
}

TEST_CASE("On release: every LEDC channel stopped, every timer freed", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    uint8_t ch, t;
    start(&c, &p);
    tmx_claim_ledc(&c, 1000, 8, &ch, &t);
    tmx_claim_ledc(&c, 50, 14, &ch, &t);
    tmx_claim_ledc(&c, 1000, 8, &ch, &t);
    tmx_fake_reset();
    tmx_claims_release_all(&c);
    TEST_ASSERT_EQUAL_STRING("ledc_stop(0,0)\nledc_stop(0,1)\nledc_timer_free(0,1)\n"
                             "ledc_stop(0,2)\nledc_timer_free(0,0)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_rmt_rx(&c));
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_ledc(&c, 1000, 8, &ch, &t));
    TEST_ASSERT_EQUAL(0, ch);
}

TEST_CASE("On release: RMT counts reset", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    start(&c, &p);
    tmx_claim_rmt_rx(&c);
    tmx_claims_release_all(&c);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_rmt_rx(&c));
}

TEST_CASE("On release: shared (application-owned) I2C and ADC handles untouched", "[claims]")
{
    telemetrix_profile_t p = profile();
    tmx_claims_t c;
    start(&c, &p);
    tmx_claim_pin(&c, 21, TMX_USE_I2C);
    tmx_claim_pin(&c, 5, TMX_USE_ANALOG);
    tmx_claims_release_all(&c);
    TEST_ASSERT_EQUAL(0, tmx_fake_count("i2c_close"));
    TEST_ASSERT_EQUAL(0, tmx_fake_count("ow_close"));
    TEST_ASSERT_EQUAL(0, tmx_fake_count("adc_read"));
    TEST_ASSERT_EQUAL_STRING("gpio_input(5,0,1)\ngpio_output(21,1)\n", tmx_fake_calls());
}
