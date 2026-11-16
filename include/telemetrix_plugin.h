#ifndef TELEMETRIX_PLUGIN_H
#define TELEMETRIX_PLUGIN_H

#include "telemetrix.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Extension plugins: trusted application code a client reaches in extended mode through PLUGIN [slot, sub, args...];
 *  a plugin answers with PLUGIN_REPORT [slot, sub, data...]. A plugin's slot is its index in cfg.plugins.
 *
 *  Every call into a plugin (command, poll, release) runs on the network task, on its stack of cfg.task_stack_bytes
 *  (default 6144, at least TELEMETRIX_TASK_STACK_MIN). Budget for it: ESP_LOGx alone can take 1.5-2.5 KB of stack on
 *  Xtensa, and the free stack left at the plugin call is unmeasured (to be measured in the board pass); raise
 *  cfg.task_stack_bytes for plugins with deep calls or large locals. Command and poll calls are timed against
 *  cfg.plugin_budget_us; an overrun is counted in the status, never aborted. A plugin gets only what the board profile
 *  grants: pins whose profile entry names one of its named capabilities, and RMT channels of the grant. */

typedef struct telemetrix_plugin_ctx telemetrix_plugin_ctx_t;

typedef struct telemetrix_plugin {
    /** Matches ^[a-z0-9_]+\.[a-z0-9_.]+$, at most 32 bytes, unique; the telemetrix. prefix is reserved. */
    const char *name;
    uint8_t major, minor;
    /** Named capabilities the plugin declares (at most 16 distinct names per server, each at most 24 bytes). */
    const char *const *named_caps;
    size_t n_named_caps;
    /** A PLUGIN command; NONE completes it. *code is written when it returns TELEMETRIX_REASON_PLUGIN. Any other
     *  telemetrix_reason_t from NO_SUCH_PIN to NO_SUCH_SLOT refuses with that reason, except SKIPPED (the server's
     *  own): SKIPPED or a value outside the reasons is sent as PLUGIN with code 0. NULL = the plugin takes no commands
     *  (NOT_IN_SCOPE). */
    telemetrix_reason_t (*command)(telemetrix_plugin_ctx_t *ctx, uint8_t sub, const uint8_t *args, size_t len,
                                   uint8_t *code);
    /** Called during an extended session only: first at once, then when the time it returned (ms) is up or after
     *  telemetrix_plugin_wake. UINT32_MAX = no further call until a wake. NULL = none. */
    uint32_t (*poll)(telemetrix_plugin_ctx_t *ctx, uint32_t now_ms);
    /** End of a session (disconnect, RESET, stop), in slot order, before the server releases the pins and channels.
     *  The plugin must stop its own tasks that call telemetrix_plugin_emit_async or telemetrix_plugin_wake (or wait for
     *  them) before this returns: after the release hook at stop the ctx is gone. NULL = none. */
    void (*release)(telemetrix_plugin_ctx_t *ctx);
    void *user;
} telemetrix_plugin_t;

/** The descriptor's user pointer. */
void *telemetrix_plugin_user(telemetrix_plugin_ctx_t *ctx);

/** Network task only (command, poll, release). Claim a profile pin for the named capability. ESP_ERR_NOT_SUPPORTED: not a profile pin or its entry does not grant
 *  named_cap; ESP_ERR_INVALID_STATE: held by another use or another plugin. Claiming a pin the plugin already holds is
 *  ESP_OK. The plugin drives the pin itself; the server puts it back in its safe state on release. */
esp_err_t telemetrix_plugin_claim_pin(telemetrix_plugin_ctx_t *ctx, uint8_t gpio, const char *named_cap);
/** Network task only. Give a pin the plugin holds back; it goes to its safe state. Other pins are left alone. */
void telemetrix_plugin_release_pin(telemetrix_plugin_ctx_t *ctx, uint8_t gpio);

/** Network task only. Count an RMT RX or TX channel against the grant. ESP_ERR_NOT_FOUND when none is left. */
esp_err_t telemetrix_plugin_claim_rmt(telemetrix_plugin_ctx_t *ctx, bool tx);
/** Network task only. Give back one RMT channel the plugin holds (no-op when it holds none of that kind). */
void telemetrix_plugin_release_rmt(telemetrix_plugin_ctx_t *ctx, bool tx);

/** Queue PLUGIN_REPORT [slot, sub, data...] as a reply. Network task only (command, poll, release).
 *  ESP_ERR_INVALID_ARG: n over 251 or data NULL with n > 0; ESP_ERR_INVALID_STATE: no client in extended mode. */
esp_err_t telemetrix_plugin_emit(telemetrix_plugin_ctx_t *ctx, uint8_t sub, const uint8_t *data, size_t n);
/** Queue PLUGIN_REPORT from any task (not an ISR) and wake the network task, which sends it. Waits at most 10 ms for
 *  the ring lock (ESP_ERR_TIMEOUT). ESP_ERR_NO_MEM when the ring of 8 is full (counted in async_dropped);
 *  ESP_ERR_INVALID_STATE when no client is in extended mode or the server has stopped; ESP_ERR_INVALID_ARG as for
 *  telemetrix_plugin_emit. Reports queued in a session that has ended are dropped. The ctx is valid only until the
 *  plugin's release hook at stop returns; a late call fails with ESP_ERR_INVALID_STATE without touching it. */
esp_err_t telemetrix_plugin_emit_async(telemetrix_plugin_ctx_t *ctx, uint8_t sub, const uint8_t *data, size_t n);
/** Ask for a poll call soon; any task (not an ISR). Never dropped: it waits for the ring lock as long as it is held
 *  (a holder copies at most 251 bytes under it). Does nothing once the server has stopped (same ctx rule as
 *  telemetrix_plugin_emit_async). */
void telemetrix_plugin_wake(telemetrix_plugin_ctx_t *ctx);
/** Status text shown in telemetrix_get_status: at most 47 characters are copied; NULL clears it. Network task only. */
void telemetrix_plugin_set_status(telemetrix_plugin_ctx_t *ctx, const char *text);

#ifdef __cplusplus
}
#endif

#endif
