#ifndef TMX_CORE_H
#define TMX_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "telemetrix.h"
#include "telemetrix_plugin.h"
#include "tmx_drv.h"

#define TMX_MAX_PINS 64
#define TMX_LEDC_CHANNELS 16
#define TMX_LEDC_TIMERS 8

/** Check a profile: pins unique and at most 64, caps and safe states in range, ADC pins with a granted unit, LEDC grant
 *  usable by PWM pins, bus numbers unique and below TMX_MAX_BUSES (4), at most 4 buses of each kind, fixed bus pins
 *  profile pins with the capability, except an application-owned I2C bus (handle set), which names two distinct pins
 *  that are not profile pins. ESP_ERR_INVALID_ARG. */
esp_err_t tmx_profile_validate(const telemetrix_profile_t *p);
/** Check the pins' named capabilities against names (the registry's table): every name a pin lists must be in it.
 *  ESP_ERR_INVALID_ARG for an unknown name, a pin with n_named but no array, more than 16 names or a name in names over
 *  24 bytes. */
esp_err_t tmx_profile_validate_named(const telemetrix_profile_t *p, const char *const *names, size_t n);
/** The profile entry of a GPIO, or NULL. */
const telemetrix_pin_t *tmx_profile_pin(const telemetrix_profile_t *p, uint8_t gpio);

typedef enum {
    TMX_USE_NONE,
    TMX_USE_INPUT,
    TMX_USE_INPUT_PULLUP,
    TMX_USE_INPUT_PULLDOWN,
    TMX_USE_OUTPUT,
    TMX_USE_ANALOG,
    TMX_USE_PWM,
    TMX_USE_SERVO,
    TMX_USE_I2C,
    TMX_USE_ONE_WIRE,
    TMX_USE_SONAR_TRIG,
    TMX_USE_SONAR_ECHO,
    TMX_USE_DHT,
    TMX_USE_PLUGIN,             /**< claimed by a plugin for a named capability; tmx_pin_owner names the slot */
} tmx_use_t;

/** Pin and peripheral claims, fixed size so it can live on a stack or in static storage. */
typedef struct tmx_claims {
    const telemetrix_profile_t *profile;
    const tmx_drv_t *drv;
    uint8_t use[TMX_MAX_PINS];               /**< tmx_use_t per profile pin index */
    uint8_t ledc_timer_of[TMX_LEDC_CHANNELS]; /**< timer of a claimed channel, 0xff = free */
    uint32_t timer_freq[TMX_LEDC_TIMERS];
    uint8_t timer_res[TMX_LEDC_TIMERS];
    uint8_t timer_refs[TMX_LEDC_TIMERS];
    uint8_t rmt_rx_used;
    uint8_t rmt_tx_used;
    uint8_t owner[TMX_MAX_PINS];             /**< plugin slot of a TMX_USE_PLUGIN pin */
} tmx_claims_t;

/** Bind the claims to a profile and a driver and put every profile pin in its safe state, in profile order. Returns the
 *  first driver error after trying every pin. The profile must outlive the claims. */
esp_err_t tmx_claims_init(tmx_claims_t *c, const telemetrix_profile_t *p, const tmx_drv_t *drv);
/** Claim a pin for a use. ESP_ERR_NOT_SUPPORTED: not a profile pin or lacks the capability; ESP_ERR_INVALID_STATE:
 *  claimed by another use (digital uses may replace each other). Does not touch the driver. */
esp_err_t tmx_claim_pin(tmx_claims_t *c, uint8_t gpio, tmx_use_t use);
/** Claim a pin for a plugin (slot owner) under a named capability its profile entry grants. ESP_ERR_NOT_SUPPORTED:
 *  not a profile pin or the entry lacks the name; ESP_ERR_INVALID_STATE: claimed by another use or another plugin.
 *  Claiming again for the same owner is ESP_OK. Does not touch the driver. */
esp_err_t tmx_claim_named(tmx_claims_t *c, uint8_t gpio, const char *name, uint8_t owner);
/** The plugin slot holding a TMX_USE_PLUGIN pin, -1 for any other pin. */
int tmx_pin_owner(const tmx_claims_t *c, uint8_t gpio);
/** Free a pin and return it to its safe state through the driver. */
void tmx_release_pin(tmx_claims_t *c, uint8_t gpio);
tmx_use_t tmx_pin_use(const tmx_claims_t *c, uint8_t gpio);
const char *tmx_use_name(tmx_use_t use);

/** Claim the lowest free LEDC channel and a timer for (freq, resolution): an existing one is shared, else the lowest
 *  free granted timer is configured through the driver. The channel is not attached. ESP_ERR_NOT_FOUND when no channel
 *  or timer is free; nothing is claimed then. */
esp_err_t tmx_claim_ledc(tmx_claims_t *c, uint32_t freq_hz, uint8_t resolution, uint8_t *channel, uint8_t *timer);
/** Stop a claimed channel and free its timer when no channel uses it any more. */
void tmx_release_ledc(tmx_claims_t *c, uint8_t channel);
/** Count an RMT RX channel against the grant. ESP_ERR_NOT_FOUND when none is left. */
esp_err_t tmx_claim_rmt_rx(tmx_claims_t *c);
void tmx_release_rmt_rx(tmx_claims_t *c);
/** Count an RMT TX channel against the grant (1-wire buses and plugins). ESP_ERR_NOT_FOUND when none is left. */
esp_err_t tmx_claim_rmt_tx(tmx_claims_t *c);
void tmx_release_rmt_tx(tmx_claims_t *c);
/** Stop every LEDC channel and free every timer, then safe state for every claimed pin in profile order, forget RMT claims.
 *  Never closes buses: that is the adapter's job. */
void tmx_claims_release_all(tmx_claims_t *c);

/** A software deadline on a free-running millisecond counter. Comparisons are wrap-safe (signed 32-bit difference), so
 *  a deadline stays valid while it is less than 2^31 ms away. */
typedef struct {
    uint32_t due_ms;
    bool armed;
} tmx_deadline_t;

/** Arm a deadline in_ms after now_ms. */
void tmx_deadline_arm(tmx_deadline_t *d, uint32_t now_ms, uint32_t in_ms);
/** True when the deadline is armed and now_ms has reached it. */
bool tmx_deadline_due(const tmx_deadline_t *d, uint32_t now_ms);
/** Milliseconds until the deadline, 0 when due, UINT32_MAX when unarmed. */
uint32_t tmx_deadline_left(const tmx_deadline_t *d, uint32_t now_ms);

/* ---- Plugin registry (plugins.c) ---- */

#define TMX_PLUGIN_NAME_MAX 32
#define TMX_NAMED_MAX 16
#define TMX_NAMED_NAME_MAX 24
#define TMX_PLUGIN_TEXT_MAX 47

struct tmx_adapter;

/** A registered plugin: what telemetrix_plugin_ctx_t points at. */
struct telemetrix_plugin_ctx {
    const telemetrix_plugin_t *plugin;
    struct tmx_adapter *adapter;        /**< set by the adapter that owns the registry */
    uint8_t slot;
    uint32_t commands, refusals, overruns, max_us;
    uint32_t async_dropped;             /**< atomic: written by any task */
    char text[TMX_PLUGIN_TEXT_MAX + 1];
    uint8_t rmt_rx, rmt_tx;             /**< RMT channels the plugin holds */
    bool poll_first;                    /**< poll at the next chance (a session or a release began) */
    tmx_deadline_t poll_next;           /**< unarmed: no call until a wake */
    bool wake;                          /**< atomic: telemetrix_plugin_wake from any task */
};

typedef struct tmx_plugins {
    telemetrix_plugin_ctx_t ctx[TELEMETRIX_PLUGINS_MAX];
    size_t n;
    const char *named[TMX_NAMED_MAX];   /**< named capability table: id = index, first declaration first */
    size_t n_named;
} tmx_plugins_t;

/** Register plugins in slot order (slot = index) and build the named capability table. ESP_ERR_INVALID_ARG for more
 *  than TELEMETRIX_PLUGINS_MAX, a NULL entry, a bad, reserved (telemetrix.) or duplicate name, more than 16 distinct
 *  named capabilities or one over 24 bytes; r is left empty then. */
esp_err_t tmx_plugins_init(tmx_plugins_t *r, const telemetrix_plugin_t *const *list, size_t n);
/** Id of a named capability, -1 when no plugin declares it. */
int tmx_plugins_named_id(const tmx_plugins_t *r, const char *name);
/** Count one timed call: max_us, and an overrun when us is over budget_us. */
void tmx_plugins_account(telemetrix_plugin_ctx_t *ctx, uint64_t us, uint32_t budget_us);
/** Fill out with the plugins in slot order (at most cap); returns how many were written. */
size_t tmx_plugins_status(const tmx_plugins_t *r, telemetrix_plugin_status_t *out, size_t cap);

#define TMX_COMPLETION_RING 16

#define TMX_COMPLETION_DHT 1
#define TMX_COMPLETION_SONAR 2

/** An asynchronous driver result (DHT frame, sonar echo) carrying the token given to dht_start or sonar_start. */
typedef struct {
    uint8_t kind;
    uint32_t token;
    esp_err_t err;
    uint32_t value;
    uint8_t data[5];
} tmx_completion_t;

/** Queue a completion. Callable from an ISR, a driver callback or another task. When the ring of 16 is full the new
 *  completion is dropped and counted (queued ones keep their order). Calls the wake function afterwards. */
void tmx_completion_post(const tmx_completion_t *c);
/** Dequeue the oldest completion; false when empty. Network task only. */
bool tmx_completion_take(tmx_completion_t *out);
/** Register the function that tmx_completion_post calls after queueing; NULL restores the no-op. The function must be
 *  safe to call from an ISR (tmx_net_wake is: an eventfd write). */
void tmx_completion_set_wake(void (*fn)(void));
/** Drop queued completions and zero the overflow count; the wake function stays. */
void tmx_completion_reset(void);
/** Completions dropped because the ring was full since the last reset. */
uint32_t tmx_completion_overflows(void);

#endif
