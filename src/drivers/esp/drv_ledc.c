#include "driver/ledc.h"

#include "tmx_drv_esp.h"

esp_err_t esp_ledc_timer(uint8_t speed_mode, uint8_t timer, uint32_t freq_hz, uint8_t resolution)
{
    const ledc_timer_config_t cfg = {
        .speed_mode = (ledc_mode_t)speed_mode,
        .duty_resolution = (ledc_timer_bit_t)resolution,
        .timer_num = (ledc_timer_t)timer,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    return ledc_timer_config(&cfg);
}

esp_err_t esp_ledc_attach(uint8_t speed_mode, uint8_t channel, uint8_t timer, uint8_t gpio)
{
    const ledc_channel_config_t cfg = {
        .gpio_num = gpio,
        .speed_mode = (ledc_mode_t)speed_mode,
        .channel = (ledc_channel_t)channel,
        .timer_sel = (ledc_timer_t)timer,
        .duty = 0,
        .hpoint = 0,
    };
    esp_gpio_forget(gpio);
    return ledc_channel_config(&cfg);
}

esp_err_t esp_ledc_duty(uint8_t speed_mode, uint8_t channel, uint32_t duty)
{
    esp_err_t err = ledc_set_duty((ledc_mode_t)speed_mode, (ledc_channel_t)channel, duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty((ledc_mode_t)speed_mode, (ledc_channel_t)channel);
}

void esp_ledc_stop(uint8_t speed_mode, uint8_t channel)
{
    ledc_stop((ledc_mode_t)speed_mode, (ledc_channel_t)channel, 0);
}

void esp_ledc_timer_free(uint8_t speed_mode, uint8_t timer)
{
    /* The timer has to be paused before it can be de-configured. */
    ledc_timer_pause((ledc_mode_t)speed_mode, (ledc_timer_t)timer);
    const ledc_timer_config_t cfg = {
        .speed_mode = (ledc_mode_t)speed_mode,
        .timer_num = (ledc_timer_t)timer,
        .deconfigure = true,
    };
    ledc_timer_config(&cfg);
}
