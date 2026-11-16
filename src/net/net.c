#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tmx_adapter.h"
#include "tmx_core.h"
#include "tmx_net.h"
#include "tmx_proto.h"

#define LOOP_MAX_MS 100
#define RECV_CHUNK 128

static const char *TAG = "telemetrix";

extern const tmx_drv_t *const tmx_drv_default;

struct tmx_server {
    telemetrix_profile_t profile;       /**< deep copy: the arrays below are ours */
    telemetrix_config_t cfg;
    telemetrix_hooks_t hooks;
    telemetrix_pin_t *pins;
    telemetrix_adc_grant_t *adc;
    telemetrix_i2c_bus_t *i2c;
    telemetrix_onewire_bus_t *onewire;
    telemetrix_cidr_t *allowed;
    const telemetrix_plugin_t **plugins;
    const char **named;                 /**< every pin's named-capability array, back to back */

    const tmx_drv_t *drv;
    tmx_adapter_t adapter;
    tmx_conn_t conn;
    tmx_frame_t frame;
    int listener, client;
    uint32_t now;                       /**< loop time handed to the adapter by the packet callback */

    uint32_t refused_connections;
    uint32_t stalled_clients;
    uint32_t dropped_before;            /**< dropped packets of earlier clients */
    char peer[TELEMETRIX_PEER_MAX];
    telemetrix_status_t status;         /**< last published snapshot */

    volatile bool stop, kick, done;
};

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000);
}

static void *dup_array(const void *src, size_t n, size_t size)
{
    if (n == 0) {
        return NULL;
    }
    void *p = malloc(n * size);
    if (p) {
        memcpy(p, src, n * size);
    }
    return p;
}

static void emit(const uint8_t *report, size_t n, bool reply, int8_t kind, uint16_t key, void *ctx)
{
    tmx_conn_queue(&((tmx_server_t *)ctx)->conn, report, n, reply, kind, key);
}

static esp_err_t check_config(const telemetrix_config_t *c)
{
    if (c->send_queue_bytes < TMX_REPORT_MAX || c->task_priority >= configMAX_PRIORITIES ||
            c->task_core >= (int)configNUMBER_OF_CORES || (c->n_allowed > 0 && !c->allowed) ||
            (c->n_plugins > 0 && !c->plugins)) {
        return ESP_ERR_INVALID_ARG;
    }
#ifndef CONFIG_TELEMETRIX_EXTENDED
    if (c->n_plugins > 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    for (size_t i = 0; i < c->n_allowed; i++) {
        const telemetrix_cidr_t *a = &c->allowed[i];
        if ((a->family != AF_INET || a->prefix > 32) && (a->family != AF_INET6 || a->prefix > 128)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

void tmx_server_free(tmx_server_t *s)
{
    if (!s) {
        return;
    }
    tmx_completion_set_wake(NULL);
    tmx_emit_ring_bump(false);
    tmx_emit_ring_set_wake(NULL);
    tmx_wake_close();
    if (s->listener >= 0) {
        close(s->listener);
    }
    if (s->client >= 0) {
        close(s->client);
    }
    tmx_conn_deinit(&s->conn);
    if (s->drv) {
        s->drv->deinit();
    }
    free(s->pins);
    free(s->adc);
    free(s->i2c);
    free(s->onewire);
    free(s->allowed);
    free(s->plugins);
    free(s->named);
    free(s);
}

/** Copy every pin's named-capability array into one block and point the (already copied) pins at it. The strings
 *  themselves are the application's. */
static esp_err_t copy_named(tmx_server_t *s)
{
    size_t total = 0;
    for (size_t i = 0; i < s->profile.n_pins; i++) {
        if (s->pins[i].n_named && !s->pins[i].named) {
            return ESP_ERR_INVALID_ARG;
        }
        total += s->pins[i].n_named;
    }
    if (total == 0) {
        return ESP_OK;
    }
    s->named = malloc(total * sizeof(*s->named));
    if (!s->named) {
        return ESP_ERR_NO_MEM;
    }
    const char **next = s->named;
    for (size_t i = 0; i < s->profile.n_pins; i++) {
        telemetrix_pin_t *pin = &s->pins[i];
        if (pin->n_named) {
            memcpy(next, pin->named, pin->n_named * sizeof(*next));
            pin->named = next;
            next += pin->n_named;
        }
    }
    return ESP_OK;
}

esp_err_t tmx_server_new(const telemetrix_profile_t *profile, const telemetrix_config_t *config,
                         const telemetrix_hooks_t *hooks, tmx_server_t **out)
{
    esp_err_t err = tmx_profile_validate(profile);
    if (err == ESP_OK) {
        err = check_config(config);
    }
    if (err != ESP_OK) {
        return err;
    }
    tmx_server_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    s->listener = s->client = -1;
    s->cfg = *config;
    s->hooks = *hooks;
    s->profile = *profile;
    s->pins = dup_array(profile->pins, profile->n_pins, sizeof(*profile->pins));
    s->adc = dup_array(profile->adc, profile->n_adc, sizeof(*profile->adc));
    s->i2c = dup_array(profile->i2c, profile->n_i2c, sizeof(*profile->i2c));
    s->onewire = dup_array(profile->onewire, profile->n_onewire, sizeof(*profile->onewire));
    s->allowed = dup_array(config->allowed, config->n_allowed, sizeof(*config->allowed));
    s->plugins = dup_array(config->plugins, config->n_plugins, sizeof(*config->plugins));
    if ((profile->n_pins && !s->pins) || (profile->n_adc && !s->adc) || (profile->n_i2c && !s->i2c) ||
            (profile->n_onewire && !s->onewire) || (config->n_allowed && !s->allowed) ||
            (config->n_plugins && !s->plugins)) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = copy_named(s);
    if (err != ESP_OK) {
        goto fail;
    }
    s->profile.pins = s->pins;
    s->profile.adc = s->adc;
    s->profile.i2c = s->i2c;
    s->profile.onewire = s->onewire;
    s->cfg.allowed = s->allowed;
    s->cfg.plugins = s->plugins;

    err = tmx_conn_init(&s->conn, s->cfg.send_queue_bytes);
    if (err != ESP_OK) {
        goto fail;
    }
    err = tmx_drv_default->init(&s->profile);
    if (err != ESP_OK) {
        goto fail;
    }
    s->drv = tmx_drv_default;           /* from here on free() calls deinit */
    tmx_completion_reset();
    tmx_emit_ring_reset();              /* closed until a client enters extended mode */
    /* registers the plugins and checks the pins' named capabilities against them, then every pin to its safe state */
    err = tmx_adapter_init(&s->adapter, &s->profile, &s->cfg, s->drv, emit, s);
    if (err != ESP_OK) {
        goto fail;
    }
    tmx_frame_init(&s->frame, TMX_STOCK_PAYLOAD_MAX);
    err = tmx_wake_open();
    if (err != ESP_OK) {
        goto fail;
    }
    tmx_completion_set_wake(tmx_net_wake);
    tmx_emit_ring_set_wake(tmx_net_wake);
    if (s->cfg.server_enabled) {
        s->listener = tmx_sock_listen(s->cfg.port);
        if (s->listener < 0 || s->listener >= FD_SETSIZE) {
            ESP_LOGE(TAG, "cannot listen on port %u (errno %d)", (unsigned)s->cfg.port, errno);
            err = ESP_FAIL;
            goto fail;
        }
    }
    *out = s;
    return ESP_OK;
fail:
    tmx_server_free(s);
    return err;
}

void tmx_server_request_stop(tmx_server_t *s)
{
    s->stop = true;
    tmx_net_wake();
}

bool tmx_server_done(const tmx_server_t *s)
{
    return s->done;
}

void tmx_server_kick(tmx_server_t *s)
{
    s->kick = true;
    tmx_net_wake();
}

/** Compose the snapshot telemetrix_get_status hands out. */
static void build_status(const tmx_server_t *s, telemetrix_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->running = true;
    st->listening = s->listener >= 0;
    memcpy(st->client, s->peer, sizeof(st->client));
    st->refused_connections = s->refused_connections;
    st->dropped_packets = s->dropped_before + (s->client >= 0 ? s->frame.dropped : 0);
    tmx_adapter_counters(&s->adapter, &st->ignored_commands, &st->refused_commands);
    st->coalesced_reports = s->conn.coalesced;
    st->stalled_clients = s->stalled_clients;
    st->n_pins = tmx_adapter_pin_uses(&s->adapter, st->pins, sizeof(st->pins) / sizeof(st->pins[0]));
}

/** Publish the snapshot when it differs from the last one; the lock is taken only then. */
static void publish_if_changed(tmx_server_t *s)
{
    telemetrix_status_t st;
    build_status(s, &st);
    if (memcmp(&st, &s->status, sizeof(st)) != 0 && tmx_api_publish(&st)) {
        s->status = st;
    }
}

void tmx_server_publish(tmx_server_t *s)
{
    publish_if_changed(s);
}

static void packet_cb(uint8_t id, const uint8_t *payload, size_t len, void *ctx)
{
    tmx_server_t *s = ctx;
    tmx_adapter_packet(&s->adapter, id, payload, len, s->now);
    /* per packet, so commands after a RESET in the same chunk meet freshly released hardware (the extended session and
     * its numbering continue) and every RESET calls the hook */
    if (tmx_adapter_reset_requested(&s->adapter)) {
        tmx_completion_reset();         /* the adapter has released: stale completions must not meet the next session */
        if (s->hooks.reset) {
            s->hooks.reset(s->hooks.ctx);
        }
    }
}

/** Release the client: socket closed, claims freed and pins in their safe states, queue and completions dropped. Then
 *  the disconnected hook runs, with no lock held. abortive closes with an RST (SO_LINGER {1,0}): a peer that stopped
 *  reading or whose socket failed would otherwise leave a FIN_WAIT socket retransmitting in lwIP's small pool. A packet
 *  the client left unfinished counts in dropped_packets. */
static void drop_client(tmx_server_t *s, const char *why, bool abortive)
{
    ESP_LOGI(TAG, "client %s gone: %s", s->peer, why);
    if (abortive) {
        tmx_sock_refuse(s->client);
    } else {
        close(s->client);
    }
    s->client = -1;
    tmx_frame_abandon(&s->frame);
    s->dropped_before += s->frame.dropped;
    tmx_adapter_session_end(&s->adapter);
    tmx_conn_reset(&s->conn);
    tmx_completion_reset();
    s->peer[0] = '\0';
    s->kick = false;
    publish_if_changed(s);
    if (s->hooks.disconnected) {
        s->hooks.disconnected(s->hooks.ctx);
    }
}

static bool peer_ok(const tmx_server_t *s, const struct sockaddr *peer)
{
    return tmx_sock_allowed(&s->cfg, peer) && (!s->hooks.access || s->hooks.access(peer, s->hooks.ctx));
}

static void accept_one(tmx_server_t *s)
{
    struct sockaddr_storage raw, peer;
    socklen_t len = sizeof(raw);
    int fd = accept(s->listener, (struct sockaddr *)&raw, &len);
    if (fd < 0) {
        return;
    }
    tmx_sock_normalise(&raw, &peer);
    /* no byte is read from a refused peer: it gets the RST at once */
    if (s->client >= 0 || !peer_ok(s, (struct sockaddr *)&peer)) {
        s->refused_connections++;
        tmx_sock_refuse(fd);
        return;
    }
    if (fd >= FD_SETSIZE || tmx_sock_tune_client(fd, &s->cfg) != ESP_OK) {
        ESP_LOGW(TAG, "client socket setup failed (errno %d)", errno);
        close(fd);
        return;
    }
    s->client = fd;
    s->kick = false;
    tmx_frame_init(&s->frame, TMX_STOCK_PAYLOAD_MAX);
    tmx_conn_reset(&s->conn);
    tmx_completion_reset();
    tmx_sock_peer_text((struct sockaddr *)&peer, s->peer);
    ESP_LOGI(TAG, "client %s connected", s->peer);
    publish_if_changed(s);
    if (s->hooks.connected) {
        s->hooks.connected(s->peer, s->hooks.ctx);
    }
}

/** Read what the client sent. Returns false when the connection is over; *abortive is true when it failed rather than
 *  ended in order. */
static bool read_client(tmx_server_t *s, const char **why, bool *abortive)
{
    uint8_t buf[RECV_CHUNK];
    ssize_t r = recv(s->client, buf, sizeof(buf), 0);
    if (r == 0) {
        *why = "closed by the peer";
        *abortive = false;
        return false;
    }
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return true;
        }
        *why = "socket error";
        *abortive = true;
        return false;
    }
    s->now = now_ms();
    tmx_frame_feed(&s->frame, buf, (size_t)r, s->now, packet_cb, s);
    return true;
}

void tmx_server_task(void *arg)
{
    tmx_server_t *s = arg;
    ESP_LOGI(TAG, "server task up, port %u", (unsigned)s->cfg.port);
    while (!s->stop) {
        uint32_t wait_ms = LOOP_MAX_MS;
        const char *why = NULL;
        bool abortive = true;
        if (s->client >= 0) {
            s->now = now_ms();
            uint32_t left = tmx_adapter_poll(&s->adapter, s->now);
            tmx_frame_tick(&s->frame, s->now);
            if (left < wait_ms) {
                wait_ms = left ? left : 1;
            }
            tmx_conn_state_t st = tmx_conn_flush(&s->conn, s->client, s->now, s->cfg.send_stall_ms);
            if (st == TMX_CONN_STALLED) {
                s->stalled_clients++;
                why = "peer stopped reading";
            } else if (st == TMX_CONN_ERROR) {
                why = "send failed";
            } else if (s->kick) {
                why = "kicked";
                abortive = false;
            }
            if (why) {
                drop_client(s, why, abortive);
            }
        }

        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        int wake = tmx_wake_fd();
        int maxfd = wake;
        FD_SET(wake, &rd);
        if (s->listener >= 0) {
            FD_SET(s->listener, &rd);
            maxfd = s->listener > maxfd ? s->listener : maxfd;
        }
        if (s->client >= 0) {
            FD_SET(s->client, &rd);
            if (tmx_conn_pending(&s->conn) > 0) {
                FD_SET(s->client, &wr);     /* write interest only while bytes wait */
            }
            maxfd = s->client > maxfd ? s->client : maxfd;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = (suseconds_t)wait_ms * 1000};
        int n = select(maxfd + 1, &rd, &wr, NULL, &tv);
        if (n < 0) {
            if (errno != EINTR) {
                ESP_LOGE(TAG, "select failed (errno %d)", errno);
                vTaskDelay(1);
            }
            continue;
        }
        if (n > 0 && FD_ISSET(wake, &rd)) {
            tmx_wake_drain();
        }
        if (s->stop) {
            break;
        }
        if (s->client >= 0 && n > 0 && FD_ISSET(s->client, &rd) && !read_client(s, &why, &abortive)) {
            drop_client(s, why, abortive);
        }
        if (s->listener >= 0 && n > 0 && FD_ISSET(s->listener, &rd)) {
            accept_one(s);
        }
        publish_if_changed(s);
    }
    if (s->client >= 0) {
        drop_client(s, "server stopping", false);
    }
    ESP_LOGI(TAG, "server task down");
    s->done = true;                     /* last access to s: telemetrix_stop frees it after this */
    vTaskDelete(NULL);
}
