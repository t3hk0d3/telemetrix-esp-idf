#include <string.h>

#include "fake/tmx_drv_fake.h"
#include "test_plugin.h"

#ifndef TMX_FUZZ
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char *const named[] = {"test.led"};

static struct {
    bool ticking;
    int led;                    /**< gpio claimed by sub 3, -1 = none */
    unsigned releases;
    volatile unsigned async_done;
    volatile esp_err_t async_last;
    telemetrix_plugin_ctx_t *ctx;
} st = {.led = -1};

void test_plugin_reset(void)
{
    st.ticking = false;
    st.led = -1;
    st.releases = 0;
    st.async_done = 0;
    st.async_last = ESP_OK;
    st.ctx = NULL;
}

unsigned test_plugin_releases(void)
{
    return st.releases;
}

unsigned test_plugin_async_done(void)
{
    return st.async_done;
}

esp_err_t test_plugin_async_last(void)
{
    return st.async_last;
}

static void async_emit(telemetrix_plugin_ctx_t *ctx, uint16_t req)
{
    const uint8_t data[] = {(uint8_t)(req >> 8), (uint8_t)req, TEST_PLUGIN_ASYNC_MARK};
    st.async_last = telemetrix_plugin_emit_async(ctx, 2, data, sizeof(data));
    st.async_done++;
}

#ifndef TMX_FUZZ
static void async_task(void *arg)
{
    async_emit(st.ctx, (uint16_t)(uintptr_t)arg);
    vTaskDelete(NULL);
}
#endif

static telemetrix_reason_t spawn_async(telemetrix_plugin_ctx_t *ctx, uint16_t req)
{
    st.ctx = ctx;
#ifdef TMX_FUZZ
    async_emit(ctx, req);
    return TELEMETRIX_REASON_NONE;
#else
    if (xTaskCreate(async_task, "test.echo", 4096, (void *)(uintptr_t)req, 5, NULL) != pdPASS) {
        return TELEMETRIX_REASON_HARDWARE;
    }
    return TELEMETRIX_REASON_NONE;
#endif
}

static telemetrix_reason_t drive_led(telemetrix_plugin_ctx_t *ctx, uint8_t gpio)
{
    switch (telemetrix_plugin_claim_pin(ctx, gpio, "test.led")) {
    case ESP_OK:
        break;
    case ESP_ERR_INVALID_STATE:
        return TELEMETRIX_REASON_PIN_BUSY;
    default:
        return TELEMETRIX_REASON_NO_CAPABILITY;
    }
    st.led = gpio;
    return tmx_drv_fake.gpio_output(gpio, true) == ESP_OK ? TELEMETRIX_REASON_NONE : TELEMETRIX_REASON_HARDWARE;
}

static void busy(void)
{
    const uint64_t t0 = tmx_drv_fake.micros();
    while (tmx_drv_fake.micros() - t0 < TEST_PLUGIN_BUSY_US) {
    }
}

static telemetrix_reason_t command(telemetrix_plugin_ctx_t *ctx, uint8_t sub, const uint8_t *args, size_t len,
                                   uint8_t *code)
{
    switch (sub) {
    case 0:
        return telemetrix_plugin_emit(ctx, 0, args, len) == ESP_OK ? TELEMETRIX_REASON_NONE :
               TELEMETRIX_REASON_BAD_ARGUMENT;
    case 1:
        *code = TEST_PLUGIN_REFUSE_CODE;
        return TELEMETRIX_REASON_PLUGIN;
    case 2:
        return len < 2 ? TELEMETRIX_REASON_BAD_ARGUMENT : spawn_async(ctx, (uint16_t)(args[0] << 8 | args[1]));
    case 3:
        return len < 1 ? TELEMETRIX_REASON_BAD_ARGUMENT : drive_led(ctx, args[0]);
    case 4:
        busy();
        return TELEMETRIX_REASON_NONE;
    case 5:
        st.ticking = true;
        telemetrix_plugin_wake(ctx);
        return TELEMETRIX_REASON_NONE;
    default:
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
}

static uint32_t poll(telemetrix_plugin_ctx_t *ctx, uint32_t now_ms)
{
    (void)now_ms;
    if (!st.ticking) {
        return UINT32_MAX;
    }
    telemetrix_plugin_emit(ctx, TEST_PLUGIN_TICK_SUB, NULL, 0);
    return TEST_PLUGIN_TICK_MS;
}

static void release(telemetrix_plugin_ctx_t *ctx)
{
    st.ticking = false;
    if (st.led >= 0) {
        telemetrix_plugin_release_pin(ctx, (uint8_t)st.led);
        st.led = -1;
    }
    st.releases++;
}

const telemetrix_plugin_t test_plugin_echo = {
    .name = "test.echo",
    .major = 1,
    .minor = 0,
    .named_caps = named,
    .n_named_caps = 1,
    .command = command,
    .poll = poll,
    .release = release,
    .user = &st,
};
