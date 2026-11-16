#include <string.h>

#include "tmx_core.h"

#define RESERVED_PREFIX "telemetrix."

static bool name_char(char ch, bool dot_ok)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || (dot_ok && ch == '.');
}

/** ^[a-z0-9_]+\.[a-z0-9_.]+$, at most 32 bytes, not in the reserved telemetrix. namespace. */
static bool name_ok(const char *name)
{
    if (!name || strnlen(name, TMX_PLUGIN_NAME_MAX + 1) > TMX_PLUGIN_NAME_MAX ||
            strncmp(name, RESERVED_PREFIX, strlen(RESERVED_PREFIX)) == 0) {
        return false;
    }
    const char *s = name;
    while (name_char(*s, false)) {
        s++;
    }
    if (s == name || *s != '.') {
        return false;
    }
    const char *rest = ++s;
    while (name_char(*s, true)) {
        s++;
    }
    return s != rest && *s == '\0';
}

int tmx_plugins_named_id(const tmx_plugins_t *r, const char *name)
{
    for (size_t i = 0; name && i < r->n_named; i++) {
        if (strcmp(r->named[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t add_named(tmx_plugins_t *r, const telemetrix_plugin_t *p)
{
    if (p->n_named_caps && !p->named_caps) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < p->n_named_caps; i++) {
        const char *name = p->named_caps[i];
        if (!name || strnlen(name, TMX_NAMED_NAME_MAX + 1) > TMX_NAMED_NAME_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        if (tmx_plugins_named_id(r, name) >= 0) {
            continue;
        }
        if (r->n_named >= TMX_NAMED_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        r->named[r->n_named++] = name;
    }
    return ESP_OK;
}

static esp_err_t add_plugin(tmx_plugins_t *r, const telemetrix_plugin_t *p)
{
    if (!p || !name_ok(p->name)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < r->n; i++) {
        if (strcmp(r->ctx[i].plugin->name, p->name) == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    esp_err_t err = add_named(r, p);
    if (err != ESP_OK) {
        return err;
    }
    telemetrix_plugin_ctx_t *ctx = &r->ctx[r->n];
    ctx->plugin = p;
    ctx->slot = (uint8_t)r->n;
    ctx->poll_first = true;
    r->n++;
    return ESP_OK;
}

esp_err_t tmx_plugins_init(tmx_plugins_t *r, const telemetrix_plugin_t *const *list, size_t n)
{
    if (!r) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(r, 0, sizeof(*r));
    if (n > TELEMETRIX_PLUGINS_MAX || (n && !list)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < n; i++) {
        esp_err_t err = add_plugin(r, list[i]);
        if (err != ESP_OK) {
            memset(r, 0, sizeof(*r));
            return err;
        }
    }
    return ESP_OK;
}

void tmx_plugins_account(telemetrix_plugin_ctx_t *ctx, uint64_t us, uint32_t budget_us)
{
    uint32_t clamped = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
    if (clamped > ctx->max_us) {
        ctx->max_us = clamped;
    }
    if (us > budget_us) {
        ctx->overruns++;
    }
}

size_t tmx_plugins_status(const tmx_plugins_t *r, telemetrix_plugin_status_t *out, size_t cap)
{
    size_t n = r->n < cap ? r->n : cap;
    for (size_t i = 0; i < n; i++) {
        const telemetrix_plugin_ctx_t *ctx = &r->ctx[i];
        telemetrix_plugin_status_t *st = &out[i];
        memset(st, 0, sizeof(*st));
        strncpy(st->name, ctx->plugin->name, sizeof(st->name) - 1);
        st->slot = ctx->slot;
        st->commands = ctx->commands;
        st->refusals = ctx->refusals;
        st->overruns = ctx->overruns;
        st->max_us = ctx->max_us;
        st->async_dropped = __atomic_load_n(&ctx->async_dropped, __ATOMIC_RELAXED);
        memcpy(st->text, ctx->text, sizeof(st->text));
        st->text[sizeof(st->text) - 1] = '\0';
    }
    return n;
}
