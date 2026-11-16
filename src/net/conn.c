#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_heap_caps.h"
#include "tmx_adapter.h"
#include "tmx_net.h"

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

esp_err_t tmx_conn_init(tmx_conn_t *c, uint16_t bytes)
{
    if (!c || bytes < TMX_REPORT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(c, 0, sizeof(*c));
    c->buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!c->buf) {
        return ESP_ERR_NO_MEM;
    }
    c->cap = bytes;
    c->send_fn = send;
    return ESP_OK;
}

void tmx_conn_deinit(tmx_conn_t *c)
{
    if (c) {
        free(c->buf);
        memset(c, 0, sizeof(*c));
    }
}

void tmx_conn_set_sender(tmx_conn_t *c, tmx_send_fn send_fn)
{
    c->send_fn = send_fn ? send_fn : send;
}

size_t tmx_conn_pending(const tmx_conn_t *c)
{
    return (size_t)(c->tail - c->head);
}

void tmx_conn_reset(tmx_conn_t *c)
{
    c->head = c->tail = 0;
    c->n_keyed = 0;
    c->broken = false;
    c->stalling = false;
    c->stall_since = 0;
}

/** Whether n more bytes fit when at most limit bytes may be queued. */
static bool fits(const tmx_conn_t *c, size_t n, size_t limit)
{
    return tmx_conn_pending(c) + n <= limit;
}

/** Move the unsent bytes to the start of the buffer. */
static void compact(tmx_conn_t *c)
{
    if (c->head == 0) {
        return;
    }
    memmove(c->buf, c->buf + c->head, c->tail - c->head);
    for (uint8_t i = 0; i < c->n_keyed; i++) {
        c->keyed[i].off -= c->head;
    }
    c->tail -= c->head;
    c->head = 0;
}

static void remove_keyed(tmx_conn_t *c, uint8_t idx)
{
    tmx_conn_key_t gone = c->keyed[idx];
    memmove(c->buf + gone.off, c->buf + gone.off + gone.len, c->tail - gone.off - gone.len);
    c->tail -= gone.len;
    for (uint8_t i = idx + 1; i < c->n_keyed; i++) {
        c->keyed[i - 1] = c->keyed[i];
        c->keyed[i - 1].off -= gone.len;
    }
    c->n_keyed--;
}

/** Append; the caller has checked the room. A keyed entry is tracked for coalescing while the table has space. */
static void append(tmx_conn_t *c, const uint8_t *r, size_t n, int8_t key_kind, uint16_t key)
{
    if (c->tail + n > c->cap) {
        compact(c);
    }
    memcpy(c->buf + c->tail, r, n);
    if (key_kind >= 0 && c->n_keyed < TMX_CONN_KEYED_MAX) {
        c->keyed[c->n_keyed++] = (tmx_conn_key_t) {.off = c->tail, .len = (uint8_t)n, .kind = key_kind, .key = key};
    }
    c->tail += (uint16_t)n;
}

void tmx_conn_queue(tmx_conn_t *c, const uint8_t *r, size_t n, bool reply, int8_t key_kind, uint16_t key)
{
    if (!c->buf || c->broken || n == 0 || n > UINT8_MAX) {
        return;
    }
    if (reply) {
        /* never coalesced, never dropped: the whole queue is open to it, and past that the connection is lost */
        if (fits(c, n, c->cap)) {
            append(c, r, n, TMX_COALESCE_NONE, 0);
        } else {
            c->broken = true;
        }
        return;
    }
    size_t limit = c->cap - c->cap / 4;     /* the last quarter is held back for replies */
    bool replaced = false;
    if (key_kind >= 0) {
        for (uint8_t i = 0; i < c->n_keyed; i++) {
            if (c->keyed[i].kind == key_kind && c->keyed[i].key == key) {
                if (c->keyed[i].len == n) {
                    memcpy(c->buf + c->keyed[i].off, r, n);
                    c->coalesced++;
                    return;
                }
                remove_keyed(c, i);         /* another size: the new report goes to the end */
                replaced = true;
                break;
            }
        }
    }
    if (replaced) {
        c->coalesced++;
    }
    if (key_kind >= 0) {
        while (!fits(c, n, limit)) {
            uint8_t i = 0;
            while (i < c->n_keyed && c->keyed[i].kind != key_kind) {
                i++;
            }
            if (i == c->n_keyed) {
                break;
            }
            remove_keyed(c, i);
            c->coalesced++;
        }
    }
    if (fits(c, n, limit)) {
        append(c, r, n, key_kind, key);
    } else {
        c->dropped++;
    }
}

/** Forget keyed entries that have begun to leave: they can no longer be rewritten. */
static void prune_keyed(tmx_conn_t *c)
{
    uint8_t gone = 0;
    while (gone < c->n_keyed && c->keyed[gone].off < c->head) {
        gone++;
    }
    if (gone) {
        memmove(c->keyed, c->keyed + gone, (size_t)(c->n_keyed - gone) * sizeof(c->keyed[0]));
        c->n_keyed -= gone;
    }
}

tmx_conn_state_t tmx_conn_flush(tmx_conn_t *c, int fd, uint32_t now_ms, uint16_t stall_ms)
{
    if (c->broken) {
        return TMX_CONN_STALLED;
    }
    while (c->head < c->tail) {
        size_t want = tmx_conn_pending(c);
        ssize_t r = c->send_fn(fd, c->buf + c->head, want, SEND_FLAGS);
        if (r > 0) {
            c->head += (uint16_t)r;
            c->stalling = true;
            c->stall_since = now_ms;
            if ((size_t)r < want) {
                break;
            }
        } else if (r == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            break;
        } else {
            return TMX_CONN_ERROR;
        }
    }
    if (c->head == c->tail) {
        c->head = c->tail = 0;
        c->n_keyed = 0;
        c->stalling = false;
        return TMX_CONN_IDLE;
    }
    prune_keyed(c);
    if (!c->stalling) {
        c->stalling = true;
        c->stall_since = now_ms;
    }
    if (stall_ms != 0 && (int32_t)(now_ms - c->stall_since) >= (int32_t)stall_ms) {
        return TMX_CONN_STALLED;
    }
    return TMX_CONN_PENDING;
}
