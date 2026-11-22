/*
 * Wersja firmware Matter2Supla 0.3.4 (konserwatywna, 32 urządzenia).
 * Bazuje na sprawdzonej gałęzi 0.3.3-gemini3 + pełny pakiet poprawek z audytu
 * (odporność startu bez abort(), naprawa backupu NVS, prawdziwe ota_health(),
 * PBKDF2 + rosnąca blokada, WIFI_AUTH_WPA2_PSK, strumieniowe /api/devices + cJSON PSRAM,
 * 1 Hz pętla SUPLA, reset overflow, natychmiastowy MQTT wake, gaszenie WS2812).
 */

#pragma once

#define FW_VERSION "0.3.4"
#define FW_BUILD 30400
