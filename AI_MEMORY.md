# Pamięć projektu (AI_MEMORY) — Stan na 21.11.2026 (18:05)

## 1. Stan fizycznej bramki (ESP32-S3 + ESP32-C6)
- **ESP32-S3 N16R8 (`COM16`, `192.168.1.191`):** działa stabilnie na **`0.3.3-gemini3` (Build `30303`)** w partycji **`ota_1`** (`0x8b0000`, `VALID`), z zachowanym bajt w bajt obrazem Codexa **`0.3.2-beta4` (`30203`)** w **`ota_0`** (`0x4b0000`) jako wersją powrotną (`Rollback`).
- **ESP32-C6 DevKitM 4 MB:** działa na RCP **ESP-IDF 6.1.1** (`051e4c4e-43cc05a`), transport `UART1 2 Mb/s` przez natywne piny USB (`S3 TX19/RX20` ↔ `C6 RX12/TX13`).
- **Potwierdzony wynik ciągłej pracy `0.3.3-gemini3` (`uptime = 7898 s` / 2 godz. 11 min):**
  - **Wycieki RAM:** `0 B` (`internal_free`: `64 375 B` przy 21 min → `64 383 B` przy 2 h 11 min; `internal_largest`: `31 744 B`; `psram_free`: `7,85 MB`).
  - **Wi-Fi Health:** `disconnects: 0`, `ip_losses: 0`, `recoveries: 0`, `gateway_misses: 0`.
  - **Radio S3–C6:** `connected: true`, `radio_tx_total: 3504`, `radio_cca_total: 1`, `probe_failures: 0`, `rx_errors: 0`, `watchdog_alert: false`.
  - **Subskrypcje Matter:** `TIMMERFLOTTE` (node 6, `c6_child`): `active: true`, **`27` raportów** (co ~5 min bez przerw); `GRILLPLATS` (node 10, `c6_router`): `active: true`, **`2140` raportów**.
  - **Crash Guard:** `boot_count: 1`, `panic_count: 0`.
  - Dowód: `M:\Matter2Supla-Gemini-Version\backups\before-033gemini3-serial-20261121-142003Z\hardware-033gemini3-verification.json`.
- **Kod oryginalny na dysku `K:` (`k:\AI PROJEKTY\matter2supla`):** w 100% nienaruszony; `C:\esp\lab-work\controller-esp32s3-next03` każdorazowo przywracany do `0.3.2-beta4`.

## 2. Zweryfikowane wnioski z audytu porównawczego (`0.3.2-beta4` vs `0.3.3` vs `0.4.0-scale64`)
1. **Prawdziwy mechanizm `linker.lf` i przyczyna 54,85% CPU w `gemini1`:**
   - W oryginalnym `linker.lf` istnieje wpis `archive: libmain.a -> * (extram_bss)`. Wszystkie zerowane tablice `.bss` w `main/` (`contacts`, `desired`, `running`, `tracked`, `s_log_buf`, `s_reporting`, `s_devices`) **od zawsze leżą w PSRAM** (`0x3c33....`–`0x3c34....`).
   - Jedynie tablice z niezerowym inicjalizatorem (`Reading readings[]` z `channel = -1` oraz `ReportingLive s_reporting_live[]` z `maximum = 600`) domyślnie trafiają do wewnętrznego `.data` (`0x3fc9....`).
   - Główną przyczyną 54,85% CPU i wywieszania WebUI w `gemini1` była **burza handshake'ów TLS ECDSA P-256 z pierwszej wersji aplikacji Android** (`Connection: close` + `conn.disconnect()` co 2,5 s na 3 żądania = >1 handshake TLS/s na jednowątkowym `esp_https_server`), a dodatkowo wyścig `ShutdownSubscriptions()` i konflikt `mdns_hostname_set("matter2supla")` z responderem mDNS Matter.
   - Zabezpieczenie tablic w PSRAM zwykłym muteksem FreeRTOS (`StaticSemaphore_t` z `DRAM_ATTR` w `.dram1` dla natywnego 1-cyklowego `S32C1I`) zamiast spinlocka `portENTER_CRITICAL` całkowicie eliminuje wyłączanie przerwań podczas operacji na PSRAM.
2. **Ukryte twarde limity w `0.3.2-beta4` blokujące skalowanie powyżej 8–17 urządzeń:**
   - `CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES` było ustawione na **`8`** (`matter_project_config.h`).
   - `CHIP_CONFIG_SECURE_SESSION_POOL_SIZE` wynosiło domyślnie **`17`** (`5*3+2` — powyżej 17 węzłów LRU wywłaszczałoby aktywne sesje CASE).
   - `CONFIG_MAX_EXCHANGE_CONTEXTS` = **`8`**, `CONFIG_NUM_TIMERS` = **`32`**, `CONFIG_OPENTHREAD_MLE_MAX_CHILDREN` = **`10`**, `CONFIG_OPENTHREAD_TMF_ADDR_CACHE_ENTRIES` = **`20`**.
3. **Błędy wykryte i naprawione w audycie `0.4.0-scale64` (Build `40002`):**
   - **Rollback OTA (`gateway_update.cpp`):** naprawiono błąd z `0.3.3` (gdzie po 180 s obraz był bezwarunkowo zatwierdzany jako `VALID`); teraz `ota_health(bool s3_healthy, bool c6_ready)` zatwierdza po 90 s (S3+C6), po 180 s (samo zdrowe S3: `internal_free > 16 KiB`), a po **240 s wywołuje automatyczny rollback**, jeśli samo S3 nie przechodzi kontroli zdrowia.
   - **Stos `httpd` i 24 KiB NVS (`quick_add.cpp`):** `DeviceRegistry snapshot` (`3592 B`) w `send_devices()` przeniesiono ze stosu `httpd` na stertę (`std::unique_ptr`), a `registry_v2` powyżej 32 urządzeń zapisuje kompaktowy blob (`8 + count * 56 B`) bez dublowania `registry_v1`.
   - **Przywrócone poprawki z `gemini1` zagubione w `gemini2`:** `CONFIG_LWIP_MAX_SOCKETS=16`, wyłączone `CONFIG_ENABLE_PERSIST_SUBSCRIPTIONS` (oszczędność NVS), niezależny `ensure_poll_task_started()` (`Opus N2`), straż `s_starting`, kolejka `kQueueCapacity = 192` i alokacja `ChannelMap` na stercie w `supla_bridge_max_reserved_node()`.
   - **Lżejsze `/api/devices`:** tymczasowy bufor w `device_telemetry::append_json()` zmniejszony z `256` do `32` wpisów per węzeł (`1,8 KiB` zamiast `14,3 KiB`).

## 3. Przygotowane wydanie `0.4.0-scale64` (Build `40002`) — GOTOWE, NIE WGRANE JESZCZE NA S3
- Lokalizacja: `M:\Matter2Supla-Gemini-Version\do-wgrania\Matter2Supla-0.4.0-scale64-S3-N16R8-OTA.m2s` (SHA-256: `0a45d9ada5dc50f0e4be026a7f619724616af1707a57af4cfad7c5c5fa767708`), `USB-UPDATE-0x10000.bin`, `FACTORY.bin`, `SHA256SUMS.txt`.
- Repozytorium GitHub (prywatne): `https://github.com/poszukiwaczz/Matter2Supla-Gemini`, tag `v0.4.0-scale64` (commit `d164cbb`).
- W `controller.map` zweryfikowano umieszczenie muteksów `DRAM_ATTR` w `.dram1` (`0x3fca2798`–`0x3fca28ac`) oraz tablic `readings[256]`, `contacts[64]`, `tracked[256]`, `s_devices[64]` w PSRAM.

## 4. Aplikacja Android `Matter2Supla Finder v1.1.0`
- Źródła: `M:\Matter2Supla-Gemini-Version\android-app\`, gotowy APK: `M:\Matter2Supla-Gemini-Version\do-wgrania\Matter2Supla-Finder-Android.apk`.
- Zweryfikowane w audycie: wszystkie trasy API (`/api/status`, `/api/devices`, `/api/gateway/diagnostics`, `/api/device/control`, `/api/matter/pair`, `/api/system/reboot`) zgodne 1:1 z `quick_add.cpp`, pojedyncza sesja TLS `Connection: keep-alive` serializowana przez `Mutex` (`requestMutex`), poprawna propagacja `CancellationException`, obsługa wielu przekaźników `OnOff`, skaner QR CameraX + ZXing offline.

## 5. Planowany montaż docelowy i zdalna aktualizacja ESP32-C6
- **Zasilanie po jednym kablu USB-C ↔ USB-C:** zlutowanie padów **`USB-OTG`** na spodzie płytki S3 N16R8 zwiera diodę na gnieździe `USB` S3, dzięki czemu S3 zasilane z portu `COM` zasila jednocześnie C6 po tym samym kablu USB-C ↔ USB-C (z zachowaniem zasady: nigdy nie podłączać drugiego zasilacza/PC do C6, gdy płytki są spięte!).
- **Zdalna aktualizacja C6 bez kabla PC:** w S3 jest już gotowa partycja `rcp_fw` (`600 KB` pod `0x410000`).
  - **Opcja A (sprzętowa, 6 przewodów: 5V/GND + UART0 `GPIO16/17` + `EN` + `BOOT/GPIO9`):** oficjalny `esp_rcp_update` + sprzętowy reset C6 przez watchdog S3 (niezniszczalne, działa z ROM bootloadera).
  - **Opcja B (czysto programowa, zostaje 1 kabelek USB-C ↔ USB-C przez `USB-OTG` + `UART1`):** wgranie raz przed montażem tablicy partycji 2×OTA na C6 (`ota_0` 1,5 MB + `ota_1` 1,5 MB) + odbiornik OTA po istniejącym `UART1` z weryfikacją SHA-256 i automatycznym rollbackiem C6.
