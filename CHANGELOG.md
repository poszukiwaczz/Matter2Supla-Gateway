# Changelog — Matter2Supla (Gemini Version + Android App)

## 2026-11-21 — `0.3.3-gemini1` (Build `30301`) na dysku `M:` + Natywna aplikacja Android `v1.1.0`

### 1. Audyt krzyżowy firmware (Gemini + Claude Opus)
- Przeprowadzono dwuetapowy audyt w trybie tylko do odczytu oraz weryfikację krzyżową na właściwym drzewie źródeł `beta/0.3.2-beta4-dev` (`0.3.2-beta4`, Build `30203`).
- Oryginalny kod firmware na dysku `K:` (`beta/0.3.2-beta4-dev` oraz `next-0.3`) pozostał w 100% nienaruszony.
- Na fizycznej bramce S3 nadal działa `0.3.2-beta4` (`30203`) w `ota_0` (oraz `0.3.2-beta2` w `ota_1`).

### 2. Wydanie `0.3.3-gemini1` (`FW_BUILD 30301`) w `M:\Matter2Supla-Gemini-Version`
- **[P1 / Opus N1] Limit backoffu resubskrypcji Matter i Watchdog martwej subskrypcji (180 s):**
  - W `BridgeSubscription::OnResubscriptionNeeded()` (`supla_bridge.cpp`) ograniczono `DefaultResubscribePolicy` (backoff Fibonacciego z SDK sięgający do 92 minut) do maks. **3 kolejnych prób**. Po 3 nieudanych próbach obiekt subskrypcji jest zamykany (`state.owned = false`) i odtwarzany w 30-sekundowym cyklu przez `maintain_reporting()`.
  - Dodano **Zombie Subscription Watchdog**: jeśli subskrypcja ma `state.owned = true`, utraciła świeżość i przez 180 s nie odebrała raportu ani potwierdzenia, wywoływane jest `ShutdownSubscriptions(..., node)` i zakładana jest nowa subskrypcja.
  - Interwały subskrypcji zmieniono z `(min=0, max=600)` na `(min=1, max=300)` s.
- **[P1 / Opus N2] Niezależny `poll_task` Matter przed konfiguracją SUPLA:**
  - Wydzielono `ensure_poll_task_started()` na początku `supla_bridge_start()`, a pętla `poll_task()` działa niezależnie od flagi `s_active`. Subskrypcje Matter i telemetria dla WebUI/Android/MQTT działają nawet bez skonfigurowanego konta SUPLA Cloud.
- **[P1 / Opus P2] Asynchroniczne sterowanie przekaźnikiem z `supla_task`:**
  - `MatterRelayElement::setNewChannelValue()` przekazuje komendę OnOff przez `gateway::relay_enqueue()` zamiast blokować `supla_task` na `ScopedChipStackLock(portMAX_DELAY)`. Chroni to `SuplaDevice.iterate()` i `drain_updates()` przed zamrożeniem oraz uruchamia bezpośredni odczyt zwrotny `read_relay_after_command()`.
- **[P1 / Opus N4] Odporna bramka zatwierdzania OTA `ota_health()` (`gateway_update.cpp`):**
  - Nowy obraz S3 (`ESP_OTA_IMG_PENDING_VERIFY`) jest zatwierdzany po **90 s** (gdy `free_internal > 8192 B` i C6 odpowiada) lub po **180 s** stabilnej pracy S3 bez paniki — chwilowa blokada zewnętrznego radia C6 nie wywołuje już fałszywego rollbacku `ABORTED`.
- **[P2] Pula gniazd LWIP zwiększona z 10 do 16 (`CONFIG_LWIP_MAX_SOCKETS=16` w `sdkconfig`).**
- **[P2] Ochrona `states[32]` i null-checki w `matter_ota.cpp` oraz `ota_provider_fix/esp_matter_ota_provider.cpp`:**
  - Dodano `find_state()` dla ścieżek tylko do odczytu (`snapshot()`, `candidate_allowed()`), sprawdzenia `nullptr` dla `get_commissioner()` i `FindFabricWithIndex()` oraz jawne `'\0'` po `strncpy`.
- **[P3] Odciążenie stosów, atomowy start i pula `s_reads[8]`:**
  - `buffer[4096]` w `gateway::update()` oraz `ChannelMap` w `supla_bridge_max_reserved_node()` przeniesiono ze stosu na stertę (`std::unique_ptr`).
  - Pula `s_reads` zwiększona z `4` do `8` slotów (timeout `30 s` zamiast `60 s`).
  - Dodano atomową straż `s_starting` w `supla_bridge_start()`, uwzględnienie `reporting_fresh` dla przekaźnika w `drain_updates()` oraz fallback w `restart_later()`.
- **[Opcja 1] +28 KiB wolnego wewnętrznego DRAM (tablice w PSRAM `EXT_RAM_BSS_ATTR`):**
  - Przeniesiono `readings[128]`, `contacts[32]` i `thread_peers[32]` (`device_telemetry.cpp`) oraz `desired`, `running` (`ca[4097]` × 2) i `tracked[128]` (`gateway_mqtt.cpp`) do PSRAM.
- **[Opcja 2] Crash & Reset Tracker w `RTC_NOINIT_ATTR` (`crash_guard` w `gateway_services.cpp`):**
  - Śledzi `boot_count`, `panic_count`, `last_abnormal_reason` i `last_uptime_before_reset_s` między ciepłymi restartami i zwraca je w `/api/gateway/diagnostics`.
- **[Opcja 3] Automatyczna reklama mDNS (`matter2supla.local` i `_matter2supla._tcp:443` w `gateway_services.cpp`).**
- **[Opcja 4] Szybki hash `compute_discovery_hash()` w `mqtt_tick()` (`gateway_mqtt.cpp`)** — eliminuje budowanie i serializację drzew `cJSON` co 2 s.
- **[Opcja 5] Dopasowanie ścieżek subskrypcji w `measurement_paths()` (`supla_bridge.cpp`)** do zmapowanych ról urządzenia.
- **[Opcja 6] 15-sekundowy cache skanu Wi-Fi w PSRAM z 250 ms timeoutem muteksu (`wifi_manager.cpp`).**
- **[Opcja 7] Rate-limiting logowania (`429 Too Many Requests` na 3 s po 5 błędnych próbach `401` w `panel_security.cpp`).**
- **[Opcja 8] Watchdog stanu radia C6 / CCA (`consecutive_cca_blocked_windows`, `consecutive_probe_failures`, `watchdog_alert` w `gateway_services.cpp`).**

### 3. Natywna aplikacja Android `Matter2Supla Finder v1.1.0` (`android-app/`)
- Automatyczne wyszukiwanie bramki w sieci LAN bez wpisywania IP: zapisane IP → tryb AP `192.168.4.1` → mDNS (`_matter2supla._tcp`, `_https._tcp`, `_matter._tcp`) → równoległy skan `/24` na porcie 443 z weryfikacją certyfikatu `Matter2Supla` i `/api/status`.
- Własna ikona Adaptive Icon (monogram M z węzłami Matter), natywne logowanie z zapamiętywaniem hasła i przypięciem odcisku SHA-256 certyfikatu.
- 4 natywne zakładki odświeżane automatycznie **w locie co 2,5 s (`LIVE`)**:
  - **Pulpit:** System, Wi-Fi, Sieć Thread, SUPLA Cloud.
  - **Urządzenia:** pełna telemetria `beta4`/`gemini1` (Temperatura, Wilgotność, Przekaźnik OnOff + sterowanie Włącz/Wyłącz, Napięcie V, Prąd A, Moc W, Energia kWh, Bateria, rola i RSSI Thread).
  - **Dodaj:** parowanie Matter (Thread / LAN / Wi-Fi) z **wbudowanym czytnikiem kodów QR przez aparat telefonu** (CameraX + ZXing Core offline).
  - **Diagnostyka:** 5 pełnych kart (Łącze S3–C6 Spinel/CCA/Watchdog, System i CPU 2 rdzenie + Crash Guard RTC, RAM i PSRAM, Flash i partycja OTA, stabilność Wi-Fi) z poprawionym układem kolumn `KeyVal`.
