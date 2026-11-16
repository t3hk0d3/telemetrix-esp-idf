#ifndef TMX_DRV_H
#define TMX_DRV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "telemetrix.h"

/** I2C buses and 1-wire buses a profile may grant, each kind; the drivers size their tables by it. */
#define TMX_MAX_BUSES 4

/** The hardware seen by the core and the adapter. src/drivers/esp/ implements it on chips, src/drivers/fake/ in tests.
 *  dht_start and sonar_start only start the measurement; the result arrives through tmx_completion_post with the token:
 *  five DHT frame bytes or an error, the sonar echo in microseconds or ESP_ERR_TIMEOUT.
 *  release ends a session (disconnect, RESET, stop): it frees what dht_start and sonar_start set up (the DHT RX channel,
 *  the echo handler) and abandons a measurement in flight; buses close through their own ops. The adapter calls it
 *  before the pins go back to their safe states. */
typedef struct {
    esp_err_t (*gpio_input)(uint8_t gpio, bool pull_up, bool pull_down);
    esp_err_t (*gpio_output)(uint8_t gpio, bool level);
    void (*gpio_set)(uint8_t gpio, bool v);
    int (*gpio_get)(uint8_t gpio);
    esp_err_t (*ledc_timer)(uint8_t speed_mode, uint8_t timer, uint32_t freq_hz, uint8_t resolution);
    esp_err_t (*ledc_attach)(uint8_t speed_mode, uint8_t channel, uint8_t timer, uint8_t gpio);
    esp_err_t (*ledc_duty)(uint8_t speed_mode, uint8_t channel, uint32_t duty);
    void (*ledc_stop)(uint8_t speed_mode, uint8_t channel);
    void (*ledc_timer_free)(uint8_t speed_mode, uint8_t timer);
    esp_err_t (*adc_read)(uint8_t unit, uint8_t channel, int *raw);
    esp_err_t (*i2c_open)(uint8_t bus, const telemetrix_i2c_bus_t *grant, int8_t sda, int8_t scl);
    esp_err_t (*i2c_read)(uint8_t bus, uint8_t addr, uint8_t reg, bool stop, uint8_t *data, size_t n,
                          uint16_t timeout_ms);
    esp_err_t (*i2c_write)(uint8_t bus, uint8_t addr, const uint8_t *data, size_t n, uint16_t timeout_ms);
    void (*i2c_close)(uint8_t bus);
    esp_err_t (*ow_open)(uint8_t bus, uint8_t gpio);
    bool (*ow_reset)(uint8_t bus);
    void (*ow_write)(uint8_t bus, uint8_t byte, bool power);
    uint8_t (*ow_read)(uint8_t bus);
    bool (*ow_search)(uint8_t bus, uint8_t rom[8]);
    void (*ow_reset_search)(uint8_t bus);
    void (*ow_close)(uint8_t bus);
    esp_err_t (*dht_start)(uint8_t gpio, uint32_t token);
    esp_err_t (*sonar_start)(uint8_t trig, uint8_t echo, uint32_t token);
    void (*release)(void);
    /** A free-running microsecond clock (plugin time budget). */
    uint64_t (*micros)(void);
    esp_err_t (*init)(const telemetrix_profile_t *p);
    void (*deinit)(void);
} tmx_drv_t;

#endif
