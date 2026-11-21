#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Trwale Wi-Fi dla bramki (panel WWW bez konsoli):
 *  - dane sieci w NVS ("wifi_cfg": ssid/pass),
 *  - start: z danymi -> STACJA (auto-connect); bez danych -> wlasny AP konfiguracyjny,
 *  - po nieudanych probach polaczenia -> dodatkowo wlasny AP (tryb konfiguracji),
 *  - skan sieci i zapominanie sieci dla panelu.
 */

esp_err_t wifi_manager_start(void);

/* Skan sieci WiFi -> JSON do bufora ({"networks":[{"ssid":..,"rssi":..,"auth":"open"|"wpa"}]}). */
esp_err_t wifi_manager_scan(char *json_out, size_t out_size);

/* Zapisuje dane sieci w NVS i laczy; zwraca ESP_OK gdy zlecenie przyjete. */
esp_err_t wifi_manager_connect(const char *ssid, const char *pass);

/* Usuwa dane sieci z NVS (wymaga restartu, aby wejsc w tryb AP). */
esp_err_t wifi_manager_forget(void);
esp_err_t wifi_manager_last_error(void);

typedef struct {
    uint32_t disconnects, ip_losses, last_disconnect_reason;
    uint32_t reconnects, recoveries, gateway_misses;
    bool gateway_probe_active, gateway_seen_reply;
} wifi_health_t;

void wifi_manager_health(wifi_health_t *out);
bool wifi_manager_in_ap_mode(void);
const char *wifi_manager_ap_ssid(void);

#ifdef __cplusplus
}
#endif
