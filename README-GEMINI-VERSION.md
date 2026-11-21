# Matter2Supla — Gemini Version (`0.3.3-gemini1`, Build `30301`)

Niniejsze wydanie na dysku `M:\Matter2Supla-Gemini-Version` powstało na bazie kodu **Codex `0.3.2-beta4` (`30203`)** i łączy zweryfikowane ustalenia z audytu **Claude Opus** oraz **Gemini** wraz z **8 dodatkowymi usprawnieniami stabilności, pamięci i diagnostyki**. Oryginalne drzewo na dysku `K:` pozostało nienaruszone.

---

## 1. Wdrożone poprawki z audytu (Opus + Gemini)

1. **[P1 / Opus N1] Ograniczenie backoffu resubskrypcji Matter i Watchdog martwej subskrypcji (`supla_bridge.cpp`)**
   - `BridgeSubscription::OnResubscriptionNeeded()` ogranicza wykładniczy backoff Fibonacciego z SDK (`DefaultResubscribePolicy`, który potrafił rosnąć do **92 minut**) do maksymalnie **3 prób**. Po 3 nieudanych próbach (np. podczas chwilowego zakłócenia RF/CCA) subskrypcja jest czysto zamykana (`state.owned = false`) i odtwarzana przez `maintain_reporting()` w 30-sekundowym cyklu.
   - Dodano **Zombie Subscription Watchdog (180 s)** w `maintain_reporting()`: jeśli subskrypcja ma `state.owned = true`, ale utraciła świeżość (`!reporting_fresh`) i przez 180 s nie odebrała żadnego raportu ani ponownego zestawienia, wykonywane jest `ShutdownSubscriptions(..., node)` i czyste ponowne zasubskrybowanie.
   - Zmieniono parametry subskrypcji z `(min=0, max=600)` na `(min=1, max=300)`, co skraca maksymalne okno ciszy o połowę i zapobiega burzom raportów w tej samej sekundzie.

2. **[P1 / Opus N2] Uniezależnienie `poll_task` i subskrypcji Matter od konfiguracji SUPLA (`supla_bridge.cpp`)**
   - Wydzielono `ensure_poll_task_started()` wywoływane zawsze na starcie `supla_bridge_start()`, przed sprawdzeniem konfiguracji SUPLA.
   - Zadanie `poll_task` utrzymuje subskrypcje Matter i telemetrię urządzeń (dla panelu WebUI, aplikacji Android i MQTT) nawet wtedy, gdy konto SUPLA Cloud nie jest jeszcze skonfigurowane.

3. **[P1 / Opus P2] Asynchroniczne sterowanie przekaźnikiem z `supla_task` bez `ScopedChipStackLock(portMAX_DELAY)` (`supla_bridge.cpp`)**
   - `MatterRelayElement::setNewChannelValue()` przekazuje komendę OnOff do kolejki `gateway::relay_enqueue()` zamiast blokować wątek `supla_task` na muteksie CHIP (`portMAX_DELAY`). Dzięki temu `SuplaDevice.iterate()` (keepalive chmury) i `drain_updates()` nigdy się nie zawieszają, a sterowanie z SUPLA korzysta z bezpośredniego odczytu zwrotnego `read_relay_after_command()`.

4. **[P1 / Opus N4] Odporna bramka zatwierdzania OTA `ota_health()` (`gateway_update.cpp`, `gateway_services.cpp`)**
   - Nowy obraz S3 (`ESP_OTA_IMG_PENDING_VERIFY`) jest zatwierdzany po **90 s** przy aktywnej odpowiedzi C6 i `free_internal > 8192 B`, albo po **180 s** stabilnej pracy S3 bez paniki — dzięki czemu przejściowy problem z zewnętrznym radiem C6 nie powoduje fałszywego wycofania (`ABORTED`) sprawnego firmware S3.

5. **[P2] Zwiększenie puli gniazd `CONFIG_LWIP_MAX_SOCKETS` z `10` na `16` (`sdkconfig`)**
   - Zapewnia bezpieczny zapas deskryptorów gniazd dla równoczesnych sesji HTTPS, SUPLA Cloud TLS, MQTT, sondy ping routera i DCL.

6. **[P2] Ochrona tablicy `states[32]` i null-checki w `matter_ota.cpp` oraz `ota_provider_fix`**
   - Ścieżki tylko do odczytu (`snapshot()`, `candidate_allowed()`) korzystają z `find_state()` i nie alokują slotów w `states[32]`.
   - Dodano sprawdzenia `nullptr` dla `get_commissioner()` oraz `FindFabricWithIndex()` i jawne terminatory `'\0'` po `strncpy`.

7. **[P3] Odciążenie stosów zadań, atomowy start i powiększona pula `PendingRead` (`gateway_update.cpp`, `supla_bridge.cpp`, `gateway_services.cpp`)**
   - Bufor `buffer[4096]` podczas OTA oraz `ChannelMap` w `supla_bridge_max_reserved_node()` są alokowane na stercie (`std::unique_ptr`), eliminując ryzyko przepełnienia 12 KiB stosu HTTPD.
   - Pula `s_reads` została zwiększona z `4` do `8` slotów, a jej timeout skrócony z `60 s` do `30 s`.
   - Dodano atomową straż `s_starting` w `supla_bridge_start()`, uwzględnienie `reporting_fresh` dla przekaźników w `drain_updates()` oraz fallback w `restart_later()` na wypadek błędu `xTaskCreate`.

---

## 2. Osiem dodatkowych usprawnień (Opcje 1–8)

1. **[Opcja 1] Przeniesienie dużych tablic statycznych do PSRAM (`EXT_RAM_BSS_ATTR`) — +28 KiB wolnego wewnętrznego DRAM**
   - Przeniesiono `readings[128]`, `contacts[32]` i `thread_peers[32]` w `device_telemetry.cpp` oraz `desired`, `running` (`ca[4097]` × 2) i `tracked[128]` w `gateway_mqtt.cpp` z wewnętrznego `.bss` do pamięci PSRAM.
2. **[Opcja 2] Crash & Reset Tracker w pamięci `RTC_NOINIT_ATTR` (`gateway_services.cpp`)**
   - Śledzi między restartami liczbę uruchomień (`boot_count`), liczbę restartów awaryjnych (`panic_count`), ostatni kod awarii (`last_abnormal_reason`) i czas działania przed restartem (`last_uptime_before_reset_s`). Dane są widoczne w `/api/gateway/diagnostics` w sekcji `crash_guard`.
3. **[Opcja 3] Automatyczna reklama mDNS `matter2supla.local` i `_matter2supla._tcp` (`gateway_services.cpp`)**
   - Po połączeniu z Wi-Fi bramka rejestruje nazwę `matter2supla.local` oraz usługę `_matter2supla._tcp` (port 443) z rekordami TXT (`version`, `name`).
4. **[Opcja 4] Odchudzenie pętli `mqtt_tick()` (`gateway_mqtt.cpp`)**
   - Zamiast budować i serializować pełne drzewa `cJSON` Home Assistant Discovery co 2 sekundy dla każdego pomiaru, pętla wylicza szybki hash `compute_discovery_hash()` ze struktury wejściowej i tworzy obiekty `cJSON` wyłącznie przy zmianie konfiguracji lub po `rediscover`.
5. **[Opcja 5] Dopasowanie ścieżek subskrypcji Matter do typu urządzenia (`supla_bridge.cpp`)**
   - W `measurement_paths()` zmapowane czujniki temperatury/wilgotności nie subskrybują klastrów energii i przekaźnika, a gniazdka nie subskrybują klastrów T/H/baterii (niezmapowane węzły zachowują pełny zestaw).
6. **[Opcja 6] Buforowany skan Wi-Fi z krótkim timeoutem blokady (`wifi_manager.cpp`)**
   - `wifi_manager_scan()` przechowuje wynik skanu w PSRAM przez 15 s i używa 250 ms timeoutu na `s_mode_mutex`, chroniąc serwer HTTPS przed wielokrotnym blokowaniem.
7. **[Opcja 7] Ochrona przed brute-force hasła panelu (`panel_security.cpp`)**
   - Po 5 kolejnych błędnych próbach autoryzacji `panel_security_authorize()` włącza 3-sekundową blokadę (`429 Too Many Requests`).
8. **[Opcja 8] Watchdog stanu radia C6 / CCA (`gateway_services.cpp`)**
   - Zlicza kolejne okna z pełną blokadą CCA (`consecutive_cca_blocked_windows`) oraz kolejne błędy sondy Spinel (`consecutive_probe_failures`) i wystawia flagę alarmową `watchdog_alert` w `/api/gateway/diagnostics` (`s3_c6`).
