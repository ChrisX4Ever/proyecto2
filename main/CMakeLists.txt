idf_component_register(SRCS "udp_client.c" "blink_example_main.c" "bmi270.c" "wifi_init.c"
                       INCLUDE_DIRS "."
                       REQUIRES
                           driver
                           lwip 
                           esp_netif
                           nvs_flash
                           esp_wifi
                           esp_timer
)
