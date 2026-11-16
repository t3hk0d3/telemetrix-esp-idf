#include "adapter_fixture.h"

#define LONG_ANSWER 7, TMX_RPT_FIRMWARE_REPORT, 3, 2, 100, TMX_EXT_MAGIC, 1, 0
#define ERR(seq, cmd, why) 5, TMX_RPT_ERROR, (uint8_t)((seq) >> 8), (uint8_t)(seq), (cmd), (why)

static const telemetrix_i2c_bus_t bus0 = {.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000};

/** A fresh adapter whose config has extended set to ext. */
static void start_cfg(bool ext)
{
    fx_start_buses(&bus0, NULL);
    fx_cfg.extended = ext;
    fx_n_reports = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_adapter_init(&fx_a, &fx_profile, &fx_cfg, &tmx_drv_fake, fx_emit, NULL));
}

/** A fresh adapter in extended mode, no report pending. */
static void start_ext(void)
{
    start_cfg(true);
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 0);
    TEST_ASSERT_TRUE(tmx_adapter_extended(&fx_a));
    fx_n_reports = 0;
}

/** DIGITAL_WRITE on a pin that is not in the profile: refused with NO_SUCH_PIN. */
static void send_refused(void)
{
    FX_SEND(TMX_CMD_DIGITAL_WRITE, 0, 99, 1);
}

static void assert_stock_answer(void)
{
    const uint8_t want[] = {4, TMX_RPT_FIRMWARE_REPORT, 3, 2, 100};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_FALSE(tmx_adapter_extended(&fx_a));
}

TEST_CASE("handshake [A7,1,0]: long answer, mode extended, next command seq 0", "[session]")
{
    const uint8_t want[] = {LONG_ANSWER};
    start_cfg(true);
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 0);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(fx_reports[0].reply);
    TEST_ASSERT_TRUE(tmx_adapter_extended(&fx_a));
    TEST_ASSERT_EQUAL(0, fx_a.session.seq);
    TEST_ASSERT_TRUE(fx_a.session.abort);
    fx_n_reports = 0;
    send_refused();
    const uint8_t err[] = {ERR(0, TMX_CMD_DIGITAL_WRITE, TELEMETRIX_REASON_NO_SUCH_PIN)};
    fx_assert_report(0, err, sizeof(err));
}

TEST_CASE("handshake [A7,1,0]: mode_changed is true once", "[session]")
{
    start_ext();
    TEST_ASSERT_TRUE(tmx_adapter_mode_changed(&fx_a));
    TEST_ASSERT_FALSE(tmx_adapter_mode_changed(&fx_a));
}

TEST_CASE("handshake [A7,1,5] from a newer-minor client: extended at minor 0", "[session]")
{
    const uint8_t want[] = {LONG_ANSWER};
    start_cfg(true);
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 5);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(tmx_adapter_extended(&fx_a));
    TEST_ASSERT_EQUAL(0, fx_a.session.minor);
}

TEST_CASE("handshake [A7,2,0]: long answer, mode stays stock", "[session]")
{
    const uint8_t want[] = {LONG_ANSWER};
    start_cfg(true);
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 2, 0);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_FALSE(tmx_adapter_extended(&fx_a));
    TEST_ASSERT_FALSE(tmx_adapter_mode_changed(&fx_a));
}

TEST_CASE("handshake on an empty payload: stock answer, mode stays stock", "[session]")
{
    start_cfg(true);
    FX_SEND0(TMX_CMD_GET_FIRMWARE_VERSION, 0);
    assert_stock_answer();
}

TEST_CASE("handshake on another length or another first byte: stock answer, mode stays stock", "[session]")
{
    start_cfg(true);
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1);
    assert_stock_answer();
    fx_n_reports = 0;
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 0, 0);
    assert_stock_answer();
    fx_n_reports = 0;
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, 0x00, 1, 0);
    assert_stock_answer();
    fx_n_reports = 0;
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC);
    assert_stock_answer();
}

TEST_CASE("handshake with cfg.extended false: stock answer for every payload", "[session]")
{
    start_cfg(false);
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 0);
    assert_stock_answer();
    fx_n_reports = 0;
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 2, 0);
    assert_stock_answer();
    fx_n_reports = 0;
    FX_SEND0(TMX_CMD_GET_FIRMWARE_VERSION, 0);
    assert_stock_answer();
}

TEST_CASE("telemetrix_config_default: extended is true", "[session]")
{
    telemetrix_config_t c;
    telemetrix_config_default(&c);
    TEST_ASSERT_TRUE(c.extended);
}

TEST_CASE("second GET_FIRMWARE_VERSION in extended mode: long answer, numbered", "[session]")
{
    const uint8_t want[] = {LONG_ANSWER};
    start_ext();
    FX_SEND0(TMX_CMD_GET_FIRMWARE_VERSION, 0);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_EQUAL(1, fx_a.session.seq);
    fx_n_reports = 0;
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 0);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_EQUAL(2, fx_a.session.seq);
    TEST_ASSERT_TRUE(tmx_adapter_extended(&fx_a));
}

TEST_CASE("three commands, the second refused: one ERROR queued as a reply before the third is handled",
          "[session]")
{
    start_ext();
    fx_a.session.abort = false;     /* the third must run to show the order */
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 7);
    send_refused();
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 8);
    const uint8_t loop7[] = {2, 0, 7};
    const uint8_t err[] = {ERR(1, TMX_CMD_DIGITAL_WRITE, TELEMETRIX_REASON_NO_SUCH_PIN)};
    const uint8_t loop8[] = {2, 0, 8};
    TEST_ASSERT_EQUAL(3, fx_n_reports);
    fx_assert_report(0, loop7, sizeof(loop7));
    fx_assert_report(1, err, sizeof(err));
    fx_assert_report(2, loop8, sizeof(loop8));
    TEST_ASSERT_TRUE(fx_reports[1].reply);
    TEST_ASSERT_EQUAL(1, fx_a.session.error_reports);
    TEST_ASSERT_EQUAL(1, fx_refused());
}

TEST_CASE("seq 65535 then one more command: the next command is seq 0", "[session]")
{
    start_ext();
    fx_a.session.abort = false;
    fx_a.session.seq = 65535;
    send_refused();
    send_refused();
    const uint8_t e0[] = {ERR(65535, TMX_CMD_DIGITAL_WRITE, TELEMETRIX_REASON_NO_SUCH_PIN)};
    const uint8_t e1[] = {ERR(0, TMX_CMD_DIGITAL_WRITE, TELEMETRIX_REASON_NO_SUCH_PIN)};
    fx_assert_report(0, e0, sizeof(e0));
    fx_assert_report(1, e1, sizeof(e1));
}

TEST_CASE("out-of-scope ids (touch mode, DAC, SPI, stepper): ERROR NOT_IN_SCOPE, ignored_commands unchanged",
          "[session]")
{
    start_ext();
    fx_a.session.abort = false;
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, TMX_MODE_TOUCH);
    FX_SEND(TMX_CMD_DAC_WRITE, 0, 1, 2);
    FX_SEND(TMX_CMD_SPI_INIT, 0, 1, 2, 3);
    FX_SEND(TMX_CMD_STEPPER_RUN, 0, 1);
    TEST_ASSERT_EQUAL(4, fx_n_reports);
    const uint8_t e0[] = {ERR(0, TMX_CMD_SET_PIN_MODE, TELEMETRIX_REASON_NOT_IN_SCOPE)};
    const uint8_t e3[] = {ERR(3, TMX_CMD_STEPPER_RUN, TELEMETRIX_REASON_NOT_IN_SCOPE)};
    fx_assert_report(0, e0, sizeof(e0));
    fx_assert_report(3, e3, sizeof(e3));
    TEST_ASSERT_EQUAL(0, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(4, fx_a.session.error_reports);
}

TEST_CASE("short payload in extended mode: ERROR BAD_ARGUMENT, connection stays", "[session]")
{
    start_ext();
    FX_SEND(TMX_CMD_DIGITAL_WRITE, 0, 12);
    const uint8_t e0[] = {ERR(0, TMX_CMD_DIGITAL_WRITE, TELEMETRIX_REASON_BAD_ARGUMENT)};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, e0, sizeof(e0));
    TEST_ASSERT_FALSE(tmx_adapter_violation(&fx_a));
    TEST_ASSERT_EQUAL(1, fx_refused());
}

TEST_CASE("id 61 and 255 in extended mode: violation requested, nothing executed", "[session]")
{
    start_ext();
    TEST_ASSERT_FALSE(tmx_adapter_violation(&fx_a));
    FX_SEND(61, 0, 1, 2);
    TEST_ASSERT_TRUE(tmx_adapter_violation(&fx_a));
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    TEST_ASSERT_EQUAL(0, fx_a.session.error_reports);
    TEST_ASSERT_EQUAL(0, fx_a.session.seq);
    fx_a.session.violation = false;
    FX_SEND0(255, 0);
    TEST_ASSERT_TRUE(tmx_adapter_violation(&fx_a));
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("id 61 in stock mode: ignored and counted as in M1, no violation", "[session]")
{
    start_cfg(true);
    FX_SEND0(61, 0);
    TEST_ASSERT_FALSE(tmx_adapter_violation(&fx_a));
    TEST_ASSERT_EQUAL(1, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("ids 59-60 in extended mode: 59 with no page byte BAD_ARGUMENT, 60 with no plugin NO_SUCH_SLOT; in stock "
          "mode ignored", "[session]")
{
    start_ext();
    fx_a.session.abort = false;
    FX_SEND0(TMX_CMD_GET_CAPABILITIES, 0);
    FX_SEND(TMX_CMD_PLUGIN, 0, 0, 0);
    const uint8_t e0[] = {ERR(0, TMX_CMD_GET_CAPABILITIES, TELEMETRIX_REASON_BAD_ARGUMENT)};
    const uint8_t e1[] = {7, TMX_RPT_ERROR, 0, 1, TMX_CMD_PLUGIN, TELEMETRIX_REASON_NO_SUCH_SLOT, 0, 0};    /* + [slot, sub] */
    fx_assert_report(0, e0, sizeof(e0));
    fx_assert_report(1, e1, sizeof(e1));
    start_cfg(true);
    FX_SEND(TMX_CMD_SYNC, 0, 0, 1, 1);
    TEST_ASSERT_EQUAL(1, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("servo exhaustion in extended mode: ERROR EXHAUSTED and no [2,6,pin]", "[session]")
{
    start_ext();
    fx_a.cfg.max_servo = 0;
    FX_SEND(TMX_CMD_SERVO_ATTACH, 0, 12, 0x02, 0x00, 0x09, 0x00);
    const uint8_t e0[] = {ERR(0, TMX_CMD_SERVO_ATTACH, TELEMETRIX_REASON_EXHAUSTED)};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, e0, sizeof(e0));
    TEST_ASSERT_TRUE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(1, fx_refused());
}

TEST_CASE("I2C read nobody answers in extended mode: ERROR NO_ANSWER and no [3,8,1,addr]", "[session]")
{
    start_ext();
    FX_SEND0(TMX_CMD_I2C_BEGIN, 0);
    FX_SEND(TMX_CMD_I2C_READ, 0, 0x33, 0x10, 3, 1);
    const uint8_t e1[] = {ERR(1, TMX_CMD_I2C_READ, TELEMETRIX_REASON_NO_ANSWER)};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, e1, sizeof(e1));
}

TEST_CASE("stock mode keeps upstream's refusal reports: [2,6,pin] and [3,8,1,addr], no ERROR", "[session]")
{
    start_cfg(true);
    fx_a.cfg.max_servo = 0;
    FX_SEND(TMX_CMD_SERVO_ATTACH, 0, 12, 0x02, 0x00, 0x09, 0x00);
    const uint8_t servo[] = {2, TMX_RPT_SERVO_UNAVAILABLE, 12};
    fx_assert_report(0, servo, sizeof(servo));
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    TEST_ASSERT_EQUAL(0, fx_a.session.error_reports);
}

TEST_CASE("abort on and command 2 of 4 refused: 3 and 4 skipped, ERROR SKIPPED with their seq", "[session]")
{
    start_ext();
    TEST_ASSERT_TRUE(fx_a.session.abort);
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 1);
    send_refused();
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 2);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, TMX_MODE_OUTPUT);
    const uint8_t e2[] = {ERR(2, TMX_CMD_LOOP_COMMAND, TELEMETRIX_REASON_SKIPPED)};
    const uint8_t e3[] = {ERR(3, TMX_CMD_SET_PIN_MODE, TELEMETRIX_REASON_SKIPPED)};
    TEST_ASSERT_EQUAL(4, fx_n_reports);
    fx_assert_report(2, e2, sizeof(e2));
    fx_assert_report(3, e3, sizeof(e3));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));    /* command 4 did not run */
    TEST_ASSERT_EQUAL(1, fx_refused());                 /* a skipped command is not a refusal of its own */
}

TEST_CASE("RESET in extended mode: pins released, the session stays extended and numbering continues", "[session]")
{
    start_ext();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, TMX_MODE_OUTPUT);
    FX_SEND0(TMX_CMD_RESET, 0);
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_TRUE(tmx_adapter_reset_requested(&fx_a));
    TEST_ASSERT_TRUE(tmx_adapter_extended(&fx_a));
    TEST_ASSERT_EQUAL(2, fx_a.session.seq);
    send_refused();
    const uint8_t e[] = {ERR(2, TMX_CMD_DIGITAL_WRITE, TELEMETRIX_REASON_NO_SUCH_PIN)};
    fx_assert_report(0, e, sizeof(e));
}

TEST_CASE("session end: back to stock, seq 0, abort on, violation cleared, pins released", "[session]")
{
    start_ext();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, TMX_MODE_OUTPUT);
    send_refused();                 /* abort on: skipping set */
    FX_SEND(61, 0, 1);
    fx_a.session.abort = false;
    tmx_adapter_session_end(&fx_a);
    TEST_ASSERT_FALSE(tmx_adapter_extended(&fx_a));
    TEST_ASSERT_FALSE(tmx_adapter_violation(&fx_a));
    TEST_ASSERT_FALSE(tmx_adapter_mode_changed(&fx_a));
    TEST_ASSERT_EQUAL(0, fx_a.session.seq);
    TEST_ASSERT_TRUE(fx_a.session.abort);
    TEST_ASSERT_FALSE(fx_a.session.skipping);
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
    fx_n_reports = 0;
    FX_SEND0(TMX_CMD_GET_FIRMWARE_VERSION, 0);
    assert_stock_answer();
}

TEST_CASE("release alone keeps the session: violation stays sticky", "[session]")
{
    start_ext();
    FX_SEND(61, 0, 1);
    tmx_adapter_release(&fx_a);
    TEST_ASSERT_TRUE(tmx_adapter_violation(&fx_a));
    TEST_ASSERT_TRUE(tmx_adapter_extended(&fx_a));
}
