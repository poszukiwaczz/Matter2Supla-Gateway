# Changelog — Matter2Supla (Gemini Version + Android App)

## 2026-11-21 — `0.3.3-gemini3` (Build `30303`) — Audyt końcowy i poprawki brzegowe

### 1. Poprawki firmware `0.3.3-gemini3` (`FW_BUILD 30303`)
- **Synchronizacja cache skanu Wi-Fi (`wifi_manager.cpp`):**
  - Zabezpieczono odczyt i zapis `s_cached_scan` oraz `s_cached_scan_at_us` w PSRAM blokadą `WifiLock` (`s_wifi_mutex`), eliminując wyścig danych na ścieżce szybkiego powrotu po przekroczeniu 250 ms timeoutu `s_mode_mutex`.
- **Kolejność inicjalizacji przy starcie (`app_main.cpp`):**
  - Przesunięto `gateway::start()` przed `quick_add_start()`, dzięki czemu kolejka komend przekaźnika (`commands`), zadanie `worker_task` oraz `init_crash_breadcrumb()` są zainicjalizowane przed uruchomieniem `supla_bridge_start()`.
- **Niezawodne sterowanie przekaźnikiem z SUPLA Cloud od pierwszej sekundy (`gateway_services.h`, `gateway_mqtt.cpp`, `supla_bridge.cpp`):**
  - Dodano flagę `bool from_supla = false` do `gateway::relay_enqueue()` oraz `bool require_proof = true` do `supla_bridge_mqtt_command()`. Rozkazy z chmury SUPLA (`MatterRelayElement::setNewChannelValue`) omijają blokadę `relay_command_ready()` przed pierwszym raportem Matter i są czytelnie logowane jako `SUPLA OnOff=...`.
- **Zachowanie `last_uptime_before_reset_s` po restarcie awaryjnym (`gateway_services.cpp`):**
  - W `init_crash_breadcrumb()` wartość `s_crash_breadcrumb.last_uptime_before_reset_s` w `RTC_NOINIT_ATTR` jest aktualizowana wyłącznie wtedy, gdy bieżący reset był awaryjny (`ESP_RST_PANIC`, `ESP_RST_INT_WDT`, `ESP_RST_TASK_WDT`, `ESP_RST_WDT`), dzięki czemu zwykły późniejszy restart OTA nie nadpisuje czasu pracy sprzed wcześniejszej awarii.

### 2. Poprawki aplikacji Android (`Matter2Supla Finder v1.1.0`)
- **Serializacja żądań HTTPS Keep-Alive (`GatewayApiClient.kt`):**
  - Dodano `kotlinx.coroutines.sync.Mutex` (`requestMutex`) wokół `request()`, dzięki czemu równoległe akcje użytkownika (np. kliknięcie przekaźnika Włącz/Wyłącz w trakcie cyklu `LIVE`) korzystają sekwencyjnie z jednego gniazda TLS `keep-alive` zamiast otwierać drugie równoległe połączenie TLS do ESP32-S3.
  - Dodano jawne `catch (e: CancellationException) { throw e }`, aby anulowanie korutyny nie było połykane jako błąd sieciowy.
- **Obsługa wielu endpointów przekaźnikowych (`GatewayModels.kt`, `GatewayApiClient.kt`, `MainActivity.kt`):**
  - Zastąpiono pojedyncze `relayEndpointId` listą `relays: List<RelayControl>`, umożliwiając niezależne sterowanie każdym endpointem `OnOff` na urządzeniach wielokanałowych.
- **Bezpieczne zamykanie skanera QR (`QrScannerDialog.kt`):**
  - Dodano sprawdzenie `!executor.isShutdown` przed `onCodeScanned(raw)` w analizatorze CameraX.

---

## 2026-11-21 — `0.3.3-gemini2` (Build `30302`) — Optymalizacja CPU/SRAM, stabilizacja subskrypcji i TLS Keep-Alive
- Przywrócono tablice chronione przez `portENTER_CRITICAL` (`readings[128]`, `contacts[32]`, `desired`, `running`, `tracked[128]`) z zewnętrznego PSRAM do wewnętrznego SRAM, pozostawiając w PSRAM tylko struktury nieużywane pod spinlockiem (`thread_peers[32]` 5,75 KiB i `s_cached_scan` 2 KiB). Obciążenie CPU spadło z **54,85%** do **9,2%–19,0%**, a rozłączenia Wi-Fi spadły do **0**.
- Przywrócono sprawdzony cykl życia `BridgeSubscription` i `maintain_reporting()` z `0.3.2-beta4` (usunięto wyścig `ShutdownSubscriptions()` oraz konflikt `mdns_hostname_set("matter2supla")`).
- W aplikacji Android przełączono klienta HTTPS z `Connection: close` na `Connection: keep-alive` i cykl odświeżania `LIVE` co 5 s, eliminując burzę handshake'ów TLS na ESP32-S3.

---

## 2026-11-21 — `0.3.3-gemini1` (Build `30301`) na dysku `M:` + Natywna aplikacja Android `v1.1.0`
- Pierwsze wydanie łączące poprawki audytu Opus + Gemini (`ensure_poll_task_started`, asynchroniczne `relay_enqueue`, odporne `ota_health()`, `CONFIG_LWIP_MAX_SOCKETS=16`, ochrona `states[32]` i null-checki w `matter_ota.cpp`, powiększona pula `s_reads[8]`, Crash Guard w `RTC_NOINIT_ATTR`, szybki hash `compute_discovery_hash()` w `mqtt_tick()`, 15 s cache skanu Wi-Fi, rate-limiting logowania `429` i Watchdog C6/CCA) oraz natywną aplikację Android `Matter2Supla Finder`.
