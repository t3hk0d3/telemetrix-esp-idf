#include <string.h>

#include "tmx_adapter.h"

#define STOCK_BUS 0
#define OW_MATCH_ROM 0x55
#define OW_SKIP_ROM 0xCC

static const telemetrix_onewire_bus_t *stock_grant(const tmx_adapter_t *a)
{
    for (size_t i = 0; i < a->profile->n_onewire; i++) {
        if (a->profile->onewire[i].bus == STOCK_BUS) {
            return &a->profile->onewire[i];
        }
    }
    return NULL;
}

static telemetrix_reason_t init(tmx_adapter_t *a, const uint8_t *p)
{
    const telemetrix_onewire_bus_t *grant = stock_grant(a);
    if (!grant) {
        return TELEMETRIX_REASON_BUS_UNKNOWN;
    }
    if (a->onewire.open) {
        return TELEMETRIX_REASON_NONE;
    }
    int gpio = grant->gpio >= 0 ? grant->gpio : p[0];
    telemetrix_reason_t why = tmx_adapter_pin_free(a, gpio, TELEMETRIX_CAP_ONE_WIRE);
    if (why != TELEMETRIX_REASON_NONE) {
        return why;
    }
    esp_err_t err = tmx_claim_pin(&a->claims, (uint8_t)gpio, TMX_USE_ONE_WIRE);
    if (err != ESP_OK) {
        return tmx_adapter_claim_reason(err);
    }
    if (tmx_claim_rmt_rx(&a->claims) != ESP_OK) {
        tmx_release_pin(&a->claims, (uint8_t)gpio);
        return TELEMETRIX_REASON_EXHAUSTED;
    }
    if (a->drv->ow_open(STOCK_BUS, (uint8_t)gpio) != ESP_OK) {
        tmx_release_rmt_rx(&a->claims);
        tmx_release_pin(&a->claims, (uint8_t)gpio);
        return TELEMETRIX_REASON_HARDWARE;
    }
    a->onewire.open = true;
    return TELEMETRIX_REASON_NONE;
}

/** Dallas/Maxim CRC-8: polynomial x^8 + x^5 + x^4 + 1, reflected, initial value 0. */
static uint8_t crc8(const uint8_t *data, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t byte = data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint8_t mix = (crc ^ byte) & 1;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            byte >>= 1;
        }
    }
    return crc;
}

static void reply(tmx_adapter_t *a, uint8_t subtype, const uint8_t *data, size_t n)
{
    uint8_t out[TMX_REPORT_MAX];
    tmx_adapter_emit(a, out, tmx_rpt_onewire(out, subtype, data, n), true, TMX_COALESCE_NONE, 0);
}

static void search(tmx_adapter_t *a)
{
    uint8_t rom[8];
    if (!a->drv->ow_search(STOCK_BUS, rom)) {
        memset(rom, 0xff, sizeof(rom));
    }
    reply(a, TMX_CMD_ONE_WIRE_SEARCH, rom, sizeof(rom));
}

telemetrix_reason_t tmx_onewire_command(tmx_adapter_t *a, uint8_t id, const uint8_t *p, size_t len)
{
    (void)len;
    const tmx_drv_t *d = a->drv;
    if (id == TMX_CMD_ONE_WIRE_INIT) {
        return init(a, p);
    }
    if (id == TMX_CMD_ONE_WIRE_CRC8) {     /* [n, bytes...]: the stock client's extra byte is ignored */
        uint8_t crc = crc8(p + 1, p[0]);
        reply(a, id, &crc, 1);
        return TELEMETRIX_REASON_NONE;
    }
    if (!a->onewire.open) {
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
    switch (id) {
    case TMX_CMD_ONE_WIRE_RESET: {
        uint8_t present = d->ow_reset(STOCK_BUS) ? 1 : 0;
        reply(a, id, &present, 1);
        break;
    }
    case TMX_CMD_ONE_WIRE_SELECT:
        d->ow_write(STOCK_BUS, OW_MATCH_ROM, false);
        for (int i = 0; i < 8; i++) {
            d->ow_write(STOCK_BUS, p[i], false);
        }
        break;
    case TMX_CMD_ONE_WIRE_SKIP:
        d->ow_write(STOCK_BUS, OW_SKIP_ROM, false);
        break;
    case TMX_CMD_ONE_WIRE_WRITE:
        d->ow_write(STOCK_BUS, p[0], p[1] != 0);
        break;
    case TMX_CMD_ONE_WIRE_READ: {
        uint8_t byte = d->ow_read(STOCK_BUS);
        reply(a, id, &byte, 1);
        break;
    }
    case TMX_CMD_ONE_WIRE_RESET_SEARCH:
        d->ow_reset_search(STOCK_BUS);
        break;
    default:
        search(a);
        break;
    }
    return TELEMETRIX_REASON_NONE;
}

void tmx_onewire_release(tmx_adapter_t *a)
{
    if (a->onewire.open) {
        a->drv->ow_close(STOCK_BUS);
    }
    a->onewire.open = false;
}
