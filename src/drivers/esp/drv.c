#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "tmx_drv_esp.h"

/* Chip driver table: GPIO, LEDC, ADC, I2C, 1-wire and the DHT and sonar measurements (drv_*.c). */

static uint64_t n_micros(void)
{
    return (uint64_t)esp_timer_get_time();
}

static void n_chip_info(uint16_t *model, uint16_t *revision, char *idf, size_t idf_cap)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    *model = (uint16_t)info.model;
    *revision = info.revision;      /* IDF already counts major x 100 + minor */
    snprintf(idf, idf_cap, "%s", esp_get_idf_version());
}

static esp_err_t n_init(const telemetrix_profile_t *p)
{
    esp_gpio_init();
    esp_pulse_init();
    esp_onewire_init(p);
    return esp_adc_init(p);
}

static void n_deinit(void)
{
    esp_pulse_deinit();
    esp_onewire_deinit();
    esp_i2c_close(0);
    esp_i2c_close(1);
    esp_adc_deinit();
}

const tmx_drv_t tmx_drv_esp = {
    .gpio_input = esp_gpio_input,
    .gpio_output = esp_gpio_output,
    .gpio_set = esp_gpio_set,
    .gpio_get = esp_gpio_get,
    .ledc_timer = esp_ledc_timer,
    .ledc_attach = esp_ledc_attach,
    .ledc_duty = esp_ledc_duty,
    .ledc_stop = esp_ledc_stop,
    .ledc_timer_free = esp_ledc_timer_free,
    .adc_read = esp_adc_read,
    .i2c_open = esp_i2c_open,
    .i2c_read = esp_i2c_read,
    .i2c_write = esp_i2c_write,
    .i2c_close = esp_i2c_close,
    .ow_open = esp_ow_open,
    .ow_reset = esp_ow_reset,
    .ow_write = esp_ow_write,
    .ow_read = esp_ow_read,
    .ow_search = esp_ow_search,
    .ow_reset_search = esp_ow_reset_search,
    .ow_close = esp_ow_close,
    .dht_start = esp_dht_start,
    .sonar_start = esp_sonar_start,
    .release = esp_pulse_release,
    .micros = n_micros,
    .chip_info = n_chip_info,
    .init = n_init,
    .deinit = n_deinit,
};

const tmx_drv_t *const tmx_drv_default = &tmx_drv_esp;
