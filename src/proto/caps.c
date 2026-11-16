#include <string.h>

#include "tmx_proto.h"

static size_t begin(uint8_t *out, uint8_t type)
{
    out[0] = type;
    out[1] = 0;
    return 2;
}

/** Close the record that ends at offset n: len is the value's length. */
static size_t finish(uint8_t *out, size_t n)
{
    out[1] = (uint8_t)(n - 2);
    return n;
}

/** Append the string, cut so the record stays within TMX_CAPS_RECORD_MAX. */
static size_t put_text(uint8_t *out, size_t n, const char *s)
{
    size_t len = s ? strlen(s) : 0;
    if (len > TMX_CAPS_RECORD_MAX - n) {
        len = TMX_CAPS_RECORD_MAX - n;
    }
    if (len > 0) {
        memcpy(out + n, s, len);
    }
    return n + len;
}

static size_t put_u16(uint8_t *out, size_t n, uint16_t v)
{
    out[n++] = (uint8_t)(v >> 8);
    out[n++] = (uint8_t)v;
    return n;
}

size_t tmx_caps_chip(uint8_t *out, uint16_t model, uint16_t revision, const char *idf)
{
    size_t n = begin(out, TMX_CAPS_CHIP);
    n = put_u16(out, n, model);
    n = put_u16(out, n, revision);
    return finish(out, put_text(out, n, idf));
}

size_t tmx_caps_pin(uint8_t *out, uint8_t gpio, uint16_t caps, uint8_t safe, int8_t adc_unit, int8_t adc_channel,
                    const uint8_t *named_ids, size_t n_named)
{
    size_t n = begin(out, TMX_CAPS_PIN);
    out[n++] = gpio;
    n = put_u16(out, n, caps);
    out[n++] = safe;
    out[n++] = (uint8_t)adc_unit;
    out[n++] = (uint8_t)adc_channel;
    if (n_named > TMX_CAPS_RECORD_MAX - n) {
        n_named = TMX_CAPS_RECORD_MAX - n;
    }
    if (n_named > 0) {
        memcpy(out + n, named_ids, n_named);
    }
    return finish(out, n + n_named);
}

size_t tmx_caps_bus_i2c(uint8_t *out, uint8_t bus, int8_t sda, int8_t scl, uint32_t speed_hz)
{
    size_t n = begin(out, TMX_CAPS_BUS);
    out[n++] = TMX_CAPS_BUS_I2C;
    out[n++] = bus;
    out[n++] = (uint8_t)sda;
    out[n++] = (uint8_t)scl;
    n = put_u16(out, n, (uint16_t)(speed_hz >> 16));
    n = put_u16(out, n, (uint16_t)speed_hz);
    return finish(out, n);
}

size_t tmx_caps_bus_onewire(uint8_t *out, uint8_t bus, int8_t gpio)
{
    size_t n = begin(out, TMX_CAPS_BUS);
    out[n++] = TMX_CAPS_BUS_ONEWIRE;
    out[n++] = bus;
    out[n++] = (uint8_t)gpio;
    return finish(out, n);
}

size_t tmx_caps_limit(uint8_t *out, uint8_t kind, uint16_t count)
{
    size_t n = begin(out, TMX_CAPS_LIMIT);
    out[n++] = kind;
    n = put_u16(out, n, count);
    return finish(out, n);
}

size_t tmx_caps_plugin(uint8_t *out, uint8_t slot, uint8_t major, uint8_t minor, const char *name)
{
    size_t n = begin(out, TMX_CAPS_PLUGIN);
    out[n++] = slot;
    out[n++] = major;
    out[n++] = minor;
    return finish(out, put_text(out, n, name));
}

size_t tmx_caps_named(uint8_t *out, uint8_t id, const char *name)
{
    size_t n = begin(out, TMX_CAPS_NAMED);
    out[n++] = id;
    return finish(out, put_text(out, n, name));
}

bool tmx_caps_page(tmx_caps_source_fn src, void *ctx, uint8_t page, uint8_t *out, size_t *n)
{
    uint8_t rec[TMX_CAPS_RECORD_MAX];
    size_t cur = 0;         /* page being filled */
    size_t used = 0;        /* record bytes in it */
    size_t body = 0;        /* record bytes copied for the wanted page */
    for (size_t i = 0;; i++) {
        size_t len = src(i, rec, ctx);
        if (len == 0) {
            break;
        }
        if (used + len > TMX_CAPS_PAGE_MAX) {
            cur++;
            used = 0;
        }
        used += len;
        if (cur == page) {
            memcpy(out + 4 + body, rec, len);
            body += len;
        }
    }
    if (page > cur || cur > UINT8_MAX) {
        return false;
    }
    out[0] = (uint8_t)(body + 3);
    out[1] = TMX_RPT_CAPABILITIES;
    out[2] = page;
    out[3] = (uint8_t)cur;
    *n = body + 4;
    return true;
}
