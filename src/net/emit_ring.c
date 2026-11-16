#include <string.h>

#include "telemetrix_plugin.h"
#include "tmx_adapter.h"
#include "tmx_net.h"

/* The async emit ring: plugins' tasks put PLUGIN_REPORT entries, the network task takes them. Entries carry the session
 * generation read under the lock when they were put; a bump (session start or end) makes every older entry stale, and
 * stale entries are dropped at take and make room at put. Since generations only grow and every put reads its
 * generation under the lock, entries in the ring never decrease in generation from head to tail: stale ones are at the
 * head. The open flag and the generation are atomics so the network task never waits to bump; a put that a bump
 * overtakes after its read is tagged with the old generation and dropped at take.
 *
 * Late calls after stop: the server registers its plugin registry and deregisters it (under the lock) before freeing
 * it; emit_async and wake only touch a ctx that belongs to the registered registry.
 *
 * The lock is a tiny shim: a FreeRTOS mutex on chips and the IDF linux target, nothing in the fuzz build (TMX_FUZZ),
 * which is single-threaded and has no FreeRTOS. */

#define LOCK_WAIT_MS 10

#ifdef TMX_FUZZ
static bool lock_take(void)
{
    return true;
}

static void lock_give(void)
{
}

static void lock_create(void)
{
}

static bool lock_ready(void)
{
    return true;
}
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;

static bool lock_take(void)
{
    TickType_t ticks = pdMS_TO_TICKS(LOCK_WAIT_MS);
    return lock && xSemaphoreTake(lock, ticks ? ticks : 1) == pdTRUE;
}

static void lock_give(void)
{
    xSemaphoreGive(lock);
}

static void lock_create(void)
{
    if (!lock) {
        lock = xSemaphoreCreateMutexStatic(&lock_storage);
    }
}

static bool lock_ready(void)
{
    return lock != NULL;
}
#endif

static tmx_emit_entry_t ring[TMX_EMIT_RING_ENTRIES];
static size_t head, count;          /* guarded by the lock */
static uint32_t generation;         /* atomic */
static bool open_for_puts;          /* atomic */
static const tmx_plugins_t *registry;   /* guarded by the lock */
static void (*wake_fn)(void);
static void (*put_hook)(void);

/** True when entry generation g is older than gen (wrap-safe). */
static bool older(uint32_t g, uint32_t gen)
{
    return (int32_t)(g - gen) < 0;
}

/** Drop the stale entries at the head. Under the lock. */
static void drop_stale(uint32_t gen)
{
    while (count > 0 && older(ring[head].generation, gen)) {
        head = (head + 1) % TMX_EMIT_RING_ENTRIES;
        count--;
    }
}

void tmx_emit_ring_reset(void)
{
    lock_create();
    __atomic_store_n(&open_for_puts, false, __ATOMIC_RELEASE);
    __atomic_fetch_add(&generation, 1, __ATOMIC_ACQ_REL);
    head = count = 0;
    registry = NULL;
    wake_fn = NULL;
    put_hook = NULL;
}

void tmx_emit_ring_set_wake(void (*fn)(void))
{
    wake_fn = fn;
}

void tmx_emit_ring_set_put_hook(void (*fn)(void))
{
    put_hook = fn;
}

void tmx_emit_ring_register(const tmx_plugins_t *r)
{
    /* the lock must be had: a NULL here is what keeps a late emit_async away from a freed ctx. The holders keep it
     * for a copy of at most 251 bytes, so the bounded wait only fails if a holder was preempted for that long; retry. */
    lock_create();
    while (!lock_take()) {
    }
    registry = r;
    lock_give();
}

/** ctx belongs to the registered registry. Under the lock; never dereferences ctx. */
static bool registered(const telemetrix_plugin_ctx_t *ctx)
{
    for (size_t i = 0; registry && i < registry->n; i++) {
        if (ctx == &registry->ctx[i]) {
            return true;
        }
    }
    return false;
}

void tmx_emit_ring_bump(bool open)
{
    /* open first, generation second: a put that reads the new generation also sees the new open flag */
    __atomic_store_n(&open_for_puts, open, __ATOMIC_RELEASE);
    __atomic_fetch_add(&generation, 1, __ATOMIC_ACQ_REL);
}

static void wake(void)
{
    void (*fn)(void) = wake_fn;
    if (fn) {
        fn();
    }
}

/** The put itself, under the lock. */
static esp_err_t put_locked(uint8_t slot, uint8_t sub, const uint8_t *data, size_t n)
{
    const uint32_t gen = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&open_for_puts, __ATOMIC_ACQUIRE)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (put_hook) {
        put_hook();
    }
    drop_stale(gen);
    if (count >= TMX_EMIT_RING_ENTRIES) {
        return ESP_ERR_NO_MEM;
    }
    tmx_emit_entry_t *e = &ring[(head + count) % TMX_EMIT_RING_ENTRIES];
    e->generation = gen;
    e->slot = slot;
    e->sub = sub;
    e->n = (uint8_t)n;
    if (n) {
        memcpy(e->data, data, n);
    }
    count++;
    return ESP_OK;
}

esp_err_t tmx_emit_ring_put(uint8_t slot, uint8_t sub, const uint8_t *data, size_t n)
{
    if (n > TMX_EMIT_DATA_MAX || (n && !data)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = put_locked(slot, sub, data, n);
    lock_give();
    if (err == ESP_OK) {
        wake();
    }
    return err;
}

bool tmx_emit_ring_take(tmx_emit_entry_t *out)
{
    if (!lock_take()) {
        return false;
    }
    drop_stale(__atomic_load_n(&generation, __ATOMIC_ACQUIRE));   /* every entry is of this generation or older */
    bool got = count > 0;
    if (got) {
        *out = ring[head];
        head = (head + 1) % TMX_EMIT_RING_ENTRIES;
        count--;
    }
    lock_give();
    return got;
}

void tmx_emit_ring_drain(struct tmx_adapter *a)
{
    tmx_emit_entry_t e;
    uint8_t out[TMX_EXT_REPORT_MAX];
    while (tmx_emit_ring_take(&e)) {
        if (a->session.extended) {
            size_t m = tmx_rpt_plugin(out, e.slot, e.sub, e.data, e.n);
            a->emit(out, m, true, TMX_COALESCE_NONE, 0, a->ctx);
        }
    }
}

esp_err_t telemetrix_plugin_emit_async(telemetrix_plugin_ctx_t *ctx, uint8_t sub, const uint8_t *data, size_t n)
{
    if (!ctx || n > TMX_EMIT_DATA_MAX || (n && !data)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (registered(ctx)) {
        err = put_locked(ctx->slot, sub, data, n);
        if (err == ESP_ERR_NO_MEM) {
            __atomic_fetch_add(&ctx->async_dropped, 1, __ATOMIC_RELAXED);
        }
    }
    lock_give();
    if (err == ESP_OK) {
        wake();
    }
    return err;
}

void telemetrix_plugin_wake(telemetrix_plugin_ctx_t *ctx)
{
    /* never dropped: the holders keep the lock for a copy of at most 251 bytes and the mutex lends them the waiter's
     * priority, so retrying the bounded take ends (as tmx_emit_ring_register does). No lock yet = no server ever ran. */
    if (!ctx || !lock_ready()) {
        return;
    }
    while (!lock_take()) {
    }
    bool ok = registered(ctx);
    if (ok) {
        __atomic_store_n(&ctx->wake, true, __ATOMIC_RELEASE);
    }
    lock_give();
    if (ok) {
        wake();
    }
}
