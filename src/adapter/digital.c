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

bool tmx_digital_mode(tmx_adapter_t *a, uint8_t gpio, tmx_use_t use, bool report)
{
    if (!tmx_adapter_pin_ok(a, gpio, use)) {
        return false;
    }
    int idx = tmx_adapter_pin_index(a, gpio);
    if (is_digital(tmx_pin_use(&a->claims, gpio))) {
        tmx_digital_forget(a, idx);     /* digital uses replace each other without a trip through the safe state */
    } else {
        tmx_adapter_retire(a, gpio);
    }
    if (tmx_claim_pin(&a->claims, gpio, use) != ESP_OK) {
        return false;
    }
    esp_err_t err;
    if (use == TMX_USE_OUTPUT) {
        err = a->drv->gpio_output(gpio, false);
    } else {
        err = a->drv->gpio_input(gpio, use == TMX_USE_INPUT_PULLUP, use == TMX_USE_INPUT_PULLDOWN);
    }
    if (err != ESP_OK) {
        tmx_release_pin(&a->claims, gpio);
        return false;
    }
    a->digital.report[idx] = use != TMX_USE_OUTPUT && report;
    a->digital.fresh[idx] = true;
    return true;
}

bool tmx_digital_write(tmx_adapter_t *a, uint8_t gpio, bool level)
{
    if (tmx_pin_use(&a->claims, gpio) != TMX_USE_OUTPUT) {
        return false;
    }
    a->drv->gpio_set(gpio, level);
    return true;
}

bool tmx_digital_set_report(tmx_adapter_t *a, uint8_t gpio, bool on)
{
    tmx_use_t use = tmx_pin_use(&a->claims, gpio);
    if (use < TMX_USE_INPUT || use > TMX_USE_INPUT_PULLDOWN) {
        return false;
    }
    a->digital.report[tmx_adapter_pin_index(a, gpio)] = on;
    return true;
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
