#include <string.h>

#include "tmx_proto.h"
#include "unity.h"

typedef struct {
    int count;
    uint8_t id;
    uint8_t payload[256];
    size_t len;
} capture_t;

static void capture(uint8_t id, const uint8_t *payload, size_t len, void *ctx)
{
    capture_t *c = ctx;
    c->count++;
    c->id = id;
    c->len = len;
    memcpy(c->payload, payload, len);
}

static void feed_bytewise(tmx_frame_t *f, const uint8_t *data, size_t n, uint32_t now_ms, capture_t *c)
{
    for (size_t i = 0; i < n; i++) {
        tmx_frame_feed(f, data + i, 1, now_ms, capture, c);
    }
}

TEST_CASE("On a complete packet in one call: emits (id, payload, len) once", "[frame]")
{
    tmx_frame_t f;
    capture_t c = {0};
    const uint8_t pkt[] = {4, 3, 0x11, 0x22, 0x33};
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    tmx_frame_feed(&f, pkt, sizeof(pkt), 0, capture, &c);
    TEST_ASSERT_EQUAL_INT(1, c.count);
    TEST_ASSERT_EQUAL_UINT8(3, c.id);
    TEST_ASSERT_EQUAL_size_t(3, c.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&pkt[2], c.payload, 3);
    TEST_ASSERT_EQUAL_UINT32(0, f.dropped);
}

TEST_CASE("On a len 1 packet: emits the command with an empty payload", "[frame]")
{
    tmx_frame_t f;
    capture_t c = {0};
    const uint8_t pkt[] = {1, 14};
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    tmx_frame_feed(&f, pkt, sizeof(pkt), 0, capture, &c);
    TEST_ASSERT_EQUAL_INT(1, c.count);
    TEST_ASSERT_EQUAL_UINT8(14, c.id);
    TEST_ASSERT_EQUAL_size_t(0, c.len);
}

TEST_CASE("On a packet split byte by byte across calls: emits it once, unchanged", "[frame]")
{
    tmx_frame_t f;
    capture_t c = {0};
    const uint8_t pkt[] = {5, 6, 1, 2, 3, 4};
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    feed_bytewise(&f, pkt, sizeof(pkt), 0, &c);
    TEST_ASSERT_EQUAL_INT(1, c.count);
    TEST_ASSERT_EQUAL_UINT8(6, c.id);
    TEST_ASSERT_EQUAL_size_t(4, c.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&pkt[2], c.payload, 4);
    TEST_ASSERT_EQUAL_UINT32(0, f.dropped);
}

TEST_CASE("On the largest valid len 31: emits it once", "[frame]")
{
    tmx_frame_t f;
    capture_t c = {0};
    uint8_t pkt[32] = {31, 7};
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    tmx_frame_feed(&f, pkt, sizeof(pkt), 0, capture, &c);
    TEST_ASSERT_EQUAL_INT(1, c.count);
    TEST_ASSERT_EQUAL_size_t(30, c.len);
    TEST_ASSERT_EQUAL_UINT32(0, f.dropped);
}

TEST_CASE("On len 0: consumes one byte, dropped_packets +1, next packet emitted normally", "[frame]")
{
    tmx_frame_t f;
    capture_t c = {0};
    const uint8_t data[] = {0, 2, 2, 9};
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    tmx_frame_feed(&f, data, sizeof(data), 0, capture, &c);
    TEST_ASSERT_EQUAL_UINT32(1, f.dropped);
    TEST_ASSERT_EQUAL_INT(1, c.count);
    TEST_ASSERT_EQUAL_UINT8(2, c.id);
    TEST_ASSERT_EQUAL_size_t(1, c.len);
    TEST_ASSERT_EQUAL_UINT8(9, c.payload[0]);
}

TEST_CASE("On len 32..255: skips len bytes, dropped_packets +1, next packet emitted normally", "[frame]")
{
    for (int len = 32; len <= 255; len++) {
        tmx_frame_t f;
        capture_t c = {0};
        uint8_t data[1 + 255 + 3];
        data[0] = (uint8_t)len;
        memset(&data[1], 0xAA, (size_t)len);
        data[1 + len] = 2;
        data[2 + len] = 2;
        data[3 + len] = 9;
        tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
        tmx_frame_feed(&f, data, (size_t)len + 4, 0, capture, &c);
        TEST_ASSERT_EQUAL_UINT32(1, f.dropped);
        TEST_ASSERT_EQUAL_INT(1, c.count);
        TEST_ASSERT_EQUAL_UINT8(2, c.id);
        TEST_ASSERT_EQUAL_UINT8(9, c.payload[0]);
    }
}

TEST_CASE("On a gap over 2000 ms inside a packet: drops the partial packet, dropped_packets +1", "[frame]")
{
    tmx_frame_t f;
    capture_t c = {0};
    const uint8_t head[] = {4, 3, 1};
    const uint8_t rest[] = {2, 4, 7};
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    tmx_frame_feed(&f, head, sizeof(head), 1000, capture, &c);
    tmx_frame_tick(&f, 3001);
    TEST_ASSERT_EQUAL_UINT32(1, f.dropped);
    tmx_frame_feed(&f, rest, sizeof(rest), 3002, capture, &c);
    TEST_ASSERT_EQUAL_INT(1, c.count);
    TEST_ASSERT_EQUAL_UINT8(4, c.id);
}

TEST_CASE("On exactly 2000 ms inside a packet: keeps it", "[frame]")
{
    tmx_frame_t f;
    capture_t c = {0};
    const uint8_t head[] = {3, 3, 1};
    const uint8_t tail[] = {2};
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    tmx_frame_feed(&f, head, sizeof(head), 1000, capture, &c);
    tmx_frame_tick(&f, 3000);
    tmx_frame_feed(&f, tail, sizeof(tail), 3000, capture, &c);
    TEST_ASSERT_EQUAL_UINT32(0, f.dropped);
    TEST_ASSERT_EQUAL_INT(1, c.count);
}

TEST_CASE("On a late byte arriving through feed: drops the partial packet and starts a new one", "[frame]")
{
    tmx_frame_t f;
    capture_t c = {0};
    const uint8_t head[] = {4, 3, 1};
    const uint8_t next[] = {2, 5, 8};
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    tmx_frame_feed(&f, head, sizeof(head), 0, capture, &c);
    tmx_frame_feed(&f, next, sizeof(next), 5000, capture, &c);
    TEST_ASSERT_EQUAL_UINT32(1, f.dropped);
    TEST_ASSERT_EQUAL_INT(1, c.count);
    TEST_ASSERT_EQUAL_UINT8(5, c.id);
    TEST_ASSERT_EQUAL_UINT8(8, c.payload[0]);
}

TEST_CASE("On a millisecond counter wrap inside a packet: keeps it", "[frame]")
{
    tmx_frame_t f;
    capture_t c = {0};
    const uint8_t head[] = {3, 3, 1};
    const uint8_t tail[] = {2};
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    tmx_frame_feed(&f, head, sizeof(head), 0xFFFFFF00u, capture, &c);
    tmx_frame_feed(&f, tail, sizeof(tail), 0x00000100u, capture, &c);
    TEST_ASSERT_EQUAL_UINT32(0, f.dropped);
    TEST_ASSERT_EQUAL_INT(1, c.count);
}

TEST_CASE("On a tick between packets: counts nothing", "[frame]")
{
    tmx_frame_t f;
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    tmx_frame_tick(&f, 100000);
    TEST_ASSERT_EQUAL_UINT32(0, f.dropped);
}

TEST_CASE("On abandon inside a packet: drops the partial packet, dropped_packets +1", "[frame]")
{
    tmx_frame_t f;
    capture_t c = {0};
    const uint8_t head[] = {3, 2, 2};
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    tmx_frame_feed(&f, head, sizeof(head), 0, capture, &c);
    tmx_frame_abandon(&f);
    TEST_ASSERT_EQUAL_UINT32(1, f.dropped);
    TEST_ASSERT_EQUAL_INT(0, c.count);
    TEST_ASSERT_EQUAL(0, f.need);
}

TEST_CASE("On abandon between packets or inside a skip: counts nothing more", "[frame]")
{
    tmx_frame_t f;
    capture_t c = {0};
    const uint8_t oversized[] = {40, 1, 2};
    tmx_frame_init(&f, TMX_STOCK_PAYLOAD_MAX);
    tmx_frame_abandon(&f);
    TEST_ASSERT_EQUAL_UINT32(0, f.dropped);
    tmx_frame_feed(&f, oversized, sizeof(oversized), 0, capture, &c);
    TEST_ASSERT_EQUAL_UINT32(1, f.dropped);     /* counted when it began */
    tmx_frame_abandon(&f);
    TEST_ASSERT_EQUAL_UINT32(1, f.dropped);
    TEST_ASSERT_EQUAL(0, f.skip);
}
