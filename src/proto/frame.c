#include "tmx_proto.h"

static bool expired(const tmx_frame_t *f, uint32_t now_ms)
{
    return (uint32_t)(now_ms - f->started_ms) > TMX_FRAME_TIMEOUT_MS;
}

void tmx_frame_init(tmx_frame_t *f, uint16_t payload_cap)
{
    f->need = 0;
    f->have = 0;
    f->skip = 0;
    f->started_ms = 0;
    f->dropped = 0;
    f->cap = payload_cap > 254 ? 254 : payload_cap;
}

void tmx_frame_tick(tmx_frame_t *f, uint32_t now_ms)
{
    if (f->need > 0 && expired(f, now_ms)) {
        f->need = 0;
        f->have = 0;
        f->dropped++;
    } else if (f->skip > 0 && expired(f, now_ms)) {
        /* The oversized packet was counted when it began; just stop discarding. */
        f->skip = 0;
    }
}

void tmx_frame_abandon(tmx_frame_t *f)
{
    if (f->need > 0) {
        f->dropped++;
    }
    f->need = 0;
    f->have = 0;
    f->skip = 0;
}

void tmx_frame_feed(tmx_frame_t *f, const uint8_t *data, size_t n, uint32_t now_ms, tmx_packet_fn fn, void *ctx)
{
    tmx_frame_tick(f, now_ms);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        if (f->skip > 0) {
            f->skip--;
        } else if (f->need == 0) {
            if (b == 0) {
                f->dropped++;
            } else if (b > f->cap + 1) {
                f->dropped++;
                f->skip = b;    /* len counts the bytes after the len byte */
                f->started_ms = now_ms;
            } else {
                f->need = b;
                f->have = 0;
                f->started_ms = now_ms;
            }
        } else {
            f->buf[f->have++] = b;
            if (f->have == f->need) {
                uint16_t len = f->need;
                f->need = 0;
                f->have = 0;
                fn(f->buf[0], f->buf + 1, (size_t)len - 1, ctx);
            }
        }
    }
}
