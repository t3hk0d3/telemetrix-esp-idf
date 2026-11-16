#include <string.h>

#include "tmx_core.h"

#if CONFIG_IDF_TARGET_LINUX
#include <pthread.h>

#define DRAM_ATTR

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
#else
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_attr.h"

static DRAM_ATTR portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL_SAFE(&lock)
#define UNLOCK() portEXIT_CRITICAL_SAFE(&lock)
#endif

static DRAM_ATTR tmx_completion_t ring[TMX_COMPLETION_RING];
static DRAM_ATTR unsigned head;  /* next to take */
static DRAM_ATTR unsigned count; /* queued entries */
static DRAM_ATTR uint32_t overflows;
static DRAM_ATTR void (*wake_fn)(void);

static void no_wake(void)
{
}

void tmx_completion_post(const tmx_completion_t *c)
{
    LOCK();
    if (count < TMX_COMPLETION_RING) {
        ring[(head + count) % TMX_COMPLETION_RING] = *c;
        count++;
    } else {
        overflows++;
    }
    void (*wake)(void) = wake_fn ? wake_fn : no_wake;
    UNLOCK();
    wake();
}

bool tmx_completion_take(tmx_completion_t *out)
{
    bool got = false;
    LOCK();
    if (count > 0) {
        *out = ring[head];
        head = (head + 1) % TMX_COMPLETION_RING;
        count--;
        got = true;
    }
    UNLOCK();
    return got;
}

void tmx_completion_set_wake(void (*fn)(void))
{
    LOCK();
    wake_fn = fn;
    UNLOCK();
}

void tmx_completion_reset(void)
{
    LOCK();
    head = 0;
    count = 0;
    overflows = 0;
    memset(ring, 0, sizeof(ring));
    UNLOCK();
}

uint32_t tmx_completion_overflows(void)
{
    LOCK();
    uint32_t n = overflows;
    UNLOCK();
    return n;
}
