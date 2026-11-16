#ifndef TMX_DRV_ESP_H
#define TMX_DRV_ESP_H

#include "tmx_drv.h"

/** The chip driver table; tmx_drv_default points at it. */
extern const tmx_drv_t tmx_drv_esp;

/* GPIO */
esp_err_t esp_gpio_input(uint8_t gpio, bool pull_up, bool pull_down);
esp_err_t esp_gpio_output(uint8_t gpio, bool level);
void esp_gpio_set(uint8_t gpio, bool v);
int esp_gpio_get(uint8_t gpio);
/** Another peripheral took the pin over: the next gpio_input or gpio_output reconfigures it. */
void esp_gpio_forget(uint8_t gpio);
/** Forget every pin (driver init): the first gpio_input or gpio_output of each pin reconfigures it. */
void esp_gpio_init(void);

/* LEDC */
esp_err_t esp_ledc_timer(uint8_t speed_mode, uint8_t timer, uint32_t freq_hz, uint8_t resolution);
esp_err_t esp_ledc_attach(uint8_t speed_mode, uint8_t channel, uint8_t timer, uint8_t gpio);
esp_err_t esp_ledc_duty(uint8_t speed_mode, uint8_t channel, uint32_t duty);
void esp_ledc_stop(uint8_t speed_mode, uint8_t channel);
void esp_ledc_timer_free(uint8_t speed_mode, uint8_t timer);

/* ADC: init prepares the units of the profile, deinit releases the handles the driver created; a read configures the
 * channel (12 dB) and makes the GPIO cache forget the pin */
esp_err_t esp_adc_init(const telemetrix_profile_t *p);
void esp_adc_deinit(void);
esp_err_t esp_adc_read(uint8_t unit, uint8_t channel, int *raw);

/* I2C: a bus handle from the grant is never deleted, only the device handles the driver created */
esp_err_t esp_i2c_open(uint8_t bus, const telemetrix_i2c_bus_t *grant, int8_t sda, int8_t scl);
esp_err_t esp_i2c_read(uint8_t bus, uint8_t addr, uint8_t reg, bool stop, uint8_t *data, size_t n,
                       uint16_t timeout_ms);
esp_err_t esp_i2c_write(uint8_t bus, uint8_t addr, const uint8_t *data, size_t n, uint16_t timeout_ms);
void esp_i2c_close(uint8_t bus);

/* 1-wire: init takes the RMT TX grant, deinit closes every bus */
void esp_onewire_init(const telemetrix_profile_t *p);
void esp_onewire_deinit(void);
esp_err_t esp_ow_open(uint8_t bus, uint8_t gpio);
bool esp_ow_reset(uint8_t bus);
void esp_ow_write(uint8_t bus, uint8_t byte, bool power);
uint8_t esp_ow_read(uint8_t bus);
bool esp_ow_search(uint8_t bus, uint8_t rom[8]);
void esp_ow_reset_search(uint8_t bus);
void esp_ow_close(uint8_t bus);

/* DHT and sonar: start without blocking, complete once through tmx_completion_post; release (end of a session) abandons
 * a measurement in flight and frees the RMT channel and the GPIO handler, deinit frees the timers as well */
esp_err_t esp_dht_start(uint8_t gpio, uint32_t token);
esp_err_t esp_sonar_start(uint8_t trig, uint8_t echo, uint32_t token);
void esp_pulse_init(void);
void esp_pulse_release(void);
void esp_pulse_deinit(void);

#endif
