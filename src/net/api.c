#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tmx_net.h"

#if CONFIG_IDF_TARGET_LINUX
#include <pthread.h>

static pthread_mutex_t crit = PTHREAD_MUTEX_INITIALIZER;
#define CRIT_ENTER() pthread_mutex_lock(&crit)
#define CRIT_EXIT() pthread_mutex_unlock(&crit)
#else
#include "freertos/portmacro.h"

static portMUX_TYPE crit = portMUX_INITIALIZER_UNLOCKED;
#define CRIT_ENTER() portENTER_CRITICAL_SAFE(&crit)
#define CRIT_EXIT() portEXIT_CRITICAL_SAFE(&crit)
#endif

#define STOP_WAIT_MS 2000
#define STOP_POLL_MS 10
#define LOCK_WAIT_MS 1000

typedef enum { STOPPED, STARTING, RUNNING, STOPPING } state_t;

/* The lock guards the state, the server pointer and the snapshot. Nothing that calls a hook or waits runs under it. */
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;
static state_t state = STOPPED;
static tmx_server_t *server;
static telemetrix_status_t snapshot;

static bool lock_take(uint32_t wait_ms)
{
    CRIT_ENTER();
    if (!lock) {
        lock = xSemaphoreCreateMutexStatic(&lock_storage);
    }
    CRIT_EXIT();
    return xSemaphoreTake(lock, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

static void lock_give(void)
{
    xSemaphoreGive(lock);
}

bool tmx_api_publish(const telemetrix_status_t *status)
{
    if (!lock_take(100)) {
        return false;
    }
    snapshot = *status;
    lock_give();
    return true;
}

esp_err_t telemetrix_start(const telemetrix_profile_t *profile, const telemetrix_config_t *config,
                           const telemetrix_hooks_t *hooks)
{
    if (!profile) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_take(LOCK_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (state != STOPPED) {
        lock_give();
        return ESP_ERR_INVALID_STATE;
    }
    state = STARTING;
    lock_give();

    telemetrix_config_t defaults;
    telemetrix_config_default(&defaults);
    const telemetrix_hooks_t no_hooks = {0};
    tmx_server_t *s = NULL;
    esp_err_t err = tmx_server_new(profile, config ? config : &defaults, hooks ? hooks : &no_hooks, &s);
    if (err == ESP_OK) {
        tmx_server_publish(s);
        const telemetrix_config_t *c = config ? config : &defaults;
        BaseType_t core = c->task_core < 0 ? tskNO_AFFINITY : c->task_core;
        if (xTaskCreatePinnedToCore(tmx_server_task, "telemetrix", c->task_stack_bytes, s, c->task_priority, NULL,
                                    core) != pdPASS) {
            tmx_server_free(s);
            s = NULL;
            err = ESP_ERR_NO_MEM;
        }
    }
    if (lock_take(LOCK_WAIT_MS)) {
        server = s;
        state = s ? RUNNING : STOPPED;
        if (!s) {
            memset(&snapshot, 0, sizeof(snapshot));     /* a failed start leaves nothing, not even a snapshot */
        }
        lock_give();
    } else {
        server = s;                     /* cannot happen with a mutex nobody holds across a wait */
        state = s ? RUNNING : STOPPED;
    }
    return err;
}

esp_err_t telemetrix_stop(void)
{
    if (!lock_take(LOCK_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (state != RUNNING) {
        lock_give();
        return ESP_ERR_INVALID_STATE;
    }
    state = STOPPING;
    tmx_server_t *s = server;
    lock_give();

    tmx_server_request_stop(s);
    for (uint32_t waited = 0; !tmx_server_done(s) && waited < STOP_WAIT_MS; waited += STOP_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(STOP_POLL_MS) ? pdMS_TO_TICKS(STOP_POLL_MS) : 1);
    }
    bool done = tmx_server_done(s);
    if (lock_take(LOCK_WAIT_MS)) {
        if (done) {
            server = NULL;
            state = STOPPED;
            memset(&snapshot, 0, sizeof(snapshot));
        } else {
            state = RUNNING;           /* the stop request stays set; a later call can finish the job */
        }
        lock_give();
    }
    if (!done) {
        return ESP_ERR_TIMEOUT;
    }
    tmx_server_free(s);
    return ESP_OK;
}

esp_err_t telemetrix_get_status(telemetrix_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_take(LOCK_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    *out = snapshot;
    lock_give();
    return ESP_OK;
}

void telemetrix_kick_client(void)
{
    if (!lock_take(LOCK_WAIT_MS)) {
        return;
    }
    if (state == RUNNING && server) {
        tmx_server_kick(server);
    }
    lock_give();
}
