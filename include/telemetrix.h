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

typedef struct {
    uint8_t rx_channels;
    uint8_t tx_channels;
} telemetrix_rmt_grant_t;

typedef struct {
    uint8_t unit;
    void *handle;                 /**< application-owned adc_oneshot_unit_handle_t, or NULL: the component makes its own */
} telemetrix_adc_grant_t;

typedef struct {
    uint8_t bus;
    uint8_t port;
    int8_t sda;                   /**< -1 = the client chooses */
    int8_t scl;                   /**< -1 = the client chooses */
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
} telemetrix_config_t;

void telemetrix_config_default(telemetrix_config_t *out);

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
    /** "input", "input_pullup", "input_pulldown", "output", "analog", "pwm", "servo", "i2c", "one_wire", "sonar", "dht" */
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
