#pragma once

/*
 * supla_config.h - trwala konfiguracja polaczenia z chmura Supla
 * (NVS, przestrzen "supla").
 *
 * Przechowuje:
 *  - adres serwera (np. "svr1.supla.org"),
 *  - e-mail konta Supla (wlasciciel urzadzenia),
 *  - GUID urzadzenia (16 B, w panelu jako 32 znaki hex),
 *  - AuthKey (16 B, w panelu jako 32 znaki hex).
 *
 * GUID/AuthKey sa generowane losowo przy pierwszym starcie (albo podawane
 * recznie - te same wartosci, ktore pokazuje kreator cloud.supla.org).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUPLA_CFG_ID_SIZE 16

typedef struct {
    bool configured;                    /* serwer + e-mail wypelnione */
    char server[64];
    char email[64];
    uint8_t guid[SUPLA_CFG_ID_SIZE];
    uint8_t authkey[SUPLA_CFG_ID_SIZE];
} supla_cfg_t;

/* Wczytuje konfiguracje z NVS (idempotentne). Przy braku tozsamosci
 * generuje losowe GUID/AuthKey i zapisuje je. */
esp_err_t supla_config_load(void);

/* Spójny snapshot; dane są publikowane dopiero po udanym zapisie NVS. */
esp_err_t supla_config_copy(supla_cfg_t *out);

/* Zapisuje konfiguracje. guid_hex/authkey_hex moga byc NULL/puste -
 * wtedy zostaja dotychczasowe wartosci. Waliduje format hex (32 znaki). */
esp_err_t supla_config_save(const char *server,
                            const char *email,
                            const char *guid_hex,
                            const char *authkey_hex);

/* Generuje nowe GUID + AuthKey (16 B kazde, esp_fill_random) i zapisuje.
 * Opcjonalne bufory wyjsciowe: 33 znaki (32 hex + NUL). */
esp_err_t supla_config_new_identity(char *guid_hex_out, char *authkey_hex_out);
/* Reads the SUPLA GUID stored in an already initialised NVS partition (by label),
 * from the current "cfg_v1" blob or the pre-0.3 legacy "guid" key. Used to check
 * that a backup really carries this gateway's identity before restoring it. */
esp_err_t supla_config_guid_from_partition(const char *partition_label, uint8_t guid[SUPLA_CFG_ID_SIZE]);

/* 16 B -> 32 znaki hex + NUL (out >= 33 B). */
void supla_config_bytes_to_hex(const uint8_t *in, char *out);

/* true gdy tekst ma dokladnie 32 znaki hex. */
bool supla_config_hex_is_valid(const char *hex);

/* 32 znaki hex -> 16 B; false przy bledzie. */
bool supla_config_hex_to_bytes(const char *hex, uint8_t *out);

#ifdef __cplusplus
}
#endif
