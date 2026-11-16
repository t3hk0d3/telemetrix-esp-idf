#ifndef TELEMETRIX_H
#define TELEMETRIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a profile pin may be used for. Bits 8-15 are reserved. */
typedef enum {
    TELEMETRIX_CAP_DIN = 1 << 0,
    TELEMETRIX_CAP_DOUT = 1 << 1,
    TELEMETRIX_CAP_PULL_UP = 1 << 2,
    TELEMETRIX_CAP_PULL_DOWN = 1 << 3,
    TELEMETRIX_CAP_PWM = 1 << 4,
    TELEMETRIX_CAP_ADC = 1 << 5,
    TELEMETRIX_CAP_I2C = 1 << 6,
    TELEMETRIX_CAP_ONE_WIRE = 1 << 7,
} telemetrix_cap_t;

/** The state a pin returns to on start, disconnect, reset and stop. */
typedef enum {
    TELEMETRIX_SAFE_FLOAT,
    TELEMETRIX_SAFE_PULL_UP,
    TELEMETRIX_SAFE_PULL_DOWN,
    TELEMETRIX_SAFE_DRIVE_LOW,
    TELEMETRIX_SAFE_DRIVE_HIGH,
} telemetrix_safe_state_t;

typedef struct {
    uint8_t gpio;
    uint16_t caps;                /**< telemetrix_cap_t bits */
    telemetrix_safe_state_t safe;
    int8_t adc_unit;              /**< -1 = no ADC */
    int8_t adc_channel;           /**< -1 = no ADC */
} telemetrix_pin_t;

typedef struct {
    uint8_t speed_mode;
    uint8_t timer_mask;
    uint16_t channel_mask;
} telemetrix_ledc_grant_t;

/** RMT channels the server may create (DHT, 1-wire).
 *  Requirements: the sonar echo handler and the DHT receive callback are not IRAM-safe (their completion path is in
 *  flash), so while sonar is used an application-installed GPIO ISR service must not use ESP_INTR_FLAG_IRAM, and
 *  CONFIG_RMT_RX_ISR_CACHE_SAFE is not supported (the DHT channel creation would fail). The server never uninstalls the
 *  GPIO ISR service. */
typedef struct {
    uint8_t rx_channels;
    uint8_t tx_channels;
} telemetrix_rmt_grant_t;

typedef struct {
    uint8_t unit;
    /** application-owned adc_oneshot_unit_handle_t, or NULL: the component makes its own. Channels the server reads are
     *  configured at 12 dB on every read; the application must not depend on other settings for them. */
    void *handle;
} telemetrix_adc_grant_t;

/** An I2C bus. With handle NULL the component creates the bus on port: sda and scl are fixed profile pins with the I2C
 *  capability, or -1 for "the client chooses" (I2C_BEGIN names them).
 *  With handle set the bus is shared with the application: sda and scl must name the bus's pins and must not be profile
 *  pins (telemetrix_start refuses -1 or a profile pin with ESP_ERR_INVALID_ARG). I2C_BEGIN then attaches to the handle;
 *  the component never claims, configures or puts those pins in a safe state, and never deletes the handle. */
typedef struct {
    uint8_t bus;
    uint8_t port;
    int8_t sda;                   /**< -1 = the client chooses (not with a handle) */
    int8_t scl;                   /**< -1 = the client chooses (not with a handle) */
    uint32_t speed_hz;
    void *handle;                 /**< application-owned i2c_master_bus_handle_t, or NULL */
} telemetrix_i2c_bus_t;

typedef struct {
    uint8_t bus;
    int8_t gpio;                  /**< -1 = the pin of ONE_WIRE_INIT */
} telemetrix_onewire_bus_t;

typedef struct {
    const telemetrix_pin_t *pins;
    size_t n_pins;
    telemetrix_ledc_grant_t ledc;
    telemetrix_rmt_grant_t rmt;
    const telemetrix_adc_grant_t *adc;
    size_t n_adc;
    const telemetrix_i2c_bus_t *i2c;
    size_t n_i2c;
    const telemetrix_onewire_bus_t *onewire;
    size_t n_onewire;
} telemetrix_profile_t;

typedef struct {
    uint8_t family;               /**< AF_INET or AF_INET6 */
    uint8_t addr[16];
    uint8_t prefix;
} telemetrix_cidr_t;

/** Parse "a.b.c.d/n", "a.b.c.d", "x::y/n" or "x::y" (no prefix = a single host). */
esp_err_t telemetrix_cidr_parse(const char *text, telemetrix_cidr_t *out);

typedef struct {
    bool server_enabled;
    uint16_t port;
    uint8_t keepalive_idle_s, keepalive_intvl_s, keepalive_count;
    uint16_t send_stall_ms;
    uint16_t send_queue_bytes;
    uint16_t i2c_timeout_ms;
    uint8_t analog_interval_floor_ms;
    uint8_t max_dht, max_sonar, max_servo;
    const telemetrix_cidr_t *allowed; /**< empty = any peer */
    size_t n_allowed;
    uint8_t task_priority;
    int8_t task_core;             /**< -1 = no affinity */
    bool extended;                /**< offer the extended protocol to a client that asks for it (needs CONFIG_TELEMETRIX_EXTENDED) */
} telemetrix_config_t;

void telemetrix_config_default(telemetrix_config_t *out);

/** Why a command was refused; NONE = done. The values are wire values of the extended protocol. */
typedef enum {
    TELEMETRIX_REASON_NONE = 0,
    TELEMETRIX_REASON_NO_SUCH_PIN = 1,      /**< the pin is not in the profile */
    TELEMETRIX_REASON_NO_CAPABILITY = 2,    /**< the pin is in the profile but lacks the capability the use needs */
    TELEMETRIX_REASON_PIN_BUSY = 3,         /**< the pin is claimed by another use */
    TELEMETRIX_REASON_EXHAUSTED = 4,        /**< no LEDC channel or timer, RMT channel or device slot left */
    TELEMETRIX_REASON_BAD_ARGUMENT = 5,     /**< short payload, value out of range, unknown mode, wrong state */
    TELEMETRIX_REASON_BUS_UNKNOWN = 6,      /**< the bus is not in the profile */
    TELEMETRIX_REASON_NOT_IN_SCOPE = 7,     /**< the command is not served by this server */
    TELEMETRIX_REASON_HARDWARE = 8,         /**< the driver failed */
    TELEMETRIX_REASON_NO_ANSWER = 9,        /**< the I2C device did not answer */
    TELEMETRIX_REASON_PLUGIN = 10,          /**< a plugin refused; its own code follows */
    TELEMETRIX_REASON_SKIPPED = 11,         /**< not executed: an earlier command of the batch was refused */
    TELEMETRIX_REASON_NO_SUCH_SLOT = 12,    /**< no plugin in that slot */
} telemetrix_reason_t;

typedef struct {
    bool (*access)(const struct sockaddr *peer, void *ctx);   /**< NULL = allow */
    void (*reset)(void *ctx);
    void (*connected)(const char *peer, void *ctx);
    void (*disconnected)(void *ctx);
    void *ctx;
} telemetrix_hooks_t;

#define TELEMETRIX_PEER_MAX 48

typedef struct {
    uint8_t gpio;
    /** "input", "input_pullup", "input_pulldown", "output", "analog", "pwm", "servo", "i2c", "one_wire", "sonar",
     *  "dht" */
    const char *use;
} telemetrix_pin_use_t;

typedef struct {
    bool running, listening;
    char client[TELEMETRIX_PEER_MAX];
    uint32_t refused_connections, dropped_packets, ignored_commands, refused_commands, coalesced_reports,
             stalled_clients;
    size_t n_pins;
    telemetrix_pin_use_t pins[64];
} telemetrix_status_t;

/** Start the server. Copies the profile, the config and the hooks, including the arrays they point to. */
esp_err_t telemetrix_start(const telemetrix_profile_t *profile, const telemetrix_config_t *config,
                           const telemetrix_hooks_t *hooks);
esp_err_t telemetrix_stop(void);
esp_err_t telemetrix_get_status(telemetrix_status_t *out);
void telemetrix_kick_client(void);

/** Sockets the application must budget for: listener, client, transient refusal. */
#define TELEMETRIX_SOCKETS_M1 3
/** eventfds the application must budget for. */
#define TELEMETRIX_EVENTFDS 1

#ifdef __cplusplus
}
#endif

#endif
