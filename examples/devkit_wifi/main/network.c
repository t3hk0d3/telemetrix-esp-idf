#include "network.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

#include "esp_wifi.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "network";

#define GOT_IP_BIT BIT0

static EventGroupHandle_t events;

static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = data;
    ESP_LOGI(TAG, "got IPv4 address " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(events, GOT_IP_BIT);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "connecting to \"%s\"", CONFIG_EXAMPLE_WIFI_SSID);
        esp_wifi_connect();
    }
}

static esp_err_t network_up(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    esp_netif_create_default_wifi_sta();
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL), TAG, "handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip, NULL), TAG, "handler");

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, CONFIG_EXAMPLE_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, CONFIG_EXAMPLE_WIFI_PASSWORD, sizeof(config.sta.password));
    config.sta.threshold.authmode = config.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "wifi config");
    /* Modem sleep adds tens of milliseconds to every packet; a telemetry server wants none of it. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi power save");
    return esp_wifi_start();
}

esp_err_t example_network_start(void)
{
    events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(events, ESP_ERR_NO_MEM, TAG, "event group");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    ESP_RETURN_ON_ERROR(network_up(), TAG, "network up");
    while (!(xEventGroupWaitBits(events, GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000)) & GOT_IP_BIT)) {
        ESP_LOGI(TAG, "waiting for an IPv4 address");
    }
    return ESP_OK;
}
