/* libFuzzer target: one extended session the way the network task runs it. The frame assembler feeds the adapter;
 * after each packet the frame switches to extended framing when the session entered extended mode and halts on a
 * violation; plugin emit_async entries are drained as PLUGIN_REPORT. The profile has two I2C and two 1-wire buses and
 * a pin named for test.echo, which is registered in slot 0.
 * Every input starts with the handshake. The input is a list of operations [op][args...]: a run of raw bytes into the
 * frame, a fake completion, a clock step, a poll (scans, plugin polls, frame tick, ring drain) or an injected driver
 * failure. A violation ends the session as a disconnect does, and the next operations meet a new client that
 * handshakes again. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fake/tmx_drv_fake.h"
#include "test_plugin.h"
#include "tmx_adapter.h"
#include "tmx_net.h"

#define N_PINS 40
#define LED_GPIO 36

static telemetrix_pin_t pins[N_PINS];
static const char *const led_named[] = {"test.led"};
static int adc_handle;
static const telemetrix_adc_grant_t adc[] = {{.unit = 1, .handle = &adc_handle}};
static const telemetrix_i2c_bus_t i2c[] = {
    {.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000},
    {.bus = 1, .port = 1, .sda = 2, .scl = 15, .speed_hz = 400000},
};
static const telemetrix_onewire_bus_t onewire[] = {{.bus = 0, .gpio = 23}, {.bus = 1, .gpio = 35}};
static const telemetrix_plugin_t *const plugins[] = {&test_plugin_echo};
static const uint8_t hello[] = {4, TMX_CMD_GET_FIRMWARE_VERSION, TMX_EXT_MAGIC, TMX_EXT_MAJOR, TMX_EXT_MINOR};

static const char *const fail_ops[] = {
    "gpio_input", "gpio_output", "ledc_timer", "ledc_attach", "ledc_duty", "adc_read", "i2c_open", "i2c_read",
    "i2c_write", "ow_open", "dht_start", "sonar_start",
};

static tmx_adapter_t a;
static tmx_frame_t frame;
static uint32_t now;

/** Every report is whole: a length byte that matches, at most 255 bytes on the wire. */
static void emit(const uint8_t *report, size_t n, bool reply, int8_t kind, uint16_t key, void *ctx)
{
    (void)reply;
    (void)kind;
    (void)key;
    (void)ctx;
    if (n < 2 || n > TMX_EXT_REPORT_MAX - 1 || report[0] != n - 1) {
        abort();
    }
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < n; i++) {
        sink ^= report[i];
    }
}

/** net.c's packet callback. */
static void packet_cb(uint8_t id, const uint8_t *payload, size_t len, void *ctx)
{
    (void)ctx;
    tmx_adapter_packet(&a, id, payload, len, now);
    if (tmx_adapter_mode_changed(&a)) {
        tmx_frame_set_mode(&frame, TMX_EXT_PAYLOAD_MAX, true);
        tmx_emit_ring_bump(true);
    }
    if (tmx_adapter_violation(&a)) {
        tmx_frame_halt(&frame);
        return;
    }
    if (tmx_adapter_reset_requested(&a)) {
        tmx_completion_reset();
    }
}

static void build_profile(telemetrix_profile_t *p)
{
    for (int i = 0; i < N_PINS; i++) {
        pins[i] = (telemetrix_pin_t) {
            .gpio = (uint8_t)i,
            .caps = TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT | TELEMETRIX_CAP_PULL_UP | TELEMETRIX_CAP_PULL_DOWN |
                    TELEMETRIX_CAP_PWM | TELEMETRIX_CAP_I2C | TELEMETRIX_CAP_ONE_WIRE |
                    (i >= 32 ? TELEMETRIX_CAP_ADC : 0),
                    .safe = (telemetrix_safe_state_t)(i % 5),
                    .adc_unit = (int8_t)(i >= 32 ? 1 : -1),
                    .adc_channel = (int8_t)(i >= 32 ? i - 32 : -1),
        };
    }
    pins[LED_GPIO].named = led_named;
    pins[LED_GPIO].n_named = 1;
    *p = (telemetrix_profile_t) {
        .pins = pins, .n_pins = N_PINS,
        .ledc = {.speed_mode = 0, .timer_mask = 0x0f, .channel_mask = 0xff},
        .rmt = {.rx_channels = 2, .tx_channels = 2},
        .adc = adc, .n_adc = 1,
        .i2c = i2c, .n_i2c = 2,
        .onewire = onewire, .n_onewire = 2,
    };
}

static void handshake(void)
{
    tmx_frame_feed(&frame, hello, sizeof(hello), now, packet_cb, NULL);
    if (!tmx_adapter_extended(&a) || frame.cap != TMX_EXT_PAYLOAD_MAX || !frame.strict) {
        abort();
    }
}

/** What the network task does when it drops a client: the session ends, the ring closes, stock framing. */
static void end_session(const telemetrix_profile_t *p)
{
    tmx_frame_abandon(&frame);
    tmx_frame_init(&frame, TMX_STOCK_PAYLOAD_MAX);
    tmx_adapter_session_end(&a);
    tmx_emit_ring_bump(false);
    for (size_t i = 0; i < p->n_pins; i++) {
        if (a.claims.use[i] != TMX_USE_NONE) {
            abort();
        }
    }
    if (a.claims.rmt_rx_used != 0 || tmx_adapter_extended(&a) || tmx_adapter_violation(&a)) {
        abort();
    }
}

static void script_devices(void)
{
    uint8_t regs[256];
    for (int i = 0; i < 256; i++) {
        regs[i] = (uint8_t)(i ^ 0xa5);
    }
    tmx_fake_i2c_device(0, 0x76, regs, sizeof(regs));
    tmx_fake_i2c_device(1, 0x48, regs, sizeof(regs));
    static const uint8_t roms[2][8] = {
        {0x28, 1, 2, 3, 4, 5, 6, 0x00},
        {0x28, 6, 5, 4, 3, 2, 1, 0x00},
    };
    tmx_fake_ow_devices(0, roms, 2);
    tmx_fake_ow_devices(1, roms, 1);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static telemetrix_profile_t profile;
    build_profile(&profile);
    telemetrix_config_t cfg;
    telemetrix_config_default(&cfg);
    cfg.plugins = plugins;
    cfg.n_plugins = 1;
    tmx_fake_reset();
    tmx_fake_set_micros_step(1000);     /* the test plugin's 50 ms handler ends after 50 clock reads */
    tmx_completion_reset();
    tmx_emit_ring_reset();
    test_plugin_reset();
    if (tmx_adapter_init(&a, &profile, &cfg, &tmx_drv_fake, emit, NULL) != ESP_OK) {
        abort();
    }
    tmx_emit_ring_register(&a.plugins);
    script_devices();
    now = 0xFFFFF000u;                  /* near the wrap of the millisecond counter */
    tmx_frame_init(&frame, TMX_STOCK_PAYLOAD_MAX);
    handshake();

    size_t pos = 0;
    while (pos < size) {
        uint8_t op = data[pos++] % 5;
        size_t left = size - pos;
        switch (op) {
        case 0: {       /* [0][n][n bytes]: one recv of raw bytes */
            if (left < 1) {
                goto out;
            }
            size_t n = data[pos++];
            if (n > size - pos) {
                n = size - pos;
            }
            tmx_frame_feed(&frame, data + pos, n, now, packet_cb, NULL);
            pos += n;
            break;
        }
        case 1:
            tmx_fake_complete_pending();
            break;
        case 2:         /* [2][step]: the clock advances step * 8 ms, sometimes by a lot */
            if (left < 1) {
                goto out;
            }
            now += (uint32_t)data[pos] * (data[pos] > 240 ? 1000u : 8u);
            pos++;
            break;
        case 3:         /* one loop of the network task without a recv */
            tmx_adapter_poll(&a, now);
            tmx_frame_tick(&frame, now);
            tmx_emit_ring_drain(&a);
            break;
        default:        /* [4][k]: the next call of a driver function fails */
            if (left < 1) {
                goto out;
            }
            tmx_fake_fail_next(fail_ops[data[pos] % (sizeof(fail_ops) / sizeof(fail_ops[0]))], ESP_FAIL);
            pos++;
            break;
        }
        if (a.claims.rmt_rx_used > profile.rmt.rx_channels) {
            abort();
        }
        if (frame.violation || tmx_adapter_violation(&a)) {
            end_session(&profile);      /* closed with an RST; the next client handshakes */
            handshake();
        }
    }
out:
    end_session(&profile);
    tmx_emit_ring_register(NULL);
    return 0;
}
