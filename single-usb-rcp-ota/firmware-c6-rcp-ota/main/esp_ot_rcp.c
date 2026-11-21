#include "driver/gpio.h"
#include "soc/usb_serial_jtag_struct.h"

#include <stdio.h>
#include <unistd.h>

#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_openthread.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "rcp_uart_ota.h"

#if CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE
#include "ot_examples_common.h"
#endif

#if !SOC_IEEE802154_SUPPORTED
#error "RCP is only supported for the SoCs which have IEEE 802.15.4 module"
#endif

#define TAG "ot_esp_rcp"

void app_main(void)
{
    // The native USB connector carries UART1 (GPIO12 RX / GPIO13 TX).
    // Release its USB PHY and D+ pull-up before UART driver assigns pins.
    USB_SERIAL_JTAG.conf0.dp_pullup = 0;
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
    ESP_ERROR_CHECK(gpio_reset_pin(GPIO_NUM_12));
    ESP_ERROR_CHECK(gpio_reset_pin(GPIO_NUM_13));

    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 2,
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    // Brief UART1 OTA boot window (exits immediately on normal Spinel 0x7E traffic).
    rcp_uart_ota_boot_window();

#if CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE
    ot_external_coexist_init();
#endif

    static esp_openthread_config_t config = {
        .netif_config = {0},
        .platform_config = {
            .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
            .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
            .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
        },
    };

    // Confirm PENDING_VERIFY OTA partition after 30 s of healthy RCP operation.
    rcp_uart_ota_start_confirm_task();

    ESP_ERROR_CHECK(esp_openthread_start(&config));
}
