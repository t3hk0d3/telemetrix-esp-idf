#include "adapter_fixture.h"

#define ERR(seq, cmd, why) 5, TMX_RPT_ERROR, (uint8_t)((seq) >> 8), (uint8_t)(seq), (cmd), (why)
#define SYNC_REPLY(tag, seq) 5, TMX_RPT_SYNC_REPLY, (uint8_t)((tag) >> 8), (uint8_t)(tag), (uint8_t)((seq) >> 8), (uint8_t)(seq)

/* 17..22 carry I2C, 23..26 carry 1-wire. */
static const telemetrix_pin_t pins[] = {
    PIN(17, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(18, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(19, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(20, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(21, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(22, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(23, DIGITAL_CAPS | TELEMETRIX_CAP_ONE_WIRE, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(24, DIGITAL_CAPS | TELEMETRIX_CAP_ONE_WIRE, TELEMETRIX_SAFE_FLOAT, -1, -1),
};
static const telemetrix_i2c_bus_t i2c_buses[] = {
    {.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000},
    {.bus = 1, .port = 1, .sda = -1, .scl = -1, .speed_hz = 400000},
};
static const telemetrix_onewire_bus_t ow_buses[] = {{.bus = 0, .gpio = 23}, {.bus = 1, .gpio = -1}};
static const uint8_t regs[] = {0xAA, 0xBB, 0xCC};

/** An extended-mode adapter with I2C buses 0 and 1 and 1-wire buses 0 and 1; no report pending. */
static void start_ext(void)
{
    fx_start();
    fx_profile.pins = pins;
    fx_profile.n_pins = sizeof(pins) / sizeof(pins[0]);
    fx_profile.n_adc = 0;
    fx_profile.rmt.rx_channels = 4;
    fx_profile.rmt.tx_channels = 4;
    fx_profile.i2c = i2c_buses;
    fx_profile.n_i2c = 2;
    fx_profile.onewire = ow_buses;
    fx_profile.n_onewire = 2;
    telemetrix_config_default(&fx_cfg);
    tmx_fake_reset();
    fx_n_reports = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_adapter_init(&fx_a, &fx_profile, &fx_cfg, &tmx_drv_fake, fx_emit, NULL));
    tmx_fake_reset();
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 0);
    TEST_ASSERT_TRUE(tmx_adapter_extended(&fx_a));
    fx_n_reports = 0;
    fx_a.session.seq = 0;
}

static void send_refused(void)
{
    FX_SEND(TMX_CMD_DIGITAL_WRITE, 0, 99, 1);       /* not a profile pin */
}

/* ---- SYNC ---- */

TEST_CASE("SYNC [tag, 1] with no refusal: SYNC_REPLY [tag, own seq] queued as a reply", "[sync]")
{
    start_ext();
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 1);
    FX_SEND(TMX_CMD_SYNC, 0, 0x12, 0x34, 1);
    const uint8_t want[] = {SYNC_REPLY(0x1234, 1)};
    TEST_ASSERT_EQUAL(2, fx_n_reports);
    fx_assert_report(1, want, sizeof(want));
    TEST_ASSERT_TRUE(fx_reports[1].reply);
    TEST_ASSERT_EQUAL(0, fx_a.session.error_reports);
}

TEST_CASE("SYNC after an I2C read in the same chunk: SYNC_REPLY queued after the read's reply", "[sync]")
{
    start_ext();
    tmx_fake_i2c_device(0, 0x40, regs, sizeof(regs));
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x40, 0, 2, 1);
    FX_SEND(TMX_CMD_SYNC, 0, 0, 9, 1);
    uint8_t read[TMX_REPORT_MAX];
    size_t n = tmx_rpt_i2c_read(read, 0x40, 0, regs, 2);
    const uint8_t sync[] = {SYNC_REPLY(9, 2)};
    TEST_ASSERT_EQUAL(2, fx_n_reports);
    fx_assert_report(0, read, n);
    fx_assert_report(1, sync, sizeof(sync));
}

TEST_CASE("abort on and command 2 of 4 refused: 3 and 4 skipped, SYNC never skipped and replies", "[sync]")
{
    start_ext();
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 1);
    send_refused();
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 2);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, TMX_MODE_OUTPUT);
    FX_SEND(TMX_CMD_SYNC, 0, 0, 5, 1);
    const uint8_t e2[] = {ERR(2, TMX_CMD_LOOP_COMMAND, TELEMETRIX_REASON_SKIPPED)};
    const uint8_t e3[] = {ERR(3, TMX_CMD_SET_PIN_MODE, TELEMETRIX_REASON_SKIPPED)};
    const uint8_t sync[] = {SYNC_REPLY(5, 4)};
    TEST_ASSERT_EQUAL(5, fx_n_reports);
    fx_assert_report(2, e2, sizeof(e2));
    fx_assert_report(3, e3, sizeof(e3));
    fx_assert_report(4, sync, sizeof(sync));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
}

TEST_CASE("SYNC after a skipped run: skipping is cleared and the next command executes", "[sync]")
{
    start_ext();
    send_refused();
    FX_SEND(TMX_CMD_SYNC, 0, 0, 1, 1);
    TEST_ASSERT_FALSE(fx_a.session.skipping);
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 7);
    const uint8_t loop[] = {2, 0, 7};
    TEST_ASSERT_EQUAL(3, fx_n_reports);
    fx_assert_report(2, loop, sizeof(loop));
}

TEST_CASE("SYNC flags 0 then a refused command: later commands still execute", "[sync]")
{
    start_ext();
    FX_SEND(TMX_CMD_SYNC, 0, 0, 1, 0);
    TEST_ASSERT_FALSE(fx_a.session.abort);
    fx_n_reports = 0;
    send_refused();
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 7);
    const uint8_t loop[] = {2, 0, 7};
    TEST_ASSERT_EQUAL(2, fx_n_reports);
    fx_assert_report(1, loop, sizeof(loop));
    TEST_ASSERT_FALSE(fx_a.session.skipping);
}

TEST_CASE("SYNC flags 1 after flags 0: abort is on again, the next refusal skips", "[sync]")
{
    start_ext();
    FX_SEND(TMX_CMD_SYNC, 0, 0, 1, 0);
    FX_SEND(TMX_CMD_SYNC, 0, 0, 2, 1);
    TEST_ASSERT_TRUE(fx_a.session.abort);
    fx_n_reports = 0;
    send_refused();
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 7);
    const uint8_t e[] = {ERR(3, TMX_CMD_LOOP_COMMAND, TELEMETRIX_REASON_SKIPPED)};
    TEST_ASSERT_EQUAL(2, fx_n_reports);
    fx_assert_report(1, e, sizeof(e));
}

TEST_CASE("SYNC flag bits other than bit 0 are ignored", "[sync]")
{
    start_ext();
    FX_SEND(TMX_CMD_SYNC, 0, 0, 1, 0xFE);
    TEST_ASSERT_FALSE(fx_a.session.abort);
}

TEST_CASE("SYNC on a session just entered: abort is on", "[sync]")
{
    start_ext();
    TEST_ASSERT_TRUE(fx_a.session.abort);
    TEST_ASSERT_FALSE(fx_a.session.skipping);
}

TEST_CASE("SYNC with a short payload: ERROR BAD_ARGUMENT, no SYNC_REPLY, abort unchanged", "[sync]")
{
    start_ext();
    FX_SEND(TMX_CMD_SYNC, 0, 0, 1, 0);          /* abort off */
    fx_n_reports = 0;
    FX_SEND(TMX_CMD_SYNC, 0, 0, 7);
    FX_SEND0(TMX_CMD_SYNC, 0);
    const uint8_t e1[] = {ERR(1, TMX_CMD_SYNC, TELEMETRIX_REASON_BAD_ARGUMENT)};
    const uint8_t e2[] = {ERR(2, TMX_CMD_SYNC, TELEMETRIX_REASON_BAD_ARGUMENT)};
    TEST_ASSERT_EQUAL(2, fx_n_reports);
    fx_assert_report(0, e1, sizeof(e1));
    fx_assert_report(1, e2, sizeof(e2));
    TEST_ASSERT_FALSE(fx_a.session.abort);
}

TEST_CASE("SYNC with a short payload while skipping: refused, not skipped, skipping unchanged", "[sync]")
{
    start_ext();
    send_refused();
    fx_n_reports = 0;
    FX_SEND(TMX_CMD_SYNC, 0, 0, 7);
    const uint8_t e[] = {ERR(1, TMX_CMD_SYNC, TELEMETRIX_REASON_BAD_ARGUMENT)};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, e, sizeof(e));
    TEST_ASSERT_TRUE(fx_a.session.skipping);
}

TEST_CASE("SYNC with a short payload while not skipping: abort on does not start skipping", "[sync]")
{
    start_ext();
    FX_SEND(TMX_CMD_SYNC, 0, 7);
    TEST_ASSERT_FALSE(fx_a.session.skipping);
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 3);
    const uint8_t loop[] = {2, 0, 3};
    fx_assert_report(1, loop, sizeof(loop));
}

TEST_CASE("SYNC is numbered: the next command gets the next seq", "[sync]")
{
    start_ext();
    FX_SEND(TMX_CMD_SYNC, 0, 0, 1, 1);
    send_refused();
    const uint8_t e[] = {ERR(1, TMX_CMD_DIGITAL_WRITE, TELEMETRIX_REASON_NO_SUCH_PIN)};
    fx_assert_report(1, e, sizeof(e));
}

TEST_CASE("re-handshake keeps error_reports: a lifetime counter", "[sync]")
{
    start_ext();
    send_refused();
    TEST_ASSERT_EQUAL(1, fx_a.session.error_reports);
    tmx_adapter_session_end(&fx_a);
    TEST_ASSERT_EQUAL(1, fx_a.session.error_reports);
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 0);     /* the handshake resets the session */
    TEST_ASSERT_TRUE(tmx_adapter_extended(&fx_a));
    TEST_ASSERT_EQUAL(1, fx_a.session.error_reports);
}

TEST_CASE("SYNC in stock mode: ignored and counted, no reply", "[sync]")
{
    fx_start();
    FX_SEND(TMX_CMD_SYNC, 0, 0, 1, 1);
    TEST_ASSERT_EQUAL(1, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

/* ---- ON_BUS ---- */

TEST_CASE("ON_BUS [1, I2C_READ] with a device on bus 1: BUS_REPORT [1, 9, count, addr, reg, data]", "[on_bus]")
{
    start_ext();
    FX_SEND(TMX_CMD_ON_BUS, 0, 1, TMX_CMD_I2C_BEGIN, 19, 20);
    tmx_fake_i2c_device(1, 0x40, regs, sizeof(regs));
    FX_SEND(TMX_CMD_ON_BUS, 0, 1, TMX_CMD_I2C_READ, 0x40, 0, 2, 1);
    uint8_t stock[TMX_REPORT_MAX];
    uint8_t want[TMX_REPORT_MAX];
    size_t n = tmx_rpt_i2c_read(stock, 0x40, 0, regs, 2);
    size_t m = tmx_rpt_bus_wrap(want, 1, stock, n);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, m);
    TEST_ASSERT_TRUE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(0, fx_a.session.error_reports);
}

TEST_CASE("ON_BUS success: on_bus is back at -1", "[on_bus]")
{
    start_ext();
    FX_SEND(TMX_CMD_ON_BUS, 0, 1, TMX_CMD_I2C_BEGIN, 19, 20);
    TEST_ASSERT_EQUAL(-1, fx_a.on_bus);
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 4);
    const uint8_t loop[] = {2, 0, 4};
    fx_assert_report(0, loop, sizeof(loop));
}

TEST_CASE("ON_BUS on a bus not in the profile: ERROR BUS_UNKNOWN with cmd 58, on_bus -1", "[on_bus]")
{
    start_ext();
    FX_SEND(TMX_CMD_ON_BUS, 0, 5, TMX_CMD_I2C_BEGIN);
    FX_SEND(TMX_CMD_SYNC, 0, 0, 1, 0);
    FX_SEND(TMX_CMD_ON_BUS, 0, 5, TMX_CMD_ONE_WIRE_RESET);
    const uint8_t e0[] = {ERR(0, TMX_CMD_ON_BUS, TELEMETRIX_REASON_BUS_UNKNOWN)};
    const uint8_t e2[] = {ERR(2, TMX_CMD_ON_BUS, TELEMETRIX_REASON_BUS_UNKNOWN)};
    fx_assert_report(0, e0, sizeof(e0));
    fx_assert_report(2, e2, sizeof(e2));
    TEST_ASSERT_EQUAL(-1, fx_a.on_bus);
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("ON_BUS ONE_WIRE_CRC8 on a bus not in the profile: BUS_UNKNOWN, no report", "[on_bus]")
{
    start_ext();
    FX_SEND(TMX_CMD_ON_BUS, 0, 5, TMX_CMD_ONE_WIRE_CRC8, 1, 0x02, 0);
    const uint8_t e[] = {ERR(0, TMX_CMD_ON_BUS, TELEMETRIX_REASON_BUS_UNKNOWN)};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, e, sizeof(e));
}

TEST_CASE("ON_BUS with an inner id outside 9-11 and 27-35: ERROR BAD_ARGUMENT, nothing executed", "[on_bus]")
{
    static const uint8_t ids[] = {0, 8, 12, 26, 36, 57, 58, 60, 255};
    for (size_t i = 0; i < sizeof(ids); i++) {
        start_ext();
        const uint8_t p[] = {0, ids[i], 7};
        fx_send(TMX_CMD_ON_BUS, p, sizeof(p), 0);
        const uint8_t e[] = {ERR(0, TMX_CMD_ON_BUS, TELEMETRIX_REASON_BAD_ARGUMENT)};
        TEST_ASSERT_EQUAL(1, fx_n_reports);
        fx_assert_report(0, e, sizeof(e));
        TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
    }
}

TEST_CASE("ON_BUS with fewer than 2 payload bytes: ERROR BAD_ARGUMENT", "[on_bus]")
{
    start_ext();
    FX_SEND0(TMX_CMD_ON_BUS, 0);
    FX_SEND(TMX_CMD_SYNC, 0, 0, 1, 0);
    FX_SEND(TMX_CMD_ON_BUS, 0, 1);
    const uint8_t e0[] = {ERR(0, TMX_CMD_ON_BUS, TELEMETRIX_REASON_BAD_ARGUMENT)};
    const uint8_t e2[] = {ERR(2, TMX_CMD_ON_BUS, TELEMETRIX_REASON_BAD_ARGUMENT)};
    fx_assert_report(0, e0, sizeof(e0));
    fx_assert_report(2, e2, sizeof(e2));
}

TEST_CASE("ON_BUS with a short inner payload: ERROR BAD_ARGUMENT, no driver call", "[on_bus]")
{
    start_ext();
    FX_SEND(TMX_CMD_ON_BUS, 0, 1, TMX_CMD_I2C_READ, 0x40);
    FX_SEND(TMX_CMD_SYNC, 0, 0, 1, 0);
    FX_SEND(TMX_CMD_ON_BUS, 0, 0, TMX_CMD_ONE_WIRE_CRC8);       /* the CRC8 handler reads p[0] unchecked */
    FX_SEND(TMX_CMD_ON_BUS, 0, 0, TMX_CMD_ONE_WIRE_CRC8, 3, 1);
    const uint8_t e0[] = {ERR(0, TMX_CMD_ON_BUS, TELEMETRIX_REASON_BAD_ARGUMENT)};
    const uint8_t e2[] = {ERR(2, TMX_CMD_ON_BUS, TELEMETRIX_REASON_BAD_ARGUMENT)};
    const uint8_t e3[] = {ERR(3, TMX_CMD_ON_BUS, TELEMETRIX_REASON_BAD_ARGUMENT)};
    TEST_ASSERT_EQUAL(4, fx_n_reports);
    fx_assert_report(0, e0, sizeof(e0));
    fx_assert_report(2, e2, sizeof(e2));
    fx_assert_report(3, e3, sizeof(e3));
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
    TEST_ASSERT_EQUAL(-1, fx_a.on_bus);
}

TEST_CASE("ON_BUS I2C read nobody answers: ERROR NO_ANSWER with cmd 58, no stock report, on_bus -1", "[on_bus]")
{
    start_ext();
    FX_SEND(TMX_CMD_ON_BUS, 0, 1, TMX_CMD_I2C_BEGIN, 19, 20);
    FX_SEND(TMX_CMD_ON_BUS, 0, 1, TMX_CMD_I2C_READ, 0x33, 0x10, 3, 1);
    const uint8_t e[] = {ERR(1, TMX_CMD_ON_BUS, TELEMETRIX_REASON_NO_ANSWER)};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, e, sizeof(e));
    TEST_ASSERT_EQUAL(-1, fx_a.on_bus);
}

TEST_CASE("ON_BUS refusal under abort: the next command is skipped", "[on_bus]")
{
    start_ext();
    FX_SEND(TMX_CMD_ON_BUS, 0, 1, TMX_CMD_I2C_READ, 0x33, 0x10, 3, 1);     /* bus 1 not begun */
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 1);
    const uint8_t e1[] = {ERR(1, TMX_CMD_LOOP_COMMAND, TELEMETRIX_REASON_SKIPPED)};
    TEST_ASSERT_EQUAL(2, fx_n_reports);
    fx_assert_report(1, e1, sizeof(e1));
    TEST_ASSERT_EQUAL(1, fx_refused());
}

TEST_CASE("ON_BUS 1-wire CRC8 on bus 1: BUS_REPORT of the stock reply, equal to the direct command's", "[on_bus]")
{
    start_ext();
    FX_SEND(TMX_CMD_ONE_WIRE_CRC8, 0, 2, 0x02, 0x55, 0);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    uint8_t want[TMX_REPORT_MAX];
    size_t m = tmx_rpt_bus_wrap(want, 1, fx_reports[0].data, fx_reports[0].n);
    fx_n_reports = 0;
    FX_SEND(TMX_CMD_ON_BUS, 0, 1, TMX_CMD_ONE_WIRE_CRC8, 2, 0x02, 0x55);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, m);
}

TEST_CASE("ON_BUS 1-wire init and reset on bus 1: the driver call carries bus 1", "[on_bus]")
{
    start_ext();
    FX_SEND(TMX_CMD_ON_BUS, 0, 1, TMX_CMD_ONE_WIRE_INIT, 24);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    FX_SEND(TMX_CMD_ON_BUS, 0, 1, TMX_CMD_ONE_WIRE_RESET);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    TEST_ASSERT_EQUAL(1, fx_reports[0].data[2]);                /* BUS_REPORT [len, 59, bus, ...] */
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "ow_reset(1)"));
}

TEST_CASE("ONE_WIRE_CRC8 sent directly with the stock client's extra byte: stock reply, bus 0, unwrapped", "[on_bus]")
{
    start_ext();
    FX_SEND(TMX_CMD_ONE_WIRE_CRC8, 0, 7, 0x28, 0xFF, 0x4C, 0x61, 0x92, 0x16, 0x05, 0x81);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    TEST_ASSERT_EQUAL(3, fx_reports[0].data[0]);
    TEST_ASSERT_EQUAL(TMX_RPT_ONE_WIRE_REPORT, fx_reports[0].data[1]);
    TEST_ASSERT_EQUAL(TMX_CMD_ONE_WIRE_CRC8, fx_reports[0].data[2]);
    TEST_ASSERT_EQUAL(0, fx_a.session.error_reports);
}

TEST_CASE("direct stock I2C in extended mode: acts on bus 0, reply is the stock report", "[on_bus]")
{
    start_ext();
    tmx_fake_i2c_device(0, 0x40, regs, sizeof(regs));
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x40, 0, 2, 1);
    uint8_t want[TMX_REPORT_MAX];
    size_t n = tmx_rpt_i2c_read(want, 0x40, 0, regs, 2);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, n);
}
