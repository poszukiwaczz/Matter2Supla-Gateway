#include "gateway_services.h"
#include "gateway_formats.h"
#include "quick_add.h"
#include "rcp_uart_ota_proto.h"

#include <driver/uart.h>
#include <esp_crc.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/pk.h>
#include <psa/crypto.h>
#include <cstring>
#include <memory>
#include <new>

namespace gateway {
namespace {

constexpr const char *kTag = "rcp_uart_ota";
constexpr uart_port_t kRcpUartPort = UART_NUM_1;
constexpr int kRcpTxPin = 19;
constexpr int kRcpRxPin = 20;
constexpr int kRcpBaud = 2000000;
constexpr uint32_t kStageOffset = 4096;
constexpr uint32_t kStageMagic = 0x4D325243u; // "M2RC"
constexpr uint32_t kStagePending = 0xFFFFFFFEu;
constexpr uint32_t kStageDone    = 0xFFFFFFFCu;
constexpr uint32_t kStageFailed  = 0xFFFFFFF0u;

extern const uint8_t key_start[] asm("_binary_update_public_key_pem_start");
extern const uint8_t key_end[] asm("_binary_update_public_key_pem_end");

struct RcpStageRecord {
    uint32_t magic;
    uint32_t image_bytes;
    uint32_t build;
    char version[32];
    uint8_t sha[32];
    uint32_t state;
};

char s_rcp_ota_status[96] = "idle";

const esp_partition_t *rcp_stage_partition()
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "rcp_fw");
}

bool receive_http(httpd_req_t *r, void *out, size_t n)
{
    auto *p = static_cast<char *>(out);
    unsigned timeout = 0;
    while (n) {
        int got = httpd_req_recv(r, p, n);
        if (got == HTTPD_SOCK_ERR_TIMEOUT && timeout++ < 5) continue;
        if (got <= 0) {
            ESP_LOGW(kTag, "HTTP receive failed: %d, %u bytes pending", got, (unsigned)n);
            return false;
        }
        p += got;
        n -= got;
    }
    return true;
}

bool verify_signature(const UpdateHeader &h)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    uint8_t hash[32];
    size_t written = 0;
    bool hashed = psa_crypto_init() == PSA_SUCCESS &&
                  psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t *>(&h),
                                   offsetof(UpdateHeader, signature_bytes), hash, sizeof(hash), &written) == PSA_SUCCESS &&
                  written == 32;
    bool ok = hashed &&
              mbedtls_pk_parse_public_key(&pk, key_start, key_end - key_start) == 0 &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), h.signature, h.signature_bytes) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

// Compute HDLC-Lite CRC-16/X-25 FCS (polynomial 0x8408, init 0xFFFF, final invert)
uint16_t hdlc_fcs(const uint8_t *data, size_t len)
{
    uint16_t fcs = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        fcs ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (fcs & 1u) fcs = (fcs >> 1) ^ 0x8408u;
            else fcs >>= 1;
        }
    }
    return static_cast<uint16_t>(fcs ^ 0xFFFFu);
}

void hdlc_push_escaped(uint8_t *dst, size_t &pos, uint8_t b)
{
    if (b == 0x7Eu || b == 0x7Du || b == 0x11u || b == 0x13u || b == 0xF8u) {
        dst[pos++] = 0x7Du;
        dst[pos++] = static_cast<uint8_t>(b ^ 0x20u);
    } else {
        dst[pos++] = b;
    }
}

// Sends standard OpenThread Spinel CMD_RESET(SPINEL_RESET_PLATFORM = 1) over HDLC UART1.
// Stock ESP-IDF ot_rcp handles this by calling otInstanceReset() -> otPlatReset() -> esp_restart().
void send_spinel_platform_reset()
{
    const uint8_t spinel[3] = {0x81u, 0x01u, 0x01u}; // Header(FLG=2,TID=1), CMD_RESET(1), SPINEL_RESET_PLATFORM(1)
    const uint16_t fcs = hdlc_fcs(spinel, sizeof(spinel));
    uint8_t out[20];
    size_t pos = 0;
    out[pos++] = 0x7Eu;
    for (uint8_t b : spinel) hdlc_push_escaped(out, pos, b);
    hdlc_push_escaped(out, pos, static_cast<uint8_t>(fcs & 0xFFu));
    hdlc_push_escaped(out, pos, static_cast<uint8_t>((fcs >> 8) & 0xFFu));
    out[pos++] = 0x7Eu;
    uart_write_bytes(kRcpUartPort, out, pos);
    uart_wait_tx_done(kRcpUartPort, pdMS_TO_TICKS(50));
}

void send_ota_packet(uint8_t type, uint32_t seq, const void *payload, uint16_t len)
{
    rcp_pkt_header_t hdr{};
    hdr.magic[0] = M2S_RCP_OTA_MAGIC0;
    hdr.magic[1] = M2S_RCP_OTA_MAGIC1;
    hdr.magic[2] = M2S_RCP_OTA_MAGIC2;
    hdr.magic[3] = M2S_RCP_OTA_MAGIC3;
    hdr.type = type;
    hdr.reserved = 0;
    hdr.len = len;
    hdr.seq = seq;

    uint32_t crc = esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&hdr), sizeof(hdr));
    if (len > 0 && payload != nullptr) {
        crc = esp_crc32_le(crc, static_cast<const uint8_t *>(payload), len);
    }

    uart_write_bytes(kRcpUartPort, &hdr, sizeof(hdr));
    if (len > 0 && payload != nullptr) {
        uart_write_bytes(kRcpUartPort, payload, len);
    }
    uart_write_bytes(kRcpUartPort, &crc, sizeof(crc));
    uart_wait_tx_done(kRcpUartPort, pdMS_TO_TICKS(100));
}

bool uart_read_exact(uint8_t *dst, size_t n, int64_t deadline_us)
{
    size_t got = 0;
    while (got < n) {
        int64_t rem_us = deadline_us - esp_timer_get_time();
        if (rem_us <= 0) return false;
        TickType_t wait_ticks = pdMS_TO_TICKS(static_cast<uint32_t>(rem_us / 1000));
        if (wait_ticks == 0) wait_ticks = 1;
        if (wait_ticks > pdMS_TO_TICKS(40)) wait_ticks = pdMS_TO_TICKS(40);
        int r = uart_read_bytes(kRcpUartPort, dst + got, n - got, wait_ticks);
        if (r > 0) got += static_cast<size_t>(r);
    }
    return true;
}

bool wait_ota_packet(rcp_pkt_header_t &hdr_out, uint8_t *payload_out, size_t max_payload, int timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000LL;
    static constexpr uint8_t kMagic[4] = {
        M2S_RCP_OTA_MAGIC0, M2S_RCP_OTA_MAGIC1, M2S_RCP_OTA_MAGIC2, M2S_RCP_OTA_MAGIC3
    };
    uint8_t magic_idx = 0;

    while (esp_timer_get_time() < deadline_us) {
        uint8_t b = 0;
        if (!uart_read_exact(&b, 1, deadline_us)) return false;
        if (b == kMagic[magic_idx]) {
            magic_idx++;
            if (magic_idx == 4) {
                uint8_t rest[M2S_RCP_OTA_HEADER_BYTES - 4];
                if (!uart_read_exact(rest, sizeof(rest), deadline_us)) return false;
                memcpy(hdr_out.magic, kMagic, 4);
                memcpy(reinterpret_cast<uint8_t *>(&hdr_out) + 4, rest, sizeof(rest));
                if (hdr_out.len > max_payload) {
                    magic_idx = 0;
                    continue;
                }
                if (hdr_out.len > 0 && !uart_read_exact(payload_out, hdr_out.len, deadline_us)) return false;
                uint32_t rx_crc = 0;
                if (!uart_read_exact(reinterpret_cast<uint8_t *>(&rx_crc), sizeof(rx_crc), deadline_us)) return false;
                uint32_t calc = esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&hdr_out), sizeof(hdr_out));
                if (hdr_out.len > 0) calc = esp_crc32_le(calc, payload_out, hdr_out.len);
                if (calc == rx_crc) return true;
                magic_idx = 0;
            }
        } else {
            magic_idx = (b == kMagic[0]) ? 1 : 0;
        }
    }
    return false;
}

bool verify_staged_sha(const esp_partition_t *part, const RcpStageRecord &rec)
{
    auto buf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[4096]);
    if (!buf) return false;
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
    uint32_t offset = kStageOffset;
    uint32_t rem = rec.image_bytes;
    bool ok = true;
    while (rem > 0) {
        uint32_t n = rem > 4096 ? 4096 : rem;
        if (esp_partition_read(part, offset, buf.get(), n) != ESP_OK ||
            psa_hash_update(&sha, buf.get(), n) != PSA_SUCCESS) {
            ok = false;
            break;
        }
        offset += n;
        rem -= n;
    }
    uint8_t digest[32]{};
    size_t dlen = 0;
    if (ok && (psa_hash_finish(&sha, digest, sizeof(digest), &dlen) != PSA_SUCCESS || dlen != 32 || memcmp(digest, rec.sha, 32) != 0)) {
        ok = false;
    }
    psa_hash_abort(&sha);
    return ok;
}

void mark_stage_state(const esp_partition_t *part, uint32_t new_state)
{
    esp_partition_write(part, offsetof(RcpStageRecord, state), &new_state, sizeof(new_state));
}

} // namespace

const char *rcp_ota_status()
{
    return s_rcp_ota_status;
}

esp_err_t update_rcp(httpd_req_t *r)
{
    ESP_LOGI(kTag, "C6 RCP OTA upload request: %u bytes", (unsigned)r->content_len);
    const auto *part = rcp_stage_partition();
    if (!part || part->size < kStageOffset + kRcpAppLimit) {
        return reply(r, 409, "Brak partycji rcp_fw (600 KB) w tablicy partycji S3.");
    }
    if (mutation_busy()) {
        return reply(r, 409, "Bramka jest zajeta.");
    }
    struct Guard { ~Guard() { mutation_done(); } } guard;
    if (quick_add_pairing_busy()) {
        return reply(r, 409, "Poczekaj na zakonczenie parowania.");
    }

    UpdateHeader h{};
    if (r->content_len < sizeof(h) || r->content_len > kRcpAppLimit + sizeof(h) ||
        !receive_http(r, &h, sizeof(h)) || !rcp_update_header(h, r->content_len) || !verify_signature(h)) {
        ESP_LOGW(kTag, "Rejected C6 RCP OTA header/signature");
        return reply(r, 400, "Niepoprawny podpis, wersja, chip (C6) lub rozmiar pakietu RCP. Niczego nie zapisano.");
    }

    constexpr size_t kChunkBytes = 4096;
    auto buf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[kChunkBytes]);
    if (!buf) {
        return reply(r, 503, "Brak pamieci tymczasowej na bufor RCP OTA.");
    }

    const size_t erase_bytes = ((kStageOffset + h.image_bytes + 4095u) / 4096u) * 4096u;
    if (erase_bytes > part->size || esp_partition_erase_range(part, 0, erase_bytes) != ESP_OK) {
        return reply(r, 500, "Nie udalo sie wyczyscic partycji buforowej rcp_fw.");
    }

    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    bool ok = (psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&sha, PSA_ALG_SHA_256) == PSA_SUCCESS);
    uint32_t offset = kStageOffset;
    size_t remaining = h.image_bytes;
    while (remaining > 0 && ok) {
        size_t n = remaining > kChunkBytes ? kChunkBytes : remaining;
        if (!receive_http(r, buf.get(), n) ||
            esp_partition_write(part, offset, buf.get(), n) != ESP_OK ||
            psa_hash_update(&sha, buf.get(), n) != PSA_SUCCESS) {
            ok = false;
            break;
        }
        offset += n;
        remaining -= n;
    }

    uint8_t digest[32]{};
    size_t digest_len = 0;
    if (ok && (psa_hash_finish(&sha, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
               digest_len != 32 || memcmp(digest, h.sha, 32) != 0)) {
        ok = false;
    }
    psa_hash_abort(&sha);
    if (!ok) {
        esp_partition_erase_range(part, 0, 4096);
        return reply(r, 400, "Przerwano transfer lub suma SHA-256 obrazu C6 jest bledna.");
    }

    RcpStageRecord rec{};
    rec.magic = kStageMagic;
    rec.image_bytes = h.image_bytes;
    rec.build = h.build;
    memcpy(rec.version, h.version, sizeof(rec.version));
    rec.version[sizeof(rec.version) - 1] = 0;
    memcpy(rec.sha, h.sha, 32);
    rec.state = kStagePending;

    if (esp_partition_write(part, 0, &rec, sizeof(rec)) != ESP_OK) {
        return reply(r, 500, "Nie zapisano naglowka aktualizacji C6 w rcp_fw.");
    }

    ESP_LOGI(kTag, "Staged signed C6 RCP %s (build %u, %u B). Rebooting S3 to flash C6 over UART1.",
             rec.version, (unsigned)rec.build, (unsigned)rec.image_bytes);
    auto resp = reply(r, 200, "Podpis i obraz C6 poprawne. Restart S3 i zdalna aktualizacja radia C6 po kablu USB-C (UART1)...");
    restart_later();
    return resp;
}

void apply_pending_rcp_update()
{
    const auto *part = rcp_stage_partition();
    if (!part) return;

    RcpStageRecord rec{};
    if (esp_partition_read(part, 0, &rec, sizeof(rec)) != ESP_OK) return;
    if (rec.magic != kStageMagic) return;

    if (rec.state == kStageDone) {
        snprintf(s_rcp_ota_status, sizeof(s_rcp_ota_status), "done:%s", rec.version);
        return;
    }
    if (rec.state == kStageFailed) {
        snprintf(s_rcp_ota_status, sizeof(s_rcp_ota_status), "failed:%s", rec.version);
        return;
    }
    if (rec.state != kStagePending || rec.image_bytes == 0 || rec.image_bytes > kRcpAppLimit) {
        return;
    }

    ESP_LOGI(kTag, "Pending C6 RCP OTA found: %s (build %u, %u bytes)",
             rec.version, (unsigned)rec.build, (unsigned)rec.image_bytes);
    if (!verify_staged_sha(part, rec)) {
        ESP_LOGE(kTag, "Staged C6 image SHA-256 verification failed; aborting");
        mark_stage_state(part, kStageFailed);
        snprintf(s_rcp_ota_status, sizeof(s_rcp_ota_status), "failed_sha:%s", rec.version);
        return;
    }

    const uart_config_t cfg = {
        .baud_rate = kRcpBaud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(kRcpUartPort, 4096, 4096, 0, nullptr, 0) != ESP_OK) {
        mark_stage_state(part, kStageFailed);
        snprintf(s_rcp_ota_status, sizeof(s_rcp_ota_status), "failed_uart");
        return;
    }
    uart_param_config(kRcpUartPort, &cfg);
    uart_set_pin(kRcpUartPort, kRcpTxPin, kRcpRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush_input(kRcpUartPort);

    // Trigger C6 reboot via standard Spinel CMD_RESET(SPINEL_RESET_PLATFORM), then knock until C6 enters OTA boot window.
    bool ready = false;
    rcp_pkt_header_t rx_hdr{};
    uint8_t rx_payload[64]{};

    for (int attempt = 0; attempt < 2 && !ready; ++attempt) {
        ESP_LOGI(kTag, "Sending Spinel platform reset to C6 (attempt %d/2)...", attempt + 1);
        send_spinel_platform_reset();
        vTaskDelay(pdMS_TO_TICKS(120));
        uart_flush_input(kRcpUartPort);

        const int64_t knock_deadline = esp_timer_get_time() + 2500000LL;
        while (esp_timer_get_time() < knock_deadline) {
            send_ota_packet(RCP_PKT_KNOCK, 0, nullptr, 0);
            if (wait_ota_packet(rx_hdr, rx_payload, sizeof(rx_payload), 40) && rx_hdr.type == RCP_PKT_READY) {
                ready = true;
                break;
            }
        }
    }

    bool transferred = false;
    if (!ready) {
        ESP_LOGE(kTag, "C6 did not respond to UART1 OTA knock (is C6 flashed with 2xOTA RCP firmware?)");
        snprintf(s_rcp_ota_status, sizeof(s_rcp_ota_status), "failed_no_c6_ota_reply");
    } else {
        rcp_ready_payload_t c6_info{};
        if (rx_hdr.len >= sizeof(c6_info)) memcpy(&c6_info, rx_payload, sizeof(c6_info));
        ESP_LOGI(kTag, "C6 READY in OTA mode (current_build=%u, slot=ota_%u)",
                 (unsigned)c6_info.current_build, (unsigned)c6_info.running_slot);

        rcp_begin_payload_t begin_msg{};
        begin_msg.image_bytes = rec.image_bytes;
        begin_msg.build = rec.build;
        memcpy(begin_msg.sha256, rec.sha, 32);

        bool began = false;
        for (int tries = 0; tries < 5 && !began; ++tries) {
            send_ota_packet(RCP_PKT_BEGIN, 0, &begin_msg, sizeof(begin_msg));
            if (wait_ota_packet(rx_hdr, rx_payload, sizeof(rx_payload), 3000) &&
                rx_hdr.type == RCP_PKT_ACK && rx_hdr.seq == 0) {
                began = true;
            }
        }

        auto chunk_buf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[M2S_RCP_OTA_MAX_CHUNK]);
        if (began && chunk_buf) {
            uint32_t seq = 0;
            uint32_t offset = kStageOffset;
            uint32_t rem = rec.image_bytes;
            bool chunks_ok = true;

            while (rem > 0 && chunks_ok) {
                uint16_t n = rem > M2S_RCP_OTA_MAX_CHUNK ? M2S_RCP_OTA_MAX_CHUNK : static_cast<uint16_t>(rem);
                if (esp_partition_read(part, offset, chunk_buf.get(), n) != ESP_OK) {
                    chunks_ok = false;
                    break;
                }
                bool acked = false;
                for (int tries = 0; tries < 5 && !acked; ++tries) {
                    send_ota_packet(RCP_PKT_CHUNK, seq, chunk_buf.get(), n);
                    if (wait_ota_packet(rx_hdr, rx_payload, sizeof(rx_payload), 1500) &&
                        rx_hdr.type == RCP_PKT_ACK && rx_hdr.seq == seq) {
                        acked = true;
                    }
                }
                if (!acked) {
                    ESP_LOGE(kTag, "Chunk %u transfer failed after 5 retries", (unsigned)seq);
                    chunks_ok = false;
                    break;
                }
                offset += n;
                rem -= n;
                seq++;
            }

            if (chunks_ok) {
                for (int tries = 0; tries < 3 && !transferred; ++tries) {
                    send_ota_packet(RCP_PKT_COMMIT, seq, nullptr, 0);
                    if (wait_ota_packet(rx_hdr, rx_payload, sizeof(rx_payload), 5000) &&
                        rx_hdr.type == RCP_PKT_ACK && rx_hdr.seq == 0xFFFFFFFFu) {
                        transferred = true;
                    }
                }
            }
        }
    }

    uart_driver_delete(kRcpUartPort);

    if (transferred) {
        mark_stage_state(part, kStageDone);
        snprintf(s_rcp_ota_status, sizeof(s_rcp_ota_status), "done:%s", rec.version);
        ESP_LOGI(kTag, "C6 RCP OTA %s committed! Waiting 600ms for C6 reboot...", rec.version);
        vTaskDelay(pdMS_TO_TICKS(600));
    } else {
        mark_stage_state(part, kStageFailed);
        if (strcmp(s_rcp_ota_status, "idle") == 0) {
            snprintf(s_rcp_ota_status, sizeof(s_rcp_ota_status), "failed_transfer:%s", rec.version);
        }
        ESP_LOGE(kTag, "C6 RCP OTA failed (%s); continuing normal S3 boot", s_rcp_ota_status);
    }
}

} // namespace gateway
