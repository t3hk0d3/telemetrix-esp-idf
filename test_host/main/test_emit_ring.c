#include <string.h>

#include "tmx_net.h"
#include "unity.h"

static int wakes;

static void count_wake(void)
{
    wakes++;
}

/** An empty ring open for puts, counting wakes. */
static void start(void)
{
    tmx_emit_ring_reset();
    tmx_emit_ring_bump(true);
    wakes = 0;
    tmx_emit_ring_set_wake(count_wake);
}

TEST_CASE("tmx_emit_ring put/take on 9 puts without a take: 8 stored in order", "[emit_ring]")
{
    start();
    for (uint8_t i = 0; i < 9; i++) {
        const uint8_t d[] = {i, (uint8_t)(i * 2)};
        tmx_emit_ring_put(i % 3, i, d, sizeof(d));
    }
    tmx_emit_entry_t e;
    for (uint8_t i = 0; i < 8; i++) {
        TEST_ASSERT_TRUE(tmx_emit_ring_take(&e));
        TEST_ASSERT_EQUAL(i % 3, e.slot);
        TEST_ASSERT_EQUAL(i, e.sub);
        TEST_ASSERT_EQUAL(2, e.n);
        TEST_ASSERT_EQUAL(i, e.data[0]);
        TEST_ASSERT_EQUAL(i * 2, e.data[1]);
    }
    TEST_ASSERT_FALSE(tmx_emit_ring_take(&e));
}

TEST_CASE("tmx_emit_ring put/take on 9 puts without a take: the 9th ESP_ERR_NO_MEM", "[emit_ring]")
{
    start();
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, tmx_emit_ring_put(0, 0, NULL, 0));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, tmx_emit_ring_put(0, 0, NULL, 0));
}

TEST_CASE("tmx_emit_ring put: a 251-byte entry kept whole, 252 bytes ESP_ERR_INVALID_ARG", "[emit_ring]")
{
    uint8_t d[252];
    for (size_t i = 0; i < sizeof(d); i++) {
        d[i] = (uint8_t)i;
    }
    start();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_emit_ring_put(0, 0, d, 252));
    TEST_ASSERT_EQUAL(ESP_OK, tmx_emit_ring_put(0, 0, d, 251));
    tmx_emit_entry_t e;
    TEST_ASSERT_TRUE(tmx_emit_ring_take(&e));
    TEST_ASSERT_EQUAL(251, e.n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(d, e.data, 251);
}

TEST_CASE("tmx_emit_ring put: the wake function called after a put", "[emit_ring]")
{
    start();
    TEST_ASSERT_EQUAL(ESP_OK, tmx_emit_ring_put(0, 0, NULL, 0));
    TEST_ASSERT_EQUAL(1, wakes);
}

TEST_CASE("tmx_emit_ring put while closed: ESP_ERR_INVALID_STATE, nothing queued, no wake", "[emit_ring]")
{
    start();
    tmx_emit_ring_bump(false);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tmx_emit_ring_put(0, 0, NULL, 0));
    tmx_emit_ring_bump(true);
    tmx_emit_entry_t e;
    TEST_ASSERT_FALSE(tmx_emit_ring_take(&e));
    TEST_ASSERT_EQUAL(0, wakes);
}

TEST_CASE("tmx_emit_ring reset: closed until a bump opens it", "[emit_ring]")
{
    tmx_emit_ring_reset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tmx_emit_ring_put(0, 0, NULL, 0));
}

TEST_CASE("tmx_emit_ring on a generation bump: older entries skipped at take", "[emit_ring]")
{
    start();
    const uint8_t old = 1, fresh = 2;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_emit_ring_put(0, 0, &old, 1));
    tmx_emit_ring_bump(true);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_emit_ring_put(0, 0, &fresh, 1));
    tmx_emit_entry_t e;
    TEST_ASSERT_TRUE(tmx_emit_ring_take(&e));
    TEST_ASSERT_EQUAL(2, e.data[0]);
    TEST_ASSERT_FALSE(tmx_emit_ring_take(&e));
}

TEST_CASE("tmx_emit_ring on a generation bump with a full ring: the new generation has all 8 entries", "[emit_ring]")
{
    start();
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, tmx_emit_ring_put(0, 0, NULL, 0));
    }
    tmx_emit_ring_bump(true);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, tmx_emit_ring_put(0, 1, NULL, 0));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, tmx_emit_ring_put(0, 1, NULL, 0));
}
