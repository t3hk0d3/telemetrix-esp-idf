#include "tmx_adapter.h"

#define STOCK_BUS 0
#define I2C_READ_MAX 59

static const telemetrix_i2c_bus_t *stock_grant(const tmx_adapter_t *a)
{
    for (size_t i = 0; i < a->profile->n_i2c; i++) {
        if (a->profile->i2c[i].bus == STOCK_BUS) {
            return &a->profile->i2c[i];
        }
    }
    return NULL;
}

/** An unclaimed profile pin with the I2C capability. */
static bool pin_usable(const tmx_adapter_t *a, int gpio)
{
    if (gpio < 0 || gpio > UINT8_MAX) {
        return false;
    }
    const telemetrix_pin_t *pin = tmx_profile_pin(a->profile, (uint8_t)gpio);
    return pin && (pin->caps & TELEMETRIX_CAP_I2C) && tmx_pin_use(&a->claims, (uint8_t)gpio) == TMX_USE_NONE;
}

static bool begin(tmx_adapter_t *a, const uint8_t *p, size_t len)
{
    const telemetrix_i2c_bus_t *grant = stock_grant(a);
    if (!grant) {
        return false;
    }
    if (a->i2c.open) {
        return true;
    }
    if (grant->handle) {
        /* shared with the application: its pins are not profile pins (validated), so nothing is claimed or driven */
        if (a->drv->i2c_open(STOCK_BUS, grant, grant->sda, grant->scl) != ESP_OK) {
            return false;
        }
        a->i2c.open = true;
        return true;
    }
    int sda = grant->sda;
    int scl = grant->scl;
    if (sda < 0 || scl < 0) {   /* the client chooses; [] and [0,0] name no pins */
        if (len < 2 || (p[0] == 0 && p[1] == 0)) {
            return false;
        }
        sda = p[0];
        scl = p[1];
    }
    if (sda == scl || !pin_usable(a, sda) || !pin_usable(a, scl)) {
        return false;
    }
    if (tmx_claim_pin(&a->claims, (uint8_t)sda, TMX_USE_I2C) != ESP_OK) {
        return false;
    }
    if (tmx_claim_pin(&a->claims, (uint8_t)scl, TMX_USE_I2C) != ESP_OK ||
            a->drv->i2c_open(STOCK_BUS, grant, (int8_t)sda, (int8_t)scl) != ESP_OK) {
        tmx_release_pin(&a->claims, (uint8_t)sda);
        tmx_release_pin(&a->claims, (uint8_t)scl);
        return false;
    }
    a->i2c.open = true;
    return true;
}

static bool i2c_read_cmd(tmx_adapter_t *a, const uint8_t *p)
{
    size_t count = p[2] < I2C_READ_MAX ? p[2] : I2C_READ_MAX;
    if (!a->i2c.open || count == 0) {
        return false;
    }
    uint8_t data[I2C_READ_MAX];
    uint8_t out[TMX_REPORT_MAX];
    esp_err_t err = a->drv->i2c_read(STOCK_BUS, p[0], p[1], p[3] != 0, data, count, a->cfg.i2c_timeout_ms);
    size_t n = err == ESP_OK ? tmx_rpt_i2c_read(out, p[0], p[1], data, count) : tmx_rpt_i2c_short(out, p[0]);
    tmx_adapter_emit(a, out, n, true, TMX_COALESCE_NONE, 0);
    return true;
}

static bool i2c_write_cmd(tmx_adapter_t *a, const uint8_t *p)
{
    if (!a->i2c.open) {
        return false;
    }
    /* upstream sends nothing when the device does not answer; the refusal counter is the only trace */
    return a->drv->i2c_write(STOCK_BUS, p[1], p + 2, p[0], a->cfg.i2c_timeout_ms) == ESP_OK;
}

bool tmx_i2c_command(tmx_adapter_t *a, uint8_t id, const uint8_t *p, size_t len)
{
    switch (id) {
    case TMX_CMD_I2C_BEGIN: return begin(a, p, len);
    case TMX_CMD_I2C_READ: return i2c_read_cmd(a, p);
    default: return i2c_write_cmd(a, p);
    }
}

void tmx_i2c_release(tmx_adapter_t *a)
{
    if (a->i2c.open) {
        a->drv->i2c_close(STOCK_BUS);   /* the driver detaches rather than deletes an application-owned handle */
    }
    a->i2c.open = false;
}
