# Matter2Supla — Gemini Version (`0.3.3-gemini3`, Build `30303`)

Niniejsze wydanie w `M:\Matter2Supla-Gemini-Version` powstało na bazie kodu **Codex `0.3.2-beta4` (`30203`)** i łączy zweryfikowane ustalenia z audytu **Claude Opus** oraz **Gemini** wraz z optymalizacjami przetestowanymi na żywej bramce ESP32-S3 + ESP32-C6 (`gemini1` → `gemini2` → `gemini3`). Oryginalne drzewo na dysku `K:` pozostało w 100% nienaruszone.

---

## 1. Wdrożone i zweryfikowane sprzętowo usprawnienia w `0.3.3-gemini3` (`FW_BUILD 30303`)

1. **Niezależny `poll_task` i prawidłowa kolejność startu (`supla_bridge.cpp`, `app_main.cpp`)**
   - `gateway::start()` jest wywoływane w `app_main.cpp` przed `quick_add_start()`, dzięki czemu kolejka komend przekaźnika (`commands`), wątek roboczy `worker_task` oraz śledzenie resetów `init_crash_breadcrumb()` działają jeszcze przed startem mostka SUPLA.
   - `ensure_poll_task_started()` uruchamia wątek `poll_task` na samym początku `supla_bridge_start()`, niezależnie od tego, czy konto SUPLA Cloud jest już skonfigurowane.

2. **Asynchroniczne sterowanie przekaźnikiem z `supla_task` bez blokowania `ScopedChipStackLock(portMAX_DELAY)` (`supla_bridge.cpp`, `gateway_services.h`, `gateway_mqtt.cpp`)**
   - `MatterRelayElement::setNewChannelValue()` przekazuje rozkaz OnOff do kolejki `gateway::relay_enqueue(node_id, endpoint_id, on, /*from_supla=*/true)` zamiast brać blokadę stosu Matter bezpośrednio w wątku `supla_task`.
   - Flaga `from_supla = true` (`require_proof = false` w `supla_bridge_mqtt_command`) gwarantuje, że sterowanie z SUPLA Cloud działa także bezpośrednio po restarcie bramki (zanim spłynie pierwszy raport telemetrii Matter) i uruchamia odczyt potwierdzający `read_relay_after_command()`.

3. **Odporna bramka zatwierdzania OTA `ota_health()` (`gateway_update.cpp`, `gateway_services.cpp`)**
   - Nowy obraz S3 (`ESP_OTA_IMG_PENDING_VERIFY`) jest zatwierdzany po **90 s** przy aktywnej komunikacji Spinel z C6 i `free_internal > 8192 B`, albo po **180 s** stabilnej pracy samego S3 — chwilowy problem z zewnętrznym układem C6 nie wywołuje fałszywego wycofania (`ABORTED`) sprawnego firmware S3.

4. **Bezpieczny podział pamięci SRAM vs PSRAM (`device_telemetry.cpp`, `gateway_mqtt.cpp`, `wifi_manager.cpp`)**
   - Wszystkie tablice odczytywane wewnątrz sekcji krytycznych `portENTER_CRITICAL` (gdzie przerwania są wyłączone) pozostają w szybkim wewnętrznym SRAM, co utrzymuje średnie obciążenie CPU na poziomie **9,2%–19,0%** i zero rozłączeń Wi-Fi.
   - Struktury nieużywane pod spinlockami (`thread_peers[32]` — 5,75 KiB oraz 15-sekundowy bufor skanu Wi-Fi `s_cached_scan` — 2 KiB, chroniony przez `WifiLock`) znajdują się w zewnętrznym PSRAM (`EXT_RAM_BSS_ATTR`).

5. **Większa pula gniazd LWIP, ochrona `states[32]` i odciążenie stosów (`sdkconfig`, `matter_ota.cpp`, `gateway_update.cpp`)**
   - `CONFIG_LWIP_MAX_SOCKETS` zwiększone z `10` na `16`.
   - `find_state()` w `matter_ota.cpp` chroni tablicę `states[32]` przed zapełnieniem podczas odczytów tylko do podglądu, a `ota_provider_fix` posiada pełne sprawdzenia `nullptr`.
   - Bufory `4096 B` podczas OTA oraz `ChannelMap` w `supla_bridge_max_reserved_node()` są alokowane na stercie (`std::unique_ptr`), a pula `s_reads` zwiększona z `4` do `8` slotów (timeout `30 s`).

6. **Szybki hash `compute_discovery_hash()` w `mqtt_tick()`, Crash Guard w `RTC_NOINIT_ATTR`, rate-limiting logowania `429` i Watchdog C6/CCA**
   - `mqtt_tick()` nie buduje drzew `cJSON` co 2 s, jeśli hash konfiguracji i struktury pomiaru się nie zmienił.
   - `crash_guard` w `/api/gateway/diagnostics` raportuje `boot_count`, `panic_count`, `last_abnormal_reason` oraz `last_uptime_before_reset_s` (zapisywany wyłącznie przy resetach awaryjnych `PANIC`/`WDT`).
   - Po 5 błędnych próbach autoryzacji panel włącza 3-sekundowy limit `429 Too Many Requests`.

---

## 2. Natywna aplikacja Android `Matter2Supla Finder v1.1.0` (`android-app/`)
- Automatyczne wykrywanie bramki w sieci LAN (zapisane IP → AP `192.168.4.1` → mDNS → równoległy skan `/24` na porcie 443 z weryfikacją certyfikatu `Matter2Supla` i `/api/status`).
- Połączenia HTTPS `Connection: keep-alive` serializowane przez `Mutex` (`requestMutex`) z obsługą `CancellationException` i odświeżaniem `LIVE` co 5 s — brak obciążania ESP32-S3 powtarzanymi handshake'ami TLS.
- Obsługa wielu przekaźników `OnOff` na jednym urządzeniu (`List<RelayControl>`), pełna telemetria T/H/V/A/W/kWh/bateria/Thread RSSI, wbudowany skaner kodów QR Matter (CameraX + ZXing offline) oraz 5-kartowa Diagnostyka.
