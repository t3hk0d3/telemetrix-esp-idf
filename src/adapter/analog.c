#include "tmx_adapter.h"

uint32_t tmx_analog_default_interval(const tmx_adapter_t *a)
{
    uint32_t floor_ms = a->cfg.analog_interval_floor_ms;
    return floor_ms > TMX_ANALOG_DEFAULT_MS ? floor_ms : TMX_ANALOG_DEFAULT_MS;
}

void tmx_analog_forget(tmx_adapter_t *a, int idx)
{
    if (idx < 0) {
        return;
    }
    a->analog.report[idx] = false;
    a->analog.fresh[idx] = false;
    a->analog.differential[idx] = 0;
    a->analog.last[idx] = 0;
}

telemetrix_reason_t tmx_analog_mode(tmx_adapter_t *a, uint8_t gpio, uint16_t differential, bool report)
{
    telemetrix_reason_t why = tmx_adapter_pin_check(a, gpio, TMX_USE_ANALOG);
    if (why != TELEMETRIX_REASON_NONE) {
        return why;
    }
    int idx = tmx_adapter_pin_index(a, gpio);
    if (tmx_pin_use(&a->claims, gpio) == TMX_USE_ANALOG) {
        tmx_analog_forget(a, idx);
    } else {
        tmx_adapter_retire(a, gpio);
        esp_err_t err = tmx_claim_pin(&a->claims, gpio, TMX_USE_ANALOG);
        if (err != ESP_OK) {
            return tmx_adapter_claim_reason(err);
        }
    }
    a->analog.report[idx] = report;
    a->analog.fresh[idx] = true;
    a->analog.differential[idx] = differential;
    return TELEMETRIX_REASON_NONE;
}

telemetrix_reason_t tmx_analog_set_report(tmx_adapter_t *a, uint8_t gpio, bool on)
{
    telemetrix_reason_t why = tmx_adapter_pin_state(a, gpio, TMX_USE_ANALOG);
    if (why != TELEMETRIX_REASON_NONE) {
        return why;
    }
    a->analog.report[tmx_adapter_pin_index(a, gpio)] = on;
    return TELEMETRIX_REASON_NONE;
}

bool tmx_analog_wanted(const tmx_adapter_t *a)
{
    for (size_t i = 0; i < a->profile->n_pins; i++) {
        if (a->analog.report[i]) {
            return true;
        }
    }
    return false;
}

void tmx_analog_set_interval(tmx_adapter_t *a, uint32_t ms)
{
    uint32_t floor_ms = a->cfg.analog_interval_floor_ms;
    a->analog.interval_ms = ms < floor_ms ? floor_ms : ms;
}

void tmx_analog_scan(tmx_adapter_t *a)
{
    for (size_t i = 0; i < a->profile->n_pins; i++) {
        const telemetrix_pin_t *pin = &a->profile->pins[i];
        int raw = 0;
        if (!a->analog.report[i] || a->drv->adc_read((uint8_t)pin->adc_unit, (uint8_t)pin->adc_channel, &raw) != ESP_OK) {
            continue;
        }
        uint16_t v = (uint16_t)(raw < 0 ? 0 : (raw > UINT16_MAX ? UINT16_MAX : raw));
        uint16_t last = a->analog.last[i];
        uint16_t delta = v > last ? (uint16_t)(v - last) : (uint16_t)(last - v);
        if (a->analog.fresh[i] || delta >= a->analog.differential[i]) {
            a->analog.fresh[i] = false;
            a->analog.last[i] = v;
            uint8_t out[TMX_REPORT_MAX];
            size_t n = tmx_rpt_analog(out, pin->gpio, v);
            tmx_adapter_emit(a, out, n, false, TMX_COALESCE_ANALOG, pin->gpio);
        }
    }
}
