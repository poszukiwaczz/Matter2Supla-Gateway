#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Brief boot window on UART1 (GPIO12 RX / GPIO13 TX @ 2 Mbps) BEFORE OpenThread starts.
// Exits immediately on Spinel 0x7E traffic (0 ms delay on normal boot), or enters
// CRC32/SHA-256 verified 2xOTA receiver when S3 sends RCP_PKT_KNOCK.
void rcp_uart_ota_boot_window(void);

// Starts background task that marks a PENDING_VERIFY OTA slot VALID after 30 s
// of crash-free operation (otherwise ESP-IDF bootloader automatically rolls back).
void rcp_uart_ota_start_confirm_task(void);

#ifdef __cplusplus
}
#endif
