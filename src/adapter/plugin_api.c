#include <string.h>

#include "tmx_adapter.h"

/* The public plugin API (telemetrix_plugin.h) except telemetrix_plugin_emit_async and telemetrix_plugin_wake, which
 * need the ring lock and live in src/net/emit_ring.c; and the adapter's side of plugins: dispatch, poll, release. */

void *telemetrix_plugin_user(telemetrix_plugin_ctx_t *ctx)
{
    return ctx ? ctx->plugin->user : NULL;
}

esp_err_t telemetrix_plugin_claim_pin(telemetrix_plugin_ctx_t *ctx, uint8_t gpio, const char *named_cap)
{
    if (!ctx || !named_cap) {
        return ESP_ERR_INVALID_ARG;
    }
    return tmx_claim_named(&ctx->adapter->claims, gpio, named_cap, ctx->slot);
}

void telemetrix_plugin_release_pin(telemetrix_plugin_ctx_t *ctx, uint8_t gpio)
{
    if (ctx && tmx_pin_owner(&ctx->adapter->claims, gpio) == ctx->slot) {
        tmx_release_pin(&ctx->adapter->claims, gpio);
    }
}

esp_err_t telemetrix_plugin_claim_rmt(telemetrix_plugin_ctx_t *ctx, bool tx)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    tmx_claims_t *c = &ctx->adapter->claims;
    esp_err_t err = tx ? tmx_claim_rmt_tx(c) : tmx_claim_rmt_rx(c);
    if (err == ESP_OK) {
        if (tx) {
            ctx->rmt_tx++;
        } else {
            ctx->rmt_rx++;
        }
    }
    return err;
}

void telemetrix_plugin_release_rmt(telemetrix_plugin_ctx_t *ctx, bool tx)
{
    if (!ctx) {
        return;
    }
    tmx_claims_t *c = &ctx->adapter->claims;
    if (tx && ctx->rmt_tx > 0) {
        ctx->rmt_tx--;
        tmx_release_rmt_tx(c);
    } else if (!tx && ctx->rmt_rx > 0) {
        ctx->rmt_rx--;
        tmx_release_rmt_rx(c);
    }
}

esp_err_t telemetrix_plugin_emit(telemetrix_plugin_ctx_t *ctx, uint8_t sub, const uint8_t *data, size_t n)
{
    if (!ctx || n > TMX_EXT_PAYLOAD_MAX - 3 || (n && !data)) {
        return ESP_ERR_INVALID_ARG;
    }
    tmx_adapter_t *a = ctx->adapter;
    if (!a->session.extended) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t out[TMX_EXT_REPORT_MAX];
    size_t m = tmx_rpt_plugin(out, ctx->slot, sub, data, n);
    a->emit(out, m, true, TMX_COALESCE_NONE, 0, a->ctx);
    return ESP_OK;
}

void telemetrix_plugin_set_status(telemetrix_plugin_ctx_t *ctx, const char *text)
{
    if (!ctx) {
        return;
    }
    size_t n = text ? strnlen(text, TMX_PLUGIN_TEXT_MAX) : 0;
    memcpy(ctx->text, text ? text : "", n);
    ctx->text[n] = '\0';
}

/* Every command and poll call is timed with the driver's clock and counted against cfg.plugin_budget_us. */
static uint64_t timer_start(const tmx_adapter_t *a)
{
    return a->drv->micros();
}

static void timer_stop(const tmx_adapter_t *a, telemetrix_plugin_ctx_t *ctx, uint64_t t0)
{
    tmx_plugins_account(ctx, a->drv->micros() - t0, a->cfg.plugin_budget_us);
}

/** A reason a plugin may return as is: NONE or a refusal reason other than SKIPPED (the server's own word). Anything
 *  else becomes PLUGIN with code 0. */
static bool plugin_reason_ok(telemetrix_reason_t why)
{
    int v = (int)why;
    return v == TELEMETRIX_REASON_NONE ||
           (v >= TELEMETRIX_REASON_NO_SUCH_PIN && v <= TELEMETRIX_REASON_NO_SUCH_SLOT && v != TELEMETRIX_REASON_SKIPPED);
}

telemetrix_reason_t tmx_adapter_plugin_command(tmx_adapter_t *a, const uint8_t *payload, size_t len, uint8_t *code)
{
    if (len < 2) {
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
    if (payload[0] >= a->plugins.n) {
        return TELEMETRIX_REASON_NO_SUCH_SLOT;
    }
    telemetrix_plugin_ctx_t *ctx = &a->plugins.ctx[payload[0]];
    ctx->commands++;
    telemetrix_reason_t why = TELEMETRIX_REASON_NOT_IN_SCOPE;
    if (ctx->plugin->command) {
        uint64_t t0 = timer_start(a);
        why = ctx->plugin->command(ctx, payload[1], payload + 2, len - 2, code);
        timer_stop(a, ctx, t0);
        if (!plugin_reason_ok(why)) {
            why = TELEMETRIX_REASON_PLUGIN;
            *code = 0;
        }
    }
    if (why != TELEMETRIX_REASON_NONE) {
        ctx->refusals++;
    }
    return why;
}

uint32_t tmx_adapter_plugins_poll(tmx_adapter_t *a, uint32_t now_ms)
{
    uint32_t next = UINT32_MAX;
    if (!a->session.extended) {
        return next;
    }
    for (size_t i = 0; i < a->plugins.n; i++) {
        telemetrix_plugin_ctx_t *ctx = &a->plugins.ctx[i];
        if (!ctx->plugin->poll) {
            continue;
        }
        bool woken = __atomic_exchange_n(&ctx->wake, false, __ATOMIC_ACQ_REL);
        if (woken || ctx->poll_first || tmx_deadline_due(&ctx->poll_next, now_ms)) {
            ctx->poll_first = false;
            uint64_t t0 = timer_start(a);
            uint32_t in_ms = ctx->plugin->poll(ctx, now_ms);
            timer_stop(a, ctx, t0);
            if (in_ms == UINT32_MAX) {
                ctx->poll_next.armed = false;
            } else {
                tmx_deadline_arm(&ctx->poll_next, now_ms, in_ms);
            }
        }
        uint32_t left = tmx_deadline_left(&ctx->poll_next, now_ms);
        next = left < next ? left : next;
    }
    return next;
}

void tmx_adapter_plugins_release(tmx_adapter_t *a)
{
    for (size_t i = 0; i < a->plugins.n; i++) {
        telemetrix_plugin_ctx_t *ctx = &a->plugins.ctx[i];
        if (ctx->plugin->release) {
            ctx->plugin->release(ctx);
        }
    }
    for (size_t i = 0; i < a->plugins.n; i++) {
        telemetrix_plugin_ctx_t *ctx = &a->plugins.ctx[i];
        ctx->rmt_rx = ctx->rmt_tx = 0;  /* tmx_claims_release_all forgets the channels next */
        ctx->poll_first = true;
        ctx->poll_next.armed = false;
    }
}
