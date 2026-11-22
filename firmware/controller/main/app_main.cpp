#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_rom_sys.h"
#include "soc/usb_serial_jtag_struct.h"

/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <cJSON.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_console.h>
#include <esp_debug_helpers.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_console.h>
#include <esp_matter_controller_utils.h>
#include <esp_matter_ota.h>
#if CONFIG_OPENTHREAD_BORDER_ROUTER
#include <esp_openthread_border_router.h>
#include <esp_openthread_lock.h>
#include <openthread/dataset.h>
#include <openthread/dataset_ftd.h>
#include <openthread/ip6.h>
#include <openthread/thread.h>
#include <esp_ot_config.h>
#include <esp_spiffs.h>
#if defined(CONFIG_AUTO_UPDATE_RCP)
#include <esp_ot_rcp_update.h>
#include <esp_rcp_update.h>
#include <platform/ThreadStackManager.h>
#endif
#include <platform/ESP32/OpenthreadLauncher.h>
#endif // CONFIG_OPENTHREAD_BORDER_ROUTER
#include <common_macros.h>

// --- lab: mini-panel WWW do szybkiego dodawania urzadzen Matter po kodzie QR ---
#include "quick_add.h"
#include "gateway_services.h"
#include "matter_ota.h"
#include "wifi_manager.h"

#include <platform/ConnectivityManager.h>
#include <app/server/Server.h>
#include <credentials/FabricTable.h>

#ifdef CONFIG_CUSTOM_REVOKED_DAC_CHAIN_CHECK
#include <esp_matter_da_revocation_delegate.h>
#include <revocation_set/json_set_da_revocation_delegate.h>
extern const uint8_t revocation_set_json_start[] asm("_binary_revocation_set_json_start");
extern const uint8_t revocation_set_json_end[] asm("_binary_revocation_set_json_end");
static chip::Credentials::json_set_da_revocation_delegate s_custom_delegate((const char *)revocation_set_json_start,
                                                                            (const char *)revocation_set_json_end);
#endif

static const char *TAG = "app_main";
uint16_t switch_endpoint_id = 0;

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::PublicEventTypes::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;
    case chip::DeviceLayer::DeviceEventType::kESPSystemEvent:
        if (event->Platform.ESPSystemEvent.Base == IP_EVENT &&
                event->Platform.ESPSystemEvent.Id == IP_EVENT_STA_GOT_IP) {
#if CONFIG_OPENTHREAD_BORDER_ROUTER
            static bool sThreadBRInitialized = false;
            if (!sThreadBRInitialized) {
                esp_openthread_set_backbone_netif(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
                esp_openthread_lock_acquire(portMAX_DELAY);
                esp_err_t br_err = esp_openthread_border_router_init();
                otInstance *instance = esp_openthread_get_instance();
                otError thread_err = OT_ERROR_INVALID_STATE;
                if (br_err == ESP_OK && instance != nullptr) {
                    thread_err = OT_ERROR_NONE;
                    // Generate unique credentials once; preserve the same network on reboot.
                    if (!otDatasetIsCommissioned(instance)) {
                        otOperationalDataset dataset = {};
                        thread_err = otDatasetCreateNewNetwork(instance, &dataset);
                        if (thread_err == OT_ERROR_NONE) thread_err = otDatasetSetActive(instance, &dataset);
                    }
                    if (thread_err == OT_ERROR_NONE) thread_err = otIp6SetEnabled(instance, true);
                    if (thread_err == OT_ERROR_NONE) thread_err = otThreadSetEnabled(instance, true);
                }
                esp_openthread_lock_release();
                sThreadBRInitialized = br_err == ESP_OK && thread_err == OT_ERROR_NONE;
                ESP_LOGI(TAG, "Thread startup: BR=%s, Thread=%d", esp_err_to_name(br_err), (int)thread_err);
            }
#endif
        }
        break;
    default:
        break;
    }
}

static int startup_trace_command(int, char **)
{
    // Physical UART diagnostic used after startup has stopped creating tasks.
    return esp_backtrace_print_all_tasks(16);
}

// Force onboard WS2812 RGB LED (GPIO48 on DevKitC-1 v1.1/YD-ESP32-S3, GPIO38 on v1.0)
// to RGB(0,0,0) on boot and hold the data line LOW so power-on glitches never leave it lit.
static void turn_off_ws2812_pin(gpio_num_t pin)
{
    rmt_channel_handle_t chan = nullptr;
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = pin;
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = 10000000; // 10 MHz -> 100 ns/tick
    tx_cfg.mem_block_symbols = 64;
    tx_cfg.trans_queue_depth = 1;
    if (rmt_new_tx_channel(&tx_cfg, &chan) == ESP_OK) {
        rmt_encoder_handle_t enc = nullptr;
        rmt_copy_encoder_config_t enc_cfg = {};
        if (rmt_new_copy_encoder(&enc_cfg, &enc) == ESP_OK) {
            if (rmt_enable(chan) == ESP_OK) {
                rmt_symbol_word_t zero_bits[24];
                for (auto &sym : zero_bits) {
                    sym.level0 = 1;
                    sym.duration0 = 4; // 400 ns HIGH (WS2812 '0' bit)
                    sym.level1 = 0;
                    sym.duration1 = 8; // 800 ns LOW
                }
                rmt_transmit_config_t xfer = {};
                xfer.loop_count = 0;
                if (rmt_transmit(chan, enc, zero_bits, sizeof(zero_bits), &xfer) == ESP_OK) {
                    rmt_tx_wait_all_done(chan, 50);
                }
                esp_rom_delay_us(100); // >50 us WS2812 latch reset
                rmt_disable(chan);
            }
            rmt_del_encoder(enc);
        }
        rmt_del_channel(chan);
    }
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
}

extern "C" void app_main()
{
    // Route cJSON allocations to PSRAM first (fallback to internal DRAM) so large
    // JSON payloads at 64 devices never starve Wi-Fi/TLS of internal SRAM.
    static cJSON_Hooks s_cjson_psram_hooks = {
        [](size_t sz) -> void * {
            return heap_caps_malloc_prefer(sz, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        },
        [](void *ptr) { heap_caps_free(ptr); },
    };
    cJSON_InitHooks(&s_cjson_psram_hooks);

    // Turn off onboard WS2812 RGB LED immediately so adapter power-on never leaves it latched ON.
    turn_off_ws2812_pin(GPIO_NUM_48);
    turn_off_ws2812_pin(GPIO_NUM_38);

    // The native USB connector carries UART. Release its PHY and D+ pull-up
    // before the UART driver assigns the pins through the GPIO matrix.
    USB_SERIAL_JTAG.conf0.dp_pullup = 0;
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
    ESP_ERROR_CHECK(gpio_reset_pin(GPIO_NUM_19));
    ESP_ERROR_CHECK(gpio_reset_pin(GPIO_NUM_20));

    esp_err_t err = ESP_OK;

    /* Initialize the ESP NVS layer */
    // A failed/corrupt staged restore must never block boot: the current NVS is
    // either untouched (bad staging copy) or retried a bounded number of times.
    const esp_err_t restore_err = gateway::apply_pending_restore();
    static RTC_NOINIT_ATTR uint32_t s_nvs_fallback_magic;
    constexpr uint32_t kNvsFallbackMagic = 0x4D324E56u; // "M2NV"
    const esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_OK) {
        s_nvs_fallback_magic = 0;
    } else if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Never erase NVS automatically (contains Matter fabric & SUPLA identity).
        // Try a one-shot fallback to another VALID app slot (or factory) before aborting.
        gateway::note_boot_fault("nvs_init", nvs_err);
        if (s_nvs_fallback_magic != kNvsFallbackMagic) {
            const auto *running = esp_ota_get_running_partition();
            const auto *target = esp_ota_get_next_update_partition(running);
            esp_ota_img_states_t state = ESP_OTA_IMG_INVALID;
            if (!target || esp_ota_get_state_partition(target, &state) != ESP_OK || state != ESP_OTA_IMG_VALID) {
                target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory");
            }
            if (target && target != running && esp_ota_set_boot_partition(target) == ESP_OK) {
                s_nvs_fallback_magic = kNvsFallbackMagic;
                ESP_LOGE(TAG, "NVS init failed (%s); switching once to %s", esp_err_to_name(nvs_err), target->label);
                esp_restart();
            }
        }
        ESP_ERROR_CHECK(nvs_err);
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }
#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
    esp_console_cmd_t trace_command = {};
    trace_command.command = "startup-trace";
    trace_command.help = "Print task backtraces after startup has stalled";
    trace_command.func = startup_trace_command;
    ESP_ERROR_CHECK(esp_console_cmd_register(&trace_command));
#if CONFIG_ESP_MATTER_CONTROLLER_ENABLE
    esp_matter::console::controller_register_commands();
#endif // CONFIG_ESP_MATTER_CONTROLLER_ENABLE
#ifdef CONFIG_OPENTHREAD_BORDER_ROUTER
    esp_matter::console::otcli_register_commands();
#endif // CONFIG_OPENTHREAD_BORDER_ROUTER
#endif // CONFIG_ENABLE_CHIP_SHELL
#ifdef CONFIG_OPENTHREAD_BORDER_ROUTER
#ifdef CONFIG_AUTO_UPDATE_RCP
    esp_vfs_spiffs_conf_t rcp_fw_conf = {
        .base_path = "/rcp_fw", .partition_label = "rcp_fw", .max_files = 10, .format_if_mount_failed = false
    };
    if (ESP_OK != esp_vfs_spiffs_register(&rcp_fw_conf)) {
        ESP_LOGE(TAG, "Failed to mount rcp firmware storage");
        return;
    }
    esp_rcp_update_config_t rcp_update_config = ESP_OPENTHREAD_RCP_UPDATE_CONFIG();
    esp_rcp_update_init(&rcp_update_config);
    esp_ot_register_rcp_handler();
#endif
    /* Set OpenThread platform config */
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif // CONFIG_OPENTHREAD_BORDER_ROUTER
    /* Matter start */
    ESP_LOGI(TAG, "Startup stage: Matter stack");
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

#if defined(CONFIG_OPENTHREAD_BORDER_ROUTER) && defined(CONFIG_AUTO_UPDATE_RCP)
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        using namespace chip::DeviceLayer;
        bool thread_was_enabled = ThreadStackMgr().IsThreadEnabled();
        if (thread_was_enabled) {
            ABORT_APP_ON_FAILURE(ThreadStackMgr().SetThreadEnabled(false) == CHIP_NO_ERROR,
                                 ESP_LOGE(TAG, "Failed to disable Thread before updating RCP"));
        }
        esp_ot_update_rcp_if_different();
        if (thread_was_enabled) {
            ABORT_APP_ON_FAILURE(ThreadStackMgr().SetThreadEnabled(true) == CHIP_NO_ERROR,
                                 ESP_LOGE(TAG, "Failed to enable Thread after updating RCP"));
        }
    }
#endif

#if CONFIG_ESP_MATTER_COMMISSIONER_ENABLE
    ESP_LOGI(TAG, "Startup stage: commissioner initialization");
    {
        // Release the CHIP lock before Wi-Fi emits events consumed by the CHIP task.
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        ESP_ERROR_CHECK(esp_matter::controller::matter_controller_client::get_instance().init(112233, 1, 5580));
#ifdef CONFIG_CUSTOM_REVOKED_DAC_CHAIN_CHECK
        chip::Credentials::set_custom_da_revocation_delegate(&s_custom_delegate);
#endif
        ESP_LOGI(TAG, "Startup stage: commissioner setup");
        ESP_ERROR_CHECK(esp_matter::controller::matter_controller_client::get_instance().setup_commissioner());
        matter_ota::init();
    }
#endif // CONFIG_ESP_MATTER_COMMISSIONER_ENABLE

    // One owner of Wi-Fi connect/retry/AP policy. Matter still receives IP events.
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        auto &connectivity=chip::DeviceLayer::ConnectivityMgr();
        ABORT_APP_ON_FAILURE(connectivity.SetWiFiStationMode(
            chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_ApplicationControlled)==CHIP_NO_ERROR,
            ESP_LOGE(TAG,"Failed to transfer station policy to wifi_manager"));
        // This ESP32 SDK has no Matter AP controller (SetWiFiAPMode is unsupported).
        // AP provisioning remains owned exclusively by wifi_manager.
    }

    // --- lab: trwale Wi-Fi (NVS + tryb konfiguracji AP) i mini-panel WWW ---
    ESP_LOGI(TAG, "Startup stage: Wi-Fi manager");
    const esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err != ESP_OK) gateway::note_boot_fault("wifi", wifi_err);
    ESP_LOGI(TAG, "Startup stage: gateway services & HTTPS panel");
    if (restore_err != ESP_OK) gateway::note_boot_fault("nvs_restore", restore_err);
    // Registry mutexes must exist before gateway services (MQTT) may read the registry.
    quick_add_init_locks();
    gateway::start();
    quick_add_start();
    ESP_LOGI(TAG, "Startup stage: ready");
}
