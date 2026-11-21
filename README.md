# Matter2Supla — Gemini Version (`0.3.3-gemini1`)

Mostek **Matter over Thread → SUPLA Cloud + MQTT (Home Assistant Discovery)** dla tandemu **ESP32-S3 N16R8 (16 MB Flash, 8 MB Octal PSRAM) + ESP32-C6 RCP (UART1 2 Mb/s)** wraz z natywną aplikacją **Android (`Matter2Supla Finder v1.1.0`)**.

Wydanie `0.3.3-gemini1` (Build `30301`) bazuje na kodzie `0.3.2-beta4` i wdraża pełny zestaw poprawek stabilności po audycie krzyżowym (Claude Opus + Gemini) oraz 8 dodatkowych usprawnień pamięci, sieci i diagnostyki.

---

## Najważniejsze zmiany w `0.3.3-gemini1`

1. **Stabilność subskrypcji i odczytów Matter (`supla_bridge.cpp`):**
   - Ograniczenie wykładniczego backoffu `DefaultResubscribePolicy` do maks. **3 prób** (eliminacja 30–92-minutowego zamrożenia subskrypcji po zakłóceniach RF/CCA).
   - **Zombie Subscription Watchdog (180 s)** automatycznie resetujący zawieszoną subskrypcję po utracie świeżości.
   - Interwały subskrypcji `(min = 1 s, max = 300 s)` oraz dopasowanie ścieżek subskrypcji w `measurement_paths()` do zmapowanych ról urządzenia.
   - Niezależne uruchamianie zadania `poll_task` przed sprawdzeniem konfiguracji SUPLA Cloud.
   - Asynchroniczne przekazywanie komend przekaźnika z `supla_task` przez `gateway::relay_enqueue()` (brak blokowania `SuplaDevice.iterate()` na `ScopedChipStackLock(portMAX_DELAY)`).
2. **Pamięć RAM / PSRAM i zasoby sieciowe:**
   - Przeniesienie dużych tablic statycznych (`device_telemetry` oraz `gateway_mqtt`) do **PSRAM (`EXT_RAM_BSS_ATTR`)** — odzyskanie ok. **+28 KiB wewnętrznego DRAM** (potwierdzone sprzętowo: minimum wolnego wewnętrznego DRAM wzrosło z `53,1 KiB` do `67,9 KiB`).
   - Zwiększenie puli gniazd `CONFIG_LWIP_MAX_SOCKETS` z `10` do `16`.
   - Buforowanie wyników skanu Wi-Fi w PSRAM (15 s) z 250 ms timeoutem muteksu.
   - Odchudzenie pętli `mqtt_tick()` przez szybki hash `compute_discovery_hash()` bez budowania drzew `cJSON` co 2 s.
3. **Bezpieczeństwo, OTA i diagnostyka:**
   - Odporna bramka zatwierdzania OTA `ota_health()` (90 s z aktywnym C6 lub 180 s stabilnej pracy S3 bez paniki).
   - **Crash & Reset Tracker (`crash_guard`)** w pamięci `RTC_NOINIT_ATTR` (`boot_count`, `panic_count`, `last_abnormal_reason`, `last_uptime_before_reset_s`).
   - **Watchdog radia C6 / CCA** (`consecutive_cca_blocked_windows`, `consecutive_probe_failures`, `watchdog_alert`).
   - Automatyczna reklama **mDNS** (`https://matter2supla.local/`, usługa `_matter2supla._tcp` na porcie 443).
   - Rate-limiting logowania do panelu (`429 Too Many Requests` na 3 s po 5 błędnych próbach).
4. **Natywna aplikacja Android (`android-app/` — `Matter2Supla Finder v1.1.0`):**
   - Automatyczne wyszukiwanie bramki w sieci LAN (zapisane IP → tryb AP `192.168.4.1` → mDNS `_matter2supla._tcp` → szybki skan `/24` na porcie 443 z weryfikacją certyfikatu i `/api/status`).
   - Natywne logowanie z przypięciem odcisku SHA-256 certyfikatu.
   - Odświeżanie danych **w locie co 2,5 s (`LIVE`)** w 4 zakładkach: **Pulpit**, **Urządzenia** (T/H, OnOff + sterowanie Włącz/Wyłącz, V/A/W/kWh, bateria, Thread RSSI), **Dodaj** (**wbudowany czytnik kodów QR Matter przez aparat telefonu** — CameraX + ZXing offline) oraz **Diagnostyka** (5 pełnych kart).

---

## Dokumentacja
- [README-GEMINI-VERSION.md](README-GEMINI-VERSION.md) — szczegółowy opis techniczny wszystkich zmian
- [CHANGELOG.md](CHANGELOG.md) — historia zmian
- [TODO.md](TODO.md) — lista zadań i procedura powrotu (Rollback)
- [00_START_HERE.md](00_START_HERE.md) — podsumowanie startowe i wyniki weryfikacji sprzętowej
