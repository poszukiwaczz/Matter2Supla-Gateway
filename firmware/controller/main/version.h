/*
 * Wersja firmware Matter2Supla — Gemini Version (r3).
 * Poprawki: WifiLock na s_cached_scan, wsparcie komend przekaźnika z SUPLA
 * przed pierwszym raportem telemetrii, kolejność startu gateway::start() przed
 * quick_add_start(), zachowanie last_uptime_before_reset_s tylko przy awarii.
 */

#pragma once

#define FW_VERSION "0.3.3-gemini3"
#define FW_BUILD 30303
