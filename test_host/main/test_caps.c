#include <string.h>

#include "tmx_proto.h"
#include "unity.h"

static void check(const uint8_t *want, size_t want_n, const uint8_t *got, size_t got_n)
{
    TEST_ASSERT_EQUAL_size_t(want_n, got_n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, got, want_n);
}

TEST_CASE("tmx_caps_chip on model, revision and IDF version: [1,len,model u16,revision u16,ASCII]", "[caps]")
{
    uint8_t out[TMX_CAPS_RECORD_MAX];
    const uint8_t want[] = {1, 9, 0x00, 0x12, 0x01, 0x03, 'v', '6', '.', '1', 'x'};
    check(want, sizeof(want), out, tmx_caps_chip(out, 0x0012, 0x0103, "v6.1x"));
}

TEST_CASE("tmx_caps_chip on an IDF string over the page: cut so the record is 251 bytes", "[caps]")
{
    uint8_t out[TMX_CAPS_RECORD_MAX + 8];
    char idf[400];
    memset(idf, 'a', sizeof(idf) - 1);
    idf[sizeof(idf) - 1] = 0;
    TEST_ASSERT_EQUAL_size_t(TMX_CAPS_RECORD_MAX, tmx_caps_chip(out, 1, 2, idf));
    TEST_ASSERT_EQUAL_UINT8(TMX_CAPS_RECORD_MAX - 2, out[1]);
}

TEST_CASE("tmx_caps_pin: [2,len,gpio,caps u16,safe,adc_unit,adc_channel,ids...], -1 as 0xFF", "[caps]")
{
    uint8_t out[TMX_CAPS_RECORD_MAX];
    const uint8_t ids[] = {0, 3};
    const uint8_t want[] = {2, 8, 34, 0x01, 0x21, 0, 1, 6, 0, 3};
    check(want, sizeof(want), out, tmx_caps_pin(out, 34, 0x0121, 0, 1, 6, ids, 2));
    const uint8_t want2[] = {2, 6, 4, 0x00, 0x07, 2, 0xFF, 0xFF};
    check(want2, sizeof(want2), out, tmx_caps_pin(out, 4, 0x0007, 2, -1, -1, NULL, 0));
}

TEST_CASE("tmx_caps_bus_i2c: [3,8,0,bus,sda,scl,speed u32 big-endian]; -1 is 0xFF", "[caps]")
{
    uint8_t out[TMX_CAPS_RECORD_MAX];
    const uint8_t want[] = {3, 8, 0, 1, 21, 0xFF, 0x00, 0x06, 0x1A, 0x80};
    check(want, sizeof(want), out, tmx_caps_bus_i2c(out, 1, 21, -1, 400000));
}

TEST_CASE("tmx_caps_bus_onewire: [3,3,1,bus,gpio]; -1 is 0xFF", "[caps]")
{
    uint8_t out[TMX_CAPS_RECORD_MAX];
    const uint8_t want[] = {3, 3, 1, 0, 0xFF};
    check(want, sizeof(want), out, tmx_caps_bus_onewire(out, 0, -1));
    const uint8_t want2[] = {3, 3, 1, 2, 23};
    check(want2, sizeof(want2), out, tmx_caps_bus_onewire(out, 2, 23));
}

TEST_CASE("tmx_caps_limit: [4,3,kind,count u16]", "[caps]")
{
    uint8_t out[TMX_CAPS_RECORD_MAX];
    const uint8_t want[] = {4, 3, 2, 0x01, 0x02};
    check(want, sizeof(want), out, tmx_caps_limit(out, 2, 0x0102));
}

TEST_CASE("tmx_caps_plugin: [5,len,slot,major,minor,name]", "[caps]")
{
    uint8_t out[TMX_CAPS_RECORD_MAX];
    const uint8_t want[] = {5, 8, 2, 1, 7, 'a', '.', 'b', 'c', 'd'};
    check(want, sizeof(want), out, tmx_caps_plugin(out, 2, 1, 7, "a.bcd"));
}

TEST_CASE("tmx_caps_named: [6,len,id,name]", "[caps]")
{
    uint8_t out[TMX_CAPS_RECORD_MAX];
    const uint8_t want[] = {6, 5, 3, 'l', 'e', 'd', 's'};
    check(want, sizeof(want), out, tmx_caps_named(out, 3, "leds"));
}

/* 30 records of 20 bytes: 12 fit a page, so pages 0..2 of 12, 12 and 6 records. */
static size_t twenty(size_t i, uint8_t *rec, void *ctx)
{
    (void)ctx;
    if (i >= 30) {
        return 0;
    }
    rec[0] = 9;
    rec[1] = 18;
    memset(rec + 2, (int)i, 18);
    return 20;
}

TEST_CASE("tmx_caps_page on records totalling 600 bytes: pages 0..2, each at most 251 record bytes, no record split, "
          "last_page 2 in every page", "[caps]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    size_t n;
    size_t seen = 0;
    for (uint8_t page = 0; page < 3; page++) {
        TEST_ASSERT_TRUE(tmx_caps_page(twenty, NULL, page, out, &n));
        TEST_ASSERT_EQUAL_UINT8(n - 1, out[0]);
        TEST_ASSERT_EQUAL_UINT8(TMX_RPT_CAPABILITIES, out[1]);
        TEST_ASSERT_EQUAL_UINT8(page, out[2]);
        TEST_ASSERT_EQUAL_UINT8(2, out[3]);
        TEST_ASSERT_TRUE(n - 4 <= 251);
        TEST_ASSERT_TRUE(n <= 255);
        TEST_ASSERT_EQUAL_size_t(0, (n - 4) % 20);
        for (size_t o = 4; o < n; o += 20) {
            TEST_ASSERT_EQUAL_UINT8(9, out[o]);
            TEST_ASSERT_EQUAL_UINT8(seen, out[o + 2]);     /* in order, each once */
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_size_t(30, seen);
}

TEST_CASE("tmx_caps_page on a page past the last: returns false", "[caps]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    size_t n = 0;
    TEST_ASSERT_FALSE(tmx_caps_page(twenty, NULL, 3, out, &n));
    TEST_ASSERT_FALSE(tmx_caps_page(twenty, NULL, 255, out, &n));
}

static size_t none(size_t i, uint8_t *rec, void *ctx)
{
    (void)i;
    (void)rec;
    (void)ctx;
    return 0;
}

TEST_CASE("tmx_caps_page on no records: page 0 is [3,60,0,0] and page 1 is past the last", "[caps]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    size_t n;
    TEST_ASSERT_TRUE(tmx_caps_page(none, NULL, 0, out, &n));
    const uint8_t want[] = {3, TMX_RPT_CAPABILITIES, 0, 0};
    check(want, sizeof(want), out, n);
    TEST_ASSERT_FALSE(tmx_caps_page(none, NULL, 1, out, &n));
}

/* 251 bytes exactly fill a page; one more record starts the next. */
static size_t fill(size_t i, uint8_t *rec, void *ctx)
{
    (void)ctx;
    static const size_t sizes[] = {100, 151, 2};
    if (i >= 3) {
        return 0;
    }
    rec[0] = 7;
    rec[1] = (uint8_t)(sizes[i] - 2);
    return sizes[i];
}

TEST_CASE("tmx_caps_page on records filling a page exactly: the page holds 251 bytes and the next record starts page 1",
          "[caps]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    size_t n;
    TEST_ASSERT_TRUE(tmx_caps_page(fill, NULL, 0, out, &n));
    TEST_ASSERT_EQUAL_size_t(255, n);
    TEST_ASSERT_EQUAL_UINT8(254, out[0]);
    TEST_ASSERT_EQUAL_UINT8(1, out[3]);
    TEST_ASSERT_TRUE(tmx_caps_page(fill, NULL, 1, out, &n));
    TEST_ASSERT_EQUAL_size_t(6, n);
}
