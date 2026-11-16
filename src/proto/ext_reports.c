#include <string.h>

#include "tmx_proto.h"

#define PLUGIN_DATA_MAX 251

size_t tmx_rpt_firmware_long(uint8_t *out, uint8_t major, uint8_t minor)
{
    const uint8_t r[] = {7, TMX_RPT_FIRMWARE_REPORT, 3, 2, 100, TMX_EXT_MAGIC, major, minor};
    memcpy(out, r, sizeof(r));
    return sizeof(r);
}

size_t tmx_rpt_error(uint8_t *out, uint16_t seq, uint8_t cmd, uint8_t reason, int16_t slot, uint8_t sub, int16_t code)
{
    size_t n = 1;
    out[n++] = TMX_RPT_ERROR;
    out[n++] = (uint8_t)(seq >> 8);
    out[n++] = (uint8_t)seq;
    out[n++] = cmd;
    out[n++] = reason;
    if (slot >= 0) {
        out[n++] = (uint8_t)slot;
        out[n++] = sub;
        if (code >= 0) {
            out[n++] = (uint8_t)code;
        }
    }
    out[0] = (uint8_t)(n - 1);
    return n;
}

size_t tmx_rpt_sync_reply(uint8_t *out, uint16_t tag, uint16_t seq)
{
    const uint8_t r[] = {5, TMX_RPT_SYNC_REPLY, (uint8_t)(tag >> 8), (uint8_t)tag, (uint8_t)(seq >> 8), (uint8_t)seq};
    memcpy(out, r, sizeof(r));
    return sizeof(r);
}

size_t tmx_rpt_bus_wrap(uint8_t *out, uint8_t bus, const uint8_t *stock_report, size_t n)
{
    /* stock_report is [len][id][payload...]; the wrapped report is [len+1][59][bus][id][payload...]. */
    if (n < 2 || stock_report[0] != n - 1 || n + 2 > 255) {
        return 0;
    }
    out[0] = (uint8_t)(n + 1);
    out[1] = TMX_RPT_BUS_REPORT;
    out[2] = bus;
    memcpy(out + 3, stock_report + 1, n - 1);
    return n + 2;
}

size_t tmx_rpt_plugin(uint8_t *out, uint8_t slot, uint8_t sub, const uint8_t *data, size_t n)
{
    if (n > PLUGIN_DATA_MAX) {
        return 0;
    }
    out[0] = (uint8_t)(n + 3);
    out[1] = TMX_RPT_PLUGIN_REPORT;
    out[2] = slot;
    out[3] = sub;
    if (n > 0) {
        memcpy(out + 4, data, n);
    }
    return n + 4;
}
