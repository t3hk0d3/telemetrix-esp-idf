#include "telemetrix.h"

/* Minimal build check: an empty profile, no network bring-up yet. */
void app_main(void)
{
    static const telemetrix_profile_t profile = {0};
    telemetrix_config_t config;
    telemetrix_config_default(&config);
    telemetrix_start(&profile, &config, NULL);
}
