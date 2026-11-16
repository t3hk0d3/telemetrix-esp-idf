#include "tmx_core.h"

#define CAP_ALL 0xFFu

static bool pin_has_caps(const telemetrix_pin_t *pin, uint16_t cap)
{
    return pin && (pin->caps & cap) == cap;
}

static bool has_pin_with_cap(const telemetrix_profile_t *p, int gpio, uint16_t cap)
{
    return gpio >= 0 && pin_has_caps(tmx_profile_pin(p, (uint8_t)gpio), cap);
}

static bool adc_unit_granted(const telemetrix_profile_t *p, int unit)
{
    for (size_t i = 0; i < p->n_adc; i++) {
        if (p->adc[i].unit == unit) {
            return true;
        }
    }
    return false;
}

static esp_err_t validate_pins(const telemetrix_profile_t *p, bool *any_pwm)
{
    for (size_t i = 0; i < p->n_pins; i++) {
        const telemetrix_pin_t *pin = &p->pins[i];
        for (size_t j = 0; j < i; j++) {
            if (p->pins[j].gpio == pin->gpio) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        if ((unsigned)pin->safe > (unsigned)TELEMETRIX_SAFE_DRIVE_HIGH || (pin->caps & ~CAP_ALL) != 0) {
            return ESP_ERR_INVALID_ARG;
        }
        if ((pin->caps & TELEMETRIX_CAP_ADC) &&
                (pin->adc_unit < 0 || pin->adc_channel < 0 || !adc_unit_granted(p, pin->adc_unit))) {
            return ESP_ERR_INVALID_ARG;
        }
        if (pin->caps & TELEMETRIX_CAP_PWM) {
            *any_pwm = true;
        }
    }
    return ESP_OK;
}

static esp_err_t validate_buses(const telemetrix_profile_t *p)
{
    for (size_t i = 0; i < p->n_i2c; i++) {
        const telemetrix_i2c_bus_t *b = &p->i2c[i];
        for (size_t j = 0; j < i; j++) {
            if (p->i2c[j].bus == b->bus) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (b->handle) {
            /* shared with the application: both pins named, neither a profile pin, so nothing here ever drives them */
            if (b->sda < 0 || b->scl < 0 || b->sda == b->scl || tmx_profile_pin(p, (uint8_t)b->sda) ||
                    tmx_profile_pin(p, (uint8_t)b->scl)) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if ((b->sda >= 0 && !has_pin_with_cap(p, b->sda, TELEMETRIX_CAP_I2C)) ||
                   (b->scl >= 0 && !has_pin_with_cap(p, b->scl, TELEMETRIX_CAP_I2C))) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (size_t i = 0; i < p->n_onewire; i++) {
        const telemetrix_onewire_bus_t *b = &p->onewire[i];
        for (size_t j = 0; j < i; j++) {
            if (p->onewire[j].bus == b->bus) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (b->gpio >= 0 && !has_pin_with_cap(p, b->gpio, TELEMETRIX_CAP_ONE_WIRE)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t tmx_profile_validate(const telemetrix_profile_t *p)
{
    if (!p || p->n_pins > TMX_MAX_PINS || (p->n_pins && !p->pins) || (p->n_adc && !p->adc) ||
            (p->n_i2c && !p->i2c) || (p->n_onewire && !p->onewire)) {
        return ESP_ERR_INVALID_ARG;
    }
    bool any_pwm = false;
    esp_err_t err = validate_pins(p, &any_pwm);
    if (err != ESP_OK) {
        return err;
    }
    if (any_pwm && (p->ledc.timer_mask == 0 || p->ledc.channel_mask == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    return validate_buses(p);
}

const telemetrix_pin_t *tmx_profile_pin(const telemetrix_profile_t *p, uint8_t gpio)
{
    for (size_t i = 0; i < p->n_pins; i++) {
        if (p->pins[i].gpio == gpio) {
            return &p->pins[i];
        }
    }
    return NULL;
}
