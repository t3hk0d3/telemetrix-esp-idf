#include <string.h>

#include "fake/tmx_drv_fake.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "telemetrix_plugin.h"
#include "test_plugin.h"
#include "tmx_adapter.h"
#include "tmx_net.h"
#include "unity.h"

/* Slots: 0 test.echo, 1 rec.one, 2 rec.two, 3 rec.idle (no command, no hooks). */

#define ERR_PLUGIN(seq, why, slot, sub) 7, TMX_RPT_ERROR, (uint8_t)((seq) >> 8), (uint8_t)(seq), TMX_CMD_PLUGIN, (why), \
    (slot), (sub)

/** A recording plugin: what its command got, how often it was polled, when its release ran. */
typedef struct {
    char id;
    telemetrix_reason_t answer;
    uint8_t sub;
    uint8_t args[8];
    size_t len;
    unsigned commands;
    uint32_t poll_ret;
    unsigned polls;
    tmx_use_t pin5_at_release;
} rec_t;

static rec_t rec1, rec2;
static char release_log[8];
static tmx_adapter_t a;

static const char *const led_named[] = {"test.led"};
static const char *const led_and_rec[] = {"test.led", "rec.x"};
static const char *const rec_caps[] = {"rec.x"};

static telemetrix_reason_t rec_command(telemetrix_plugin_ctx_t *ctx, uint8_t sub, const uint8_t *args, size_t len,
                                       uint8_t *code)
{
    (void)code;
    rec_t *r = telemetrix_plugin_user(ctx);
    r->sub = sub;
    r->len = len < sizeof(r->args) ? len : sizeof(r->args);
    memcpy(r->args, args, r->len);
    r->commands++;
    return r->answer;
}

static uint32_t rec_poll(telemetrix_plugin_ctx_t *ctx, uint32_t now_ms)
{
    (void)now_ms;
    rec_t *r = telemetrix_plugin_user(ctx);
    r->polls++;
    return r->poll_ret;
}

static void rec_release(telemetrix_plugin_ctx_t *ctx)
{
    rec_t *r = telemetrix_plugin_user(ctx);
    size_t n = strlen(release_log);
    if (n + 1 < sizeof(release_log)) {
        release_log[n] = r->id;
        release_log[n + 1] = '\0';
    }
    r->pin5_at_release = tmx_pin_use(&a.claims, 5);
}

static const telemetrix_plugin_t rec_one = {
    .name = "rec.one", .major = 2, .minor = 3, .named_caps = rec_caps, .n_named_caps = 1, .command = rec_command,
    .poll = rec_poll, .release = rec_release, .user = &rec1,
};
static const telemetrix_plugin_t rec_two = {
    .name = "rec.two", .major = 1, .minor = 0, .command = rec_command, .poll = rec_poll, .release = rec_release,
    .user = &rec2,
};
static const telemetrix_plugin_t rec_idle = {.name = "rec.idle", .major = 1, .minor = 0};
static const telemetrix_plugin_t *const plugins[] = {&test_plugin_echo, &rec_one, &rec_two, &rec_idle};

#define DIO (TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT)

/* 2 rests driven low and grants test.led; 4 grants nothing named; 5 rests pulled up and grants test.led and rec.x. */
static const telemetrix_pin_t pins[] = {
    {
        .gpio = 2, .caps = DIO, .safe = TELEMETRIX_SAFE_DRIVE_LOW, .adc_unit = -1, .adc_channel = -1,
        .named = led_named, .n_named = 1
    },
    {.gpio = 4, .caps = DIO, .safe = TELEMETRIX_SAFE_FLOAT, .adc_unit = -1, .adc_channel = -1},
    {
        .gpio = 5, .caps = DIO | TELEMETRIX_CAP_PULL_UP, .safe = TELEMETRIX_SAFE_PULL_UP, .adc_unit = -1,
        .adc_channel = -1, .named = led_and_rec, .n_named = 2
    },
};

static telemetrix_profile_t profile;
static telemetrix_config_t cfg;

#define MAX_REPORTS 32

static struct {
    uint8_t data[TMX_EXT_REPORT_MAX];
    size_t n;
    bool reply;
} reports[MAX_REPORTS];
static size_t n_reports;
static int wakes;

static void sink(const uint8_t *report, size_t n, bool reply, int8_t kind, uint16_t key, void *ctx)
{
    (void)kind, (void)key, (void)ctx;
    if (n_reports < MAX_REPORTS && n <= TMX_EXT_REPORT_MAX) {
        memcpy(reports[n_reports].data, report, n);
        reports[n_reports].n = n;
        reports[n_reports].reply = reply;
        n_reports++;
    }
}

static void count_wake(void)
{
    wakes++;
}

/** A fresh adapter in stock mode with the four plugins; the ring closed as the net layer leaves it without a client. */
static void start(void)
{
    profile = (telemetrix_profile_t) {
        .pins = pins, .n_pins = sizeof(pins) / sizeof(pins[0]), .rmt = {.rx_channels = 1, .tx_channels = 1},
    };
    telemetrix_config_default(&cfg);
    cfg.plugins = plugins;
    cfg.n_plugins = sizeof(plugins) / sizeof(plugins[0]);
    memset(&rec1, 0, sizeof(rec1));
    memset(&rec2, 0, sizeof(rec2));
    rec1.id = '1';
    rec2.id = '2';
    rec1.poll_ret = rec2.poll_ret = UINT32_MAX;
    release_log[0] = '\0';
    test_plugin_reset();
    tmx_fake_reset();
    TEST_ASSERT_EQUAL(ESP_OK, tmx_adapter_init(&a, &profile, &cfg, &tmx_drv_fake, sink, NULL));
    tmx_fake_reset();
    n_reports = 0;
    wakes = 0;
    tmx_emit_ring_reset();
    tmx_emit_ring_set_wake(count_wake);
}

/** As the net layer does on entering extended mode: the ring opens for the session. */
static void handshake(void)
{
    static const uint8_t hello[] = {TMX_EXT_MAGIC, 1, 0};
    tmx_adapter_packet(&a, TMX_CMD_GET_FIRMWARE_VERSION, hello, sizeof(hello), 0);
    TEST_ASSERT_TRUE(tmx_adapter_extended(&a));
    tmx_emit_ring_bump(true);
    n_reports = 0;
}

/** As the net layer does when the client goes. */
static void disconnect(void)
{
    tmx_adapter_session_end(&a);
    tmx_emit_ring_bump(false);
}

static void start_ext(void)
{
    start();
    handshake();
}

static telemetrix_plugin_ctx_t *ctx(uint8_t slot)
{
    return &a.plugins.ctx[slot];
}

#define SEND(id, now, ...) \
    do { \
        static const uint8_t p_[] = {__VA_ARGS__}; \
        tmx_adapter_packet(&a, (id), p_, sizeof(p_), (now)); \
    } while (0)
#define PLUGIN(...) SEND(TMX_CMD_PLUGIN, 0, __VA_ARGS__)

static void assert_report(size_t i, const uint8_t *want, size_t n)
{
    TEST_ASSERT_TRUE(i < n_reports);
    TEST_ASSERT_EQUAL(n, reports[i].n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, reports[i].data, n);
}

static bool calls_have(const char *line)
{
    return strstr(tmx_fake_calls(), line) != NULL;
}

/* ---- registration through the adapter ---- */

TEST_CASE("tmx_adapter_init with plugins: slot per registration index, each bound to the adapter", "[plugin_api]")
{
    start();
    TEST_ASSERT_EQUAL(4, a.plugins.n);
    TEST_ASSERT_EQUAL_PTR(&test_plugin_echo, ctx(0)->plugin);
    TEST_ASSERT_EQUAL_PTR(&rec_one, ctx(1)->plugin);
    TEST_ASSERT_EQUAL(1, ctx(1)->slot);
    TEST_ASSERT_EQUAL_PTR(&a, ctx(3)->adapter);
    TEST_ASSERT_EQUAL(2, a.plugins.n_named);
}

TEST_CASE("tmx_adapter_init with a pin naming a capability no plugin declares: ESP_ERR_INVALID_ARG", "[plugin_api]")
{
    start();
    cfg.n_plugins = 1;      /* test.echo only: nobody declares rec.x, which gpio 5 names */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_adapter_init(&a, &profile, &cfg, &tmx_drv_fake, sink, NULL));
}

TEST_CASE("tmx_adapter_init with a duplicate plugin: ESP_ERR_INVALID_ARG", "[plugin_api]")
{
    static const telemetrix_plugin_t *const dup[] = {&rec_one, &rec_two, &rec_one};
    start();
    cfg.plugins = dup;
    cfg.n_plugins = 3;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_adapter_init(&a, &profile, &cfg, &tmx_drv_fake, sink, NULL));
}

TEST_CASE("telemetrix_plugin_user: the descriptor's user pointer", "[plugin_api]")
{
    start();
    TEST_ASSERT_EQUAL_PTR(&rec1, telemetrix_plugin_user(ctx(1)));
}

/* ---- session: PLUGIN ---- */

TEST_CASE("PLUGIN on a valid slot: the handler gets (sub, args) and its NONE completes the command", "[plugin_api]")
{
    start_ext();
    PLUGIN(1, 7, 0xa1, 0xa2, 0xa3);
    TEST_ASSERT_EQUAL(0, n_reports);
    TEST_ASSERT_EQUAL(1, rec1.commands);
    TEST_ASSERT_EQUAL(7, rec1.sub);
    TEST_ASSERT_EQUAL(3, rec1.len);
    const uint8_t want[] = {0xa1, 0xa2, 0xa3};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, rec1.args, 3);
    TEST_ASSERT_EQUAL(1, ctx(1)->commands);
    TEST_ASSERT_EQUAL(0, ctx(1)->refusals);
    TEST_ASSERT_EQUAL(1, a.session.seq);
}

TEST_CASE("PLUGIN on an unknown slot: ERROR NO_SUCH_SLOT with [slot, sub]", "[plugin_api]")
{
    const uint8_t want[] = {ERR_PLUGIN(0, TELEMETRIX_REASON_NO_SUCH_SLOT, 4, 9)};
    start_ext();
    PLUGIN(4, 9);
    TEST_ASSERT_EQUAL(1, n_reports);
    assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(reports[0].reply);
}

TEST_CASE("PLUGIN under 2 bytes: ERROR BAD_ARGUMENT without [slot, sub]", "[plugin_api]")
{
    const uint8_t want[] = {5, TMX_RPT_ERROR, 0, 0, TMX_CMD_PLUGIN, TELEMETRIX_REASON_BAD_ARGUMENT};
    start_ext();
    PLUGIN(0);
    TEST_ASSERT_EQUAL(1, n_reports);
    assert_report(0, want, sizeof(want));
}

TEST_CASE("PLUGIN with a handler returning PLUGIN with code 7: ERROR [seq, 60, 10, slot, sub, 7]", "[plugin_api]")
{
    const uint8_t want[] = {8, TMX_RPT_ERROR, 0, 0, TMX_CMD_PLUGIN, TELEMETRIX_REASON_PLUGIN, 0, 1, 7};
    start_ext();
    PLUGIN(0, 1);
    TEST_ASSERT_EQUAL(1, n_reports);
    assert_report(0, want, sizeof(want));
}

TEST_CASE("PLUGIN with a handler refusing: commands and refusals counted for its slot", "[plugin_api]")
{
    start_ext();
    SEND(TMX_CMD_SYNC, 0, 0, 1, 0);     /* abort off: the refusal does not skip the next command */
    PLUGIN(0, 1);
    PLUGIN(0, 0);
    TEST_ASSERT_EQUAL(2, ctx(0)->commands);
    TEST_ASSERT_EQUAL(1, ctx(0)->refusals);
}

TEST_CASE("PLUGIN with a handler returning another reason: ERROR with [slot, sub] and no code", "[plugin_api]")
{
    const uint8_t want[] = {ERR_PLUGIN(0, TELEMETRIX_REASON_HARDWARE, 1, 3)};
    start_ext();
    rec1.answer = TELEMETRIX_REASON_HARDWARE;
    PLUGIN(1, 3);
    TEST_ASSERT_EQUAL(1, n_reports);
    assert_report(0, want, sizeof(want));
}

TEST_CASE("PLUGIN to a plugin without a command handler: ERROR NOT_IN_SCOPE with [slot, sub]", "[plugin_api]")
{
    const uint8_t want[] = {ERR_PLUGIN(0, TELEMETRIX_REASON_NOT_IN_SCOPE, 3, 0)};
    start_ext();
    PLUGIN(3, 0);
    assert_report(0, want, sizeof(want));
}

/* ---- session: release (disconnect, RESET) ---- */

TEST_CASE("release on session end: plugin release hooks in slot order", "[plugin_api]")
{
    start_ext();
    disconnect();
    TEST_ASSERT_EQUAL_STRING("12", release_log);
    TEST_ASSERT_EQUAL(1, test_plugin_releases());
}

TEST_CASE("release on RESET: plugin release hooks in slot order, the session stays extended", "[plugin_api]")
{
    start_ext();
    SEND(TMX_CMD_RESET, 0);
    TEST_ASSERT_EQUAL_STRING("12", release_log);
    TEST_ASSERT_TRUE(tmx_adapter_extended(&a));
}

TEST_CASE("release: the hooks run while the plugin's pins are still claimed, then the pins go to safe states",
          "[plugin_api]")
{
    start_ext();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_claim_pin(ctx(1), 5, "rec.x"));
    disconnect();
    TEST_ASSERT_EQUAL(TMX_USE_PLUGIN, rec1.pin5_at_release);
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&a.claims, 5));
    TEST_ASSERT_TRUE(calls_have("gpio_input(5,1,0)"));
}

/* ---- telemetrix_plugin_claim_pin / release_pin ---- */

TEST_CASE("telemetrix_plugin_claim_pin on a granted named pin: ESP_OK, the plugin drives it", "[plugin_api]")
{
    start_ext();
    PLUGIN(0, 3, 2);
    TEST_ASSERT_EQUAL(0, n_reports);
    TEST_ASSERT_EQUAL(TMX_USE_PLUGIN, tmx_pin_use(&a.claims, 2));
    TEST_ASSERT_EQUAL(0, tmx_pin_owner(&a.claims, 2));
    TEST_ASSERT_TRUE(calls_have("gpio_output(2,1)"));
}

TEST_CASE("telemetrix_plugin_claim_pin on a granted named pin: released to its safe state on session release",
          "[plugin_api]")
{
    start_ext();
    PLUGIN(0, 3, 2);
    tmx_fake_reset();
    disconnect();
    TEST_ASSERT_TRUE(calls_have("gpio_output(2,0)"));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&a.claims, 2));
}

TEST_CASE("telemetrix_plugin_claim_pin on a pin without the name: ESP_ERR_NOT_SUPPORTED", "[plugin_api]")
{
    start_ext();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, telemetrix_plugin_claim_pin(ctx(0), 4, "test.led"));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, telemetrix_plugin_claim_pin(ctx(0), 2, "rec.x"));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, telemetrix_plugin_claim_pin(ctx(0), 40, "test.led"));
}

TEST_CASE("telemetrix_plugin_claim_pin on a pin in use: ESP_ERR_INVALID_STATE", "[plugin_api]")
{
    start_ext();
    SEND(TMX_CMD_SET_PIN_MODE, 0, 2, TMX_MODE_OUTPUT, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, telemetrix_plugin_claim_pin(ctx(0), 2, "test.led"));
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_claim_pin(ctx(1), 5, "rec.x"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, telemetrix_plugin_claim_pin(ctx(0), 5, "test.led"));
}

TEST_CASE("SET_PIN_MODE on a plugin's pin: ERROR PIN_BUSY, the pin stays the plugin's", "[plugin_api]")
{
    const uint8_t want[] = {5, TMX_RPT_ERROR, 0, 1, TMX_CMD_SET_PIN_MODE, TELEMETRIX_REASON_PIN_BUSY};
    start_ext();
    PLUGIN(0, 3, 2);
    SEND(TMX_CMD_SET_PIN_MODE, 0, 2, TMX_MODE_OUTPUT, 0);
    assert_report(0, want, sizeof(want));
    TEST_ASSERT_EQUAL(TMX_USE_PLUGIN, tmx_pin_use(&a.claims, 2));
}

TEST_CASE("telemetrix_plugin_release_pin of its own pin: the pin to its safe state and free", "[plugin_api]")
{
    start_ext();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_claim_pin(ctx(1), 5, "rec.x"));
    telemetrix_plugin_release_pin(ctx(1), 5);
    TEST_ASSERT_EQUAL_STRING("gpio_input(5,1,0)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&a.claims, 5));
}

TEST_CASE("telemetrix_plugin_release_pin of a pin it does not hold: nothing changes", "[plugin_api]")
{
    start_ext();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_claim_pin(ctx(1), 5, "rec.x"));
    SEND(TMX_CMD_SET_PIN_MODE, 0, 4, TMX_MODE_OUTPUT, 0);
    tmx_fake_reset();
    telemetrix_plugin_release_pin(ctx(0), 5);
    telemetrix_plugin_release_pin(ctx(0), 4);
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_PLUGIN, tmx_pin_use(&a.claims, 5));
    TEST_ASSERT_EQUAL(TMX_USE_OUTPUT, tmx_pin_use(&a.claims, 4));
}

/* ---- telemetrix_plugin_claim_rmt / release_rmt ---- */

TEST_CASE("telemetrix_plugin_claim_rmt beyond the grant: ESP_ERR_NOT_FOUND for TX and RX", "[plugin_api]")
{
    start_ext();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_claim_rmt(ctx(0), true));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, telemetrix_plugin_claim_rmt(ctx(1), true));
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_claim_rmt(ctx(0), false));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, telemetrix_plugin_claim_rmt(ctx(0), false));
}

TEST_CASE("telemetrix_plugin_release_rmt: the channel free again; a plugin holding none frees nothing", "[plugin_api]")
{
    start_ext();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_claim_rmt(ctx(0), true));
    telemetrix_plugin_release_rmt(ctx(1), true);
    TEST_ASSERT_EQUAL(1, a.claims.rmt_tx_used);
    telemetrix_plugin_release_rmt(ctx(0), true);
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_claim_rmt(ctx(1), true));
}

TEST_CASE("telemetrix_plugin_claim_rmt: forgotten on session release", "[plugin_api]")
{
    start_ext();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_claim_rmt(ctx(0), true));
    disconnect();
    handshake();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_claim_rmt(ctx(1), true));
    telemetrix_plugin_release_rmt(ctx(0), true);     /* holds nothing any more */
    TEST_ASSERT_EQUAL(1, a.claims.rmt_tx_used);
}

/* ---- telemetrix_plugin_emit ---- */

TEST_CASE("telemetrix_plugin_emit on an extended session: PLUGIN_REPORT [slot, sub, data] queued as reply",
          "[plugin_api]")
{
    const uint8_t want[] = {5, TMX_RPT_PLUGIN_REPORT, 0, 0, 0xde, 0xad};
    start_ext();
    PLUGIN(0, 0, 0xde, 0xad);
    TEST_ASSERT_EQUAL(1, n_reports);
    assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(reports[0].reply);
}

TEST_CASE("telemetrix_plugin_emit of 251 bytes: one report of 254 bytes after its length", "[plugin_api]")
{
    uint8_t data[251];
    memset(data, 0x3c, sizeof(data));
    start_ext();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_emit(ctx(2), 4, data, sizeof(data)));
    TEST_ASSERT_EQUAL(1, n_reports);
    TEST_ASSERT_EQUAL(255, reports[0].n);
    TEST_ASSERT_EQUAL(254, reports[0].data[0]);
    TEST_ASSERT_EQUAL(2, reports[0].data[2]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, reports[0].data + 4, sizeof(data));
}

TEST_CASE("telemetrix_plugin_emit in stock mode: ESP_ERR_INVALID_STATE, nothing queued", "[plugin_api]")
{
    const uint8_t d = 1;
    start();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, telemetrix_plugin_emit(ctx(0), 0, &d, 1));
    TEST_ASSERT_EQUAL(0, n_reports);
}

TEST_CASE("telemetrix_plugin_emit with no client (after session end): ESP_ERR_INVALID_STATE, nothing queued",
          "[plugin_api]")
{
    const uint8_t d = 1;
    start_ext();
    disconnect();
    n_reports = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, telemetrix_plugin_emit(ctx(0), 0, &d, 1));
    TEST_ASSERT_EQUAL(0, n_reports);
}

TEST_CASE("telemetrix_plugin_emit of data over 251 bytes: ESP_ERR_INVALID_ARG, nothing queued", "[plugin_api]")
{
    uint8_t data[252] = {0};
    start_ext();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, telemetrix_plugin_emit(ctx(0), 0, data, sizeof(data)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, telemetrix_plugin_emit(ctx(0), 0, NULL, 1));
    TEST_ASSERT_EQUAL(0, n_reports);
}

/* ---- telemetrix_plugin_emit_async ---- */

static bool wait_async(unsigned n)
{
    for (int i = 0; i < 300 && test_plugin_async_done() < n; i++) {
        vTaskDelay(1);
    }
    return test_plugin_async_done() >= n;
}

TEST_CASE("telemetrix_plugin_emit_async from another task: delivered as PLUGIN_REPORT on the network task",
          "[plugin_api]")
{
    const uint8_t want[] = {6, TMX_RPT_PLUGIN_REPORT, 0, 2, 0x12, 0x34, TEST_PLUGIN_ASYNC_MARK};
    start_ext();
    PLUGIN(0, 2, 0x12, 0x34);
    TEST_ASSERT_TRUE(wait_async(1));
    TEST_ASSERT_EQUAL(ESP_OK, test_plugin_async_last());
    TEST_ASSERT_EQUAL(0, n_reports);
    tmx_emit_ring_drain(&a);
    TEST_ASSERT_EQUAL(1, n_reports);
    assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(reports[0].reply);
}

TEST_CASE("telemetrix_plugin_emit_async from another task: the network task woken", "[plugin_api]")
{
    start_ext();
    PLUGIN(0, 2, 0, 1);
    TEST_ASSERT_TRUE(wait_async(1));
    TEST_ASSERT_EQUAL(1, wakes);
}

TEST_CASE("telemetrix_plugin_emit_async with the ring full: ESP_ERR_NO_MEM, async_dropped +1", "[plugin_api]")
{
    start_ext();
    for (int i = 0; i < TMX_EMIT_RING_ENTRIES; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_emit_async(ctx(1), 0, NULL, 0));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, telemetrix_plugin_emit_async(ctx(1), 0, NULL, 0));
    TEST_ASSERT_EQUAL(1, ctx(1)->async_dropped);
    TEST_ASSERT_EQUAL(0, ctx(0)->async_dropped);
}

TEST_CASE("telemetrix_plugin_emit_async queued before a disconnect: dropped at drain", "[plugin_api]")
{
    start_ext();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_plugin_emit_async(ctx(1), 0, NULL, 0));
    disconnect();
    handshake();
    tmx_emit_ring_drain(&a);
    TEST_ASSERT_EQUAL(0, n_reports);
}

TEST_CASE("telemetrix_plugin_emit_async in stock mode: ESP_ERR_INVALID_STATE, nothing queued", "[plugin_api]")
{
    start();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, telemetrix_plugin_emit_async(ctx(1), 0, NULL, 0));
    handshake();
    tmx_emit_ring_drain(&a);
    TEST_ASSERT_EQUAL(0, n_reports);
}

TEST_CASE("telemetrix_plugin_emit_async of data over 251 bytes: ESP_ERR_INVALID_ARG", "[plugin_api]")
{
    uint8_t data[252] = {0};
    start_ext();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, telemetrix_plugin_emit_async(ctx(1), 0, data, sizeof(data)));
}

/* ---- budget ---- */

TEST_CASE("budget on a handler taking 50 ms (fake micros): overruns +1, max_us about 50000", "[plugin_api]")
{
    start_ext();
    tmx_fake_set_micros_step(1000);
    PLUGIN(0, 4);
    TEST_ASSERT_EQUAL(1, ctx(0)->overruns);
    /* 50000 of busy work plus the steps of the timing calls themselves */
    TEST_ASSERT_UINT32_WITHIN(5000, 52500, ctx(0)->max_us);
}

TEST_CASE("budget on a handler taking 50 ms: the command completes and the next LOOP is answered", "[plugin_api]")
{
    uint8_t loop[TMX_REPORT_MAX];
    size_t n = tmx_rpt_loop(loop, 0x5a);
    start_ext();
    tmx_fake_set_micros_step(1000);
    PLUGIN(0, 4);
    SEND(TMX_CMD_LOOP_COMMAND, 0, 0x5a);
    TEST_ASSERT_EQUAL(1, n_reports);
    assert_report(0, loop, n);
}

TEST_CASE("budget on a handler within the budget: no overrun", "[plugin_api]")
{
    start_ext();
    tmx_fake_set_micros_step(2000);
    PLUGIN(1, 0);
    TEST_ASSERT_EQUAL(0, ctx(1)->overruns);
    TEST_ASSERT_EQUAL(2000, ctx(1)->max_us);
}

TEST_CASE("budget on a poll call over the budget: overruns +1", "[plugin_api]")
{
    start_ext();
    tmx_fake_set_micros_step(6000);
    tmx_adapter_poll(&a, 0);
    TEST_ASSERT_EQUAL(1, ctx(1)->overruns);
    TEST_ASSERT_EQUAL(6000, ctx(1)->max_us);
}

/* ---- poll hook ---- */

TEST_CASE("poll hook in an extended session: first called at once", "[plugin_api]")
{
    start_ext();
    tmx_adapter_poll(&a, 100);
    TEST_ASSERT_EQUAL(1, rec1.polls);
    TEST_ASSERT_EQUAL(1, rec2.polls);
}

TEST_CASE("poll hook returning 20 ms: called again after 20 ms, the poll timeout never longer", "[plugin_api]")
{
    start_ext();
    rec1.poll_ret = 20;
    TEST_ASSERT_EQUAL(20, tmx_adapter_poll(&a, 100));
    TEST_ASSERT_EQUAL(10, tmx_adapter_poll(&a, 110));
    TEST_ASSERT_EQUAL(1, rec1.polls);
    tmx_adapter_poll(&a, 120);
    TEST_ASSERT_EQUAL(2, rec1.polls);
}

TEST_CASE("poll hook returning UINT32_MAX: not called again until a wake", "[plugin_api]")
{
    start_ext();
    TEST_ASSERT_EQUAL(UINT32_MAX, tmx_adapter_poll(&a, 0));
    tmx_adapter_poll(&a, 50000);
    TEST_ASSERT_EQUAL(1, rec1.polls);
    telemetrix_plugin_wake(ctx(1));
    tmx_adapter_poll(&a, 50001);
    TEST_ASSERT_EQUAL(2, rec1.polls);
    TEST_ASSERT_EQUAL(1, rec2.polls);
}

TEST_CASE("telemetrix_plugin_wake: the network task woken", "[plugin_api]")
{
    start_ext();
    telemetrix_plugin_wake(ctx(1));
    TEST_ASSERT_EQUAL(1, wakes);
}

TEST_CASE("poll hook in stock mode: not called", "[plugin_api]")
{
    start();
    rec1.poll_ret = 20;
    tmx_adapter_poll(&a, 0);
    tmx_adapter_poll(&a, 100);
    TEST_ASSERT_EQUAL(0, rec1.polls);
}

TEST_CASE("poll hook after a RESET: called at once again", "[plugin_api]")
{
    start_ext();
    tmx_adapter_poll(&a, 0);
    SEND(TMX_CMD_RESET, 10);
    tmx_adapter_poll(&a, 10);
    TEST_ASSERT_EQUAL(2, rec1.polls);
}

TEST_CASE("test plugin poll hook once sub 5 enables it: sub 9 at once, then every 1000 ms", "[plugin_api]")
{
    const uint8_t tick[] = {3, TMX_RPT_PLUGIN_REPORT, 0, TEST_PLUGIN_TICK_SUB};
    start_ext();
    tmx_adapter_poll(&a, 0);
    TEST_ASSERT_EQUAL(0, n_reports);
    PLUGIN(0, 5);
    tmx_adapter_poll(&a, 1);
    TEST_ASSERT_EQUAL(1, n_reports);
    assert_report(0, tick, sizeof(tick));
    tmx_adapter_poll(&a, 1000);
    TEST_ASSERT_EQUAL(1, n_reports);
    tmx_adapter_poll(&a, 1001);
    TEST_ASSERT_EQUAL(2, n_reports);
}

/* ---- telemetrix_plugin_set_status and the registry status ---- */

TEST_CASE("telemetrix_plugin_set_status: at most 47 characters copied, NULL clears", "[plugin_api]")
{
    char text[64];
    memset(text, 'x', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    start_ext();
    telemetrix_plugin_set_status(ctx(1), text);
    text[5] = '\0';                     /* the copy does not follow the caller's buffer */
    TEST_ASSERT_EQUAL(47, strlen(ctx(1)->text));
    telemetrix_plugin_status_t st[4];
    TEST_ASSERT_EQUAL(4, tmx_plugins_status(&a.plugins, st, 4));
    TEST_ASSERT_EQUAL(47, strlen(st[1].text));
    telemetrix_plugin_set_status(ctx(1), NULL);
    TEST_ASSERT_EQUAL_STRING("", ctx(1)->text);
}
