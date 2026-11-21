# Matter2Supla — Zacznij tutaj (`00_START_HERE.md`)

## 21.11.2026 — Audyt krzyżowy (Opus + Gemini), wydanie `0.3.3-gemini1` na dysku `M:` oraz natywna aplikacja Android `v1.1.0`

1. **Stan fizycznej bramki S3 + C6 (PO WGRANIU `0.3.3-gemini1`):**
   - Na fizycznej bramce S3 (`COM16`) działa teraz **`0.3.3-gemini1` (Build `30301`)** z partycji **`ota_1`**, a poprzednia wersja Codexa **`0.3.2-beta4` (Build `30203`)** została zachowana nienaruszona w **`ota_0`** jako gotowy obraz powrotny (`Rollback`). Na C6 działa RCP **ESP-IDF 6.1.1** (`051e4c4e-43cc05a`).
   - Kopia przed wgraniem (tablica partycji, NVS, PHY, `otadata`, `recovery`) oraz log rozruchu i raport weryfikacji: `M:\Matter2Supla-Gemini-Version\backups\before-033gemini1-serial-20261121-110249Z\`.
   - **Potwierdzony wynik sprzętowy po starcie (`uptime = 117 s`):**
     - `SUPLA`: `connected`, kod **17** (po 8,5 s od startu)
     - `mDNS`: `https://matter2supla.local/` (`_matter2supla._tcp:443`) aktywne od 7,8 s
     - `Subskrypcje Matter`: **obie aktywne (`True`)** — `GRILLPLATS` (node 10, 5 pomiarów, zestawiona w 18,5 s) oraz `TIMMERFLOTTE` (node 6, 3 pomiary, aktywna przed 117 s)
     - `Radio S3–C6`: `connected: true`, `probe_failures: 0`, `radio_cca_total: 0`, `watchdog_alert: false`
     - `Pamięć RAM`: wolny wewnętrzny DRAM **72 487 B (70,8 KiB)**, minimum historyczne **69 519 B (67,9 KiB — wzrost o +14,8 KiB względem 53,1 KiB w beta2/beta4)**, wolny PSRAM **7,53 MiB**
     - `Crash Guard (RTC)`: `boot_count: 1`, `panic_count: 0`
   - Oryginalny kod firmware na dysku `K:` pozostał w 100% nietknięty.

2. **Nowe wydanie `0.3.3-gemini1` (Build `30301`) na dysku `M:` (`M:\Matter2Supla-Gemini-Version`):**
   - Przygotowane na bazie `0.3.2-beta4` z wdrożeniem ustaleń audytu Claude Opus i Gemini oraz 8 dodatkowych usprawnień (m.in. limit 3 prób `DefaultResubscribePolicy` + 180 s Zombie Subscription Watchdog, niezależny `poll_task`, asynchroniczne sterowanie przekaźnikiem z `supla_task` przez `relay_enqueue`, odporna bramka `ota_health()`, `CONFIG_LWIP_MAX_SOCKETS=16`, przeniesienie ~28 KiB tablic `device_telemetry` i `gateway_mqtt` do PSRAM `EXT_RAM_BSS_ATTR`, RTC Crash Guard, reklama mDNS `matter2supla.local` / `_matter2supla._tcp`, szybki hash `mqtt_tick`, cache skanu Wi-Fi, rate-limiting logowania i Watchdog C6).
   - Skompilowane i podpisane obrazy (`FACTORY.bin`, `USB-UPDATE-0x10000.bin`, `OTA.m2s`, `SHA256SUMS.txt`) znajdują się w `M:\Matter2Supla-Gemini-Version\do-wgrania\`.
   - Kopia powrotna binarek Codexa `0.3.2-beta4` (`30203`) znajduje się w `M:\Matter2Supla-Gemini-Version\do-wgrania\powrot-do-codex-beta4\` (oraz w oryginalnym `K:\AI PROJEKTY\matter2supla\beta\0.3.2-beta4-dev\build-artifacts\s3\`).

3. **Natywna aplikacja Android `Matter2Supla Finder v1.1.0`:**
   - Źródła: `K:\AI PROJEKTY\matter2supla\android-app` (oraz kopia w `M:\Matter2Supla-Gemini-Version\android-app`).
   - Gotowy plik instalacyjny APK: `K:\AI PROJEKTY\matter2supla\android-app\Matter2Supla-Finder-debug.apk` (oraz w `M:\Matter2Supla-Gemini-Version\do-wgrania\Matter2Supla-Finder-Android.apk`).
   - Wyszukuje bramkę w sieci lokalnej bez wpisywania IP, obsługuje natywne logowanie, odświeża wszystkie dane **w locie co 2,5 s (`LIVE`)**, wyświetla pełne pomiary T/H/OnOff/V/A/W/kWh/Thread RSSI, steruje gniazdkiem Włącz/Wyłącz, posiada wbudowany czytnik kodów QR Matter przez aparat telefonu oraz 5-kartową Diagnostykę.

4. **Dokumenty szczegółowe:**
   - [README-GEMINI-VERSION.md](README-GEMINI-VERSION.md)
   - [AI_MEMORY.md](AI_MEMORY.md)
   - [CHANGELOG.md](CHANGELOG.md)
   - [TODO.md](TODO.md)
