#ifndef TMX_ADAPTER_H
#define TMX_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "telemetrix.h"
#include "tmx_core.h"
#include "tmx_drv.h"
#include "tmx_proto.h"

/** Coalesce key kinds of an emitted report. */
#define TMX_COALESCE_NONE (-1)
#define TMX_COALESCE_DIGITAL 0
#define TMX_COALESCE_ANALOG 1
#define TMX_COALESCE_SONAR 2
#define TMX_COALESCE_DHT 3

/** Receives every report the adapter produces. reply true = never drop. coalesce_key_kind is one of TMX_COALESCE_*;
 *  coalesce_key is the pin (digital, analog) or the device (sonar, DHT). */
typedef void (*tmx_emit_fn)(const uint8_t *report, size_t n, bool reply, int8_t coalesce_key_kind,
                            uint16_t coalesce_key, void *ctx);

#define TMX_DIGITAL_POLL_MS 2
#define TMX_ANALOG_DEFAULT_MS 19
#define TMX_SONAR_SLOT_MS 33
#define TMX_DHT_PASS_MS 2000

/* Per-feature state, one sub-struct per source file; tables are indexed by profile pin index. */
typedef struct {
    bool report[TMX_MAX_PINS];
    bool fresh[TMX_MAX_PINS];   /**< no sample taken since the mode change: the next one is reported */
    bool last[TMX_MAX_PINS];
    tmx_deadline_t scan;
} tmx_digital_t;

typedef struct {
    bool report[TMX_MAX_PINS];
    bool fresh[TMX_MAX_PINS];
    uint16_t differential[TMX_MAX_PINS];
    uint16_t last[TMX_MAX_PINS];
    uint32_t interval_ms;
    tmx_deadline_t scan;
} tmx_analog_t;

typedef struct {
    uint8_t channel[TMX_MAX_PINS];      /**< LEDC channel of a PWM or servo pin */
    uint8_t resolution[TMX_MAX_PINS];
    uint16_t min_us[TMX_MAX_PINS];      /**< servo pulse range */
    uint16_t max_us[TMX_MAX_PINS];
} tmx_pwm_t;

typedef struct {
    bool open[TMX_MAX_BUSES];   /**< indexed by profile bus index: opened through the driver */
} tmx_i2c_t;

typedef struct {
    bool open[TMX_MAX_BUSES];   /**< indexed by profile bus index: opened; holds one RMT RX claim and the bus pin claim */
} tmx_onewire_t;

/** Sonar and DHT devices are indexed in registration order; a completion token is (generation << 8 | index). */
typedef struct {
    uint32_t generation;        /**< bumped by every release; survives it, so late completions are recognised */
    uint8_t n_sonar;
    uint8_t sonar_trig[TMX_MAX_PINS];
    uint8_t sonar_echo[TMX_MAX_PINS];
    uint16_t sonar_last[TMX_MAX_PINS];
    bool sonar_has_last[TMX_MAX_PINS];
    uint8_t sonar_next_dev;     /**< round-robin cursor */
    bool sonar_busy;            /**< a ping is in flight: never two */
    uint8_t sonar_busy_dev;
    uint8_t sonar_lost;         /**< slots passed with the ping still in flight */
    tmx_deadline_t sonar_slot;  /**< next 33 ms slot */
    uint8_t n_dht;
    uint8_t dht_pin[TMX_MAX_PINS];
    bool dht_rmt;               /**< the shared RMT RX channel is claimed */
    bool dht_cycle;             /**< a pass over all devices is running */
    uint8_t dht_cursor;         /**< device being read, or the next to read */
    bool dht_from_new;          /**< the armed pass starts at dht_from: devices registered while no pass ran */
    uint8_t dht_from;
    bool dht_busy;
    tmx_deadline_t dht_pass;    /**< next 2000 ms pass */
} tmx_sensors_t;

/** The extended-mode session of one client; all clear (stock mode) after tmx_adapter_session_end. */
typedef struct {
    bool extended;
    uint8_t minor;              /**< extension protocol minor in force: the lower of client's and server's */
    uint16_t seq;               /**< number of the next command; wraps at 65536 */
    bool abort;                 /**< a refusal skips the commands up to the next SYNC; on at the start of a session */
    bool skipping;              /**< a command was refused with abort on: skip until the next SYNC */
    bool violation;             /**< a framing violation: the net layer closes the client; sticky until session end */
    bool mode_changed;          /**< entered extended mode, not yet told to the net layer */
    uint32_t error_reports;     /**< ERROR reports queued, over the adapter's life */
} tmx_session_t;

/** Fully defined so it can live on a stack or in static storage. */
struct tmx_adapter {
    const telemetrix_profile_t *profile;
    telemetrix_config_t cfg;
    const tmx_drv_t *drv;
    tmx_claims_t claims;
    tmx_emit_fn emit;
    void *ctx;
    uint32_t ignored_commands;
    uint32_t refused_commands;
    bool reporting;             /**< STOP_ALL_REPORTS clears it, ENABLE_ALL_REPORTS and release set it */
    bool reset_requested;
    int16_t on_bus;             /**< bus of the ON_BUS command being handled, -1 otherwise: replies are wrapped while set */
    tmx_digital_t digital;
    tmx_analog_t analog;
    tmx_pwm_t pwm;
    tmx_i2c_t i2c;
    tmx_onewire_t onewire;
    tmx_sensors_t sensors;
    tmx_session_t session;
    tmx_plugins_t plugins;
};
typedef struct tmx_adapter tmx_adapter_t;

/** Bind the adapter to a profile (which must outlive it), a driver and a report sink, register cfg's plugins and put
 *  every profile pin in its safe state. cfg is copied (the plugins must outlive the adapter, their array need not).
 *  ESP_ERR_INVALID_ARG for a NULL argument, a profile over 64 pins, a plugin list tmx_plugins_init refuses or a pin
 *  naming a capability no plugin declares; ESP_ERR_NOT_SUPPORTED for plugins without CONFIG_TELEMETRIX_EXTENDED. */
esp_err_t tmx_adapter_init(tmx_adapter_t *a, const telemetrix_profile_t *p, const telemetrix_config_t *cfg,
                           const tmx_drv_t *drv, tmx_emit_fn emit, void *ctx);
/** The stock dispatch: length check, switched-off check, the command. Returns why it was refused, NONE when done.
 *  A short payload is BAD_ARGUMENT; an id out of scope or switched off in Kconfig is NOT_IN_SCOPE. Replies of a
 *  command that succeeded (LOOP, firmware, I2C data, 1-wire) are emitted; refusal reports and counters are not. */
telemetrix_reason_t tmx_adapter_command(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len,
                                        uint32_t now_ms);
/** Handle one packet (the tmx_packet_fn target); the session entry.
 *  Stock mode: GET_FIRMWARE_VERSION with exactly [0xA7, major, minor] gets the long answer (and a session with the
 *  same major enters extended mode at the lower minor, when cfg.extended and CONFIG_TELEMETRIX_EXTENDED allow);
 *  every other packet is tmx_adapter_command, then the counters (NOT_IN_SCOPE counts as ignored, any other refusal as
 *  refused) and upstream's refusal reports ([2,6,pin] for a servo attach with no servo available, [3,8,1,addr] for an
 *  I2C read nobody answered; those two are not counted as refused).
 *  Extended mode: ids above 60 are a violation (nothing runs, nothing is queued); any other packet is numbered and,
 *  unless a refusal earlier in the batch skips it, dispatched; a refusal is exactly one ERROR report queued as a
 *  reply, counted in error_reports (and refused_commands, except NOT_IN_SCOPE). PLUGIN (60) goes to the plugin in its
 *  slot, whose PLUGIN refusal adds its code to the ERROR; GET_CAPABILITIES (59) is tmx_discovery_command.
 *  After every packet (both modes, reporting on, no violation) the reporting digital inputs are scanned, as upstream
 *  does after every command; the 2 ms poll covers the time between packets. */
void tmx_adapter_packet(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len, uint32_t now_ms);
/** Run the scans that are due and, during an extended session, the plugin poll hooks that are due or woken (timed
 *  against cfg.plugin_budget_us); return the milliseconds until the next deadline, UINT32_MAX when none. */
uint32_t tmx_adapter_poll(tmx_adapter_t *a, uint32_t now_ms);
/** Run the plugin release hooks in slot order, then close the buses, forget the sensors, let the driver free its session
 *  resources (release), then release every claim (LEDC first, then safe states), clear the tables, enable reporting,
 *  default analog interval. The plugins' next poll comes at once. */
void tmx_adapter_release(tmx_adapter_t *a);
/** release plus the session back to stock mode (seq 0, abort on, no violation): a client is gone. RESET uses
 *  tmx_adapter_release only: a session stays extended. */
void tmx_adapter_session_end(tmx_adapter_t *a);
bool tmx_adapter_extended(const tmx_adapter_t *a);
/** A framing violation was seen: sticky until tmx_adapter_session_end. */
bool tmx_adapter_violation(const tmx_adapter_t *a);
/** True once after the session entered extended mode, for the net layer's tmx_frame_set_mode. */
bool tmx_adapter_mode_changed(tmx_adapter_t *a);
void tmx_adapter_counters(const tmx_adapter_t *a, uint32_t *ignored, uint32_t *refused);
/** Fill out with the claimed pins in profile order (at most cap); returns how many were written. */
size_t tmx_adapter_pin_uses(const tmx_adapter_t *a, telemetrix_pin_use_t *out, size_t cap);
/** True once after a RESET command, for the net layer's hook call. */
bool tmx_adapter_reset_requested(tmx_adapter_t *a);

/* ---- Internal: shared by the adapter's source files, not part of the adapter's contract. ---- */

/* adapter.c */
/** The packet counters and upstream's refusal reports for a stock-mode refusal. */
void tmx_adapter_stock_refusal(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, telemetrix_reason_t why);
/** A command of a feature switched off in Kconfig: ignored and counted like an id out of scope (stock payload). */
bool tmx_adapter_switched_off(uint8_t id, const uint8_t *p);
/** Profile index of a GPIO, -1 when it is not a profile pin. */
int tmx_adapter_pin_index(const tmx_adapter_t *a, uint8_t gpio);
/** NONE when the pin exists, has the capability the use needs and is not held by a feature outside digital, analog,
 *  PWM and servo; else NO_SUCH_PIN, NO_CAPABILITY or PIN_BUSY. No side effects. */
telemetrix_reason_t tmx_adapter_pin_check(const tmx_adapter_t *a, uint8_t gpio, tmx_use_t use);
/** NONE when the pin exists, has all the capabilities in need and is unclaimed; else NO_SUCH_PIN, NO_CAPABILITY or
 *  PIN_BUSY. gpio is an int so a grant's -1 or an out-of-range value is NO_SUCH_PIN. No side effects. */
telemetrix_reason_t tmx_adapter_pin_free(const tmx_adapter_t *a, int gpio, uint16_t need);
/** NONE when the pin is currently claimed for use; else NO_SUCH_PIN, PIN_BUSY (held by a feature outside digital,
 *  analog, PWM and servo) or BAD_ARGUMENT (the pin is not in that mode). No side effects. */
telemetrix_reason_t tmx_adapter_pin_state(const tmx_adapter_t *a, uint8_t gpio, tmx_use_t use);
/** The reason for a failed tmx_claim_pin: NO_CAPABILITY, PIN_BUSY or HARDWARE. */
telemetrix_reason_t tmx_adapter_claim_reason(esp_err_t err);
/** Forget the pin's digital, analog, PWM and servo state, stop its LEDC channel and return it to its safe state. */
void tmx_adapter_retire(tmx_adapter_t *a, uint8_t gpio);
void tmx_adapter_emit(tmx_adapter_t *a, const uint8_t *report, size_t n, bool reply, int8_t kind, uint16_t key);

/* session.c */
/** The state of a fresh adapter: stock mode, abort on. */
void tmx_session_init(tmx_adapter_t *a);

/* digital.c */
telemetrix_reason_t tmx_digital_mode(tmx_adapter_t *a, uint8_t gpio, tmx_use_t use, bool report);
telemetrix_reason_t tmx_digital_write(tmx_adapter_t *a, uint8_t gpio, bool level);
telemetrix_reason_t tmx_digital_set_report(tmx_adapter_t *a, uint8_t gpio, bool on);
void tmx_digital_forget(tmx_adapter_t *a, int idx);
bool tmx_digital_wanted(const tmx_adapter_t *a);
void tmx_digital_scan(tmx_adapter_t *a);

/* analog.c */
telemetrix_reason_t tmx_analog_mode(tmx_adapter_t *a, uint8_t gpio, uint16_t differential, bool report);
telemetrix_reason_t tmx_analog_set_report(tmx_adapter_t *a, uint8_t gpio, bool on);
void tmx_analog_forget(tmx_adapter_t *a, int idx);
bool tmx_analog_wanted(const tmx_adapter_t *a);
void tmx_analog_scan(tmx_adapter_t *a);
void tmx_analog_set_interval(tmx_adapter_t *a, uint32_t ms);
uint32_t tmx_analog_default_interval(const tmx_adapter_t *a);

/* pwm.c */
telemetrix_reason_t tmx_pwm_mode(tmx_adapter_t *a, uint8_t gpio, uint8_t resolution, double freq_hz);
telemetrix_reason_t tmx_pwm_write(tmx_adapter_t *a, uint8_t gpio, uint16_t value);
telemetrix_reason_t tmx_pwm_attach(tmx_adapter_t *a, uint8_t gpio);
telemetrix_reason_t tmx_pwm_detach(tmx_adapter_t *a, uint8_t gpio);
telemetrix_reason_t tmx_servo_attach(tmx_adapter_t *a, uint8_t gpio, uint16_t min_us, uint16_t max_us);
telemetrix_reason_t tmx_servo_write(tmx_adapter_t *a, uint8_t gpio, uint8_t angle);
telemetrix_reason_t tmx_servo_detach(tmx_adapter_t *a, uint8_t gpio);
/** Stop the LEDC channel of a PWM or servo pin and forget its state; the claim itself stays. */
void tmx_pwm_forget(tmx_adapter_t *a, int idx);

/* i2c.c */
/** I2C_BEGIN, I2C_READ, I2C_WRITE (ids 9-11) on the profile bus number bus; a bus not in the profile is BUS_UNKNOWN.
 *  A read that no device answered is NO_ANSWER and emits nothing. Stock dispatch passes bus 0. */
telemetrix_reason_t tmx_i2c_command(tmx_adapter_t *a, uint8_t bus, uint8_t id, const uint8_t *p, size_t len);
/** Close every open bus; the claims go with tmx_claims_release_all. */
void tmx_i2c_release(tmx_adapter_t *a);

/* onewire.c */
/** ONE_WIRE_INIT .. ONE_WIRE_CRC8 (ids 27-35) on the profile bus number bus; a bus not in the profile is
 *  BUS_UNKNOWN. Stock dispatch passes bus 0. */
telemetrix_reason_t tmx_onewire_command(tmx_adapter_t *a, uint8_t bus, uint8_t id, const uint8_t *p, size_t len);
/** Close every open bus. */
void tmx_onewire_release(tmx_adapter_t *a);

/* discovery.c */
/** The IDF version string of the CHIP record is cut to this many bytes. */
#define TMX_IDF_VERSION_MAX 32
/** Record i of the capabilities (CHIP, NAMED_CAP, PIN, BUS, LIMIT, PLUGIN in that order), 0 length after the last;
 *  adapter is a tmx_adapter_t *. The source for tmx_caps_page. */
size_t tmx_discovery_record(size_t i, uint8_t *rec, void *adapter);
/** GET_CAPABILITIES [page]: emit the page as a reply. BAD_ARGUMENT without a page byte or past the last page. */
telemetrix_reason_t tmx_discovery_command(tmx_adapter_t *a, const uint8_t *payload, size_t len);

/* plugin_api.c */
/** PLUGIN [slot, sub, args...]: the plugin's command, timed and counted. BAD_ARGUMENT under 2 bytes, NO_SUCH_SLOT for
 *  an empty slot; *code is set when the result is PLUGIN. */
telemetrix_reason_t tmx_adapter_plugin_command(tmx_adapter_t *a, const uint8_t *payload, size_t len, uint8_t *code);
/** The poll hooks that are due or woken, during an extended session only; ms until the next one, UINT32_MAX = none. */
uint32_t tmx_adapter_plugins_poll(tmx_adapter_t *a, uint32_t now_ms);
/** The release hooks in slot order; every plugin's RMT holdings forgotten and its next poll due at once. */
void tmx_adapter_plugins_release(tmx_adapter_t *a);

/* sensors.c */
telemetrix_reason_t tmx_sonar_new(tmx_adapter_t *a, uint8_t trig, uint8_t echo, uint32_t now_ms);
telemetrix_reason_t tmx_dht_new(tmx_adapter_t *a, uint8_t pin, uint32_t now_ms);
/** Drain the completion queue and start the sonar ping and DHT read that are due. Never waits. */
void tmx_sensors_poll(tmx_adapter_t *a, uint32_t now_ms);
/** Milliseconds until the next sensor deadline, UINT32_MAX when none or while reports are stopped. */
uint32_t tmx_sensors_left(const tmx_adapter_t *a, uint32_t now_ms);
/** Forget every device and bump the generation. The RMT RX claim goes with tmx_claims_release_all. */
void tmx_sensors_release(tmx_adapter_t *a);

#endif
