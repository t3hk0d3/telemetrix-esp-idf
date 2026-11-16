#include <stdio.h>

#include "adapter_fixture.h"

static const telemetrix_i2c_bus_t fixed_bus = {.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000};
static const telemetrix_i2c_bus_t choose_bus = {.bus = 0, .port = 0, .sda = -1, .scl = -1, .speed_hz = 100000};
static int app_bus;     /* stands in for an application-owned i2c_master_bus_handle_t */
/* Shared with the application: its pins (25, 26) are not profile pins. */
static const telemetrix_i2c_bus_t shared_bus = {
    .bus = 0, .port = 0, .sda = 25, .scl = 26, .speed_hz = 100000, .handle = &app_bus
};

static void start_open_fixed(void)
{
    fx_start_buses(&fixed_bus, NULL);
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    TEST_ASSERT_EQUAL(0, fx_refused());
}

TEST_CASE("I2C_READ before begin: refused_commands +1, no driver call", "[adapter]")
{
    fx_start_buses(&fixed_bus, NULL);
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x76, 0xD0, 3, 1);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("I2C_WRITE before begin: refused_commands +1, no driver call", "[adapter]")
{
    fx_start_buses(&fixed_bus, NULL);
    FX_SEND(TMX_CMD_I2C_WRITE, 0, 1, 0x40, 0xAA);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("i2c_begin() with no pins on a fixed bus 0: bus 0 opened on its fixed pins", "[adapter]")
{
    fx_start_buses(&fixed_bus, NULL);
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("i2c_open(0,21,22)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_I2C, tmx_pin_use(&fx_a.claims, 21));
    TEST_ASSERT_EQUAL(TMX_USE_I2C, tmx_pin_use(&fx_a.claims, 22));
}

TEST_CASE("I2C_BEGIN with pins on a fixed bus 0: the payload is ignored", "[adapter]")
{
    fx_start_buses(&fixed_bus, NULL);
    FX_SEND(TMX_CMD_I2C_BEGIN, 0, 12, 13);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("i2c_open(0,21,22)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
}

TEST_CASE("I2C_BEGIN with pins on a client-chooses bus 0: opened on those pins, both claimed as i2c", "[adapter]")
{
    fx_start_buses(&choose_bus, NULL);
    FX_SEND(TMX_CMD_I2C_BEGIN, 0, 22, 21);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("i2c_open(0,22,21)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_I2C, tmx_pin_use(&fx_a.claims, 21));
    TEST_ASSERT_EQUAL(TMX_USE_I2C, tmx_pin_use(&fx_a.claims, 22));
}

TEST_CASE("I2C_BEGIN [0,0] on a client-chooses bus 0: refused_commands +1, nothing opened", "[adapter]")
{
    fx_start_buses(&choose_bus, NULL);
    FX_SEND(TMX_CMD_I2C_BEGIN, 0, 0, 0);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("I2C_BEGIN with no pins on a client-chooses bus 0: refused_commands +1, nothing opened", "[adapter]")
{
    fx_start_buses(&choose_bus, NULL);
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("I2C_BEGIN on a pin without the I2C capability: refused_commands +1, nothing claimed", "[adapter]")
{
    fx_start_buses(&choose_bus, NULL);
    FX_SEND(TMX_CMD_I2C_BEGIN, 0, 21, 12);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 21));
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("I2C_BEGIN with the same pin twice: refused_commands +1", "[adapter]")
{
    fx_start_buses(&choose_bus, NULL);
    FX_SEND(TMX_CMD_I2C_BEGIN, 0, 21, 21);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("I2C_BEGIN on a pin another feature holds: refused_commands +1, no open", "[adapter]")
{
    fx_start_buses(&fixed_bus, NULL);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 22, TMX_MODE_OUTPUT);
    tmx_fake_reset();
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(0, tmx_fake_count("i2c_open"));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 21));
    TEST_ASSERT_EQUAL(TMX_USE_OUTPUT, tmx_pin_use(&fx_a.claims, 22));
}

TEST_CASE("I2C_BEGIN while bus 0 is open: no-op success, no second open", "[adapter]")
{
    start_open_fixed();
    tmx_fake_reset();
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("I2C_BEGIN with no bus 0 in the profile: refused_commands +1", "[adapter]")
{
    fx_start();
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("I2C_BEGIN when the driver cannot open: refused_commands +1, claims released, a retry works", "[adapter]")
{
    fx_start_buses(&fixed_bus, NULL);
    tmx_fake_fail_next("i2c_open", ESP_FAIL);
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 21));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 22));
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_I2C, tmx_pin_use(&fx_a.claims, 21));
}

TEST_CASE("I2C_READ of 3 bytes from 0x76 reg 0xD0: [7,9,3,0x76,0xD0,b0,b1,b2] as a reply", "[adapter]")
{
    start_open_fixed();
    uint8_t regs[256] = {0};
    regs[0xD0] = 0x58;
    regs[0xD1] = 0x11;
    regs[0xD2] = 0x22;
    tmx_fake_i2c_device(0, 0x76, regs, sizeof(regs));
    tmx_fake_reset();
    tmx_fake_i2c_device(0, 0x76, regs, sizeof(regs));
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x76, 0xD0, 3, 1);
    const uint8_t want[] = {7, 9, 3, 0x76, 0xD0, 0x58, 0x11, 0x22};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(0, fx_refused());
}

TEST_CASE("I2C_READ: the driver gets bus 0, the stop flag and cfg.i2c_timeout_ms", "[adapter]")
{
    start_open_fixed();
    fx_a.cfg.i2c_timeout_ms = 77;
    tmx_fake_reset();
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x76, 0xD0, 3, 0);
    TEST_ASSERT_EQUAL_STRING("i2c_read(0,118,208,0,3,77)\n", tmx_fake_calls());
}

TEST_CASE("I2C_READ of 200 bytes: clamped to 59", "[adapter]")
{
    start_open_fixed();
    uint8_t regs[256];
    for (int i = 0; i < 256; i++) {
        regs[i] = (uint8_t)i;
    }
    tmx_fake_i2c_device(0, 0x50, regs, sizeof(regs));
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x50, 0, 200, 1);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("i2c_read"));
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "i2c_read(0,80,0,1,59,50)"));
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    TEST_ASSERT_EQUAL(64, fx_reports[0].n);
    TEST_ASSERT_EQUAL(59, fx_reports[0].data[2]);
    TEST_ASSERT_EQUAL(58, fx_reports[0].data[5 + 58]);
}

TEST_CASE("I2C_READ the device NACKs: [3,8,1,addr] as a reply, nothing refused", "[adapter]")
{
    start_open_fixed();
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x33, 0, 2, 1);
    const uint8_t want[] = {3, 8, 1, 0x33};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(0, fx_refused());
}

TEST_CASE("I2C_READ the driver times out: [3,8,1,addr]", "[adapter]")
{
    start_open_fixed();
    tmx_fake_fail_next("i2c_read", ESP_ERR_TIMEOUT);
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x76, 0xD0, 3, 1);
    const uint8_t want[] = {3, 8, 1, 0x76};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
}

TEST_CASE("I2C_READ of 0 bytes: refused_commands +1, no driver call", "[adapter]")
{
    start_open_fixed();
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x76, 0xD0, 0, 1);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(0, tmx_fake_count("i2c_read"));
}

TEST_CASE("I2C_WRITE to a device that answers: the driver gets the address, count and timeout, no report", "[adapter]")
{
    start_open_fixed();
    uint8_t regs[4] = {0};
    tmx_fake_i2c_device(0, 0x40, regs, sizeof(regs));
    tmx_fake_reset();
    tmx_fake_i2c_device(0, 0x40, regs, sizeof(regs));
    FX_SEND(TMX_CMD_I2C_WRITE, 0, 2, 0x40, 0xAA, 0xBB);
    TEST_ASSERT_EQUAL_STRING("i2c_write(0,64,2,50)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    TEST_ASSERT_EQUAL(0, fx_refused());
}

TEST_CASE("I2C_WRITE the device NACKs: refused_commands +1 and no report", "[adapter]")
{
    start_open_fixed();
    FX_SEND(TMX_CMD_I2C_WRITE, 0, 1, 0x41, 0xAA);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("RESET with bus 0 open: i2c_close(0), pins to their safe state, a read is refused afterwards", "[adapter]")
{
    start_open_fixed();
    tmx_fake_reset();
    FX_SEND0(TMX_CMD_RESET, 0);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("i2c_close"));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 21));
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "gpio_output(21,1)"));
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "gpio_output(22,1)"));
    FX_SEND(TMX_CMD_I2C_READ, 1, 0x76, 0xD0, 3, 1);
    TEST_ASSERT_EQUAL(1, fx_refused());
}

TEST_CASE("RESET then I2C_BEGIN: the bus opens again", "[adapter]")
{
    start_open_fixed();
    FX_SEND0(TMX_CMD_RESET, 0);
    tmx_fake_reset();
    FX_SEND0(TMX_CMD_I2C_BEGIN, 1);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("i2c_open"));
}

TEST_CASE("Release with bus 0 never opened: no i2c_close", "[adapter]")
{
    fx_start_buses(&fixed_bus, NULL);
    tmx_adapter_release(&fx_a);
    TEST_ASSERT_EQUAL(0, tmx_fake_count("i2c_close"));
}

/* ---- an application-owned (shared) bus 0 ---- */

/** True when a recorded gpio_* call names gpio as its first argument. */
static bool gpio_touched(uint8_t gpio)
{
    char needle[24];
    const char *ops[] = {"gpio_input(", "gpio_output(", "gpio_set("};
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        snprintf(needle, sizeof(needle), "%s%u,", ops[i], gpio);
        if (strstr(tmx_fake_calls(), needle)) {
            return true;
        }
    }
    return false;
}

TEST_CASE("I2C_BEGIN on an application-owned bus 0: opened with the grant's handle, nothing claimed, no GPIO call",
          "[adapter]")
{
    fx_start_buses(&shared_bus, NULL);
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("i2c_open(0,25,26,app)\n", tmx_fake_calls());
    telemetrix_pin_use_t uses[4];
    TEST_ASSERT_EQUAL(0, tmx_adapter_pin_uses(&fx_a, uses, 4));
}

TEST_CASE("An application-owned bus 0 through a session: start, begin, read, write, RESET, release touch no bus pin",
          "[adapter]")
{
    static const uint8_t regs[4] = {1, 2, 3, 4};
    telemetrix_config_default(&fx_cfg);
    tmx_fake_reset();
    tmx_completion_reset();
    fx_n_reports = 0;
    fx_profile = (telemetrix_profile_t) {
        .pins = fx_pins, .n_pins = sizeof(fx_pins) / sizeof(fx_pins[0]),
        .ledc = {.speed_mode = 0, .timer_mask = 0x0f, .channel_mask = 0xff},
        .adc = fx_adc, .n_adc = 1, .i2c = &shared_bus, .n_i2c = 1,
    };
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate(&fx_profile));
    TEST_ASSERT_EQUAL(ESP_OK, tmx_adapter_init(&fx_a, &fx_profile, &fx_cfg, &tmx_drv_fake, fx_emit, NULL));  /* start */
    tmx_fake_i2c_device(0, 0x76, regs, sizeof(regs));
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x76, 0, 2, 1);
    FX_SEND(TMX_CMD_I2C_WRITE, 0, 1, 0x76, 0xAA);
    FX_SEND0(TMX_CMD_RESET, 1);
    FX_SEND0(TMX_CMD_I2C_BEGIN, 2);
    tmx_adapter_release(&fx_a);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(2, tmx_fake_count("i2c_close"));
    TEST_ASSERT_FALSE(gpio_touched(25));
    TEST_ASSERT_FALSE(gpio_touched(26));
    TEST_ASSERT_TRUE(gpio_touched(21));         /* the check does see gpio calls: start set the profile pins */
}
