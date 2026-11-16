#include "tmx_drv_esp.h"

/* Chip driver table. GPIO, LEDC and ADC are real (drv_gpio.c, drv_ledc.c, drv_adc.c); the rest reports
 * ESP_ERR_NOT_SUPPORTED until task 11. */

static esp_err_t n_i2c_open(uint8_t bus, const telemetrix_i2c_bus_t *grant, int8_t sda, int8_t scl)
{
    (void)bus, (void)grant, (void)sda, (void)scl;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t n_i2c_read(uint8_t bus, uint8_t addr, uint8_t reg, bool stop, uint8_t *data, size_t n,
                            uint16_t timeout_ms)
{
    (void)bus, (void)addr, (void)reg, (void)stop, (void)data, (void)n, (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t n_i2c_write(uint8_t bus, uint8_t addr, const uint8_t *data, size_t n, uint16_t timeout_ms)
{
    (void)bus, (void)addr, (void)data, (void)n, (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

static void n_i2c_close(uint8_t bus)
{
    (void)bus;
}

static esp_err_t n_ow_open(uint8_t bus, uint8_t gpio)
{
    (void)bus, (void)gpio;
    return ESP_ERR_NOT_SUPPORTED;
}

static bool n_ow_reset(uint8_t bus)
{
    (void)bus;
    return false;
}

static void n_ow_write(uint8_t bus, uint8_t byte, bool power)
{
    (void)bus, (void)byte, (void)power;
}

static uint8_t n_ow_read(uint8_t bus)
{
    (void)bus;
    return 0xff;
}

static bool n_ow_search(uint8_t bus, uint8_t rom[8])
{
    (void)bus, (void)rom;
    return false;
}

static void n_ow_reset_search(uint8_t bus)
{
    (void)bus;
}

static void n_ow_close(uint8_t bus)
{
    (void)bus;
}

static esp_err_t n_dht_start(uint8_t gpio, uint32_t token)
{
    (void)gpio, (void)token;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t n_sonar_start(uint8_t trig, uint8_t echo, uint32_t token)
{
    (void)trig, (void)echo, (void)token;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t n_init(const telemetrix_profile_t *p)
{
    return esp_adc_init(p);
}

static void n_deinit(void)
{
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
    .i2c_open = n_i2c_open,
    .i2c_read = n_i2c_read,
    .i2c_write = n_i2c_write,
    .i2c_close = n_i2c_close,
    .ow_open = n_ow_open,
    .ow_reset = n_ow_reset,
    .ow_write = n_ow_write,
    .ow_read = n_ow_read,
    .ow_search = n_ow_search,
    .ow_reset_search = n_ow_reset_search,
    .ow_close = n_ow_close,
    .dht_start = n_dht_start,
    .sonar_start = n_sonar_start,
    .init = n_init,
    .deinit = n_deinit,
};

const tmx_drv_t *const tmx_drv_default = &tmx_drv_esp;
