#pragma once

/*
 * supla_bridge.h - warstwa Supla bramki Matter.
 *
 * Mostek:
 *   - laczy sie z chmura Supla (supla-device, TLS :2016) korzystajac
 *     z juz podniesionego Wi-Fi (wifi_manager) - nie zarzadza radiem,
 *   - dla kazdego urzadzenia Matter (z rejestru panelu) okresowo odczytuje:
 *       OnOff (0x0006), TemperatureMeasurement (0x0402),
 *       RelativeHumidityMeasurement (0x0405) i tworzy odpowiednie kanaly Supla,
 *   - polecenia z chmury Supla (wlacz/wylacz) wysyla do urzadzenia Matter
 *     jako komende OnOff (invoke przez CASE).
 *
 * Kanaly powstaja leniwie: dopiero po pierwszym udanym odczycie z urzadzenia
 * (dzieki temu czujnik nie dostaje kanalu przelacznika, a gniazdko kanalu
 * temperatury). Po dodaniu nowych kanalow protokol Supla jest restartowany,
 * aby chmura zobaczyla nowa liste kanalow.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Uruchamia warstwe Supla (wywolywane raz z quick_add_start(), po Wi-Fi).
 * Bez kompletnej konfiguracji (serwer + e-mail) nic nie robi - panel
 * pokaze wtedy stan "not_configured". */
void supla_bridge_start(void);
void supla_bridge_request_name_reload(void);

/* Czy warstwa Supla zostala uruchomiona (begin() poszedl). */
bool supla_bridge_is_started(void);

/* Stan dla panelu:
 *   "not_configured" - brak serwera/e-maila w konfiguracji,
 *   "starting"       - skonfigurowana, warstwa startuje,
 *   "connecting"     - laczy/rejestruje sie w chmurze,
 *   "connected"      - zarejestrowana i gotowa (status 17),
 *   "error"          - blad krytyczny (zle GUID/AuthKey, urzadzenie wylaczone...).
 */
const char *supla_bridge_state_name(void);

/* Ostatni kod statusu supla-device (STATUS_* z SuplaDevice.h). */
int supla_bridge_status_code(void);

/* Ostatni komunikat statusu (moze byc pusty). */
void supla_bridge_status_message_copy(char *out,size_t size);
esp_err_t supla_bridge_max_reserved_node(uint64_t *maximum);

#ifdef __cplusplus
}
#endif
