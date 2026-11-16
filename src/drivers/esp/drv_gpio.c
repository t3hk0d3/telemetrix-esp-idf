#include "driver/gpio.h"

#include "tmx_drv_esp.h"

/* gpio_config reserves the pin and warns about a conflict on every later call for a pin that is already an output or
 * has its input enabled; only gpio_reset_pin (banned) drops the reservation. So a pin goes through gpio_config once and
 * every later mode change uses gpio_set_direction and gpio_set_pull_mode. */
typedef enum { MODE_UNKNOWN, MODE_INPUT, MODE_OUTPUT } pin_mode_t;

typedef struct {
    bool configured; /* gpio_config has run once */
    pin_mode_t mode;
    bool pull_up, pull_down;
} pin_state_t;

static pin_state_t pins[GPIO_NUM_MAX];

static pin_state_t *pin_state(uint8_t gpio)
{
    return gpio < GPIO_NUM_MAX ? &pins[gpio] : NULL;
}

static gpio_pull_mode_t pull_mode(bool pull_up, bool pull_down)
{
    if (pull_up && pull_down) {
        return GPIO_PULLUP_PULLDOWN;
    }
    if (pull_up) {
        return GPIO_PULLUP_ONLY;
    }
    return pull_down ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING;
}

/* Input and output buffers both on, so gpio_get reads the pad of an output as well. */
static esp_err_t configure(uint8_t gpio, gpio_mode_t mode, bool pull_up, bool pull_down)
{
    pin_state_t *s = pin_state(gpio);
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    if (!s->configured) {
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << gpio,
                                 .mode = mode,
                                 .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
                                 .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
                                 .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&cfg);
        if (err == ESP_OK) {
            s->configured = true;
        }
    } else {
        err = gpio_set_pull_mode((gpio_num_t)gpio, pull_mode(pull_up, pull_down));
        if (err == ESP_OK) {
            err = gpio_set_direction((gpio_num_t)gpio, mode);
        }
    }
    if (err != ESP_OK) {
        s->mode = MODE_UNKNOWN;
        return err;
    }
    s->mode = mode == GPIO_MODE_INPUT ? MODE_INPUT : MODE_OUTPUT;
    s->pull_up = pull_up;
    s->pull_down = pull_down;
    return ESP_OK;
}

esp_err_t esp_gpio_input(uint8_t gpio, bool pull_up, bool pull_down)
{
    const pin_state_t *s = pin_state(gpio);
    if (s && s->mode == MODE_INPUT && s->pull_up == pull_up && s->pull_down == pull_down) {
        return ESP_OK;
    }
    return configure(gpio, GPIO_MODE_INPUT, pull_up, pull_down);
}

esp_err_t esp_gpio_output(uint8_t gpio, bool level)
{
    /* The output register takes the level while the pin is still an input (or already an output); enabling the output
     * afterwards drives that level at once, so a DRIVE_HIGH safe state never glitches low. */
    esp_err_t err = gpio_set_level((gpio_num_t)gpio, level);
    if (err != ESP_OK) {
        return err;
    }
    const pin_state_t *s = pin_state(gpio);
    if (s && s->mode == MODE_OUTPUT) {
        return ESP_OK;
    }
    return configure(gpio, GPIO_MODE_INPUT_OUTPUT, false, false);
}

void esp_gpio_init(void)
{
    /* Between a stop and the next start the application, or a peripheral the server released, may have changed any
     * pin: no cached mode survives. Whether gpio_config has run stays known, so no pin goes through it twice. */
    for (int i = 0; i < GPIO_NUM_MAX; i++) {
        pins[i].mode = MODE_UNKNOWN;
    }
}

void esp_gpio_forget(uint8_t gpio)
{
    pin_state_t *s = pin_state(gpio);
    if (s) {
        s->mode = MODE_UNKNOWN;
    }
}

void esp_gpio_set(uint8_t gpio, bool v)
{
    gpio_set_level((gpio_num_t)gpio, v);
}

int esp_gpio_get(uint8_t gpio)
{
    return gpio_get_level((gpio_num_t)gpio);
}
