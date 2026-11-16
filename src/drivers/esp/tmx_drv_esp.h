#ifndef TMX_DRV_ESP_H
#define TMX_DRV_ESP_H

#include "tmx_drv.h"

/** The chip driver table; tmx_drv_default points at it. Operations of peripherals not yet implemented report
 *  ESP_ERR_NOT_SUPPORTED. */
extern const tmx_drv_t tmx_drv_esp;

/* GPIO */
esp_err_t esp_gpio_input(uint8_t gpio, bool pull_up, bool pull_down);
esp_err_t esp_gpio_output(uint8_t gpio, bool level);
void esp_gpio_set(uint8_t gpio, bool v);
int esp_gpio_get(uint8_t gpio);
/** Another peripheral took the pin over: the next gpio_input or gpio_output reconfigures it. */
void esp_gpio_forget(uint8_t gpio);

/* LEDC */
esp_err_t esp_ledc_timer(uint8_t speed_mode, uint8_t timer, uint32_t freq_hz, uint8_t resolution);
esp_err_t esp_ledc_attach(uint8_t speed_mode, uint8_t channel, uint8_t timer, uint8_t gpio);
esp_err_t esp_ledc_duty(uint8_t speed_mode, uint8_t channel, uint32_t duty);
void esp_ledc_stop(uint8_t speed_mode, uint8_t channel);
void esp_ledc_timer_free(uint8_t speed_mode, uint8_t timer);

/* ADC: init prepares the units of the profile, deinit releases the handles the driver created */
esp_err_t esp_adc_init(const telemetrix_profile_t *p);
void esp_adc_deinit(void);
esp_err_t esp_adc_read(uint8_t unit, uint8_t channel, int *raw);

#endif
