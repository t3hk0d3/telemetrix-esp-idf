#include <string.h>

#include "tmx_adapter.h"

/* A sonar ping or DHT read whose completion never arrives must not stop the sensor for good. */
#define SONAR_LOST_SLOTS 30

static uint32_t token_of(const tmx_adapter_t *a, uint8_t dev)
{
    return (a->sensors.generation << 8) | dev;
}

static uint8_t limit(uint8_t configured)
{
    return configured < TMX_MAX_PINS ? configured : TMX_MAX_PINS;
}

/** An unclaimed profile pin with all the capabilities in need. */
static bool pin_free(const tmx_adapter_t *a, uint8_t gpio, uint16_t need)
{
    const telemetrix_pin_t *pin = tmx_profile_pin(a->profile, gpio);
    return pin && (pin->caps & need) == need && tmx_pin_use(&a->claims, gpio) == TMX_USE_NONE;
}

/* ---- sonar ---- */

bool tmx_sonar_new(tmx_adapter_t *a, uint8_t trig, uint8_t echo, uint32_t now_ms)
{
    tmx_sensors_t *s = &a->sensors;
    if (s->n_sonar >= limit(a->cfg.max_sonar) || trig == echo || !pin_free(a, trig, TELEMETRIX_CAP_DOUT) ||
            !pin_free(a, echo, TELEMETRIX_CAP_DIN)) {
        return false;
    }
    if (tmx_claim_pin(&a->claims, trig, TMX_USE_SONAR_TRIG) != ESP_OK) {
        return false;
    }
    if (tmx_claim_pin(&a->claims, echo, TMX_USE_SONAR_ECHO) != ESP_OK) {
        tmx_release_pin(&a->claims, trig);
        return false;
    }
    s->sonar_trig[s->n_sonar] = trig;
    s->sonar_echo[s->n_sonar] = echo;
    s->n_sonar++;
    if (!s->sonar_slot.armed) {
        tmx_deadline_arm(&s->sonar_slot, now_ms, 0);
    }
    return true;
}

static void sonar_slot(tmx_adapter_t *a, uint32_t now_ms)
{
    tmx_sensors_t *s = &a->sensors;
    tmx_deadline_arm(&s->sonar_slot, now_ms, TMX_SONAR_SLOT_MS);
    if (s->sonar_busy) {
        if (++s->sonar_lost >= SONAR_LOST_SLOTS) {
            s->sonar_busy = false;
        }
        return;
    }
    uint8_t dev = s->sonar_next_dev < s->n_sonar ? s->sonar_next_dev : 0;
    s->sonar_next_dev = (uint8_t)((dev + 1) % s->n_sonar);
    if (a->drv->sonar_start(s->sonar_trig[dev], s->sonar_echo[dev], token_of(a, dev)) == ESP_OK) {
        s->sonar_busy = true;
        s->sonar_busy_dev = dev;
        s->sonar_lost = 0;
    }
}

static void sonar_done(tmx_adapter_t *a, const tmx_completion_t *c)
{
    tmx_sensors_t *s = &a->sensors;
    uint8_t dev = (uint8_t)(c->token & 0xff);
    if (!s->sonar_busy || dev != s->sonar_busy_dev) {
        return;
    }
    s->sonar_busy = false;
    if (!a->reporting || c->err != ESP_OK) {
        return;
    }
    uint32_t cm = c->value / 58;
    if (cm > UINT16_MAX) {
        cm = UINT16_MAX;
    }
    if (s->sonar_has_last[dev] && s->sonar_last[dev] == cm) {
        return;
    }
    s->sonar_has_last[dev] = true;
    s->sonar_last[dev] = (uint16_t)cm;
    uint8_t out[TMX_REPORT_MAX];
    tmx_adapter_emit(a, out, tmx_rpt_sonar(out, s->sonar_trig[dev], (uint16_t)cm), false, TMX_COALESCE_SONAR, dev);
}

/* ---- DHT ---- */

bool tmx_dht_new(tmx_adapter_t *a, uint8_t pin, uint32_t now_ms)
{
    tmx_sensors_t *s = &a->sensors;
    if (s->n_dht >= limit(a->cfg.max_dht) || !pin_free(a, pin, TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT)) {
        return false;
    }
    bool claimed_rmt = false;
    if (!s->dht_rmt) {     /* the DHTs share one RX channel */
        if (tmx_claim_rmt_rx(&a->claims) != ESP_OK) {
            return false;
        }
        claimed_rmt = true;
    }
    if (tmx_claim_pin(&a->claims, pin, TMX_USE_DHT) != ESP_OK) {
        if (claimed_rmt) {
            tmx_release_rmt_rx(&a->claims);
        }
        return false;
    }
    s->dht_rmt = true;
    s->dht_pin[s->n_dht++] = pin;
    if (!s->dht_cycle) {    /* the first read of a new device is not left waiting for the 2000 ms pass */
        tmx_deadline_arm(&s->dht_pass, now_ms, 0);
    }
    return true;
}

static void dht_error(tmx_adapter_t *a, uint8_t dev, uint8_t err)
{
    uint8_t out[TMX_REPORT_MAX];
    tmx_adapter_emit(a, out, tmx_rpt_dht_error(out, a->sensors.dht_pin[dev], err), true, TMX_COALESCE_NONE, 0);
}

/** Start the read of the device at the cursor; a device whose start fails gets its error report and the next is tried. */
static void dht_advance(tmx_adapter_t *a)
{
    tmx_sensors_t *s = &a->sensors;
    while (s->dht_cursor < s->n_dht) {
        uint8_t dev = s->dht_cursor;
        if (a->drv->dht_start(s->dht_pin[dev], token_of(a, dev)) == ESP_OK) {
            s->dht_busy = true;
            return;
        }
        dht_error(a, dev, 2);
        s->dht_cursor++;
    }
    s->dht_busy = false;
    s->dht_cycle = false;
}

static void dht_report(tmx_adapter_t *a, uint8_t dev, const tmx_completion_t *c)
{
    const uint8_t *f = c->data;
    if (c->err != ESP_OK) {
        dht_error(a, dev, 2);
        return;
    }
    if ((uint8_t)(f[0] + f[1] + f[2] + f[3]) != f[4]) {
        dht_error(a, dev, 1);
        return;
    }
    float hum = (float)((f[0] << 8) | f[1]) / 10.0f;
    float temp = (float)(((f[2] & 0x7f) << 8) | f[3]) / 10.0f;
    if (f[2] & 0x80) {
        temp = -temp;
    }
    uint8_t out[TMX_REPORT_MAX];
    tmx_adapter_emit(a, out, tmx_rpt_dht_data(out, a->sensors.dht_pin[dev], hum, temp), false, TMX_COALESCE_DHT, dev);
}

static void dht_done(tmx_adapter_t *a, const tmx_completion_t *c)
{
    tmx_sensors_t *s = &a->sensors;
    uint8_t dev = (uint8_t)(c->token & 0xff);
    if (!s->dht_busy || dev != s->dht_cursor) {
        return;
    }
    s->dht_busy = false;
    if (a->reporting) {
        dht_report(a, dev, c);
    }
    s->dht_cursor++;
    dht_advance(a);
}

static void dht_pass(tmx_adapter_t *a, uint32_t now_ms)
{
    tmx_sensors_t *s = &a->sensors;
    tmx_deadline_arm(&s->dht_pass, now_ms, TMX_DHT_PASS_MS);
    s->dht_cycle = true;        /* a pass still running after 2000 ms lost its completion: start over */
    s->dht_cursor = 0;
    s->dht_busy = false;
    dht_advance(a);
}

/* ---- common ---- */

static void completion(tmx_adapter_t *a, const tmx_completion_t *c)
{
    tmx_sensors_t *s = &a->sensors;
    if ((c->token >> 8) != (s->generation & 0xffffff)) {
        return;     /* a device released by RESET or a disconnect */
    }
    uint8_t dev = (uint8_t)(c->token & 0xff);
    if (c->kind == TMX_COMPLETION_SONAR && dev < s->n_sonar) {
        sonar_done(a, c);
    } else if (c->kind == TMX_COMPLETION_DHT && dev < s->n_dht) {
        dht_done(a, c);
    }
}

void tmx_sensors_poll(tmx_adapter_t *a, uint32_t now_ms)
{
    tmx_sensors_t *s = &a->sensors;
    tmx_completion_t c;
    while (tmx_completion_take(&c)) {
        completion(a, &c);
    }
    if (!a->reporting) {
        return;
    }
    if (s->n_sonar > 0 && tmx_deadline_due(&s->sonar_slot, now_ms)) {
        sonar_slot(a, now_ms);
    }
    if (s->n_dht > 0 && tmx_deadline_due(&s->dht_pass, now_ms)) {
        dht_pass(a, now_ms);
    }
}

uint32_t tmx_sensors_left(const tmx_adapter_t *a, uint32_t now_ms)
{
    const tmx_sensors_t *s = &a->sensors;
    uint32_t left = UINT32_MAX;
    if (!a->reporting) {
        return left;
    }
    if (s->n_sonar > 0) {
        left = tmx_deadline_left(&s->sonar_slot, now_ms);
    }
    if (s->n_dht > 0) {
        uint32_t dht = tmx_deadline_left(&s->dht_pass, now_ms);
        left = dht < left ? dht : left;
    }
    return left;
}

void tmx_sensors_release(tmx_adapter_t *a)
{
    uint32_t generation = a->sensors.generation + 1;
    memset(&a->sensors, 0, sizeof(a->sensors));
    a->sensors.generation = generation;
}
