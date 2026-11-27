# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Please set ESP_MATTER_PATH to the path of esp-matter repo")
endif(NOT DEFINED ENV{ESP_MATTER_PATH})

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# This should be done before using the IDF_TARGET variable.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    "$ENV{SUPLA_DEVICE_PATH}/extras/esp-idf/supla-device")

project(controller)

# Compile the HTTPS allocation fix without modifying the SDK checkout.
if(CONFIG_ESP_HTTPS_SERVER_ENABLE)
    set(m2s_https_original "$ENV{IDF_PATH}/components/esp_https_server/src/https_server.c")
    set(m2s_https_fixed "${CMAKE_BINARY_DIR}/m2s_https/https_server.c")
    set(m2s_https_script "${CMAKE_SOURCE_DIR}/main/https_session_fix.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${m2s_https_script}" "${m2s_https_original}")
    execute_process(COMMAND "${PYTHON}" "${m2s_https_script}" "${m2s_https_original}" "${m2s_https_fixed}"
                    COMMAND_ERROR_IS_FATAL ANY)
    idf_component_get_property(m2s_https_library esp_https_server COMPONENT_LIB)
    get_target_property(m2s_https_sources ${m2s_https_library} SOURCES)
    set(m2s_https_matches 0)
    foreach(m2s_https_source IN LISTS m2s_https_sources)
        if(m2s_https_source MATCHES "(^|/)https_server[.]c$")
            list(REMOVE_ITEM m2s_https_sources "${m2s_https_source}")
            list(APPEND m2s_https_sources "${m2s_https_fixed}")
            math(EXPR m2s_https_matches "${m2s_https_matches}+1")
        endif()
    endforeach()
    if(NOT m2s_https_matches EQUAL 1)
        message(FATAL_ERROR "Expected one HTTPS server source")
    endif()
    set_property(TARGET ${m2s_https_library} PROPERTY SOURCES "${m2s_https_sources}")
endif()

# Keep the UART RCP startup retry local to this S3 build.
if(CONFIG_OPENTHREAD_RADIO_SPINEL_UART)
    set(m2s_spinel_dir "$ENV{IDF_PATH}/components/openthread/openthread/src/lib/spinel")
    file(SHA256 "${m2s_spinel_dir}/spinel_driver.cpp" m2s_spinel_hash)
    if(NOT m2s_spinel_hash STREQUAL "078700f939212c8cdfe1fde233f7aba61cba798c2ff342a8635c533fd28a7784")
        message(FATAL_ERROR "Review local Spinel reset retry after SDK change")
    endif()
    idf_component_get_property(m2s_openthread_library openthread COMPONENT_LIB)
    target_compile_definitions(${m2s_openthread_library} PRIVATE OPENTHREAD_CONFIG_DATASET_UPDATER_ENABLE=1)
    get_target_property(m2s_spinel_sources ${m2s_openthread_library} SOURCES)
    set(m2s_spinel_replacements 0)
    foreach(m2s_spinel_source IN LISTS m2s_spinel_sources)
        if(m2s_spinel_source MATCHES "(^|/)spinel_driver.cpp$")
            list(REMOVE_ITEM m2s_spinel_sources "${m2s_spinel_source}")
            list(APPEND m2s_spinel_sources "${CMAKE_SOURCE_DIR}/main/spinel_reset_fix/spinel_driver.cpp")
            math(EXPR m2s_spinel_replacements "${m2s_spinel_replacements}+1")
        endif()
    endforeach()
    if(NOT m2s_spinel_replacements EQUAL 1)
        message(FATAL_ERROR "Expected exactly one OpenThread radio Spinel driver source")
    endif()
    if(CONFIG_M2S_SINGLE_USB_RCP_OTA)
        set(rcp_port "$ENV{IDF_PATH}/components/openthread/src/port/esp_openthread_radio_spinel.cpp")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rcp_port}")
        file(READ "${rcp_port}" rcp_port_text)
        set(rcp_probe_anchor "const char *otPlatRadioGetVersionString(otInstance *aInstance)")
        set(rcp_query [=[
extern "C" bool m2s_read_rcp_ota_version(char *out, size_t capacity)
{
    return s_radio.Get(SPINEL_PROP_NCP_VERSION, SPINEL_DATATYPE_UTF8_S, out, capacity) == OT_ERROR_NONE;
}

]=])
        string(REPLACE "${rcp_probe_anchor}" "${rcp_query}${rcp_probe_anchor}" patched_rcp_port "${rcp_port_text}")
        if(patched_rcp_port STREQUAL rcp_port_text)
            message(FATAL_ERROR "Review the Single USB RCP version query after the ESP-IDF change")
        endif()
        set(rcp_port_copy "${CMAKE_CURRENT_BINARY_DIR}/m2s_radio_spinel.cpp")
        file(WRITE "${rcp_port_copy}" "${patched_rcp_port}")
        list(FILTER m2s_spinel_sources EXCLUDE REGEX "(^|/)esp_openthread_radio_spinel[.]cpp$")
        list(APPEND m2s_spinel_sources "${rcp_port_copy}")
    endif()
    set_property(TARGET ${m2s_openthread_library} PROPERTY SOURCES "${m2s_spinel_sources}")
    target_include_directories(${m2s_openthread_library} PRIVATE "${m2s_spinel_dir}")
endif()

if(CONFIG_CUSTOM_REVOKED_DAC_CHAIN_CHECK)
    target_add_binary_data(controller.elf "main/revocation_set/revocation_set.json" TEXT)
endif()

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual;-Wno-error=unused-result" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)

# We don't need Thread Network Commissioning Driver
idf_build_set_property(COMPILE_OPTIONS "-D_NO_GENERIC_THREAD_NETWORK_COMMISSIONING_DRIVER_" APPEND)

# For RISCV chips, project_include.cmake sets -Wno-format, but does not clear various
# flags that depend on -Wformat
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wformat=0" APPEND)

# Apply a project-local source correction after GN generation, then rebuild CHIP.
# The shared SDK sources remain unchanged.
idf_component_get_property(m2s_chip_library chip COMPONENT_LIB)
add_custom_target(m2s_thread_init_fix
    COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/main/thread_init_fix/apply.py"
            "${CMAKE_BINARY_DIR}/esp-idf/chip"
            "${CMAKE_SOURCE_DIR}/main/thread_init_fix/ThreadStackManagerImpl.cpp"
            "${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip"
    COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/main/ble_connection_fix/apply.py"
            "${CMAKE_BINARY_DIR}/esp-idf/chip"
            "${CMAKE_SOURCE_DIR}/main/ble_connection_fix/BLEManagerImpl.cpp"
            "${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip"
    COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/main/pairing_timeout_fix/apply.py"
            "${CMAKE_BINARY_DIR}/esp-idf/chip"
            "${CMAKE_SOURCE_DIR}/main/pairing_timeout_fix/SetUpCodePairer.cpp"
            "${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip"
    COMMAND "${CMAKE_MAKE_PROGRAM}" esp32
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/esp-idf/chip"
    DEPENDS chip_gn
            "${CMAKE_SOURCE_DIR}/main/thread_init_fix/apply.py"
            "${CMAKE_SOURCE_DIR}/main/thread_init_fix/ThreadStackManagerImpl.cpp"
            "${CMAKE_SOURCE_DIR}/main/thread_init_fix/ThreadStackManagerImpl.json"
            "${CMAKE_SOURCE_DIR}/main/ble_connection_fix/apply.py"
            "${CMAKE_SOURCE_DIR}/main/ble_connection_fix/BLEManagerImpl.cpp"
            "${CMAKE_SOURCE_DIR}/main/ble_connection_fix/BLEManagerImpl.json"
            "${CMAKE_SOURCE_DIR}/main/pairing_timeout_fix/apply.py"
            "${CMAKE_SOURCE_DIR}/main/pairing_timeout_fix/SetUpCodePairer.cpp"
            "${CMAKE_SOURCE_DIR}/main/pairing_timeout_fix/SetUpCodePairer.json"
    VERBATIM
)
add_dependencies(${m2s_chip_library} m2s_thread_init_fix)

# Keep the shared OTA SDK intact; guard the project-local worker/approval correction.
if(CONFIG_ESP_MATTER_OTA_PROVIDER_ENABLED)
    file(SHA256 "${ESP_MATTER_PATH}/components/esp_matter_ota_provider/src/esp_matter_ota_provider.cpp" m2s_ota_hash)
    if(NOT m2s_ota_hash STREQUAL "4de0a2509bfa6bdc99f13f07ca6fab168f40cd53a95786140b2990574b577c54")
        message(FATAL_ERROR "Review local OTA provider correction after SDK change")
    endif()
    idf_component_get_property(m2s_ota_library esp_matter_ota_provider COMPONENT_LIB)
    get_target_property(m2s_ota_sources ${m2s_ota_library} SOURCES)
    set(m2s_ota_replacements 0)
    foreach(m2s_ota_source IN LISTS m2s_ota_sources)
        if(m2s_ota_source MATCHES "(^|/)esp_matter_ota_provider.cpp$")
            list(REMOVE_ITEM m2s_ota_sources "${m2s_ota_source}")
            list(APPEND m2s_ota_sources "${CMAKE_SOURCE_DIR}/main/ota_provider_fix/esp_matter_ota_provider.cpp")
            math(EXPR m2s_ota_replacements "${m2s_ota_replacements}+1")
        endif()
    endforeach()
    if(NOT m2s_ota_replacements EQUAL 1)
        message(FATAL_ERROR "Expected exactly one OTA provider source")
    endif()
    set_property(TARGET ${m2s_ota_library} PROPERTY SOURCES "${m2s_ota_sources}")
endif()

if(CONFIG_ESP_MATTER_OTA_PROVIDER_ENABLED)
    if(NOT EXISTS "${MATTER_SDK_PATH}/src/app/clusters/ota-provider/ota-provider-cluster.h" AND
       EXISTS "${MATTER_SDK_PATH}/src/app/clusters/ota-provider/OTAProviderCluster.h")
        idf_component_get_property(m2s_controller_library esp_matter_controller COMPONENT_LIB)
        target_include_directories(${m2s_controller_library} BEFORE PUBLIC "${CMAKE_SOURCE_DIR}/main/sdk_compat")
    endif()
endif()

if(CONFIG_ESP_MATTER_OTA_PROVIDER_ENABLED)
    file(SHA256 "${ESP_MATTER_PATH}/components/esp_matter_ota_provider/src/esp_matter_ota_candidates.cpp" m2s_candidates_hash)
    if(NOT m2s_candidates_hash STREQUAL "c5d7517f619f94b20b58be547b9eb716193b486fee8a2b0b5bca830e8b4195f9")
        message(FATAL_ERROR "Review local DCL file-size correction after SDK change")
    endif()
    get_target_property(m2s_ota_sources ${m2s_ota_library} SOURCES)
    set(m2s_ota_replacements 0)
    foreach(m2s_ota_source IN LISTS m2s_ota_sources)
        if(m2s_ota_source MATCHES "(^|/)esp_matter_ota_candidates.cpp$")
            list(REMOVE_ITEM m2s_ota_sources "${m2s_ota_source}")
            list(APPEND m2s_ota_sources "${CMAKE_SOURCE_DIR}/main/ota_provider_fix/esp_matter_ota_candidates.cpp")
            math(EXPR m2s_ota_replacements "${m2s_ota_replacements}+1")
        endif()
    endforeach()
    if(NOT m2s_ota_replacements EQUAL 1)
        message(FATAL_ERROR "Expected exactly one OTA candidates source")
    endif()
    set_property(TARGET ${m2s_ota_library} PROPERTY SOURCES "${m2s_ota_sources}")
endif()
