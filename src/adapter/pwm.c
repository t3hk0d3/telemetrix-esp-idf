#include "tmx_adapter.h"

#define SERVO_FREQ_HZ 50
#define SERVO_RESOLUTION 14
#define SERVO_PERIOD_US 20000u
#define PWM_RESOLUTION_MAX 14
#define PWM_FREQ_MAX_HZ 80000000.0

void tmx_pwm_forget(tmx_adapter_t *a, int idx)
{
    if (idx < 0) {
        return;
    }
    tmx_use_t use = tmx_pin_use(&a->claims, a->profile->pins[idx].gpio);
    if (use == TMX_USE_PWM || use == TMX_USE_SERVO) {
        tmx_release_ledc(&a->claims, a->pwm.channel[idx]);
    }
    a->pwm.channel[idx] = 0;
    a->pwm.resolution[idx] = 0;
    a->pwm.min_us[idx] = 0;
    a->pwm.max_us[idx] = 0;
}

static size_t servo_count(const tmx_adapter_t *a)
{
    size_t n = 0;
    for (size_t i = 0; i < a->profile->n_pins; i++) {
        n += a->claims.use[i] == TMX_USE_SERVO;
    }
    return n;
}

/* Put a pin on a fresh LEDC channel. The pin's previous mode is torn down only once the channel and timer are in hand;
 * a driver failure after that leaves the pin in its safe state, unclaimed. */
static tmx_res_t ledc_pin(tmx_adapter_t *a, uint8_t gpio, tmx_use_t use, uint32_t freq_hz, uint8_t resolution)
{
    uint8_t channel, timer;
    esp_err_t err = tmx_claim_ledc(&a->claims, freq_hz, resolution, &channel, &timer);
    if (err != ESP_OK) {
        return err == ESP_ERR_NOT_FOUND ? TMX_RES_UNAVAILABLE : TMX_RES_REFUSED;
    }
    int idx = tmx_adapter_pin_index(a, gpio);
    tmx_adapter_retire(a, gpio);
    if (tmx_claim_pin(&a->claims, gpio, use) != ESP_OK) {
        tmx_release_ledc(&a->claims, channel);
        return TMX_RES_REFUSED;
    }
    if (a->drv->ledc_attach(a->profile->ledc.speed_mode, channel, timer, gpio) != ESP_OK) {
        tmx_release_ledc(&a->claims, channel);
        tmx_release_pin(&a->claims, gpio);
        return TMX_RES_REFUSED;
    }
    a->pwm.channel[idx] = channel;
    a->pwm.resolution[idx] = resolution;
    return TMX_RES_OK;
}

bool tmx_pwm_mode(tmx_adapter_t *a, uint8_t gpio, uint8_t resolution, double freq_hz)
{
    if (resolution < 1 || resolution > PWM_RESOLUTION_MAX || !(freq_hz >= 1.0 && freq_hz <= PWM_FREQ_MAX_HZ) ||
            !tmx_adapter_pin_ok(a, gpio, TMX_USE_PWM)) {
        return false;
    }
    return ledc_pin(a, gpio, TMX_USE_PWM, (uint32_t)freq_hz, resolution) == TMX_RES_OK;
}

bool tmx_pwm_write(tmx_adapter_t *a, uint8_t gpio, uint16_t value)
{
    if (tmx_pin_use(&a->claims, gpio) != TMX_USE_PWM) {
        return false;
    }
    int idx = tmx_adapter_pin_index(a, gpio);
    uint32_t max = (1u << a->pwm.resolution[idx]) - 1;
    a->drv->ledc_duty(a->profile->ledc.speed_mode, a->pwm.channel[idx], value > max ? max : value);
    return true;
}

bool tmx_pwm_attach(tmx_adapter_t *a, uint8_t gpio)
{
    return tmx_pin_use(&a->claims, gpio) == TMX_USE_PWM;    /* the channel exists already: a no-op success */
}

bool tmx_pwm_detach(tmx_adapter_t *a, uint8_t gpio)
{
    if (tmx_pin_use(&a->claims, gpio) != TMX_USE_PWM) {
        return false;
    }
    tmx_adapter_retire(a, gpio);
    return true;
}

tmx_res_t tmx_servo_attach(tmx_adapter_t *a, uint8_t gpio, uint16_t min_us, uint16_t max_us)
{
    if (!tmx_adapter_pin_ok(a, gpio, TMX_USE_SERVO)) {
        return TMX_RES_REFUSED;
    }
    if (tmx_pin_use(&a->claims, gpio) != TMX_USE_SERVO && servo_count(a) >= a->cfg.max_servo) {
        return TMX_RES_UNAVAILABLE;
    }
    tmx_res_t res = ledc_pin(a, gpio, TMX_USE_SERVO, SERVO_FREQ_HZ, SERVO_RESOLUTION);
    if (res == TMX_RES_OK) {
        int idx = tmx_adapter_pin_index(a, gpio);
        a->pwm.min_us[idx] = min_us;
        a->pwm.max_us[idx] = max_us;
    }
    return res;
}

bool tmx_servo_write(tmx_adapter_t *a, uint8_t gpio, uint8_t angle)
{
    if (tmx_pin_use(&a->claims, gpio) != TMX_USE_SERVO) {
        return false;
    }
    int idx = tmx_adapter_pin_index(a, gpio);
    int32_t min = a->pwm.min_us[idx], max = a->pwm.max_us[idx];
    int32_t pulse = min + (int32_t)(angle > 180 ? 180 : angle) * (max - min) / 180;
    uint32_t duty = pulse < 0 ? 0 : ((uint32_t)pulse << SERVO_RESOLUTION) / SERVO_PERIOD_US;
    uint32_t top = (1u << SERVO_RESOLUTION) - 1;
    a->drv->ledc_duty(a->profile->ledc.speed_mode, a->pwm.channel[idx], duty > top ? top : duty);
    return true;
}

bool tmx_servo_detach(tmx_adapter_t *a, uint8_t gpio)
{
    if (tmx_pin_use(&a->claims, gpio) != TMX_USE_SERVO) {
        return false;
    }
    tmx_adapter_retire(a, gpio);
    return true;
}
