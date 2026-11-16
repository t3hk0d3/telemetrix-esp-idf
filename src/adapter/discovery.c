#include "tmx_adapter.h"

/* The records of GET_CAPABILITIES, in order: CHIP, NAMED_CAP..., PIN..., BUS... (I2C then 1-wire), LIMIT 1-6,
 * PLUGIN... A record's index selects its segment. */

#define N_LIMITS 6

static unsigned popcount(uint32_t v)
{
    unsigned c = 0;
    for (; v; v &= v - 1) {
        c++;
    }
    return c;
}

static uint16_t limit_count(const tmx_adapter_t *a, uint8_t kind)
{
    const telemetrix_profile_t *p = a->profile;
    const unsigned channels = popcount(p->ledc.channel_mask);
    switch (kind) {
    case TMX_CAPS_LIMIT_SERVOS:
        return (uint16_t)(a->cfg.max_servo < channels ? a->cfg.max_servo : channels);
    case TMX_CAPS_LIMIT_PWM_CHANNELS:
        return (uint16_t)channels;
    case TMX_CAPS_LIMIT_PWM_TIMERS:
        return (uint16_t)popcount(p->ledc.timer_mask);
    case TMX_CAPS_LIMIT_DHT:
        return p->rmt.rx_channels > 0 ? a->cfg.max_dht : 0;
    case TMX_CAPS_LIMIT_SONAR:
        return a->cfg.max_sonar;
    default:
        return a->cfg.analog_interval_floor_ms;
    }
}

static size_t pin_record(const tmx_adapter_t *a, size_t i, uint8_t *rec)
{
    const telemetrix_pin_t *pin = &a->profile->pins[i];
    uint8_t ids[TMX_NAMED_MAX];
    size_t n = 0;
    for (size_t k = 0; k < pin->n_named && n < sizeof(ids); k++) {
        int id = tmx_plugins_named_id(&a->plugins, pin->named[k]);
        if (id >= 0) {
            ids[n++] = (uint8_t)id;
        }
    }
    return tmx_caps_pin(rec, pin->gpio, pin->caps, (uint8_t)pin->safe, pin->adc_unit, pin->adc_channel, ids, n);
}

size_t tmx_discovery_record(size_t i, uint8_t *rec, void *adapter)
{
    const tmx_adapter_t *a = adapter;
    const telemetrix_profile_t *p = a->profile;
    if (i == 0) {
        uint16_t model = 0, revision = 0;
        char idf[TMX_IDF_VERSION_MAX + 1] = "";
        a->drv->chip_info(&model, &revision, idf, sizeof(idf));
        return tmx_caps_chip(rec, model, revision, idf);
    }
    i--;
    if (i < a->plugins.n_named) {
        return tmx_caps_named(rec, (uint8_t)i, a->plugins.named[i]);
    }
    i -= a->plugins.n_named;
    if (i < p->n_pins) {
        return pin_record(a, i, rec);
    }
    i -= p->n_pins;
    if (i < p->n_i2c) {
        const telemetrix_i2c_bus_t *b = &p->i2c[i];
        return tmx_caps_bus_i2c(rec, b->bus, b->sda, b->scl, b->speed_hz);
    }
    i -= p->n_i2c;
    if (i < p->n_onewire) {
        return tmx_caps_bus_onewire(rec, p->onewire[i].bus, p->onewire[i].gpio);
    }
    i -= p->n_onewire;
    if (i < N_LIMITS) {
        const uint8_t kind = (uint8_t)(i + 1);
        return tmx_caps_limit(rec, kind, limit_count(a, kind));
    }
    i -= N_LIMITS;
    if (i < a->plugins.n) {
        const telemetrix_plugin_t *pl = a->plugins.ctx[i].plugin;
        return tmx_caps_plugin(rec, (uint8_t)i, pl->major, pl->minor, pl->name);
    }
    return 0;
}

telemetrix_reason_t tmx_discovery_command(tmx_adapter_t *a, const uint8_t *payload, size_t len)
{
    static uint8_t report[TMX_EXT_REPORT_MAX];     /* network task only */
    size_t n = 0;
    if (len < 1 || !tmx_caps_page(tmx_discovery_record, a, payload[0], report, &n)) {
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
    a->emit(report, n, true, TMX_COALESCE_NONE, 0, a->ctx);
    return TELEMETRIX_REASON_NONE;
}
