#include "adapter_fixture.h"

#define BUS_UNKNOWN TELEMETRIX_REASON_BUS_UNKNOWN
#define BAD_ARG TELEMETRIX_REASON_BAD_ARGUMENT
#define BUSY TELEMETRIX_REASON_PIN_BUSY
#define R_NONE TELEMETRIX_REASON_NONE

#define MULTI_PINS 10

/* 17..22 carry I2C, 23..26 carry 1-wire. */
static const telemetrix_pin_t multi_pins[MULTI_PINS] = {
    PIN(17, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(18, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(19, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(20, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(21, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(22, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(23, DIGITAL_CAPS | TELEMETRIX_CAP_ONE_WIRE, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(24, DIGITAL_CAPS | TELEMETRIX_CAP_ONE_WIRE, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(25, DIGITAL_CAPS | TELEMETRIX_CAP_ONE_WIRE, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(26, DIGITAL_CAPS | TELEMETRIX_CAP_ONE_WIRE, TELEMETRIX_SAFE_FLOAT, -1, -1),
};

static const telemetrix_i2c_bus_t two_i2c[] = {
    {.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000},
    {.bus = 1, .port = 1, .sda = -1, .scl = -1, .speed_hz = 400000},
};
static const telemetrix_onewire_bus_t two_ow[] = {{.bus = 0, .gpio = 23}, {.bus = 1, .gpio = -1}};

static void start_multi(const telemetrix_i2c_bus_t *i2c, size_t n_i2c, const telemetrix_onewire_bus_t *ow,
                        size_t n_ow)
{
    fx_start();
    fx_profile.pins = multi_pins;
    fx_profile.n_pins = MULTI_PINS;
    fx_profile.n_adc = 0;
    fx_profile.rmt.rx_channels = 4;
    fx_profile.rmt.tx_channels = 4;
    fx_profile.i2c = i2c;
    fx_profile.n_i2c = n_i2c;
    fx_profile.onewire = ow;
    fx_profile.n_onewire = n_ow;
    telemetrix_config_default(&fx_cfg);
    tmx_fake_reset();
    fx_n_reports = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_adapter_init(&fx_a, &fx_profile, &fx_cfg, &tmx_drv_fake, fx_emit, NULL));
    tmx_fake_reset();
}

#define I2C(bus, id, ...) \
    ({ \
        static const uint8_t p_[] = {__VA_ARGS__}; \
        tmx_i2c_command(&fx_a, (bus), (id), p_, sizeof(p_)); \
    })
#define OW(bus, id, ...) \
    ({ \
        static const uint8_t p_[] = {__VA_ARGS__}; \
        tmx_onewire_command(&fx_a, (bus), (id), p_, sizeof(p_)); \
    })

TEST_CASE("On a new adapter: on_bus is -1", "[adapter]")
{
    start_multi(two_i2c, 2, two_ow, 2);
    TEST_ASSERT_EQUAL(-1, fx_a.on_bus);
}

TEST_CASE("On begin, write, read on bus 1 while bus 0 is open: the driver calls carry bus 1, bus 0 untouched", "[adapter]")
{
    start_multi(two_i2c, 2, NULL, 0);
    TEST_ASSERT_EQUAL(R_NONE, I2C(0, TMX_CMD_I2C_BEGIN, 0));
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_BEGIN, 19, 20));
    static const uint8_t regs[] = {0xAA, 0xBB, 0xCC};
    tmx_fake_reset();
    tmx_fake_i2c_device(1, 0x40, regs, sizeof(regs));
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_WRITE, 1, 0x40, 0x01));
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_READ, 0x40, 0, 2, 1));
    TEST_ASSERT_EQUAL_STRING("i2c_write(1,64,1,50)\ni2c_read(1,64,0,1,2,50)\n", tmx_fake_calls());
}

TEST_CASE("On begin on bus 1: its own pins are claimed and bus 0 is not opened", "[adapter]")
{
    start_multi(two_i2c, 2, NULL, 0);
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_BEGIN, 19, 20));
    TEST_ASSERT_EQUAL_STRING("i2c_open(1,19,20)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_I2C, tmx_pin_use(&fx_a.claims, 19));
    TEST_ASSERT_EQUAL(TMX_USE_I2C, tmx_pin_use(&fx_a.claims, 20));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 21));
    TEST_ASSERT_EQUAL(BAD_ARG, I2C(0, TMX_CMD_I2C_WRITE, 1, 0x40, 0x01));   /* bus 0 is not open */
}

TEST_CASE("On begin on bus 1 without pins while bus 0 is fixed: BAD_ARGUMENT", "[adapter]")
{
    start_multi(two_i2c, 2, NULL, 0);
    TEST_ASSERT_EQUAL(R_NONE, I2C(0, TMX_CMD_I2C_BEGIN, 0));
    TEST_ASSERT_EQUAL(BAD_ARG, I2C(1, TMX_CMD_I2C_BEGIN, 0, 0));
    TEST_ASSERT_EQUAL(BAD_ARG, tmx_i2c_command(&fx_a, 1, TMX_CMD_I2C_BEGIN, NULL, 0));
}

TEST_CASE("On begin on bus 1 with a pin that bus 0 holds: PIN_BUSY", "[adapter]")
{
    start_multi(two_i2c, 2, NULL, 0);
    TEST_ASSERT_EQUAL(R_NONE, I2C(0, TMX_CMD_I2C_BEGIN, 0));
    TEST_ASSERT_EQUAL(BUSY, I2C(1, TMX_CMD_I2C_BEGIN, 21, 20));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 20));
}

TEST_CASE("On a second begin on an open bus 1: no-op success", "[adapter]")
{
    start_multi(two_i2c, 2, NULL, 0);
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_BEGIN, 19, 20));
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_BEGIN, 17, 18));
    TEST_ASSERT_EQUAL(1, tmx_fake_count("i2c_open"));
}

TEST_CASE("On an I2C command for a bus not in the profile: BUS_UNKNOWN, no driver call", "[adapter]")
{
    start_multi(two_i2c, 2, NULL, 0);
    TEST_ASSERT_EQUAL(BUS_UNKNOWN, I2C(2, TMX_CMD_I2C_BEGIN, 17, 18));
    TEST_ASSERT_EQUAL(BUS_UNKNOWN, I2C(3, TMX_CMD_I2C_READ, 0x40, 0, 2, 1));
    TEST_ASSERT_EQUAL(BUS_UNKNOWN, I2C(255, TMX_CMD_I2C_WRITE, 1, 0x40, 0x01));
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("On release with I2C buses 0 and 1 open: both closed", "[adapter]")
{
    start_multi(two_i2c, 2, NULL, 0);
    TEST_ASSERT_EQUAL(R_NONE, I2C(0, TMX_CMD_I2C_BEGIN, 0));
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_BEGIN, 19, 20));
    tmx_fake_reset();
    tmx_adapter_release(&fx_a);
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "i2c_close(0)"));
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "i2c_close(1)"));
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_BEGIN, 19, 20));   /* reopens: the state was cleared */
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "i2c_open(1,19,20)"));
}

TEST_CASE("On release with a shared bus open on bus 1: only that bus is detached, no pin touched", "[adapter]")
{
    static int app;
    static const telemetrix_i2c_bus_t shared[] = {
        {.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000},
        {.bus = 1, .port = 1, .sda = 31, .scl = 32, .speed_hz = 100000, .handle = &app},
    };
    start_multi(shared, 2, NULL, 0);
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_BEGIN, 0));
    tmx_fake_reset();
    tmx_adapter_release(&fx_a);
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "i2c_close(1)"));
    TEST_ASSERT_NULL(strstr(tmx_fake_calls(), "i2c_close(0)"));
}

TEST_CASE("On stock dispatch of I2C_BEGIN: acts on bus 0 even when bus 1 exists", "[adapter]")
{
    start_multi(two_i2c, 2, NULL, 0);
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    TEST_ASSERT_EQUAL_STRING("i2c_open(0,21,22)\n", tmx_fake_calls());
}

TEST_CASE("On search on bus 1 after a search on bus 0: each bus keeps its own search state", "[adapter]")
{
    start_multi(NULL, 0, two_ow, 2);
    static const uint8_t roms0[2][8] = {{0x28, 1, 2, 3, 4, 5, 6, 7}, {0x28, 9, 9, 9, 9, 9, 9, 9}};
    static const uint8_t roms1[1][8] = {{0x10, 8, 8, 8, 8, 8, 8, 8}};
    tmx_fake_ow_devices(0, roms0, 2);
    tmx_fake_ow_devices(1, roms1, 1);
    TEST_ASSERT_EQUAL(R_NONE, OW(0, TMX_CMD_ONE_WIRE_INIT, 0));
    TEST_ASSERT_EQUAL(R_NONE, OW(1, TMX_CMD_ONE_WIRE_INIT, 24));
    TEST_ASSERT_EQUAL(R_NONE, OW(0, TMX_CMD_ONE_WIRE_SEARCH, 0));
    TEST_ASSERT_EQUAL(R_NONE, OW(1, TMX_CMD_ONE_WIRE_SEARCH, 0));
    TEST_ASSERT_EQUAL(R_NONE, OW(0, TMX_CMD_ONE_WIRE_SEARCH, 0));
    TEST_ASSERT_EQUAL(3, fx_n_reports);
    uint8_t want[TMX_REPORT_MAX];
    size_t n = tmx_rpt_onewire(want, TMX_CMD_ONE_WIRE_SEARCH, roms0[0], 8);
    fx_assert_report(0, want, n);
    n = tmx_rpt_onewire(want, TMX_CMD_ONE_WIRE_SEARCH, roms1[0], 8);
    fx_assert_report(1, want, n);
    n = tmx_rpt_onewire(want, TMX_CMD_ONE_WIRE_SEARCH, roms0[1], 8);
    fx_assert_report(2, want, n);
}

TEST_CASE("On 1-wire init on bus 1: the driver call carries bus 1 and its own pin and RMT claim", "[adapter]")
{
    start_multi(NULL, 0, two_ow, 2);
    TEST_ASSERT_EQUAL(R_NONE, OW(1, TMX_CMD_ONE_WIRE_INIT, 24));
    TEST_ASSERT_EQUAL_STRING("ow_open(1,24)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_ONE_WIRE, tmx_pin_use(&fx_a.claims, 24));
    TEST_ASSERT_EQUAL(1, fx_a.claims.rmt_rx_used);
    TEST_ASSERT_EQUAL(BAD_ARG, OW(0, TMX_CMD_ONE_WIRE_RESET));
}

TEST_CASE("On a 1-wire command for a bus not in the profile: BUS_UNKNOWN, no driver call", "[adapter]")
{
    start_multi(NULL, 0, two_ow, 2);
    TEST_ASSERT_EQUAL(BUS_UNKNOWN, OW(2, TMX_CMD_ONE_WIRE_INIT, 25));
    TEST_ASSERT_EQUAL(BUS_UNKNOWN, OW(2, TMX_CMD_ONE_WIRE_RESET));
    TEST_ASSERT_EQUAL(BUS_UNKNOWN, OW(9, TMX_CMD_ONE_WIRE_CRC8, 1, 0x01));
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("On release with 1-wire buses 0 and 1 open: both closed, every RMT claim returned", "[adapter]")
{
    start_multi(NULL, 0, two_ow, 2);
    TEST_ASSERT_EQUAL(R_NONE, OW(0, TMX_CMD_ONE_WIRE_INIT, 0));
    TEST_ASSERT_EQUAL(R_NONE, OW(1, TMX_CMD_ONE_WIRE_INIT, 24));
    tmx_fake_reset();
    tmx_adapter_release(&fx_a);
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "ow_close(0)"));
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "ow_close(1)"));
    TEST_ASSERT_EQUAL(0, fx_a.claims.rmt_rx_used);
}

TEST_CASE("On a reply while on_bus is 1: it is wrapped as a BUS_REPORT of bus 1", "[adapter]")
{
    start_multi(two_i2c, 2, NULL, 0);
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_BEGIN, 19, 20));
    static const uint8_t regs[] = {0xAA, 0xBB};
    tmx_fake_i2c_device(1, 0x40, regs, sizeof(regs));
    fx_a.on_bus = 1;
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_READ, 0x40, 0, 2, 1));
    uint8_t stock[TMX_REPORT_MAX];
    uint8_t want[TMX_REPORT_MAX];
    size_t n = tmx_rpt_i2c_read(stock, 0x40, 0, regs, 2);
    size_t m = tmx_rpt_bus_wrap(want, 1, stock, n);
    TEST_ASSERT_TRUE(m > 0);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, m);
    TEST_ASSERT_TRUE(fx_reports[0].reply);
}

TEST_CASE("On a reply while on_bus is -1: the stock report unchanged", "[adapter]")
{
    start_multi(two_i2c, 2, NULL, 0);
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_BEGIN, 19, 20));
    static const uint8_t regs[] = {0xAA, 0xBB};
    tmx_fake_i2c_device(1, 0x40, regs, sizeof(regs));
    TEST_ASSERT_EQUAL(R_NONE, I2C(1, TMX_CMD_I2C_READ, 0x40, 0, 2, 1));
    uint8_t want[TMX_REPORT_MAX];
    size_t n = tmx_rpt_i2c_read(want, 0x40, 0, regs, 2);
    fx_assert_report(0, want, n);
}

static telemetrix_reason_t validate_buses(size_t n_i2c, size_t n_ow)
{
    static telemetrix_i2c_bus_t i2c[5];
    static telemetrix_onewire_bus_t ow[5];
    for (uint8_t i = 0; i < 5; i++) {
        i2c[i] = (telemetrix_i2c_bus_t) {.bus = i, .sda = -1, .scl = -1, .speed_hz = 100000};
        ow[i] = (telemetrix_onewire_bus_t) {.bus = i, .gpio = -1};
    }
    telemetrix_profile_t p = {
        .pins = multi_pins, .n_pins = MULTI_PINS, .i2c = i2c, .n_i2c = n_i2c, .onewire = ow, .n_onewire = n_ow,
    };
    return tmx_profile_validate(&p);
}

TEST_CASE("On 4 I2C buses and 4 1-wire buses: ESP_OK", "[adapter]")
{
    TEST_ASSERT_EQUAL(ESP_OK, validate_buses(4, 4));
}

TEST_CASE("On more than 4 I2C buses: ESP_ERR_INVALID_ARG", "[adapter]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, validate_buses(5, 0));
}

TEST_CASE("On more than 4 1-wire buses: ESP_ERR_INVALID_ARG", "[adapter]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, validate_buses(0, 5));
}
