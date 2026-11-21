idf_component_register(SRCS "esp_ot_rcp.c" "rcp_uart_ota.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES app_update esp_driver_gpio esp_driver_uart esp_event esp_rom esp_timer mbedtls nvs_flash openthread vfs)
