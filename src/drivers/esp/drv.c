#include "tmx_drv.h"

/* Chip driver: every operation reports ESP_ERR_NOT_SUPPORTED until the peripherals are filled in (tasks 10 and 11).
 * init succeeds, so the server starts with an empty profile. */

static esp_err_t n_gpio_input(uint8_t gpio, bool pull_up, bool pull_down)
{
    (void)gpio, (void)pull_up, (void)pull_down;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t n_gpio_output(uint8_t gpio, bool level)
{
    (void)gpio, (void)level;
    return ESP_ERR_NOT_SUPPORTED;
}

static void n_gpio_set(uint8_t gpio, bool v)
{
    (void)gpio, (void)v;
}

static int n_gpio_get(uint8_t gpio)
{
    (void)gpio;
    return 0;
}

static esp_err_t n_ledc_timer(uint8_t speed_mode, uint8_t timer, uint32_t freq_hz, uint8_t resolution)
{
    (void)speed_mode, (void)timer, (void)freq_hz, (void)resolution;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t n_ledc_attach(uint8_t speed_mode, uint8_t channel, uint8_t timer, uint8_t gpio)
{
    (void)speed_mode, (void)channel, (void)timer, (void)gpio;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t n_ledc_duty(uint8_t speed_mode, uint8_t channel, uint32_t duty)
{
    (void)speed_mode, (void)channel, (void)duty;
    return ESP_ERR_NOT_SUPPORTED;
}

static void n_ledc_stop(uint8_t speed_mode, uint8_t channel)
{
    (void)speed_mode, (void)channel;
}

static void n_ledc_timer_free(uint8_t speed_mode, uint8_t timer)
{
    (void)speed_mode, (void)timer;
}

static esp_err_t n_adc_read(uint8_t unit, uint8_t channel, int *raw)
{
    (void)unit, (void)channel, (void)raw;
    return ESP_ERR_NOT_SUPPORTED;
}

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
    (void)p;
    return ESP_OK;
}

static void n_deinit(void)
{
}

static const tmx_drv_t drv_esp = {
    .gpio_input = n_gpio_input,
    .gpio_output = n_gpio_output,
    .gpio_set = n_gpio_set,
    .gpio_get = n_gpio_get,
    .ledc_timer = n_ledc_timer,
    .ledc_attach = n_ledc_attach,
    .ledc_duty = n_ledc_duty,
    .ledc_stop = n_ledc_stop,
    .ledc_timer_free = n_ledc_timer_free,
    .adc_read = n_adc_read,
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

const tmx_drv_t *const tmx_drv_default = &drv_esp;
