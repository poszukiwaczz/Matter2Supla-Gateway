get_filename_component(M2S_RCP_OTA_PROTOCOL_DIR "${PROJECT_DIR}/../rcp-ota-common" ABSOLUTE)
if(EXISTS "${PROJECT_DIR}/rcp-ota-common/rcp_uart_ota_proto.h")
    set(M2S_RCP_OTA_PROTOCOL_DIR "${PROJECT_DIR}/rcp-ota-common")
endif()

idf_component_register(SRCS "esp_ot_rcp.c" "rcp_uart_ota.c"
                       INCLUDE_DIRS "." "${M2S_RCP_OTA_PROTOCOL_DIR}"
                       PRIV_REQUIRES app_update esp_driver_gpio esp_driver_uart esp_event esp_rom esp_timer mbedtls nvs_flash openthread vfs)

# Expose the running RCP build and OTA state in the standard Spinel version response.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=otGetVersionString")
