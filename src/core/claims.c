#include "tmx_core.h"

#define LEDC_FREE 0xffu

static const char *const use_names[] = {
    [TMX_USE_NONE] = "none",
    [TMX_USE_INPUT] = "input",
    [TMX_USE_INPUT_PULLUP] = "input_pullup",
    [TMX_USE_INPUT_PULLDOWN] = "input_pulldown",
    [TMX_USE_OUTPUT] = "output",
    [TMX_USE_ANALOG] = "analog",
    [TMX_USE_PWM] = "pwm",
    [TMX_USE_SERVO] = "servo",
    [TMX_USE_I2C] = "i2c",
    [TMX_USE_ONE_WIRE] = "one_wire",
    [TMX_USE_SONAR_TRIG] = "sonar",
    [TMX_USE_SONAR_ECHO] = "sonar",
    [TMX_USE_DHT] = "dht",
};

static uint16_t caps_for(tmx_use_t use)
{
    switch (use) {
    case TMX_USE_INPUT: return TELEMETRIX_CAP_DIN;
    case TMX_USE_INPUT_PULLUP: return TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_PULL_UP;
    case TMX_USE_INPUT_PULLDOWN: return TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_PULL_DOWN;
    case TMX_USE_OUTPUT: return TELEMETRIX_CAP_DOUT;
    case TMX_USE_ANALOG: return TELEMETRIX_CAP_ADC;
    case TMX_USE_PWM:
    case TMX_USE_SERVO: return TELEMETRIX_CAP_PWM;
    case TMX_USE_I2C: return TELEMETRIX_CAP_I2C;
    case TMX_USE_ONE_WIRE: return TELEMETRIX_CAP_ONE_WIRE;
    case TMX_USE_SONAR_TRIG: return TELEMETRIX_CAP_DOUT;
    case TMX_USE_SONAR_ECHO: return TELEMETRIX_CAP_DIN;
    case TMX_USE_DHT: return TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT;
    default: return 0;
    }
}

static bool is_digital(tmx_use_t use)
{
    return use >= TMX_USE_INPUT && use <= TMX_USE_OUTPUT;
}

static int pin_index(const tmx_claims_t *c, uint8_t gpio)
{
    for (size_t i = 0; i < c->profile->n_pins; i++) {
        if (c->profile->pins[i].gpio == gpio) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t apply_safe_state(const tmx_claims_t *c, const telemetrix_pin_t *pin)
{
    switch (pin->safe) {
    case TELEMETRIX_SAFE_PULL_UP: return c->drv->gpio_input(pin->gpio, true, false);
    case TELEMETRIX_SAFE_PULL_DOWN: return c->drv->gpio_input(pin->gpio, false, true);
    case TELEMETRIX_SAFE_DRIVE_LOW: return c->drv->gpio_output(pin->gpio, false);
    case TELEMETRIX_SAFE_DRIVE_HIGH: return c->drv->gpio_output(pin->gpio, true);
    case TELEMETRIX_SAFE_FLOAT:
    default: return c->drv->gpio_input(pin->gpio, false, false);
    }
}

static void reset_peripherals(tmx_claims_t *c)
{
    for (size_t i = 0; i < TMX_LEDC_CHANNELS; i++) {
        c->ledc_timer_of[i] = LEDC_FREE;
    }
    for (size_t i = 0; i < TMX_LEDC_TIMERS; i++) {
        c->timer_freq[i] = 0;
        c->timer_res[i] = 0;
        c->timer_refs[i] = 0;
    }
    c->rmt_rx_used = 0;
}

esp_err_t tmx_claims_init(tmx_claims_t *c, const telemetrix_profile_t *p, const tmx_drv_t *drv)
{
    if (!c || !p || !drv || p->n_pins > TMX_MAX_PINS) {
        return ESP_ERR_INVALID_ARG;
    }
    c->profile = p;
    c->drv = drv;
    for (size_t i = 0; i < TMX_MAX_PINS; i++) {
        c->use[i] = TMX_USE_NONE;
    }
    reset_peripherals(c);
    esp_err_t first = ESP_OK;
    for (size_t i = 0; i < p->n_pins; i++) {
        esp_err_t err = apply_safe_state(c, &p->pins[i]);
        if (first == ESP_OK) {
            first = err;
        }
    }
    return first;
}

esp_err_t tmx_claim_pin(tmx_claims_t *c, uint8_t gpio, tmx_use_t use)
{
    uint16_t need = caps_for(use);
    if (need == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int idx = pin_index(c, gpio);
    if (idx < 0 || (c->profile->pins[idx].caps & need) != need) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    tmx_use_t current = (tmx_use_t)c->use[idx];
    if (current != TMX_USE_NONE && current != use && !(is_digital(current) && is_digital(use))) {
        return ESP_ERR_INVALID_STATE;
    }
    c->use[idx] = (uint8_t)use;
    return ESP_OK;
}

void tmx_release_pin(tmx_claims_t *c, uint8_t gpio)
{
    int idx = pin_index(c, gpio);
    if (idx < 0 || c->use[idx] == TMX_USE_NONE) {
        return;
    }
    c->use[idx] = TMX_USE_NONE;
    apply_safe_state(c, &c->profile->pins[idx]);
}

tmx_use_t tmx_pin_use(const tmx_claims_t *c, uint8_t gpio)
{
    int idx = pin_index(c, gpio);
    return idx < 0 ? TMX_USE_NONE : (tmx_use_t)c->use[idx];
}

const char *tmx_use_name(tmx_use_t use)
{
    if ((unsigned)use >= sizeof(use_names) / sizeof(use_names[0])) {
        return "none";
    }
    return use_names[use];
}

static int lowest_free_channel(const tmx_claims_t *c)
{
    for (int ch = 0; ch < TMX_LEDC_CHANNELS; ch++) {
        if ((c->profile->ledc.channel_mask >> ch & 1) && c->ledc_timer_of[ch] == LEDC_FREE) {
            return ch;
        }
    }
    return -1;
}

esp_err_t tmx_claim_ledc(tmx_claims_t *c, uint32_t freq_hz, uint8_t resolution, uint8_t *channel, uint8_t *timer)
{
    int ch = lowest_free_channel(c);
    if (ch < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    int t = -1;
    for (int i = 0; i < TMX_LEDC_TIMERS; i++) {
        if (c->timer_refs[i] && c->timer_freq[i] == freq_hz && c->timer_res[i] == resolution) {
            t = i;
            break;
        }
    }
    if (t < 0) {
        for (int i = 0; i < TMX_LEDC_TIMERS; i++) {
            if ((c->profile->ledc.timer_mask >> i & 1) && c->timer_refs[i] == 0) {
                t = i;
                break;
            }
        }
        if (t < 0) {
            return ESP_ERR_NOT_FOUND;
        }
        esp_err_t err = c->drv->ledc_timer(c->profile->ledc.speed_mode, (uint8_t)t, freq_hz, resolution);
        if (err != ESP_OK) {
            return err;
        }
        c->timer_freq[t] = freq_hz;
        c->timer_res[t] = resolution;
    }
    c->timer_refs[t]++;
    c->ledc_timer_of[ch] = (uint8_t)t;
    *channel = (uint8_t)ch;
    *timer = (uint8_t)t;
    return ESP_OK;
}

void tmx_release_ledc(tmx_claims_t *c, uint8_t channel)
{
    if (channel >= TMX_LEDC_CHANNELS || c->ledc_timer_of[channel] == LEDC_FREE) {
        return;
    }
    uint8_t t = c->ledc_timer_of[channel];
    c->ledc_timer_of[channel] = LEDC_FREE;
    c->drv->ledc_stop(c->profile->ledc.speed_mode, channel);
    if (--c->timer_refs[t] == 0) {
        c->drv->ledc_timer_free(c->profile->ledc.speed_mode, t);
    }
}

esp_err_t tmx_claim_rmt_rx(tmx_claims_t *c)
{
    if (c->rmt_rx_used >= c->profile->rmt.rx_channels) {
        return ESP_ERR_NOT_FOUND;
    }
    c->rmt_rx_used++;
    return ESP_OK;
}

void tmx_release_rmt_rx(tmx_claims_t *c)
{
    if (c->rmt_rx_used > 0) {
        c->rmt_rx_used--;
    }
}

void tmx_claims_release_all(tmx_claims_t *c)
{
    for (size_t i = 0; i < c->profile->n_pins; i++) {
        if (c->use[i] != TMX_USE_NONE) {
            c->use[i] = TMX_USE_NONE;
            apply_safe_state(c, &c->profile->pins[i]);
        }
    }
    for (uint8_t ch = 0; ch < TMX_LEDC_CHANNELS; ch++) {
        tmx_release_ledc(c, ch);
    }
    reset_peripherals(c);
}
