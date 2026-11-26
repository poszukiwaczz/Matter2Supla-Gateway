set(SRC_DIRS_LIST ".")
if (CONFIG_CUSTOM_REVOKED_DAC_CHAIN_CHECK)
    list(APPEND SRC_DIRS_LIST "revocation_set")
endif()

idf_component_register(SRC_DIRS          ${SRC_DIRS_LIST}
                       INCLUDE_DIRS      "."
                       PRIV_INCLUDE_DIRS  "." "${ESP_MATTER_PATH}/examples/common/utils"
                       EMBED_TXTFILES     "www/theme.js"
                                          "www/index.html"
                                          "www/app.js"
                                          "www/gateway.js"
                                          "update_public_key.pem"
                                          "www/jsQR.js"
                       LDFRAGMENTS "linker.lf")

foreach(asset IN ITEMS index.html app.js jsQR.js)
    set(asset_source "${CMAKE_CURRENT_SOURCE_DIR}/www/${asset}")
    set(asset_gzip "${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz")
    add_custom_command(OUTPUT "${asset_gzip}"
        COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/gzip_web_asset.py" "${asset_source}" "${asset_gzip}"
        DEPENDS "${asset_source}" "${CMAKE_CURRENT_SOURCE_DIR}/gzip_web_asset.py"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${asset_gzip}" BINARY)
endforeach()

if(CONFIG_SPIFFS_ATTESTATION_TRUST_STORE)
    spiffs_create_partition_image(paa_cert ${CMAKE_SOURCE_DIR}/paa_cert FLASH_IN_PROJECT)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
