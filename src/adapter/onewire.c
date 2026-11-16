#include <string.h>

#include "tmx_adapter.h"

#define OW_MATCH_ROM 0x55
#define OW_SKIP_ROM 0xCC

/** Profile index of a bus number, -1 when the profile has no such bus. */
static int bus_index(const tmx_adapter_t *a, uint8_t bus)
{
    for (size_t i = 0; i < a->profile->n_onewire; i++) {
        if (a->profile->onewire[i].bus == bus) {
            return (int)i;
        }
    }
    return -1;
}

static telemetrix_reason_t init(tmx_adapter_t *a, int idx, const uint8_t *p)
{
    const telemetrix_onewire_bus_t *grant = &a->profile->onewire[idx];
    if (a->onewire.open[idx]) {
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
    if (a->drv->ow_open(grant->bus, (uint8_t)gpio) != ESP_OK) {
        tmx_release_rmt_rx(&a->claims);
        tmx_release_pin(&a->claims, (uint8_t)gpio);
        return TELEMETRIX_REASON_HARDWARE;
    }
    a->onewire.open[idx] = true;
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

static void search(tmx_adapter_t *a, uint8_t bus)
{
    uint8_t rom[8];
    if (!a->drv->ow_search(bus, rom)) {
        memset(rom, 0xff, sizeof(rom));
    }
    reply(a, TMX_CMD_ONE_WIRE_SEARCH, rom, sizeof(rom));
}

telemetrix_reason_t tmx_onewire_command(tmx_adapter_t *a, uint8_t bus, uint8_t id, const uint8_t *p, size_t len)
{
    (void)len;
    const tmx_drv_t *d = a->drv;
    if (id == TMX_CMD_ONE_WIRE_CRC8) {     /* [n, bytes...]: pure computation, no bus needed; the stock extra byte is ignored */
        uint8_t crc = crc8(p + 1, p[0]);
        reply(a, id, &crc, 1);
        return TELEMETRIX_REASON_NONE;
    }
    int idx = bus_index(a, bus);
    if (idx < 0) {
        return TELEMETRIX_REASON_BUS_UNKNOWN;
    }
    if (id == TMX_CMD_ONE_WIRE_INIT) {
        return init(a, idx, p);
    }
    if (!a->onewire.open[idx]) {
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
    switch (id) {
    case TMX_CMD_ONE_WIRE_RESET: {
        uint8_t present = d->ow_reset(bus) ? 1 : 0;
        reply(a, id, &present, 1);
        break;
    }
    case TMX_CMD_ONE_WIRE_SELECT:
        d->ow_write(bus, OW_MATCH_ROM, false);
        for (int i = 0; i < 8; i++) {
            d->ow_write(bus, p[i], false);
        }
        break;
    case TMX_CMD_ONE_WIRE_SKIP:
        d->ow_write(bus, OW_SKIP_ROM, false);
        break;
    case TMX_CMD_ONE_WIRE_WRITE:
        d->ow_write(bus, p[0], p[1] != 0);
        break;
    case TMX_CMD_ONE_WIRE_READ: {
        uint8_t byte = d->ow_read(bus);
        reply(a, id, &byte, 1);
        break;
    }
    case TMX_CMD_ONE_WIRE_RESET_SEARCH:
        d->ow_reset_search(bus);
        break;
    default:
        search(a, bus);
        break;
    }
    return TELEMETRIX_REASON_NONE;
}

void tmx_onewire_release(tmx_adapter_t *a)
{
    for (size_t i = 0; i < a->profile->n_onewire; i++) {
        if (a->onewire.open[i]) {
            a->drv->ow_close(a->profile->onewire[i].bus);
        }
        a->onewire.open[i] = false;
    }
}
