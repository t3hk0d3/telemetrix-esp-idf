#include "tmx_adapter.h"

#define I2C_READ_MAX 59

/** Profile index of a bus number, -1 when the profile has no such bus. */
static int bus_index(const tmx_adapter_t *a, uint8_t bus)
{
    for (size_t i = 0; i < a->profile->n_i2c; i++) {
        if (a->profile->i2c[i].bus == bus) {
            return (int)i;
        }
    }
    return -1;
}

static telemetrix_reason_t begin(tmx_adapter_t *a, int idx, const uint8_t *p, size_t len)
{
    const telemetrix_i2c_bus_t *grant = &a->profile->i2c[idx];
    uint8_t bus = grant->bus;
    if (a->i2c.open[idx]) {
        return TELEMETRIX_REASON_NONE;
    }
    if (grant->handle) {
        /* shared with the application: its pins are not profile pins (validated), so nothing is claimed or driven */
        if (a->drv->i2c_open(bus, grant, grant->sda, grant->scl) != ESP_OK) {
            return TELEMETRIX_REASON_HARDWARE;
        }
        a->i2c.open[idx] = true;
        return TELEMETRIX_REASON_NONE;
    }
    int sda = grant->sda;
    int scl = grant->scl;
    if (sda < 0 || scl < 0) {   /* the client chooses; [] and [0,0] name no pins */
        if (len < 2 || (p[0] == 0 && p[1] == 0)) {
            return TELEMETRIX_REASON_BAD_ARGUMENT;
        }
        sda = p[0];
        scl = p[1];
    }
    if (sda == scl) {
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
    telemetrix_reason_t why = tmx_adapter_pin_free(a, sda, TELEMETRIX_CAP_I2C);
    if (why == TELEMETRIX_REASON_NONE) {
        why = tmx_adapter_pin_free(a, scl, TELEMETRIX_CAP_I2C);
    }
    if (why != TELEMETRIX_REASON_NONE) {
        return why;
    }
    esp_err_t err = tmx_claim_pin(&a->claims, (uint8_t)sda, TMX_USE_I2C);
    if (err != ESP_OK) {
        return tmx_adapter_claim_reason(err);
    }
    err = tmx_claim_pin(&a->claims, (uint8_t)scl, TMX_USE_I2C);
    if (err != ESP_OK || a->drv->i2c_open(bus, grant, (int8_t)sda, (int8_t)scl) != ESP_OK) {
        tmx_release_pin(&a->claims, (uint8_t)sda);
        tmx_release_pin(&a->claims, (uint8_t)scl);
        return err != ESP_OK ? tmx_adapter_claim_reason(err) : TELEMETRIX_REASON_HARDWARE;
    }
    a->i2c.open[idx] = true;
    return TELEMETRIX_REASON_NONE;
}

static telemetrix_reason_t i2c_read_cmd(tmx_adapter_t *a, int idx, const uint8_t *p)
{
    size_t count = p[2] < I2C_READ_MAX ? p[2] : I2C_READ_MAX;
    uint8_t bus = a->profile->i2c[idx].bus;
    if (!a->i2c.open[idx] || count == 0) {
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
    uint8_t data[I2C_READ_MAX];
    uint8_t out[TMX_REPORT_MAX];
    if (a->drv->i2c_read(bus, p[0], p[1], p[3] != 0, data, count, a->cfg.i2c_timeout_ms) != ESP_OK) {
        return TELEMETRIX_REASON_NO_ANSWER;     /* the caller reports it: upstream's [3,8,1,addr] or an ERROR */
    }
    tmx_adapter_emit(a, out, tmx_rpt_i2c_read(out, p[0], p[1], data, count), true, TMX_COALESCE_NONE, 0);
    return TELEMETRIX_REASON_NONE;
}

static telemetrix_reason_t i2c_write_cmd(tmx_adapter_t *a, int idx, const uint8_t *p)
{
    if (!a->i2c.open[idx]) {
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
    /* upstream sends nothing when the device does not answer; the refusal counter is the only trace */
    return a->drv->i2c_write(a->profile->i2c[idx].bus, p[1], p + 2, p[0], a->cfg.i2c_timeout_ms) == ESP_OK ?
           TELEMETRIX_REASON_NONE : TELEMETRIX_REASON_NO_ANSWER;
}

telemetrix_reason_t tmx_i2c_command(tmx_adapter_t *a, uint8_t bus, uint8_t id, const uint8_t *p, size_t len)
{
    int idx = bus_index(a, bus);
    if (idx < 0) {
        return TELEMETRIX_REASON_BUS_UNKNOWN;
    }
    switch (id) {
    case TMX_CMD_I2C_BEGIN: return begin(a, idx, p, len);
    case TMX_CMD_I2C_READ: return i2c_read_cmd(a, idx, p);
    default: return i2c_write_cmd(a, idx, p);
    }
}

void tmx_i2c_release(tmx_adapter_t *a)
{
    for (size_t i = 0; i < a->profile->n_i2c; i++) {
        if (a->i2c.open[i]) {
            a->drv->i2c_close(a->profile->i2c[i].bus);   /* an application-owned handle is detached, not deleted */
        }
        a->i2c.open[i] = false;
    }
}
