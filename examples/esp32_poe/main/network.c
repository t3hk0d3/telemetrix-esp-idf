#include "network.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

#if CONFIG_EXAMPLE_NETWORK_ETHERNET
#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#else
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <string.h>
#endif

static const char *TAG = "network";

#define GOT_IP_BIT BIT0

static EventGroupHandle_t events;

static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = data;
    ESP_LOGI(TAG, "got IPv4 address " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(events, GOT_IP_BIT);
}

#if CONFIG_EXAMPLE_NETWORK_ETHERNET

/* Olimex ESP32-POE(-ISO): LAN8720 on RMII, 50 MHz clock out of GPIO17, PHY power enable on GPIO12. */
#define PHY_POWER_GPIO 12
#define RMII_CLOCK_GPIO 17
#define SMI_MDC_GPIO 23
#define SMI_MDIO_GPIO 18
#define PHY_ADDRESS 0

static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started, waiting for the link");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static esp_err_t network_up(void)
{
    const gpio_config_t power = {.pin_bit_mask = 1ULL << PHY_POWER_GPIO, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&power), TAG, "PHY power pin");
    ESP_RETURN_ON_ERROR(gpio_set_level(PHY_POWER_GPIO, 1), TAG, "PHY power on");
    vTaskDelay(pdMS_TO_TICKS(20)); /* the oscillator behind the enable pin needs time to settle */

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = PHY_ADDRESS;
    phy_config.reset_gpio_num = -1; /* the PHY resets with its power enable */

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = SMI_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = SMI_MDIO_GPIO;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    emac_config.clock_config.rmii.clock_gpio = RMII_CLOCK_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    /* The generic 802.3 PHY driver covers the LAN8720. */
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_FAIL, TAG, "MAC or PHY instance");

    esp_eth_handle_t eth = NULL;
    const esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&config, &eth), TAG, "driver install");

    esp_netif_t *netif = esp_netif_new(&(esp_netif_config_t)ESP_NETIF_DEFAULT_ETH());
    ESP_RETURN_ON_FALSE(netif, ESP_FAIL, TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)), TAG, "netif attach");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event, NULL), TAG, "handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip, NULL), TAG, "handler");
    return esp_eth_start(eth);
}

#else

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        /* the reason code (wifi_err_reason_t) tells a wrong password, a missing AP or a weak signal apart */
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "disconnected from \"%s\", reason %u, rssi %d", CONFIG_EXAMPLE_WIFI_SSID, event->reason,
                 event->rssi);
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "associated with \"%s\"", CONFIG_EXAMPLE_WIFI_SSID);
    }
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

#endif

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
