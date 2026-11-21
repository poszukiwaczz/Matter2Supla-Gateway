# Matter2Supla — Gemini Version (`0.4.0-scale64`, Build `40002` / aktywne na sprzęcie: `0.3.3-gemini3`, Build `30303`)

Niniejsza linia w `M:\Matter2Supla-Gemini-Version` powstała na bazie kodu **Codex `0.3.2-beta4` (`30203`)** i łączy zweryfikowane ustalenia z audytu **Claude Opus** oraz **Gemini** wraz ze skalowaniem do **64 urządzeń Matter / 256 pomiarów** oraz natywną aplikacją Android. Oryginalne drzewo na dysku `K:` pozostało w 100% nienaruszone.

---

## 1. Najnowsze wydanie: `0.4.0-scale64` (`FW_BUILD 40002`, po pełnym audycie kodu)

1. **Zdjęcie twardych barier rdzenia Matter (CHIP) i OpenThread (`matter_project_config.h`, `sdkconfig`)**
   - `CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES`: `8` → **`64`**, `CHIP_CONFIG_SECURE_SESSION_POOL_SIZE`: `17` → **`72`**, `CHIP_CONFIG_CASE_SESSION_RESUME_CACHE_SIZE`: `15` → **`64`**, `CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_CASE_CLIENTS`: **`16`**, `CHIP_CONFIG_UNAUTHENTICATED_CONNECTION_POOL_SIZE`: `4` → **`8`**.
   - `CONFIG_MAX_EXCHANGE_CONTEXTS`: `8` → **`64`**, `CONFIG_MAX_UNSOLICITED_MESSAGE_HANDLERS`: `8` → **`32`**, `CONFIG_NUM_TIMERS`: `32` → **`128`**, `CONFIG_MAX_EVENT_QUEUE_SIZE`: `40` → **`96`**, `CONFIG_LWIP_MAX_SOCKETS`: `10` → **`16`**.
   - `CONFIG_OPENTHREAD_MLE_MAX_CHILDREN`: `10` → **`64`**, `CONFIG_OPENTHREAD_TMF_ADDR_CACHE_ENTRIES`: `20` → **`64`**.
   - `CONFIG_CHIP_MEM_ALLOC_MODE_EXTERNAL=y`, `CONFIG_CHIP_SYSTEM_CONFIG_POOL_USE_HEAP=y` (pule obiektów CHIP alokowane w 8 MB PSRAM) oraz wyłączone `CONFIG_ENABLE_PERSIST_SUBSCRIPTIONS` (oszczędność miejsca w 24 KiB NVS i Flash).

2. **Muteksy FreeRTOS w wewnętrznym DRAM (`DRAM_ATTR`) + tablice 64/256 w PSRAM (`device_telemetry.cpp`, `gateway_mqtt.cpp`, `quick_add.cpp`)**
   - `kNodes` `32` → **`64`**, `kSamples` `128` → **`256`**, `tracked` `128` → **`256`**, `MAX_DEVICES` `32` → **`64`**.
   - Spinlocki `portENTER_CRITICAL` w `device_telemetry.cpp` i `gateway_mqtt.cpp` zastąpiono muteksami FreeRTOS (`TelemetryLock`, `MqttLock`) z blokami `StaticSemaphore_t` umieszczonymi jawnie w `.dram1` (`DRAM_ATTR`, natywne 1-cyklowe `S32C1I`), bez wyłączania przerwań podczas skanowania PSRAM.
   - `DeviceRegistry snapshot` (`3592 B`) w `send_devices()` przeniesiono ze stosu `httpd` na stertę, bufor w `device_telemetry::append_json()` zmniejszono do `32` wpisów per urządzenie, a `registry_v2` powyżej 32 urządzeń zapisuje kompaktowy blob bez marnowania 24 KiB NVS.
   - `log_hook()` i `log_tail()` używają szybkiego dwuetapowego `memcpy()`.

3. **Niezawodne OTA, sterowanie SUPLA i stabilność subskrypcji (`gateway_update.cpp`, `gateway_services.cpp`, `supla_bridge.cpp`, `app_main.cpp`)**
   - `ota_health(bool s3_healthy, bool c6_ready)`: potwierdza po **90 s** (zdrowe S3 + C6), po **180 s** (zdrowe samo S3 z `internal_free > 16 KiB`), a po **240 s** wykonuje **automatyczny rollback**, jeśli samo S3 nie przechodzi kontroli zdrowia.
   - `gateway::start()` przed `quick_add_start()`; niezależny `ensure_poll_task_started()` (`Opus N2`); asynchroniczne `relay_enqueue(..., /*from_supla=*/true)` bez blokowania `supla_task`; kolejka aktualizacji `kQueueCapacity = 192`.
   - Adaptacyjne `MinInterval` subskrypcji (`0 s` dla `<= 8`, `2 s` dla `9–24`, `5 s` dla `25–64` urządzeń) i limit `3` nowych inicjalizacji subskrypcji na sekundę po starcie.
   - Szybki hash `compute_discovery_hash()` w `mqtt_tick()`, Crash Guard w `RTC_NOINIT_ATTR`, 15 s cache skanu Wi-Fi pod `WifiLock`, rate-limiting `429` i Watchdog C6/CCA (ze zliczaniem timeoutów OpenThread).

---

## 2. Natywna aplikacja Android `Matter2Supla Finder v1.1.0` (`android-app/`)
- Automatyczne wykrywanie bramki w sieci LAN (zapisane IP → AP `192.168.4.1` → mDNS → równoległy skan `/24` na porcie 443 z weryfikacją certyfikatu `Matter2Supla` i `/api/status`).
- Połączenia HTTPS `Connection: keep-alive` serializowane przez `Mutex` (`requestMutex`) z propagacją `CancellationException` i odświeżaniem `LIVE` co 5 s — brak burzy handshake'ów TLS na ESP32-S3.
- Obsługa wielu przekaźników `OnOff` na jednym urządzeniu (`List<RelayControl>`), pełna telemetria T/H/V/A/W/kWh/bateria/Thread RSSI, wbudowany skaner kodów QR Matter (CameraX + ZXing offline) oraz 5-kartowa Diagnostyka.
