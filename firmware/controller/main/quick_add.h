#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Uruchamia mini-panel WWW bramki (port 80):
 *   GET  /            - strona "Dodaj urzadzenie Matter" + status
 *   POST /api/add     - szybkie dodawanie po kodzie QR / 11-cyfrowym kodzie
 *   GET  /api/status  - JSON ze statusem Wi-Fi / Thread / parowania
 *
 * Wywolywane z app_main() po starcie Matter/OTBR.
 */
void quick_add_init_locks(void);
void quick_add_start(void);
bool quick_add_pairing_busy(void);
unsigned int quick_add_devices_count(void);

/* Rejestr urzadzen (dla warstwy Supla): kopiuje node_id do bufora,
 * zwraca liczbe skopiowanych pozycji. */
unsigned int quick_add_devices_copy(uint64_t *nodes, unsigned int max);

/* Nazwa urzadzenia z rejestru panelu ("" gdy nie znaleziono). */
bool quick_add_device_name_copy(uint64_t node,char *out,size_t size);
bool quick_add_device_exists(uint64_t node);
/* Device-reported metadata; never replaces a name edited in the panel. */
bool quick_add_device_metadata(uint64_t node, const char *product, const char *label,
                               uint16_t vid, uint16_t pid, bool has_vid, bool has_pid);

#ifdef __cplusplus
}
#endif
