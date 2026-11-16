idf_component_register(SRCS "main.c" "board_profile.c" "network.c"
                       REQUIRES telemetrix-esp-idf esp_event esp_netif esp_wifi nvs_flash)
