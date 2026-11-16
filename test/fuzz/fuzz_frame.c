/* libFuzzer target: tmx_frame_feed and tmx_stock_check.
 * Input layout: byte 0 selects the payload cap (even: stock 30, odd: 254), then repeated chunks of
 * [size][time step][size bytes of stream]; the step advances the clock by step * 16 ms (0..4080), so gaps over
 * 2000 ms occur, and chunk sizes give random split points. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tmx_proto.h"

static void on_packet(uint8_t id, const uint8_t *payload, size_t len, void *ctx)
{
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < len; i++) {
        sink ^= payload[i];     /* AddressSanitizer checks the whole payload is readable */
    }
    (void)sink;
    tmx_stock_verdict_t v = tmx_stock_check(id, payload, len);
    if (v != TMX_STOCK_OK && v != TMX_STOCK_SHORT && v != TMX_STOCK_IGNORED) {
        abort();
    }
    (*(uint32_t *)ctx)++;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1) {
        return 0;
    }
    tmx_frame_t f;
    uint32_t emitted = 0;
    uint32_t now = 0xFFFFF000u;     /* start near the wrap of the millisecond counter */
    tmx_frame_init(&f, (data[0] & 1) ? 254 : TMX_STOCK_PAYLOAD_MAX);
    size_t pos = 1;
    while (pos + 2 <= size) {
        size_t n = data[pos];
        now += (uint32_t)data[pos + 1] * 16u;
        pos += 2;
        if (n > size - pos) {
            n = size - pos;
        }
        tmx_frame_feed(&f, data + pos, n, now, on_packet, &emitted);
        if (n % 5 == 0) {
            tmx_frame_tick(&f, now);
        }
        pos += n;
        if (f.have > f.need || f.need > f.cap + 1) {
            abort();
        }
    }
    return 0;
}
