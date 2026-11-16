#include <string.h>

#include "tmx_core.h"
#include "unity.h"

static int wakes;

static void wake_counter(void)
{
    wakes++;
}

static tmx_completion_t make(uint32_t token)
{
    tmx_completion_t c;
    memset(&c, 0, sizeof(c));
    c.kind = 1;
    c.token = token;
    c.value = token * 10;
    return c;
}

TEST_CASE("deadlines 19 ms and 2 ms: the nearest is reported, then due after it passes", "[sched]")
{
    tmx_deadline_t a, b;
    tmx_deadline_arm(&a, 1000, 19);
    tmx_deadline_arm(&b, 1000, 2);
    TEST_ASSERT_EQUAL_UINT32(19, tmx_deadline_left(&a, 1000));
    TEST_ASSERT_EQUAL_UINT32(2, tmx_deadline_left(&b, 1000));
    TEST_ASSERT_FALSE(tmx_deadline_due(&b, 1001));
    TEST_ASSERT_TRUE(tmx_deadline_due(&b, 1002));
    TEST_ASSERT_FALSE(tmx_deadline_due(&a, 1002));
    TEST_ASSERT_EQUAL_UINT32(17, tmx_deadline_left(&a, 1002));
    TEST_ASSERT_TRUE(tmx_deadline_due(&a, 1019));
}

TEST_CASE("deadline past due: left is 0", "[sched]")
{
    tmx_deadline_t d;
    tmx_deadline_arm(&d, 50, 5);
    TEST_ASSERT_EQUAL_UINT32(0, tmx_deadline_left(&d, 55));
    TEST_ASSERT_EQUAL_UINT32(0, tmx_deadline_left(&d, 500));
    TEST_ASSERT_TRUE(tmx_deadline_due(&d, 500));
}

TEST_CASE("unarmed deadline: never due and left is UINT32_MAX", "[sched]")
{
    tmx_deadline_t d = {0};
    TEST_ASSERT_FALSE(tmx_deadline_due(&d, 0));
    TEST_ASSERT_FALSE(tmx_deadline_due(&d, 123456));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tmx_deadline_left(&d, 123456));
}

TEST_CASE("millisecond counter wrap at 0xFFFFFFF0: deadlines still fire in order", "[sched]")
{
    tmx_deadline_t early, late;
    uint32_t now = 0xFFFFFFF0u;
    tmx_deadline_arm(&early, now, 10);   /* 0xFFFFFFFA */
    tmx_deadline_arm(&late, now, 30);    /* wraps to 0x0000000E */
    TEST_ASSERT_EQUAL_UINT32(10, tmx_deadline_left(&early, now));
    TEST_ASSERT_EQUAL_UINT32(30, tmx_deadline_left(&late, now));
    TEST_ASSERT_FALSE(tmx_deadline_due(&early, 0xFFFFFFF9u));
    TEST_ASSERT_TRUE(tmx_deadline_due(&early, 0xFFFFFFFAu));
    TEST_ASSERT_FALSE(tmx_deadline_due(&late, 0xFFFFFFFAu));
    TEST_ASSERT_FALSE(tmx_deadline_due(&late, 0x0000000Du));
    TEST_ASSERT_TRUE(tmx_deadline_due(&late, 0x0000000Eu));
    TEST_ASSERT_EQUAL_UINT32(4, tmx_deadline_left(&late, 0x0000000Au));
}

TEST_CASE("deadline armed with 0 ms: due at once", "[sched]")
{
    tmx_deadline_t d;
    tmx_deadline_arm(&d, 77, 0);
    TEST_ASSERT_TRUE(tmx_deadline_due(&d, 77));
    TEST_ASSERT_EQUAL_UINT32(0, tmx_deadline_left(&d, 77));
}

TEST_CASE("empty queue: take returns false and leaves the output alone", "[completion]")
{
    tmx_completion_set_wake(NULL);
    tmx_completion_reset();
    tmx_completion_t out = make(99);
    TEST_ASSERT_FALSE(tmx_completion_take(&out));
    TEST_ASSERT_EQUAL_UINT32(99, out.token);
}

TEST_CASE("three posts: taken in posting order with every field intact", "[completion]")
{
    tmx_completion_set_wake(NULL);
    tmx_completion_reset();
    for (uint32_t i = 1; i <= 3; i++) {
        tmx_completion_t c = make(i);
        c.err = (esp_err_t)(i == 2 ? ESP_ERR_TIMEOUT : ESP_OK);
        memset(c.data, (int)i, sizeof(c.data));
        tmx_completion_post(&c);
    }
    for (uint32_t i = 1; i <= 3; i++) {
        tmx_completion_t out;
        TEST_ASSERT_TRUE(tmx_completion_take(&out));
        TEST_ASSERT_EQUAL_UINT32(i, out.token);
        TEST_ASSERT_EQUAL_UINT32(i * 10, out.value);
        TEST_ASSERT_EQUAL_UINT8(1, out.kind);
        TEST_ASSERT_EQUAL_INT(i == 2 ? ESP_ERR_TIMEOUT : ESP_OK, out.err);
        TEST_ASSERT_EQUAL_UINT8(i, out.data[4]);
    }
    tmx_completion_t out;
    TEST_ASSERT_FALSE(tmx_completion_take(&out));
    TEST_ASSERT_EQUAL_UINT32(0, tmx_completion_overflows());
}

TEST_CASE("17 posts into a ring of 16: the 17th is dropped and counted, the first 16 keep order", "[completion]")
{
    tmx_completion_set_wake(NULL);
    tmx_completion_reset();
    for (uint32_t i = 1; i <= 17; i++) {
        tmx_completion_t c = make(i);
        tmx_completion_post(&c);
    }
    TEST_ASSERT_EQUAL_UINT32(1, tmx_completion_overflows());
    for (uint32_t i = 1; i <= 16; i++) {
        tmx_completion_t out;
        TEST_ASSERT_TRUE(tmx_completion_take(&out));
        TEST_ASSERT_EQUAL_UINT32(i, out.token);
    }
    tmx_completion_t out;
    TEST_ASSERT_FALSE(tmx_completion_take(&out));
}

TEST_CASE("ring wraps around: order survives many post/take cycles", "[completion]")
{
    tmx_completion_set_wake(NULL);
    tmx_completion_reset();
    uint32_t next_post = 1, next_take = 1;
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 11; i++) {
            tmx_completion_t c = make(next_post++);
            tmx_completion_post(&c);
        }
        for (int i = 0; i < 11; i++) {
            tmx_completion_t out;
            TEST_ASSERT_TRUE(tmx_completion_take(&out));
            TEST_ASSERT_EQUAL_UINT32(next_take++, out.token);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, tmx_completion_overflows());
}

TEST_CASE("reset: queued entries dropped and overflow count zeroed", "[completion]")
{
    tmx_completion_set_wake(NULL);
    tmx_completion_reset();
    for (uint32_t i = 0; i < 20; i++) {
        tmx_completion_t c = make(i);
        tmx_completion_post(&c);
    }
    TEST_ASSERT_EQUAL_UINT32(4, tmx_completion_overflows());
    tmx_completion_reset();
    tmx_completion_t out;
    TEST_ASSERT_FALSE(tmx_completion_take(&out));
    TEST_ASSERT_EQUAL_UINT32(0, tmx_completion_overflows());
}

TEST_CASE("wake registered: called once per post, also when the ring is full", "[completion]")
{
    tmx_completion_reset();
    wakes = 0;
    tmx_completion_set_wake(wake_counter);
    tmx_completion_t c = make(1);
    tmx_completion_post(&c);
    tmx_completion_post(&c);
    TEST_ASSERT_EQUAL_INT(2, wakes);
    for (int i = 0; i < 20; i++) {
        tmx_completion_post(&c);
    }
    TEST_ASSERT_EQUAL_INT(22, wakes);
    tmx_completion_reset();
    tmx_completion_post(&c);
    TEST_ASSERT_EQUAL_INT(23, wakes);
    tmx_completion_set_wake(NULL);
    tmx_completion_reset();
}

TEST_CASE("wake set to NULL: posting is a no-op wake and does not crash", "[completion]")
{
    tmx_completion_reset();
    wakes = 0;
    tmx_completion_set_wake(wake_counter);
    tmx_completion_set_wake(NULL);
    tmx_completion_t c = make(1);
    tmx_completion_post(&c);
    TEST_ASSERT_EQUAL_INT(0, wakes);
    tmx_completion_reset();
}
