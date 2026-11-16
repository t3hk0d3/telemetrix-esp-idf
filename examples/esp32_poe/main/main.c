#include "board_profile.h"
#include "esp_log.h"
#include "network.h"
#include "telemetrix.h"

static const char *TAG = "example";

void app_main(void)
{
    ESP_ERROR_CHECK(example_network_start());

    telemetrix_config_t config;
    telemetrix_config_default(&config);
    const esp_err_t err = telemetrix_start(&board_profile, &config, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "telemetrix_start: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Telemetrix server listening on port %u", config.port);
}
