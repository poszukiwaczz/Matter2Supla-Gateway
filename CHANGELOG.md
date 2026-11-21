# Changelog — Matter2Supla (Gemini Version + Android App)

## 2026-11-21 — `0.4.0-scale64` (Build `40001`) — Skalowanie bramki do 64 urządzeń Matter / 256 pomiarów

### 1. Zdjęcie ukrytych barier w stosie Matter (CHIP) i OpenThread (`matter_project_config.h`, `sdkconfig`)
- **Kontroler Matter (`matter_project_config.h`):**
  - `CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES`: `8` → **`64`** (usuwa twardy limit 8 równoczesnych urządzeń z `0.3.2-beta4`).
  - `CHIP_CONFIG_SECURE_SESSION_POOL_SIZE`: `17` → **`72`** (zapobiega wywłaszczaniu aktywnych sesji CASE przez LRU powyżej 17 urządzeń).
  - `CHIP_CONFIG_CASE_SESSION_RESUME_CACHE_SIZE`: `15` → **`64`** (umożliwia szybkie wznawianie sesji Sigma1 dla wszystkich 64 węzłów).
  - `CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_CASE_CLIENTS`: **`16`**, `CHIP_CONFIG_UNAUTHENTICATED_CONNECTION_POOL_SIZE`: `4` → **`8`**.
- **Rdzeń CHIP i OpenThread (`sdkconfig`):**
  - `CONFIG_MAX_EXCHANGE_CONTEXTS`: `8` → **`64`**, `CONFIG_MAX_UNSOLICITED_MESSAGE_HANDLERS`: `8` → **`32`**, `CONFIG_NUM_TIMERS`: `32` → **`128`**, `CONFIG_MAX_EVENT_QUEUE_SIZE`: `40` → **`96`**.
  - `CONFIG_CHIP_MEM_ALLOC_MODE_EXTERNAL=y` oraz `CONFIG_CHIP_SYSTEM_CONFIG_POOL_USE_HEAP=y` — dynamiczne pule obiektów CHIP i alokacje `chip::Platform::New` trafiają bezpośrednio do 8 MB zewnętrznego PSRAM zamiast obciążać wewnętrzny SRAM.
  - `CONFIG_OPENTHREAD_MLE_MAX_CHILDREN`: `10` → **`64`**, `CONFIG_OPENTHREAD_TMF_ADDR_CACHE_ENTRIES`: `20` → **`64`**.

### 2. Zamiana spinlocków na muteksy FreeRTOS i podwojenie tablic w PSRAM
- **`device_telemetry.cpp`:** `kNodes` `32` → **`64`**, `kSamples` `128` → **`256`**; `readings[256]`, `contacts[64]` i `thread_peers[64]` w PSRAM (`EXT_RAM_BSS_ATTR`), chronione przez zwykły muteks FreeRTOS (`TelemetryLock` bez wyłączania przerwań CPU).
- **`gateway_mqtt.cpp`:** `tracked` `128` → **`256`**, `nodes` `32` → **`64`**, `desired`/`running`/`tracked` w PSRAM chronione przez muteks FreeRTOS (`MqttLock`).
- **`quick_add.cpp`:** `MAX_DEVICES` `32` → **`64`** z pełną kompatybilnością wsteczną i powrotną NVS (`registry_v1` zachowuje 32-elementowy format binarny dla bezpiecznego rollbacku do `0.3.2`/`0.3.3`, a powyżej 32 urządzeń zapisywany jest pełny `registry_v2`). Zoptymalizowano też `log_hook()` i `log_tail()` przez dwuetapowy `memcpy()` zamiast pętli bajt po bajcie pod spinlockiem.
- **`supla_bridge.cpp`, `matter_ota.cpp`, `gateway_recovery.cpp`:** `kMaxSlots`, `s_reporting`, `s_reporting_live`, `states`, `nodes` powiększone z `32` → **`64`**, pula `s_reads` z `8` → **`16`**.
- **Adaptacyjne tempo subskrypcji i dynamiczne `MinInterval` (`supla_bridge.cpp`):**
  - `MinInterval` ustawiany automatycznie: `0 s` dla `<= 8` urządzeń, `2 s` dla `9–24` urządzeń, `5 s` dla `25–64` urządzeń.
  - Ograniczenie nowych inicjalizacji subskrypcji po starcie do maks. **3 węzłów na sekundę** (`kMaxNewSubscribesPerTick = 3`), co zapobiega kolizjom na radiu Thread przy starcie dużej sieci.

---

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
