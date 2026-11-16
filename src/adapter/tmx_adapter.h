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
    bool open;                  /**< bus 0 opened through the driver */
} tmx_i2c_t;

typedef struct {
    bool open;                  /**< bus 0 opened; holds one RMT RX claim and the bus pin claim */
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
    bool dht_busy;
    tmx_deadline_t dht_pass;    /**< next 2000 ms pass */
} tmx_sensors_t;

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
    tmx_digital_t digital;
    tmx_analog_t analog;
    tmx_pwm_t pwm;
    tmx_i2c_t i2c;
    tmx_onewire_t onewire;
    tmx_sensors_t sensors;
};
typedef struct tmx_adapter tmx_adapter_t;

/** Bind the adapter to a profile (which must outlive it), a driver and a report sink, and put every profile pin in its
 *  safe state. cfg is copied. ESP_ERR_INVALID_ARG for a NULL argument or a profile over 64 pins. */
esp_err_t tmx_adapter_init(tmx_adapter_t *a, const telemetrix_profile_t *p, const telemetrix_config_t *cfg,
                           const tmx_drv_t *drv, tmx_emit_fn emit, void *ctx);
/** Handle one stock packet (the tmx_packet_fn target). Short payloads count as refused, ignored ids as ignored. */
void tmx_adapter_packet(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len, uint32_t now_ms);
/** Run the scans that are due and return the milliseconds until the next deadline, UINT32_MAX when none. */
uint32_t tmx_adapter_poll(tmx_adapter_t *a, uint32_t now_ms);
/** Close the buses, forget the sensors, let the driver free its session resources (release), then release every claim
 *  (LEDC first, then safe states), clear the tables, enable reporting, default analog interval. */
void tmx_adapter_release(tmx_adapter_t *a);
void tmx_adapter_counters(const tmx_adapter_t *a, uint32_t *ignored, uint32_t *refused);
/** Fill out with the claimed pins in profile order (at most cap); returns how many were written. */
size_t tmx_adapter_pin_uses(const tmx_adapter_t *a, telemetrix_pin_use_t *out, size_t cap);
/** True once after a RESET command, for the net layer's hook call. */
bool tmx_adapter_reset_requested(tmx_adapter_t *a);

/* ---- Internal: shared by the adapter's source files, not part of the adapter's contract. ---- */

typedef enum { TMX_RES_OK, TMX_RES_REFUSED, TMX_RES_UNAVAILABLE } tmx_res_t;

/* adapter.c */
/** Profile index of a GPIO, -1 when it is not a profile pin. */
int tmx_adapter_pin_index(const tmx_adapter_t *a, uint8_t gpio);
/** True when the pin exists, has the capability the use needs and is not held by a feature outside digital, analog,
 *  PWM and servo. No side effects. */
bool tmx_adapter_pin_ok(const tmx_adapter_t *a, uint8_t gpio, tmx_use_t use);
/** Forget the pin's digital, analog, PWM and servo state, stop its LEDC channel and return it to its safe state. */
void tmx_adapter_retire(tmx_adapter_t *a, uint8_t gpio);
void tmx_adapter_emit(tmx_adapter_t *a, const uint8_t *report, size_t n, bool reply, int8_t kind, uint16_t key);

/* digital.c */
bool tmx_digital_mode(tmx_adapter_t *a, uint8_t gpio, tmx_use_t use, bool report);
bool tmx_digital_write(tmx_adapter_t *a, uint8_t gpio, bool level);
bool tmx_digital_set_report(tmx_adapter_t *a, uint8_t gpio, bool on);
void tmx_digital_forget(tmx_adapter_t *a, int idx);
bool tmx_digital_wanted(const tmx_adapter_t *a);
void tmx_digital_scan(tmx_adapter_t *a);

/* analog.c */
bool tmx_analog_mode(tmx_adapter_t *a, uint8_t gpio, uint16_t differential, bool report);
bool tmx_analog_set_report(tmx_adapter_t *a, uint8_t gpio, bool on);
void tmx_analog_forget(tmx_adapter_t *a, int idx);
bool tmx_analog_wanted(const tmx_adapter_t *a);
void tmx_analog_scan(tmx_adapter_t *a);
void tmx_analog_set_interval(tmx_adapter_t *a, uint32_t ms);
uint32_t tmx_analog_default_interval(const tmx_adapter_t *a);

/* pwm.c */
bool tmx_pwm_mode(tmx_adapter_t *a, uint8_t gpio, uint8_t resolution, double freq_hz);
bool tmx_pwm_write(tmx_adapter_t *a, uint8_t gpio, uint16_t value);
bool tmx_pwm_attach(tmx_adapter_t *a, uint8_t gpio);
bool tmx_pwm_detach(tmx_adapter_t *a, uint8_t gpio);
tmx_res_t tmx_servo_attach(tmx_adapter_t *a, uint8_t gpio, uint16_t min_us, uint16_t max_us);
bool tmx_servo_write(tmx_adapter_t *a, uint8_t gpio, uint8_t angle);
bool tmx_servo_detach(tmx_adapter_t *a, uint8_t gpio);
/** Stop the LEDC channel of a PWM or servo pin and forget its state; the claim itself stays. */
void tmx_pwm_forget(tmx_adapter_t *a, int idx);

/* i2c.c */
/** I2C_BEGIN, I2C_READ, I2C_WRITE (ids 9-11); false = refused. */
bool tmx_i2c_command(tmx_adapter_t *a, uint8_t id, const uint8_t *p, size_t len);
/** Close bus 0 when open; the claims go with tmx_claims_release_all. */
void tmx_i2c_release(tmx_adapter_t *a);

/* onewire.c */
/** ONE_WIRE_INIT .. ONE_WIRE_CRC8 (ids 27-35); false = refused. */
bool tmx_onewire_command(tmx_adapter_t *a, uint8_t id, const uint8_t *p, size_t len);
void tmx_onewire_release(tmx_adapter_t *a);

/* sensors.c */
bool tmx_sonar_new(tmx_adapter_t *a, uint8_t trig, uint8_t echo, uint32_t now_ms);
bool tmx_dht_new(tmx_adapter_t *a, uint8_t pin, uint32_t now_ms);
/** Drain the completion queue and start the sonar ping and DHT read that are due. Never waits. */
void tmx_sensors_poll(tmx_adapter_t *a, uint32_t now_ms);
/** Milliseconds until the next sensor deadline, UINT32_MAX when none or while reports are stopped. */
uint32_t tmx_sensors_left(const tmx_adapter_t *a, uint32_t now_ms);
/** Forget every device and bump the generation. The RMT RX claim goes with tmx_claims_release_all. */
void tmx_sensors_release(tmx_adapter_t *a);

#endif
