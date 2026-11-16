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
static telemetrix_reason_t ledc_pin(tmx_adapter_t *a, uint8_t gpio, tmx_use_t use, uint32_t freq_hz,
                                    uint8_t resolution)
{
    uint8_t channel, timer;
    esp_err_t err = tmx_claim_ledc(&a->claims, freq_hz, resolution, &channel, &timer);
    if (err != ESP_OK) {
        return err == ESP_ERR_NOT_FOUND ? TELEMETRIX_REASON_EXHAUSTED : TELEMETRIX_REASON_HARDWARE;
    }
    int idx = tmx_adapter_pin_index(a, gpio);
    tmx_adapter_retire(a, gpio);
    err = tmx_claim_pin(&a->claims, gpio, use);
    if (err != ESP_OK) {
        tmx_release_ledc(&a->claims, channel);
        return tmx_adapter_claim_reason(err);
    }
    if (a->drv->ledc_attach(a->profile->ledc.speed_mode, channel, timer, gpio) != ESP_OK) {
        tmx_release_ledc(&a->claims, channel);
        tmx_release_pin(&a->claims, gpio);
        return TELEMETRIX_REASON_HARDWARE;
    }
    a->pwm.channel[idx] = channel;
    a->pwm.resolution[idx] = resolution;
    return TELEMETRIX_REASON_NONE;
}

telemetrix_reason_t tmx_pwm_mode(tmx_adapter_t *a, uint8_t gpio, uint8_t resolution, double freq_hz)
{
    if (resolution < 1 || resolution > PWM_RESOLUTION_MAX || !(freq_hz >= 1.0 && freq_hz <= PWM_FREQ_MAX_HZ)) {
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
    telemetrix_reason_t why = tmx_adapter_pin_check(a, gpio, TMX_USE_PWM);
    return why != TELEMETRIX_REASON_NONE ? why : ledc_pin(a, gpio, TMX_USE_PWM, (uint32_t)freq_hz, resolution);
}

telemetrix_reason_t tmx_pwm_write(tmx_adapter_t *a, uint8_t gpio, uint16_t value)
{
    telemetrix_reason_t why = tmx_adapter_pin_state(a, gpio, TMX_USE_PWM);
    if (why != TELEMETRIX_REASON_NONE) {
        return why;
    }
    int idx = tmx_adapter_pin_index(a, gpio);
    uint32_t max = (1u << a->pwm.resolution[idx]) - 1;
    a->drv->ledc_duty(a->profile->ledc.speed_mode, a->pwm.channel[idx], value > max ? max : value);
    return TELEMETRIX_REASON_NONE;
}

telemetrix_reason_t tmx_pwm_attach(tmx_adapter_t *a, uint8_t gpio)
{
    return tmx_adapter_pin_state(a, gpio, TMX_USE_PWM);     /* the channel exists already: a no-op success */
}

telemetrix_reason_t tmx_pwm_detach(tmx_adapter_t *a, uint8_t gpio)
{
    telemetrix_reason_t why = tmx_adapter_pin_state(a, gpio, TMX_USE_PWM);
    if (why == TELEMETRIX_REASON_NONE) {
        tmx_adapter_retire(a, gpio);
    }
    return why;
}

telemetrix_reason_t tmx_servo_attach(tmx_adapter_t *a, uint8_t gpio, uint16_t min_us, uint16_t max_us)
{
    telemetrix_reason_t why = tmx_adapter_pin_check(a, gpio, TMX_USE_SERVO);
    if (why != TELEMETRIX_REASON_NONE) {
        return why;
    }
    if (tmx_pin_use(&a->claims, gpio) != TMX_USE_SERVO && servo_count(a) >= a->cfg.max_servo) {
        return TELEMETRIX_REASON_EXHAUSTED;
    }
    telemetrix_reason_t res = ledc_pin(a, gpio, TMX_USE_SERVO, SERVO_FREQ_HZ, SERVO_RESOLUTION);
    if (res == TELEMETRIX_REASON_NONE) {
        int idx = tmx_adapter_pin_index(a, gpio);
        a->pwm.min_us[idx] = min_us;
        a->pwm.max_us[idx] = max_us;
    }
    return res;
}

telemetrix_reason_t tmx_servo_write(tmx_adapter_t *a, uint8_t gpio, uint8_t angle)
{
    telemetrix_reason_t why = tmx_adapter_pin_state(a, gpio, TMX_USE_SERVO);
    if (why != TELEMETRIX_REASON_NONE) {
        return why;
    }
    int idx = tmx_adapter_pin_index(a, gpio);
    int32_t min = a->pwm.min_us[idx], max = a->pwm.max_us[idx];
    int32_t pulse = min + (int32_t)(angle > 180 ? 180 : angle) * (max - min) / 180;
    uint32_t duty = pulse < 0 ? 0 : ((uint32_t)pulse << SERVO_RESOLUTION) / SERVO_PERIOD_US;
    uint32_t top = (1u << SERVO_RESOLUTION) - 1;
    a->drv->ledc_duty(a->profile->ledc.speed_mode, a->pwm.channel[idx], duty > top ? top : duty);
    return TELEMETRIX_REASON_NONE;
}

telemetrix_reason_t tmx_servo_detach(tmx_adapter_t *a, uint8_t gpio)
{
    telemetrix_reason_t why = tmx_adapter_pin_state(a, gpio, TMX_USE_SERVO);
    if (why == TELEMETRIX_REASON_NONE) {
        tmx_adapter_retire(a, gpio);
    }
    return why;
}
