# Pamięć projektu (AI_MEMORY) — Stan na 21.11.2026

## 1. Stan fizycznej bramki (21.11.2026 — `0.3.3-gemini1` WGRANE i zweryfikowane na S3)
- **ESP32-S3 N16R8 (`COM16`):** działa na **`0.3.3-gemini1` (Build `30301`)** w partycji **`ota_1`** (`VALID` po zatwierdzeniu przez `ota_health()`), z zachowanym nienaruszonym obrazem Codexa **`0.3.2-beta4` (`30203`)** w **`ota_0`** jako wersją awaryjną (`Rollback`).
- **ESP32-C6 DevKitM 4 MB:** działa na RCP **ESP-IDF 6.1.1** (`051e4c4e-43cc05a`).
- **Weryfikacja sprzętowa po wgraniu (`M:\Matter2Supla-Gemini-Version\backups\before-033gemini1-serial-20261121-110249Z\hardware-033gemini1-verification.json`):**
  - Tablica partycji, NVS, PHY i slot `ota_0` (`0.3.2-beta4`) zweryfikowane bajt w bajt przed i po zapisie.
  - SUPLA: `connected`, kod `17` (w 8,59 s od rozruchu).
  - mDNS: `https://matter2supla.local/` (`_matter2supla._tcp:443`) rozgłoszone w 7,86 s.
  - Subskrypcje Matter (`min=1s, max=300s`): `GRILLPLATS` (node 10, 5 pomiarów) zestawiona w 18,56 s; `TIMMERFLOTTE` (node 6, 3 pomiary) aktywna przy kontroli w 117 s.
  - Radio S3–C6: `connected: true`, `probe_failures: 0`, `radio_cca_total: 0`, `radio_rx_errors_total: 0`, `watchdog_alert: false`.
  - Pamięć RAM: `internal_free = 72 487 B` (70,8 KiB), `internal_minimum = 69 519 B` (67,9 KiB — wzrost minimum o **+14,8 KiB** względem 53,1 KiB w `beta2`/`beta4`), `psram_free = 7 894 324 B` (7,53 MiB).
  - `crash_guard`: `boot_count = 1`, `panic_count = 0`.
- **Kod firmware na dysku `K:`:** w 100% nienaruszony.

## 2. Wnioski z audytu krzyżowego (Gemini + Claude Opus)
Pełny raport weryfikacyjny znajduje się w `M:\Matter2Supla-Gemini-Version\README-GEMINI-VERSION.md`. Najważniejsze potwierdzone ustalenia dla kodu `0.3.2-beta4`:
1. **Backoff Fibonacciego w `DefaultResubscribePolicy` do 92 minut (`Opus N1`):** W `supla_bridge.cpp:694` delegacja do domyślnej polityki SDK (`CHIP_RESUBSCRIBE_MAX_RETRY_WAIT_INTERVAL_MS = 5538000`) przy utrzymanym `state.owned = true` powodowała, że po przejściowym zakłóceniu RF/CCA subskrypcja mogła czekać 30–92 min na kolejną próbę, a `maintain_reporting()` nigdy nie tworzyła świeżej subskrypcji.
2. **Zależność `poll_task` od konfiguracji SUPLA (`Opus N2`):** `poll_task` był tworzony na końcu `supla_bridge_start()` tylko po pomyślnej walidacji konfiguracji SUPLA i działał wyłącznie przy `s_active == true`.
3. **Blokujący `ScopedChipStackLock(portMAX_DELAY)` w `supla_task` (`supla_bridge.cpp:181`):** Sterowanie przekaźnikiem z chmury SUPLA brało blokadę CHIP bezpośrednio w wątku `supla_task`, mogąc zamrozić `SuplaDevice.iterate()` i `drain_updates()`.
4. **Zależność zatwierdzenia OTA S3 od stanu C6 w `ota_health()` (`Opus N4`):** Wymóg odpowiedzi Spinel z C6 w oknie 120–240 s powodował wycofanie (`ABORTED`) poprawnego obrazu S3, jeśli zewnętrzny układ C6 był akurat w stanie blokady.
5. **Presja na wewnętrzny DRAM (~54–66 KiB wolnego) i ciasny limit `CONFIG_LWIP_MAX_SOCKETS=10`:** Duże tablice statyczne w `device_telemetry.cpp` (~14 KiB) i `gateway_mqtt.cpp` (~14 KiB) zajmowały wewnętrzny DRAM mimo 7,5 MiB wolnego PSRAM.

## 3. Przygotowane wydanie `0.3.3-gemini1` (Build `30301`) na dysku `M:`
- Lokalizacja: `M:\Matter2Supla-Gemini-Version`
- Zawiera wszystkie poprawki z audytu (Opus N1/N2/N4, P2/P3) oraz 8 dodatkowych usprawnień (PSRAM `EXT_RAM_BSS_ATTR` +28 KiB DRAM, RTC Crash Guard, mDNS `matter2supla.local` / `_matter2supla._tcp`, szybki hash `mqtt_tick`, profilowane ścieżki subskrypcji `measurement_paths`, 15 s cache skanu Wi-Fi, rate-limiting logowania 429, Watchdog C6/CCA).
- Skompilowane na ESP-IDF 6.0.2, podpisane kluczem ECDSA projektu (`.m2s`). Gotowe pliki w `M:\Matter2Supla-Gemini-Version\do-wgrania\`.
- Oryginalne binarki powrotne Codexa (`0.3.2-beta4` / `30203`) są zabezpieczone w `M:\Matter2Supla-Gemini-Version\do-wgrania\powrot-do-codex-beta4\` oraz w `K:\AI PROJEKTY\matter2supla\beta\0.3.2-beta4-dev\build-artifacts\s3\`.

## 4. Natywna aplikacja Android `Matter2Supla Finder v1.1.0`
- Lokalizacja źródeł: `K:\AI PROJEKTY\matter2supla\android-app` oraz kopia w `M:\Matter2Supla-Gemini-Version\android-app`.
- Gotowy plik APK: `K:\AI PROJEKTY\matter2supla\android-app\Matter2Supla-Finder-debug.apk` oraz `M:\Matter2Supla-Gemini-Version\do-wgrania\Matter2Supla-Finder-Android.apk`.
- Funkcje: automatyczne wykrywanie bramki w LAN (zapisane IP → `192.168.4.1` → mDNS → równoległy skan `/24` na porcie 443 z weryfikacją certyfikatu i `/api/status`), natywne logowanie z przypięciem SHA-256 certyfikatu, odświeżanie danych **w locie co 2,5 s (`LIVE`)**, pełne odczyty urządzeń `beta4`/`gemini1` (T/H, OnOff + sterowanie Włącz/Wyłącz, V/A/W/kWh, bateria, Thread RSSI), wbudowany skaner kodów QR Matter przez aparat (CameraX + ZXing offline) oraz 5-kartowa Diagnostyka z poprawionym układem kolumn.
