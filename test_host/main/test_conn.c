#include <errno.h>
#include <string.h>

#include "tmx_adapter.h"
#include "tmx_net.h"
#include "unity.h"

/* A scripted sender: each call takes the next step; a step is how many bytes the "socket" accepts, or a negative errno.
 * Accepted bytes are appended to sent[]. */
#define SCRIPT_MAX 32
static int script[SCRIPT_MAX];
static size_t script_n, script_i;
static uint8_t sent[1024];
static size_t sent_n;

static ssize_t scripted_send(int fd, const void *buf, size_t n, int flags)
{
    (void)fd;
    (void)flags;
    int step = script_i < script_n ? script[script_i++] : -EAGAIN;
    if (step < 0) {
        errno = -step;
        return -1;
    }
    size_t take = (size_t)step < n ? (size_t)step : n;
    memcpy(sent + sent_n, buf, take);
    sent_n += take;
    return (ssize_t)take;
}

static void script_set(const int *steps, size_t n)
{
    memcpy(script, steps, n * sizeof(int));
    script_n = n;
    script_i = 0;
    sent_n = 0;
}

#define SCRIPT(...) \
    do { \
        const int s_[] = {__VA_ARGS__}; \
        script_set(s_, sizeof(s_) / sizeof(s_[0])); \
    } while (0)

static tmx_conn_t conn;

static void conn_start(uint16_t bytes)
{
    memset(&conn, 0, sizeof(conn));
    TEST_ASSERT_EQUAL(ESP_OK, tmx_conn_init(&conn, bytes));
    tmx_conn_set_sender(&conn, scripted_send);
    SCRIPT(0);
}

static void conn_stop(void)
{
    tmx_conn_deinit(&conn);
}

static void digital(uint8_t pin, uint8_t v)
{
    const uint8_t r[] = {3, 2, pin, v};
    tmx_conn_queue(&conn, r, sizeof(r), false, TMX_COALESCE_DIGITAL, pin);
}

static void analog(uint8_t pin, uint8_t hi, uint8_t lo)
{
    const uint8_t r[] = {4, 3, pin, hi, lo};
    tmx_conn_queue(&conn, r, sizeof(r), false, TMX_COALESCE_ANALOG, pin);
}

static void reply_loop(uint8_t c)
{
    const uint8_t r[] = {2, 0, c};
    tmx_conn_queue(&conn, r, sizeof(r), true, TMX_COALESCE_NONE, 0);
}

static void drain(void)
{
    SCRIPT(1000, 1000, 1000, 1000);
    TEST_ASSERT_EQUAL(TMX_CONN_IDLE, tmx_conn_flush(&conn, 3, 0, 1000));
}

TEST_CASE("init with too few bytes: ESP_ERR_INVALID_ARG", "[conn]")
{
    tmx_conn_t c;
    memset(&c, 0, sizeof(c));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_conn_init(&c, 63));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_conn_init(NULL, 4096));
}

TEST_CASE("queue on room: reports leave in queue order", "[conn]")
{
    conn_start(256);
    digital(2, 1);
    reply_loop(7);
    analog(34, 0x0a, 0xbc);
    drain();
    const uint8_t want[] = {3, 2, 2, 1, 2, 0, 7, 4, 3, 34, 0x0a, 0xbc};
    TEST_ASSERT_EQUAL(sizeof(want), sent_n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sent, sizeof(want));
    conn_stop();
}

TEST_CASE("queue of a digital report for a pin already queued: the queued value is replaced, not duplicated", "[conn]")
{
    conn_start(256);
    digital(2, 0);
    digital(2, 1);
    TEST_ASSERT_EQUAL_UINT32(1, conn.coalesced);
    TEST_ASSERT_EQUAL(4, tmx_conn_pending(&conn));
    drain();
    const uint8_t want[] = {3, 2, 2, 1};
    TEST_ASSERT_EQUAL(sizeof(want), sent_n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sent, sizeof(want));
    conn_stop();
}

TEST_CASE("queue of the same pin number in another kind: both are kept", "[conn]")
{
    conn_start(256);
    digital(34, 1);
    analog(34, 0, 5);
    TEST_ASSERT_EQUAL(9, tmx_conn_pending(&conn));
    TEST_ASSERT_EQUAL_UINT32(0, conn.coalesced);
    conn_stop();
}

TEST_CASE("queue of a keyed report of another size: the old entry goes and the new one is queued last", "[conn]")
{
    conn_start(256);
    const uint8_t data[] = {11, 11, 0, 4, 0, 0, 0xa0, 0x41, 0, 0, 0xc8, 0x41};
    const uint8_t err[] = {4, 11, 1, 4, 2};
    tmx_conn_queue(&conn, err, sizeof(err), false, TMX_COALESCE_DHT, 0);
    digital(5, 1);
    tmx_conn_queue(&conn, data, sizeof(data), false, TMX_COALESCE_DHT, 0);
    TEST_ASSERT_EQUAL_UINT32(1, conn.coalesced);
    drain();
    const uint8_t want[] = {3, 2, 5, 1, 11, 11, 0, 4, 0, 0, 0xa0, 0x41, 0, 0, 0xc8, 0x41};
    TEST_ASSERT_EQUAL(sizeof(want), sent_n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sent, sizeof(want));
    conn_stop();
}

static void fill_digital(void)
{
    for (uint8_t pin = 0; pin < 12; pin++) {
        digital(pin, 1);
    }
    TEST_ASSERT_EQUAL(48, tmx_conn_pending(&conn));
}

TEST_CASE("queue of a digital report with no room: nothing queued is lost and the report is delivered once space frees",
          "[conn]")
{
    conn_start(64); /* 48 bytes outside the reply reserve: twelve digital reports */
    fill_digital();
    digital(40, 0);
    TEST_ASSERT_EQUAL(48, tmx_conn_pending(&conn));
    TEST_ASSERT_EQUAL_UINT32(0, conn.dropped);
    drain();
    TEST_ASSERT_EQUAL(52, sent_n);
    const uint8_t first[] = {3, 2, 0, 1};
    const uint8_t last[] = {3, 2, 40, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, sent, sizeof(first));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(last, sent + 48, sizeof(last));
    conn_stop();
}

TEST_CASE("queue of an analog report with no room and no queued analog entry: delivered once space frees", "[conn]")
{
    conn_start(64);
    fill_digital();
    analog(34, 0, 1);
    TEST_ASSERT_EQUAL_UINT32(0, conn.dropped);
    drain();
    const uint8_t want[] = {4, 3, 34, 0, 1};
    TEST_ASSERT_EQUAL(53, sent_n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sent + 48, sizeof(want));
    conn_stop();
}

TEST_CASE("queue of a pin change with the queue full of replies: the change survives and arrives after them", "[conn]")
{
    conn_start(64);
    for (int i = 0; i < 17; i++) { /* 51 bytes: past the 48 outside the reserve */
        reply_loop((uint8_t)i);
    }
    digital(7, 1);
    TEST_ASSERT_FALSE(conn.broken);
    drain();
    TEST_ASSERT_EQUAL(55, sent_n);
    const uint8_t want[] = {3, 2, 7, 1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sent + 51, sizeof(want));
    conn_stop();
}

TEST_CASE("queue of two updates to a key that is pending: only the latest is delivered", "[conn]")
{
    conn_start(64);
    fill_digital();
    digital(40, 0);
    digital(40, 1);
    TEST_ASSERT_EQUAL_UINT32(1, conn.coalesced);
    drain();
    TEST_ASSERT_EQUAL(52, sent_n);
    const uint8_t want[] = {3, 2, 40, 1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sent + 48, sizeof(want));
    conn_stop();
}

TEST_CASE("flush with several pending keyed reports: they enter the queue in arrival order", "[conn]")
{
    conn_start(64);
    fill_digital();
    analog(34, 0, 9);
    digital(41, 1);
    digital(40, 1);
    drain();
    TEST_ASSERT_EQUAL(61, sent_n);
    const uint8_t want[] = {4, 3, 34, 0, 9, 3, 2, 41, 1, 3, 2, 40, 1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sent + 48, sizeof(want));
    conn_stop();
}

TEST_CASE("flush after a partial send: pending reports move in behind the unsent bytes", "[conn]")
{
    conn_start(64);
    fill_digital();
    digital(40, 1);
    digital(41, 1);
    SCRIPT(8);
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 0, 1000));
    TEST_ASSERT_EQUAL(8, sent_n);
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 1, 1000)); /* the script is spent: EAGAIN */
    drain();
    TEST_ASSERT_EQUAL(48, sent_n); /* 40 left of the queue, then the two pending reports; drain() reset the count */
    const uint8_t tail[] = {3, 2, 40, 1, 3, 2, 41, 1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tail, sent + sent_n - 8, sizeof(tail));
    conn_stop();
}

TEST_CASE("queue of an unkeyed non-reply report with no room: it is dropped and counted", "[conn]")
{
    conn_start(64);
    for (uint8_t pin = 0; pin < 12; pin++) {
        digital(pin, 1);
    }
    const uint8_t r[] = {2, 0, 1};
    tmx_conn_queue(&conn, r, sizeof(r), false, TMX_COALESCE_NONE, 0);
    TEST_ASSERT_EQUAL_UINT32(1, conn.dropped);
    TEST_ASSERT_EQUAL(48, tmx_conn_pending(&conn));
    conn_stop();
}

TEST_CASE("queue of a reply with a full queue: the reply is kept in the reserve and never dropped", "[conn]")
{
    conn_start(64);
    for (uint8_t pin = 0; pin < 12; pin++) {
        digital(pin, 1);
    }
    reply_loop(1);
    reply_loop(2);
    TEST_ASSERT_EQUAL(54, tmx_conn_pending(&conn));
    TEST_ASSERT_FALSE(conn.broken);
    drain();
    const uint8_t tail[] = {2, 0, 1, 2, 0, 2};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tail, sent + 48, sizeof(tail));
    conn_stop();
}

TEST_CASE("queue of a reply that fits nowhere: flush returns TMX_CONN_OVERFLOW", "[conn]")
{
    conn_start(64);
    for (int i = 0; i < 30; i++) { /* 90 bytes into a queue of 64 */
        reply_loop((uint8_t)i);
    }
    SCRIPT(0);
    TEST_ASSERT_EQUAL(TMX_CONN_OVERFLOW, tmx_conn_flush(&conn, 3, 0, 10000));
    conn_stop();
}

TEST_CASE("queue of a reply after a partial send: the sent bytes make room again", "[conn]")
{
    conn_start(64);
    for (uint8_t pin = 0; pin < 12; pin++) {
        digital(pin, 1);
    }
    SCRIPT(20);
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 0, 1000));
    digital(60, 1);                     /* 28 live + 4 fits outside the reserve; the tail needs compacting */
    TEST_ASSERT_EQUAL(32, tmx_conn_pending(&conn));
    TEST_ASSERT_EQUAL_UINT32(0, conn.coalesced);
    SCRIPT(1000);
    TEST_ASSERT_EQUAL(TMX_CONN_IDLE, tmx_conn_flush(&conn, 3, 0, 1000));
    TEST_ASSERT_EQUAL(32, sent_n);
    const uint8_t last[] = {3, 2, 60, 1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(last, sent + 28, sizeof(last));
    conn_stop();
}

TEST_CASE("flush with partial sends: the bytes arrive complete and in order", "[conn]")
{
    conn_start(256);
    digital(1, 1);
    analog(34, 1, 2);
    reply_loop(9);
    SCRIPT(3, 2, 1, -EAGAIN);
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 0, 1000));
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 1, 1000));
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 2, 1000));
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 3, 1000));
    TEST_ASSERT_EQUAL(6, sent_n);
    TEST_ASSERT_EQUAL(6, tmx_conn_pending(&conn));
    uint8_t first[6];
    memcpy(first, sent, sizeof(first));
    SCRIPT(1000);
    TEST_ASSERT_EQUAL(TMX_CONN_IDLE, tmx_conn_flush(&conn, 3, 4, 1000));
    const uint8_t want[] = {3, 2, 1, 1, 4, 3, 34, 1, 2, 2, 0, 9};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, first, 6);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want + 6, sent, 6);
    conn_stop();
}

TEST_CASE("flush after a partial send of a keyed entry: a newer value is queued behind it, not written into it", "[conn]")
{
    conn_start(256);
    digital(1, 0);
    SCRIPT(2);
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 0, 1000));
    digital(1, 1);
    TEST_ASSERT_EQUAL_UINT32(0, conn.coalesced);
    SCRIPT(1000);
    TEST_ASSERT_EQUAL(TMX_CONN_IDLE, tmx_conn_flush(&conn, 3, 1, 1000));
    TEST_ASSERT_EQUAL(6, sent_n); /* the 2 bytes sent earlier were reset by SCRIPT: 2 left of the first, then the second */
    const uint8_t want[] = {1, 0, 3, 2, 1, 1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sent, sizeof(want));
    conn_stop();
}

TEST_CASE("flush on an empty queue: TMX_CONN_IDLE and no send call", "[conn]")
{
    conn_start(256);
    SCRIPT(5);
    TEST_ASSERT_EQUAL(TMX_CONN_IDLE, tmx_conn_flush(&conn, 3, 0, 1000));
    TEST_ASSERT_EQUAL_size_t(0, script_i);
    conn_stop();
}

TEST_CASE("flush on a peer that accepts no bytes for stall_ms: TMX_CONN_STALLED", "[conn]")
{
    conn_start(256);
    digital(1, 1);
    SCRIPT(0, 0, 0, 0);
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 1000, 500));
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 1499, 500));
    TEST_ASSERT_EQUAL(TMX_CONN_STALLED, tmx_conn_flush(&conn, 3, 1500, 500));
    conn_stop();
}

TEST_CASE("flush on a full socket (EAGAIN) for stall_ms: TMX_CONN_STALLED", "[conn]")
{
    conn_start(256);
    digital(1, 1);
    SCRIPT(-EAGAIN, -EWOULDBLOCK, -EINTR);
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 0, 500));
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 400, 500));
    TEST_ASSERT_EQUAL(TMX_CONN_STALLED, tmx_conn_flush(&conn, 3, 600, 500));
    conn_stop();
}

TEST_CASE("flush with progress between tries: the stall clock restarts", "[conn]")
{
    conn_start(256);
    digital(1, 1);
    SCRIPT(0, 1, 0);
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 0, 500));
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 400, 500)); /* 1 byte went */
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 800, 500)); /* 400 ms since the progress */
    conn_stop();
}

TEST_CASE("flush after the queue emptied and filled again: the stall clock starts at the new data", "[conn]")
{
    conn_start(256);
    digital(1, 1);
    SCRIPT(100);
    TEST_ASSERT_EQUAL(TMX_CONN_IDLE, tmx_conn_flush(&conn, 3, 0, 500));
    digital(1, 0);
    SCRIPT(0);
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 9000, 500));
    conn_stop();
}

TEST_CASE("flush with a stall_ms of 0: never stalls", "[conn]")
{
    conn_start(256);
    digital(1, 1);
    SCRIPT(0, 0);
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 0, 0));
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 100000, 0));
    conn_stop();
}

TEST_CASE("flush on a socket error: TMX_CONN_ERROR", "[conn]")
{
    conn_start(256);
    digital(1, 1);
    SCRIPT(-EPIPE);
    TEST_ASSERT_EQUAL(TMX_CONN_ERROR, tmx_conn_flush(&conn, 3, 0, 500));
    conn_stop();
}

TEST_CASE("reset: the queue is empty, the broken and stall state are clear, the counters stay", "[conn]")
{
    conn_start(64);
    digital(1, 0);
    digital(1, 1);
    for (int i = 0; i < 30; i++) {
        reply_loop((uint8_t)i);
    }
    TEST_ASSERT_TRUE(conn.broken);
    tmx_conn_reset(&conn);
    TEST_ASSERT_FALSE(conn.broken);
    TEST_ASSERT_EQUAL(0, tmx_conn_pending(&conn));
    TEST_ASSERT_EQUAL_UINT32(1, conn.coalesced);
    TEST_ASSERT_EQUAL(TMX_CONN_IDLE, tmx_conn_flush(&conn, 3, 0, 500));
    digital(1, 1);
    TEST_ASSERT_EQUAL_UINT32(1, conn.coalesced); /* the key table was cleared: a fresh entry, no replacement */
    conn_stop();
}

TEST_CASE("reset with pending keyed reports: they are forgotten", "[conn]")
{
    conn_start(64);
    fill_digital();
    digital(40, 1);
    tmx_conn_reset(&conn);
    TEST_ASSERT_EQUAL(TMX_CONN_IDLE, tmx_conn_flush(&conn, 3, 0, 500));
    TEST_ASSERT_EQUAL(0, sent_n);
    conn_stop();
}

TEST_CASE("flush that empties a nearly full queue with a report pending, then a requeue of a sent key: no stale entry is "
          "rewritten", "[conn]")
{
    conn_start(64);
    fill_digital();                                     /* 48 bytes, keyed pins 0..11 */
    for (int i = 0; i < 3; i++) {
        reply_loop((uint8_t)i);                         /* 57 */
    }
    const uint8_t five[] = {4, 0, 0, 0, 0};
    tmx_conn_queue(&conn, five, sizeof(five), true, TMX_COALESCE_NONE, 0);   /* 62: the tail is close to cap */
    digital(40, 1);                                     /* pending: no room outside the reserve */
    SCRIPT(62, -EAGAIN);                                /* sends 62, then the pending report needs a compaction */
    TEST_ASSERT_EQUAL(TMX_CONN_PENDING, tmx_conn_flush(&conn, 3, 0, 1000));
    TEST_ASSERT_EQUAL(4, tmx_conn_pending(&conn));
    digital(0, 0);                                      /* pin 0 was sent: a new entry, not a rewrite of old bytes */
    TEST_ASSERT_EQUAL_UINT32(0, conn.coalesced);
    drain();
    const uint8_t want[] = {3, 2, 40, 1, 3, 2, 0, 0};
    TEST_ASSERT_EQUAL(8, sent_n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sent, sizeof(want));
    conn_stop();
}

/* Extended-mode reports pass the queue whole: CAPABILITIES at most 255 bytes, BUS_REPORT 66. */

static void queue_reply_of(size_t n, uint8_t id)
{
    static uint8_t r[TMX_EXT_REPORT_MAX];
    r[0] = (uint8_t)(n - 1);
    r[1] = id;
    for (size_t i = 2; i < n; i++) {
        r[i] = (uint8_t)i;
    }
    tmx_conn_queue(&conn, r, n, true, TMX_COALESCE_NONE, 0);
}

TEST_CASE("queue of a 255-byte CAPABILITIES reply: delivered whole", "[conn]")
{
    conn_start(4096);
    queue_reply_of(255, TMX_RPT_CAPABILITIES);
    TEST_ASSERT_EQUAL(255, tmx_conn_pending(&conn));
    drain();
    TEST_ASSERT_EQUAL(255, sent_n);
    TEST_ASSERT_EQUAL_UINT8(254, sent[0]);
    TEST_ASSERT_EQUAL_UINT8(254, sent[254]);
    conn_stop();
}

TEST_CASE("queue of a 66-byte BUS_REPORT reply: delivered whole", "[conn]")
{
    conn_start(4096);
    queue_reply_of(66, TMX_RPT_BUS_REPORT);
    TEST_ASSERT_EQUAL(66, tmx_conn_pending(&conn));
    drain();
    TEST_ASSERT_EQUAL(66, sent_n);
    TEST_ASSERT_EQUAL_UINT8(65, sent[0]);
    TEST_ASSERT_EQUAL_UINT8(TMX_RPT_BUS_REPORT, sent[1]);
    conn_stop();
}
