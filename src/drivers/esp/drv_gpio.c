#include "driver/gpio.h"

#include "tmx_drv_esp.h"

esp_err_t esp_gpio_input(uint8_t gpio, bool pull_up, bool pull_down)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

esp_err_t esp_gpio_output(uint8_t gpio, bool level)
{
    /* The output register takes the level while the pin is still an input; enabling the output afterwards drives that
     * level at once, so a DRIVE_HIGH safe state never glitches low. */
    esp_err_t err = gpio_set_level((gpio_num_t)gpio, level);
    if (err != ESP_OK) {
        return err;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

void esp_gpio_set(uint8_t gpio, bool v)
{
    gpio_set_level((gpio_num_t)gpio, v);
}

int esp_gpio_get(uint8_t gpio)
{
    return gpio_get_level((gpio_num_t)gpio);
}
