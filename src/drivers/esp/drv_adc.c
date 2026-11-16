#include <stdint.h>

#include "esp_adc/adc_oneshot.h"
#include "soc/soc_caps.h"

#include "tmx_drv_esp.h"

#define UNITS SOC_ADC_PERIPH_NUM
#define CHANNELS 16

typedef struct {
    adc_oneshot_unit_handle_t handle; /* NULL = no grant */
    bool owned;                       /* created here, so deleted here */
    uint16_t configured;              /* channel bits already configured */
} unit_state_t;

static unit_state_t units[UNITS];

esp_err_t esp_adc_init(const telemetrix_profile_t *p)
{
    esp_adc_deinit();
    for (size_t i = 0; i < p->n_adc; i++) {
        const telemetrix_adc_grant_t *g = &p->adc[i];
        if (g->unit >= UNITS) {
            esp_adc_deinit();
            return ESP_ERR_INVALID_ARG;
        }
        unit_state_t *u = &units[g->unit];
        if (g->handle) {
            u->handle = (adc_oneshot_unit_handle_t)g->handle;
            continue;
        }
        const adc_oneshot_unit_init_cfg_t cfg = {
            .unit_id = (adc_unit_t)g->unit,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        esp_err_t err = adc_oneshot_new_unit(&cfg, &u->handle);
        if (err != ESP_OK) {
            u->handle = NULL;
            esp_adc_deinit();
            return err;
        }
        u->owned = true;
    }
    return ESP_OK;
}

void esp_adc_deinit(void)
{
    for (int i = 0; i < UNITS; i++) {
        if (units[i].handle && units[i].owned) {
            adc_oneshot_del_unit(units[i].handle);
        }
        units[i] = (unit_state_t){0};
    }
}

esp_err_t esp_adc_read(uint8_t unit, uint8_t channel, int *raw)
{
    if (unit >= UNITS || channel >= CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    unit_state_t *u = &units[unit];
    if (!u->handle) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!(u->configured & (1u << channel))) {
        const adc_oneshot_chan_cfg_t cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        esp_err_t err = adc_oneshot_config_channel(u->handle, (adc_channel_t)channel, &cfg);
        if (err != ESP_OK) {
            return err;
        }
        u->configured |= (uint16_t)(1u << channel);
    }
    return adc_oneshot_read(u->handle, (adc_channel_t)channel, raw);
}
