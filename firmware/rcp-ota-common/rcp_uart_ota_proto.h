#pragma once
#include <stdint.h>

// Shared framed UART1 2 Mbps OTA wire protocol between ESP32-S3 host and ESP32-C6 RCP.
// Used over the existing single USB-C <-> USB-C cable (S3 TX19/RX20 <-> C6 RX12/TX13).

#define M2S_RCP_OTA_MAGIC0 0x4Du /* 'M' */
#define M2S_RCP_OTA_MAGIC1 0x32u /* '2' */
#define M2S_RCP_OTA_MAGIC2 0x53u /* 'S' */
#define M2S_RCP_OTA_MAGIC3 0x4Fu /* 'O' */

#define M2S_RCP_OTA_MAX_CHUNK 1024u
#define M2S_RCP_OTA_MAX_IMAGE (580u * 1024u)
#define M2S_RCP_OTA_HEADER_BYTES 12u
#define M2S_RCP_OTA_CRC_BYTES 4u

typedef enum {
    RCP_PKT_KNOCK  = 0x01,
    RCP_PKT_READY  = 0x02,
    RCP_PKT_BEGIN  = 0x03,
    RCP_PKT_CHUNK  = 0x04,
    RCP_PKT_COMMIT = 0x05,
    RCP_PKT_ACK    = 0x06,
    RCP_PKT_NACK   = 0x07,
} rcp_pkt_type_t;

typedef enum {
    RCP_OTA_ERR_OK = 0,
    RCP_OTA_ERR_CRC = 1,
    RCP_OTA_ERR_STATE = 2,
    RCP_OTA_ERR_SIZE = 3,
    RCP_OTA_ERR_SEQ = 4,
    RCP_OTA_ERR_FLASH = 5,
    RCP_OTA_ERR_HASH = 6,
    RCP_OTA_ERR_VERIFY = 7,
    RCP_OTA_ERR_DOWNGRADE = 8,
} rcp_ota_err_t;

#pragma pack(push, 1)
typedef struct {
    uint8_t magic[4];
    uint8_t type;
    uint8_t reserved;
    uint16_t len;
    uint32_t seq;
} rcp_pkt_header_t;

typedef struct {
    uint32_t current_build;
    uint8_t running_slot;
    uint8_t reserved[3];
} rcp_ready_payload_t;

typedef struct {
    uint32_t image_bytes;
    uint32_t build;
    uint8_t sha256[32];
} rcp_begin_payload_t;
#pragma pack(pop)
