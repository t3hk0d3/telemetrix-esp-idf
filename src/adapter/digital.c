#include "tmx_adapter.h"

static bool is_digital(tmx_use_t use)
{
    return use >= TMX_USE_INPUT && use <= TMX_USE_OUTPUT;
}

void tmx_digital_forget(tmx_adapter_t *a, int idx)
{
    if (idx < 0) {
        return;
    }
    a->digital.report[idx] = false;
    a->digital.fresh[idx] = false;
    a->digital.last[idx] = false;
}

telemetrix_reason_t tmx_digital_mode(tmx_adapter_t *a, uint8_t gpio, tmx_use_t use, bool report)
{
    telemetrix_reason_t why = tmx_adapter_pin_check(a, gpio, use);
    if (why != TELEMETRIX_REASON_NONE) {
        return why;
    }
    int idx = tmx_adapter_pin_index(a, gpio);
    if (is_digital(tmx_pin_use(&a->claims, gpio))) {
        tmx_digital_forget(a, idx);     /* digital uses replace each other without a trip through the safe state */
    } else {
        tmx_adapter_retire(a, gpio);
    }
    esp_err_t err = tmx_claim_pin(&a->claims, gpio, use);
    if (err != ESP_OK) {
        return tmx_adapter_claim_reason(err);
    }
    if (use == TMX_USE_OUTPUT) {
        err = a->drv->gpio_output(gpio, false);
    } else {
        err = a->drv->gpio_input(gpio, use == TMX_USE_INPUT_PULLUP, use == TMX_USE_INPUT_PULLDOWN);
    }
    if (err != ESP_OK) {
        tmx_release_pin(&a->claims, gpio);
        return TELEMETRIX_REASON_HARDWARE;
    }
    a->digital.report[idx] = use != TMX_USE_OUTPUT && report;
    a->digital.fresh[idx] = true;
    return TELEMETRIX_REASON_NONE;
}

telemetrix_reason_t tmx_digital_write(tmx_adapter_t *a, uint8_t gpio, bool level)
{
    telemetrix_reason_t why = tmx_adapter_pin_state(a, gpio, TMX_USE_OUTPUT);
    if (why != TELEMETRIX_REASON_NONE) {
        return why;
    }
    a->drv->gpio_set(gpio, level);
    return TELEMETRIX_REASON_NONE;
}

telemetrix_reason_t tmx_digital_set_report(tmx_adapter_t *a, uint8_t gpio, bool on)
{
    tmx_use_t use = tmx_pin_use(&a->claims, gpio);
    if (use < TMX_USE_INPUT || use > TMX_USE_INPUT_PULLDOWN) {
        return tmx_adapter_pin_state(a, gpio, TMX_USE_INPUT);   /* never NONE: use is not an input */
    }
    a->digital.report[tmx_adapter_pin_index(a, gpio)] = on;
    return TELEMETRIX_REASON_NONE;
}

bool tmx_digital_wanted(const tmx_adapter_t *a)
{
    for (size_t i = 0; i < a->profile->n_pins; i++) {
        if (a->digital.report[i]) {
            return true;
        }
    }
    return false;
}

void tmx_digital_scan(tmx_adapter_t *a)
{
    for (size_t i = 0; i < a->profile->n_pins; i++) {
        if (!a->digital.report[i]) {
            continue;
        }
        uint8_t gpio = a->profile->pins[i].gpio;
        bool v = a->drv->gpio_get(gpio) != 0;
        if (a->digital.fresh[i] || v != a->digital.last[i]) {
            a->digital.fresh[i] = false;
            a->digital.last[i] = v;
            uint8_t out[TMX_REPORT_MAX];
            size_t n = tmx_rpt_digital(out, gpio, v);
            tmx_adapter_emit(a, out, n, false, TMX_COALESCE_DIGITAL, gpio);
        }
    }
}
