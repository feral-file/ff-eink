idf_component_register(
    SRCS "main.c" "panel.c" "patterns.c" "net_usb.c" "api.c" "identity.c" "restart.c" "ota.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_spi esp_driver_gpio esp_http_server esp_netif esp_event
             nvs_flash esp_timer usb json app_update spi_flash)
