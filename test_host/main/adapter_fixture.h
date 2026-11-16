#ifndef ADAPTER_FIXTURE_H
#define ADAPTER_FIXTURE_H

#include <string.h>

#include "fake/tmx_drv_fake.h"
#include "tmx_adapter.h"
#include "unity.h"

#define DIGITAL_CAPS (TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT | TELEMETRIX_CAP_PULL_UP | TELEMETRIX_CAP_PULL_DOWN)
#define PIN(g, c, s, u, ch) {.gpio = (g), .caps = (c), .safe = (s), .adc_unit = (u), .adc_channel = (ch)}

/* 2 drives low and has PWM; 4 has no pull-down; 12..16 are PWM pins; 34 and 35 are ADC inputs; 21 is I2C. */
static const telemetrix_pin_t fx_pins[] = {
    PIN(2, DIGITAL_CAPS | TELEMETRIX_CAP_PWM, TELEMETRIX_SAFE_DRIVE_LOW, -1, -1),
    PIN(4, TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT | TELEMETRIX_CAP_PULL_UP, TELEMETRIX_SAFE_PULL_UP, -1, -1),
    PIN(12, DIGITAL_CAPS | TELEMETRIX_CAP_PWM, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(13, DIGITAL_CAPS | TELEMETRIX_CAP_PWM, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(14, DIGITAL_CAPS | TELEMETRIX_CAP_PWM, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(15, DIGITAL_CAPS | TELEMETRIX_CAP_PWM, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(16, DIGITAL_CAPS | TELEMETRIX_CAP_PWM, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(21, DIGITAL_CAPS | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_DRIVE_HIGH, -1, -1),
    PIN(34, TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_ADC, TELEMETRIX_SAFE_FLOAT, 1, 6),
    PIN(35, TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_ADC, TELEMETRIX_SAFE_FLOAT, 1, 7),
};
static int fx_adc_handle;
static const telemetrix_adc_grant_t fx_adc[] = {{.unit = 1, .handle = &fx_adc_handle}};

#define FX_MAX_REPORTS 32

typedef struct {
    uint8_t data[TMX_REPORT_MAX];
    size_t n;
    bool reply;
    int8_t kind;
    uint16_t key;
} fx_report_t;

static telemetrix_profile_t fx_profile;
static telemetrix_config_t fx_cfg;
static tmx_adapter_t fx_a;
static fx_report_t fx_reports[FX_MAX_REPORTS];
static size_t fx_n_reports;

static void fx_emit(const uint8_t *report, size_t n, bool reply, int8_t kind, uint16_t key, void *ctx)
{
    (void)ctx;
    if (fx_n_reports < FX_MAX_REPORTS) {
        fx_report_t *r = &fx_reports[fx_n_reports++];
        memcpy(r->data, report, n);
        r->n = n;
        r->reply = reply;
        r->kind = kind;
        r->key = key;
    }
}

/** A fresh adapter on the fake driver; the call log is empty afterwards. */
static inline void fx_start(void)
{
    fx_profile = (telemetrix_profile_t) {
        .pins = fx_pins, .n_pins = sizeof(fx_pins) / sizeof(fx_pins[0]),
        .ledc = {.speed_mode = 0, .timer_mask = 0x0f, .channel_mask = 0xff},
        .rmt = {.rx_channels = 1, .tx_channels = 1},
        .adc = fx_adc, .n_adc = 1,
    };
    /* the defaults the adapter reads (telemetrix_config_default arrives with the public API) */
    fx_cfg = (telemetrix_config_t) {
        .i2c_timeout_ms = 50, .analog_interval_floor_ms = 10, .max_dht = 6, .max_sonar = 6, .max_servo = 4,
    };
    tmx_fake_reset();
    fx_n_reports = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_adapter_init(&fx_a, &fx_profile, &fx_cfg, &tmx_drv_fake, fx_emit, NULL));
    tmx_fake_reset();
}

static inline void fx_send(uint8_t id, const uint8_t *payload, size_t len, uint32_t now)
{
    tmx_adapter_packet(&fx_a, id, payload, len, now);
}

#define FX_SEND(id, now, ...) \
    do { \
        static const uint8_t p_[] = {__VA_ARGS__}; \
        fx_send((id), p_, sizeof(p_), (now)); \
    } while (0)
#define FX_SEND0(id, now) fx_send((id), NULL, 0, (now))

static inline uint32_t fx_refused(void)
{
    uint32_t ign, ref;
    tmx_adapter_counters(&fx_a, &ign, &ref);
    return ref;
}

static inline uint32_t fx_ignored(void)
{
    uint32_t ign, ref;
    tmx_adapter_counters(&fx_a, &ign, &ref);
    return ign;
}

static inline void fx_assert_report(size_t i, const uint8_t *want, size_t n)
{
    TEST_ASSERT_TRUE(i < fx_n_reports);
    TEST_ASSERT_EQUAL(n, fx_reports[i].n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, fx_reports[i].data, n);
}

#endif
