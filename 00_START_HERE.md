# Matter2Supla — Zacznij tutaj (`00_START_HERE.md`)

## 21.11.2026 — Audyt krzyżowy (Opus + Gemini), wydanie `0.3.3-gemini3` (Build `30303`) na dysku `M:` oraz natywna aplikacja Android `v1.1.0`

1. **Stan fizycznej bramki S3 + C6 (PO WGRANIU `0.3.3-gemini3`, Build `30303`):**
   - Na fizycznej bramce S3 (`COM16`, `192.168.1.191`) działa teraz **`0.3.3-gemini3` (Build `30303`)** z partycji **`ota_1`** (`0x8b0000`), a oryginalna wersja Codexa **`0.3.2-beta4` (Build `30203`)** pozostaje nienaruszona w **`ota_0`** (`0x4b0000`) jako gotowy obraz powrotny (`Rollback`). Na C6 działa RCP **ESP-IDF 6.1.1** (`051e4c4e-43cc05a`).
   - **Potwierdzone poprawki w `0.3.3-gemini2` (`30302`) i `0.3.3-gemini3` (`30303`):**
     - `CPU (2 rdzenie)`: średnie obciążenie spadło z **54,8%** (w `gemini1`, gdy tablice pod `portENTER_CRITICAL` leżały w zewnętrznym PSRAM) do **9,2%–19,0%** po pozostawieniu wszystkich struktur chronionych przez `portENTER_CRITICAL` w wewnętrznym SRAM (w PSRAM pozostaje `thread_peers[32]` 5,75 KiB oraz bufor skanu Wi-Fi 2 KiB chroniony przez `WifiLock`).
     - `Subskrypcje Matter`: oryginalny, stabilny cykl życia `BridgeSubscription` / `maintain_reporting()` z `0.3.2-beta4` (bez wyścigu `ShutdownSubscriptions()` i bez nadpisywania `mdns_hostname_set`), aktywne subskrypcje dla `TIMMERFLOTTE` (node 6, `c6_child`) oraz `GRILLPLATS` (node 10, `c6_router`).
     - `Kolejność inicjalizacji i sterowanie SUPLA Cloud`: `gateway::start()` jest wywoływane przed `quick_add_start()`, dzięki czemu kolejka komend przekaźnika i `init_crash_breadcrumb()` działają od pierwszej milisekundy startu mostka, a rozkazy z SUPLA Cloud (`from_supla = true`) nie są blokowane przez `relay_command_ready()` przed pierwszym raportem Matter.
     - `Synchronizacja i diagnostyka`: `s_cached_scan` w `wifi_manager.cpp` chroniony przez `WifiLock` (`s_wifi_mutex`); `last_uptime_before_reset_s` w `RTC_NOINIT_ATTR` nadpisywany wyłącznie przy faktycznych resetach awaryjnych (`PANIC`/`WDT`).
     - `Aplikacja Android`: pojedyncza sesja TLS (`Connection: keep-alive`) z serializacją zapytań przez `Mutex` (`requestMutex`), poprawną propagacją `CancellationException`, cyklem `LIVE` co 5 s, obsługą wielu przekaźników (`RelayControl`), strażnikiem `executor.isShutdown` w skanerze QR oraz wyrównanymi kolumnami `KeyVal` (`0.48f`/`0.52f`).
   - Oryginalny kod firmware na dysku `K:` pozostał w 100% nietknięty.

2. **Pliki do wgrania (`M:\Matter2Supla-Gemini-Version\do-wgrania\`):**
   - **Nowe wydanie `0.4.0-scale64` (`FW_BUILD 40001`, gotowe do wgrania — nie wgrane jeszcze na fizyczną S3):**
     - `Matter2Supla-0.4.0-scale64-S3-N16R8-OTA.m2s` — podpisany pakiet OTA ECDSA (skalowanie do 64 urządzeń / 256 pomiarów, pule CHIP w PSRAM, muteksy FreeRTOS zamiast spinlocków)
     - `Matter2Supla-0.4.0-scale64-S3-N16R8-USB-UPDATE-0x10000.bin` — obraz aplikacji S3 (`ota_0`/`ota_1`)
     - `Matter2Supla-0.4.0-scale64-S3-N16R8-FACTORY.bin` — pełny obraz fabryczny `0x0`
   - **Aktualnie wgrane i zweryfikowane sprzętowo na S3 (`0.3.3-gemini3`, `FW_BUILD 30303`):**
     - `Matter2Supla-0.3.3-gemini3-S3-N16R8-OTA.m2s`
     - `Matter2Supla-0.3.3-gemini3-S3-N16R8-USB-UPDATE-0x10000.bin`
     - `Matter2Supla-0.3.3-gemini3-S3-N16R8-FACTORY.bin`
   - `Matter2Supla-Finder-Android.apk` — natywna aplikacja Android `v1.1.0`
   - `SHA256SUMS.txt` — sumy kontrolne SHA-256
   - Kopia powrotna binarek Codexa `0.3.2-beta4` (`30203`) znajduje się w `M:\Matter2Supla-Gemini-Version\do-wgrania\powrot-do-codex-beta4\`.

3. **Dokumenty szczegółowe:**
   - [README-GEMINI-VERSION.md](README-GEMINI-VERSION.md)
   - [AI_MEMORY.md](AI_MEMORY.md)
   - [CHANGELOG.md](CHANGELOG.md)
   - [TODO.md](TODO.md)
