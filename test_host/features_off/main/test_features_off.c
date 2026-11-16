#include "adapter_fixture.h"
#include "sdkconfig.h"

#if defined(CONFIG_TELEMETRIX_DHT) || defined(CONFIG_TELEMETRIX_SONAR) || defined(CONFIG_TELEMETRIX_ONE_WIRE) || \
    defined(CONFIG_TELEMETRIX_SERVO) || defined(CONFIG_TELEMETRIX_I2C) || defined(CONFIG_TELEMETRIX_ANALOG) || defined(CONFIG_TELEMETRIX_EXTENDED)
#error "this project must be built with every CONFIG_TELEMETRIX_* feature off (see sdkconfig.defaults)"
#endif

/* Bus 0 grants that would make every bus command succeed if it were not switched off. */
static const telemetrix_i2c_bus_t i2c_fixed = {.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000};
static const telemetrix_onewire_bus_t ow_fixed = {.bus = 0, .gpio = 23};

static void start(void)
{
    fx_start_buses(&i2c_fixed, &ow_fixed);
    fx_profile.rmt.rx_channels = 2;
}

TEST_CASE("I2C off: I2C_BEGIN, I2C_READ and I2C_WRITE are ignored and counted, no driver call, no report",
          "[features_off]")
{
    start();
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x76, 0x10, 3, 1);
    FX_SEND(TMX_CMD_I2C_WRITE, 0, 1, 0x76, 0x10);
    TEST_ASSERT_EQUAL(3, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 21));
}

TEST_CASE("Sonar off: SONAR_NEW is ignored and counted, nothing claimed, no ping ever starts", "[features_off]")
{
    start();
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    tmx_adapter_poll(&fx_a, 0);
    tmx_adapter_poll(&fx_a, 100);
    TEST_ASSERT_EQUAL(1, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 13));
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("DHT off: DHT_NEW is ignored and counted, nothing claimed, no read ever starts", "[features_off]")
{
    start();
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    tmx_adapter_poll(&fx_a, 0);
    tmx_adapter_poll(&fx_a, 2000);
    TEST_ASSERT_EQUAL(1, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 14));
    TEST_ASSERT_EQUAL(0, fx_a.claims.rmt_rx_used);
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("1-wire off: ids 27-35 are each ignored and counted, no driver call, no report", "[features_off]")
{
    start();
    FX_SEND(TMX_CMD_ONE_WIRE_INIT, 0, 23);
    FX_SEND0(TMX_CMD_ONE_WIRE_RESET, 0);
    FX_SEND(TMX_CMD_ONE_WIRE_SELECT, 0, 1, 2, 3, 4, 5, 6, 7, 8);
    FX_SEND0(TMX_CMD_ONE_WIRE_SKIP, 0);
    FX_SEND(TMX_CMD_ONE_WIRE_WRITE, 0, 0x44, 1);
    FX_SEND0(TMX_CMD_ONE_WIRE_READ, 0);
    FX_SEND0(TMX_CMD_ONE_WIRE_RESET_SEARCH, 0);
    FX_SEND0(TMX_CMD_ONE_WIRE_SEARCH, 0);
    FX_SEND(TMX_CMD_ONE_WIRE_CRC8, 0, 2, 0x10, 0x20, 0x00);
    TEST_ASSERT_EQUAL(9, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 23));
}

TEST_CASE("Analog off: SET_PIN_MODE analog is ignored and counted, the pin is not claimed", "[features_off]")
{
    start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, TMX_MODE_ANALOG, 0, 0, 1);
    TEST_ASSERT_EQUAL(1, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 34));
}

TEST_CASE("Servo off: SERVO_ATTACH, SERVO_WRITE and SERVO_DETACH are ignored and counted, no driver call",
          "[features_off]")
{
    start();
    FX_SEND(TMX_CMD_SERVO_ATTACH, 0, 12, 0x02, 0x20, 0x09, 0x60);
    FX_SEND(TMX_CMD_SERVO_WRITE, 0, 12, 90);
    FX_SEND(TMX_CMD_SERVO_DETACH, 0, 12);
    TEST_ASSERT_EQUAL(3, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("Every feature off: LOOP and digital output still work", "[features_off]")
{
    start();
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 'B');
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, TMX_MODE_OUTPUT);
    FX_SEND(TMX_CMD_DIGITAL_WRITE, 0, 12, 1);
    const uint8_t loop[] = {2, 0, 'B'};
    fx_assert_report(0, loop, sizeof(loop));
    TEST_ASSERT_EQUAL(0, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "gpio_set(12,1)"));
}

TEST_CASE("Extended off: the handshake probe gets the stock answer and the session stays stock", "[features_off]")
{
    const uint8_t want[] = {4, TMX_RPT_FIRMWARE_REPORT, 3, 2, 100};
    start();
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 0);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_FALSE(tmx_adapter_extended(&fx_a));
    TEST_ASSERT_FALSE(tmx_adapter_mode_changed(&fx_a));
}
