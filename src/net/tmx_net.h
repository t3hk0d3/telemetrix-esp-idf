#ifndef TMX_NET_H
#define TMX_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "telemetrix.h"

/** Wake the network task out of select(). Safe to call from an ISR and from any task; no-op while no server runs.
 *  Registered with tmx_completion_set_wake. */
void tmx_net_wake(void);

/* ---- Per-connection send queue (conn.c) ---- */

/** Keyed (coalescable) unsent entries tracked per queue; more keys than that are queued without coalescing. */
#define TMX_CONN_KEYED_MAX 160

/** Keyed reports waiting for room, one per (kind, key), latest value wins. Sized for 64 digital pins, 64 analog pins and
 *  up to 64 sonar and DHT devices between them (a device needs a pin). */
#define TMX_CONN_PENDING_MAX 192
/** The longest keyed report: DHT data, [11,11,0,pin,hum f32,temp f32] = 12 bytes with its length byte. */
#define TMX_CONN_PENDING_BYTES 12

typedef ssize_t (*tmx_send_fn)(int fd, const void *buf, size_t n, int flags);

typedef struct {
    uint16_t off;       /**< offset of the entry in the queue buffer */
    uint8_t len;
    int8_t kind;        /**< TMX_COALESCE_DIGITAL .. TMX_COALESCE_DHT */
    uint16_t key;
} tmx_conn_key_t;

typedef struct {
    int8_t kind;
    uint8_t len;
    uint16_t key;
    uint8_t data[TMX_CONN_PENDING_BYTES];
} tmx_conn_pend_t;

/** Fully defined so tests can keep one on the stack; the buffer is allocated once by tmx_conn_init. */
typedef struct tmx_conn {
    uint8_t *buf;
    uint16_t cap;
    uint16_t head;                              /**< first unsent byte */
    uint16_t tail;                              /**< end of queued bytes */
    tmx_conn_key_t keyed[TMX_CONN_KEYED_MAX];   /**< unsent keyed entries in queue order (all at or after head) */
    uint8_t n_keyed;
    tmx_conn_pend_t pend[TMX_CONN_PENDING_MAX]; /**< keyed reports that did not fit, oldest first */
    uint16_t n_pend;
    bool broken;                                /**< a reply found no room even in the reply reserve */
    bool stalling;                              /**< bytes are queued; stall_since is the last progress or first try */
    uint32_t stall_since;
    tmx_send_fn send_fn;
    uint32_t coalesced;                         /**< reports replaced in place or merged into a pending one for a newer value, since init */
    uint32_t dropped;                           /**< unkeyed non-reply reports lost for lack of room, since init */
} tmx_conn_t;

typedef enum { TMX_CONN_IDLE, TMX_CONN_PENDING, TMX_CONN_STALLED, TMX_CONN_ERROR } tmx_conn_state_t;

/** Allocate the queue buffer (internal RAM when available). bytes must be at least TMX_REPORT_MAX (64): ESP_ERR_INVALID_ARG
 *  otherwise, ESP_ERR_NO_MEM when the allocation fails. */
esp_err_t tmx_conn_init(tmx_conn_t *c, uint16_t bytes);
void tmx_conn_deinit(tmx_conn_t *c);
/** Replace the function flush sends with (default send()); for tests that script partial and zero-byte sends. */
void tmx_conn_set_sender(tmx_conn_t *c, tmx_send_fn send_fn);
/** Queue one report. reply true: never dropped, keys ignored, may use the reply reserve (a quarter of the queue); a reply
 *  that fits nowhere marks the connection broken and flush reports it as STALLED. Other reports need room outside the
 *  reserve; a keyed one replaces an unsent entry of the same key, and when there is no room waits in a pending table
 *  (latest value per key) that flush moves into the queue as room frees: keyed reports are coalesced, never lost; an
 *  unkeyed one with no room is counted in dropped. key_kind TMX_COALESCE_NONE (-1) = no key. */
void tmx_conn_queue(tmx_conn_t *c, const uint8_t *r, size_t n, bool reply, int8_t key_kind, uint16_t key);
/** Send what the socket takes without blocking. IDLE: queue empty. PENDING: bytes left. STALLED: bytes left and no
 *  progress for stall_ms (0 = never), or broken. ERROR: the socket failed. */
tmx_conn_state_t tmx_conn_flush(tmx_conn_t *c, int fd, uint32_t now_ms, uint16_t stall_ms);
/** Empty the queue and clear the broken and stall state; the buffer and the counters stay. */
void tmx_conn_reset(tmx_conn_t *c);
/** Bytes queued and not yet sent (pending keyed reports not included). */
size_t tmx_conn_pending(const tmx_conn_t *c);

/* ---- Sockets (sock.c) ---- */

/** Listener on the port, nonblocking, SO_REUSEADDR, backlog 1: dual-stack on in6addr_any with IPV6_V6ONLY 0 where the
 *  stack has IPv6, else IPv4. Returns the fd, -1 on failure. */
int tmx_sock_listen(uint16_t port);
/** Nonblocking, TCP_NODELAY, SO_KEEPALIVE with idle, interval and count from cfg. */
esp_err_t tmx_sock_tune_client(int fd, const telemetrix_config_t *cfg);
/** Close with SO_LINGER {1, 0}: the peer gets an RST and no socket lingers (refusals, failed or stalled clients). */
void tmx_sock_refuse(int fd);
/** An IPv4-mapped IPv6 peer becomes a plain IPv4 sockaddr_in; anything else is copied. */
void tmx_sock_normalise(const struct sockaddr_storage *in, struct sockaddr_storage *out);
/** True when the (normalised) peer matches an entry of cfg->allowed, or the list is empty. */
bool tmx_sock_allowed(const telemetrix_config_t *cfg, const struct sockaddr *peer);
/** "a.b.c.d:port" or "[v6]:port"; "" for another family. */
void tmx_sock_peer_text(const struct sockaddr *peer, char out[TELEMETRIX_PEER_MAX]);

/* ---- Wake (wake.c) ---- */

/** Create the eventfd (ISR-capable on chips). ESP_ERR_INVALID_STATE when one is open. */
esp_err_t tmx_wake_open(void);
int tmx_wake_fd(void);
/** Empty the eventfd after select() reported it readable. */
void tmx_wake_drain(void);
void tmx_wake_close(void);

/* ---- Server (net.c, api.c) ---- */

typedef struct tmx_server tmx_server_t;

/** Validate and deep-copy everything, start the driver, put every profile pin in its safe state and open the listener
 *  and the wake. Nothing runs yet; on failure nothing is left allocated or open. */
esp_err_t tmx_server_new(const telemetrix_profile_t *profile, const telemetrix_config_t *config,
                         const telemetrix_hooks_t *hooks, tmx_server_t **out);
/** The network task body (xTaskCreate entry); it releases the client, runs the hooks and sets the done flag last. */
void tmx_server_task(void *arg);
/** Publish the status snapshot now (telemetrix_start does, so get_status says running as soon as start returns). */
void tmx_server_publish(tmx_server_t *s);
void tmx_server_request_stop(tmx_server_t *s);
bool tmx_server_done(const tmx_server_t *s);
void tmx_server_kick(tmx_server_t *s);
/** After the task is done or was never started. */
void tmx_server_free(tmx_server_t *s);
/** Stores the snapshot telemetrix_get_status returns (api.c); false when the lock was not free in time. */
bool tmx_api_publish(const telemetrix_status_t *status);

#endif
