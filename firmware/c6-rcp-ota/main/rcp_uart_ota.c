#include "rcp_uart_ota.h"
#include "rcp_uart_ota_proto.h"
#include "rcp_ota_version.h"
#include "esp_ot_config.h"

#include <string.h>
#include <stdbool.h>
#include "driver/uart.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/link_raw.h"
#include <stdio.h>

#define RCP_OTA_UART_PORT      UART_NUM_1
#define RCP_OTA_BAUD           2000000
#define RCP_OTA_BOOT_WINDOW_MS 750
#define RCP_OTA_IO_TIMEOUT_MS  10000

static void send_packet(uint8_t type, uint32_t seq, const void *payload, uint16_t len)
{
    uint8_t frame[M2S_RCP_OTA_HEADER_BYTES + 64 + M2S_RCP_OTA_CRC_BYTES];
    if (len > 64) {
        return;
    }
    rcp_pkt_header_t hdr;
    hdr.magic[0] = M2S_RCP_OTA_MAGIC0;
    hdr.magic[1] = M2S_RCP_OTA_MAGIC1;
    hdr.magic[2] = M2S_RCP_OTA_MAGIC2;
    hdr.magic[3] = M2S_RCP_OTA_MAGIC3;
    hdr.type = type;
    hdr.reserved = 0;
    hdr.len = len;
    hdr.seq = seq;

    memcpy(frame, &hdr, sizeof(hdr));
    if (len > 0 && payload != NULL) {
        memcpy(frame + sizeof(hdr), payload, len);
    }
    const uint32_t crc = esp_rom_crc32_le(0, frame, sizeof(hdr) + len);
    memcpy(frame + sizeof(hdr) + len, &crc, sizeof(crc));
    uart_write_bytes(RCP_OTA_UART_PORT, frame, sizeof(hdr) + len + sizeof(crc));
    uart_wait_tx_done(RCP_OTA_UART_PORT, pdMS_TO_TICKS(50));
}

static bool read_exact(uint8_t *dst, size_t n, int64_t deadline_us)
{
    size_t got = 0;
    while (got < n) {
        int64_t rem_us = deadline_us - esp_timer_get_time();
        if (rem_us <= 0) {
            return false;
        }
        TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)(rem_us / 1000));
        if (wait_ticks == 0) {
            wait_ticks = 1;
        }
        if (wait_ticks > pdMS_TO_TICKS(50)) {
            wait_ticks = pdMS_TO_TICKS(50);
        }
        int r = uart_read_bytes(RCP_OTA_UART_PORT, dst + got, n - got, wait_ticks);
        if (r > 0) {
            got += (size_t)r;
        }
    }
    return true;
}

// Reads next valid framed packet. Returns 1 on valid packet, 0 on timeout, -1 on Spinel 0x7E seen (when exit_on_spinel=true).
static int read_packet(rcp_pkt_header_t *hdr_out, uint8_t *payload_out, size_t max_payload,
                       int timeout_ms, bool exit_on_spinel)
{
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    uint8_t magic_idx = 0;
    static const uint8_t kMagic[4] = {
        M2S_RCP_OTA_MAGIC0, M2S_RCP_OTA_MAGIC1, M2S_RCP_OTA_MAGIC2, M2S_RCP_OTA_MAGIC3
    };

    while (esp_timer_get_time() < deadline_us) {
        uint8_t b = 0;
        if (!read_exact(&b, 1, deadline_us)) {
            return 0;
        }
        if (exit_on_spinel && magic_idx == 0 && b == 0x7E) {
            return -1;
        }
        if (b == kMagic[magic_idx]) {
            magic_idx++;
            if (magic_idx == 4) {
                uint8_t rest_hdr[M2S_RCP_OTA_HEADER_BYTES - 4];
                if (!read_exact(rest_hdr, sizeof(rest_hdr), deadline_us)) {
                    return 0;
                }
                memcpy(hdr_out->magic, kMagic, 4);
                memcpy(((uint8_t *)hdr_out) + 4, rest_hdr, sizeof(rest_hdr));
                if (hdr_out->len > max_payload) {
                    magic_idx = 0;
                    continue;
                }
                if (hdr_out->len > 0) {
                    if (!read_exact(payload_out, hdr_out->len, deadline_us)) {
                        return 0;
                    }
                }
                uint32_t rx_crc = 0;
                if (!read_exact((uint8_t *)&rx_crc, sizeof(rx_crc), deadline_us)) {
                    return 0;
                }
                uint32_t calc = esp_rom_crc32_le(0, (const uint8_t *)hdr_out, sizeof(*hdr_out));
                if (hdr_out->len > 0) {
                    calc = esp_rom_crc32_le(calc, payload_out, hdr_out->len);
                }
                if (calc == rx_crc) {
                    return 1;
                }
                // Bad CRC: let host retry
                uint8_t err = RCP_OTA_ERR_CRC;
                send_packet(RCP_PKT_NACK, hdr_out->seq, &err, 1);
                magic_idx = 0;
            }
        } else {
            magic_idx = (b == kMagic[0]) ? 1 : 0;
        }
    }
    return 0;
}

static void run_ota_session(void)
{
    static uint8_t payload[M2S_RCP_OTA_MAX_CHUNK];
    rcp_pkt_header_t hdr;

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    rcp_ready_payload_t ready = {0};
    ready.current_build = RCP_OTA_BUILD;
    ready.running_slot = (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ? 1 : 0;
    send_packet(RCP_PKT_READY, 0, &ready, sizeof(ready));

    bool began = false;
    esp_ota_handle_t ota = 0;
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    uint32_t expected_bytes = 0;
    uint32_t received_bytes = 0;
    uint32_t expected_seq = 0;
    uint8_t expected_sha[32] = {0};

    while (true) {
        int res = read_packet(&hdr, payload, sizeof(payload), RCP_OTA_IO_TIMEOUT_MS, false);
        if (res <= 0) {
            break;
        }
        if (hdr.type == RCP_PKT_KNOCK) {
            if (!began) {
                send_packet(RCP_PKT_READY, 0, &ready, sizeof(ready));
            }
            continue;
        }
        if (hdr.type == RCP_PKT_BEGIN) {
            if (began) {
                esp_ota_abort(ota);
                psa_hash_abort(&sha);
                began = false;
            }
            if (hdr.len != sizeof(rcp_begin_payload_t) || target == NULL) {
                uint8_t err = RCP_OTA_ERR_STATE;
                send_packet(RCP_PKT_NACK, hdr.seq, &err, 1);
                continue;
            }
            rcp_begin_payload_t begin_msg;
            memcpy(&begin_msg, payload, sizeof(begin_msg));
            if (begin_msg.build <= RCP_OTA_BUILD) {
                uint8_t err = RCP_OTA_ERR_DOWNGRADE;
                send_packet(RCP_PKT_NACK, hdr.seq, &err, 1);
                continue;
            }
            if (begin_msg.image_bytes == 0 || begin_msg.image_bytes > M2S_RCP_OTA_MAX_IMAGE ||
                begin_msg.image_bytes > target->size) {
                uint8_t err = RCP_OTA_ERR_SIZE;
                send_packet(RCP_PKT_NACK, hdr.seq, &err, 1);
                continue;
            }
            if (psa_crypto_init() != PSA_SUCCESS ||
                psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS ||
                esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
                psa_hash_abort(&sha);
                uint8_t err = RCP_OTA_ERR_FLASH;
                send_packet(RCP_PKT_NACK, hdr.seq, &err, 1);
                continue;
            }
            expected_bytes = begin_msg.image_bytes;
            received_bytes = 0;
            expected_seq = 0;
            memcpy(expected_sha, begin_msg.sha256, 32);
            began = true;
            uint8_t ok = RCP_OTA_ERR_OK;
            send_packet(RCP_PKT_ACK, 0, &ok, 1);
            continue;
        }
        if (hdr.type == RCP_PKT_CHUNK) {
            if (!began || hdr.len == 0) {
                uint8_t err = RCP_OTA_ERR_STATE;
                send_packet(RCP_PKT_NACK, hdr.seq, &err, 1);
                continue;
            }
            if (expected_seq > 0 && hdr.seq == expected_seq - 1) {
                // Idempotent retry after dropped ACK
                uint8_t ok = RCP_OTA_ERR_OK;
                send_packet(RCP_PKT_ACK, hdr.seq, &ok, 1);
                continue;
            }
            if (hdr.seq != expected_seq) {
                uint8_t err = RCP_OTA_ERR_SEQ;
                send_packet(RCP_PKT_NACK, expected_seq, &err, 1);
                continue;
            }
            if (received_bytes + hdr.len > expected_bytes) {
                uint8_t err = RCP_OTA_ERR_SIZE;
                send_packet(RCP_PKT_NACK, hdr.seq, &err, 1);
                break;
            }
            if (esp_ota_write(ota, payload, hdr.len) != ESP_OK ||
                psa_hash_update(&sha, payload, hdr.len) != PSA_SUCCESS) {
                uint8_t err = RCP_OTA_ERR_FLASH;
                send_packet(RCP_PKT_NACK, hdr.seq, &err, 1);
                break;
            }
            received_bytes += hdr.len;
            expected_seq++;
            uint8_t ok = RCP_OTA_ERR_OK;
            send_packet(RCP_PKT_ACK, hdr.seq, &ok, 1);
            continue;
        }
        if (hdr.type == RCP_PKT_COMMIT) {
            if (!began || received_bytes != expected_bytes) {
                uint8_t err = RCP_OTA_ERR_SIZE;
                send_packet(RCP_PKT_NACK, hdr.seq, &err, 1);
                break;
            }
            uint8_t digest[32] = {0};
            size_t digest_len = 0;
            bool hash_ok = (psa_hash_finish(&sha, digest, sizeof(digest), &digest_len) == PSA_SUCCESS &&
                            digest_len == 32 && memcmp(digest, expected_sha, 32) == 0);
            psa_hash_abort(&sha);
            if (!hash_ok) {
                uint8_t err = RCP_OTA_ERR_HASH;
                send_packet(RCP_PKT_NACK, hdr.seq, &err, 1);
                break;
            }
            if (esp_ota_end(ota) != ESP_OK) {
                began = false;
                uint8_t err = RCP_OTA_ERR_VERIFY;
                send_packet(RCP_PKT_NACK, hdr.seq, &err, 1);
                break;
            }
            began = false;
            if (esp_ota_set_boot_partition(target) != ESP_OK) {
                uint8_t err = RCP_OTA_ERR_FLASH;
                send_packet(RCP_PKT_NACK, hdr.seq, &err, 1);
                break;
            }
            uint8_t ok = RCP_OTA_ERR_OK;
            send_packet(RCP_PKT_ACK, 0xFFFFFFFFu, &ok, 1);
            uart_wait_tx_done(RCP_OTA_UART_PORT, pdMS_TO_TICKS(100));
            vTaskDelay(pdMS_TO_TICKS(40));
            esp_restart();
        }
    }

    if (began) {
        esp_ota_abort(ota);
        psa_hash_abort(&sha);
    }
}

void rcp_uart_ota_boot_window(void)
{
    const uart_config_t cfg = {
        .baud_rate = RCP_OTA_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (uart_driver_install(RCP_OTA_UART_PORT, 4096, 2048, 0, NULL, 0) != ESP_OK) {
        return;
    }
    uart_param_config(RCP_OTA_UART_PORT, &cfg);
    uart_set_pin(RCP_OTA_UART_PORT, OPENTHREAD_RCP_UART_TX_PIN, OPENTHREAD_RCP_UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    rcp_pkt_header_t hdr;
    uint8_t dummy[32];
    int res = read_packet(&hdr, dummy, sizeof(dummy), RCP_OTA_BOOT_WINDOW_MS, true);
    if (res == 1 && hdr.type == RCP_PKT_KNOCK && hdr.len == 0) {
        run_ota_session();
    }

    uart_driver_delete(RCP_OTA_UART_PORT);
}

// A new RCP image is confirmed only after the S3 host has really talked Spinel to
// it: the host enables the raw link (SPINEL_PROP_PHY_ENABLED) when it brings Thread
// up. That state must hold for kConfirmHoldS consecutive seconds. A radio that
// never gets enabled within kConfirmDeadlineS (does not crash, but does not work)
// is rolled back to the previous slot.
#define RCP_OTA_CONFIRM_HOLD_S     30
#define RCP_OTA_CONFIRM_DEADLINE_S 600

static bool rcp_host_link_enabled(void)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!instance || !esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
        return false;
    }
    bool enabled = otLinkRawIsEnabled(instance);
    esp_openthread_lock_release();
    return enabled;
}

const char *__real_otGetVersionString(void);

const char *__wrap_otGetVersionString(void)
{
    static char version[128];
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    char slot = '?';
    char health = 'u';
    if (running) {
        if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) slot = '0';
        if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) slot = '1';
        if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
            if (state == ESP_OTA_IMG_VALID) health = 'v';
            if (state == ESP_OTA_IMG_PENDING_VERIFY) health = 'p';
        }
    }
    snprintf(version, sizeof(version), "m2s-rcp:%lu:%c:%c %s",
             (unsigned long)RCP_OTA_BUILD, slot, health, __real_otGetVersionString());
    return version;
}

static void rcp_ota_confirm_task(void *arg)
{
    (void)arg;
    int healthy_s = 0;
    for (int elapsed_s = 0; elapsed_s < RCP_OTA_CONFIRM_DEADLINE_S; ++elapsed_s) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        healthy_s = rcp_host_link_enabled() ? healthy_s + 1 : 0;
        if (healthy_s >= RCP_OTA_CONFIRM_HOLD_S) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                vTaskDelete(NULL);
                return;
            }
            healthy_s = 0;
        }
    }
    // No working Spinel session with the host: return to the previous RCP image.
    esp_ota_mark_app_invalid_rollback_and_reboot();
    vTaskDelete(NULL);
}

void rcp_uart_ota_start_confirm_task(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_VALID;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        xTaskCreate(rcp_ota_confirm_task, "rcp_ota_ok", 3072, NULL, 2, NULL);
    }
}
