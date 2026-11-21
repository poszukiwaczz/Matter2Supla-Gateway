/*
 * Wersja firmware Matter2Supla — Gemini Version (r2).
 * Poprawki: usunięcie konfliktu mDNS z CHIP, tablice spod spinlocków
 * (portENTER_CRITICAL) w wewnętrznym SRAM (w PSRAM wyłącznie thread_peers 5,75 KiB).
 */

#pragma once

#define FW_VERSION "0.3.3-gemini2"
#define FW_BUILD 30302
