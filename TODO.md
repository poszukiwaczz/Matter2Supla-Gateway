# TODO — Matter2Supla (Stan na 21.11.2026)

## 1. Weryfikacja sprzętowa wydania `0.4.0-scale64` (Build `40002`)
- [ ] **Decyzja użytkownika i wgranie `0.4.0-scale64` (Build `40002`) na fizyczną S3 (`COM16` / `192.168.1.191`):**
  - Gotowy podpisany pakiet OTA: `M:\Matter2Supla-Gemini-Version\do-wgrania\Matter2Supla-0.4.0-scale64-S3-N16R8-OTA.m2s` (SHA-256: `0a45d9ada5dc50f0e4be026a7f619724616af1707a57af4cfad7c5c5fa767708`).
  - Przy wgraniu przez USB (`ota_1` pod `0x8b0000`) lub przez OTA zachować nienaruszone `ota_0` (`0.3.2-beta4` / `30203`) jako obraz powrotny (`Rollback`).
- [ ] **Pomiar wewnętrznego SRAM, PSRAM i CPU po starcie `0.4.0-scale64`:**
  - Zweryfikować w `/api/gateway/diagnostics` zysk wolnego wewnętrznego SRAM (`internal_free`, `internal_minimum`) po włączeniu `CONFIG_CHIP_MEM_ALLOC_MODE_EXTERNAL=y` + `CONFIG_CHIP_SYSTEM_CONFIG_POOL_USE_HEAP=y` oraz przeniesieniu `readings[256]` do PSRAM z muteksami `DRAM_ATTR`.
  - Potwierdzić zatwierdzenie obrazu przez naprawione `ota_health(s3_healthy, c6_ready)` po 90 s (`OTA image confirmed VALID at 90s`), `SUPLA 17`, aktywne subskrypcje `TIMMERFLOTTE` (node 6) i `GRILLPLATS` (node 10), `wifi_health.disconnects == 0` oraz stabilne CPU ~10–20%.
- [ ] **Opublikowanie oficjalnego Release `v0.4.0-scale64` na GitHubie (opcjonalnie):**
  - Załączyć paczki `OTA.m2s`, `USB-UPDATE-0x10000.bin`, `FACTORY.bin`, `Matter2Supla-Finder-Android.apk` oraz `SHA256SUMS.txt` w zakładce Releases repozytorium `poszukiwaczz/Matter2Supla-Gemini`.

## 2. Zdalna aktualizacja ESP32-C6 (przed montażem w docelowym miejscu)
- [ ] **Wybór architektury połączenia S3 ↔ C6 w docelowej obudowie:**
  - **Wariant B (jeden kabelek USB-C ↔ USB-C ze zlutowaną zworką `USB-OTG` na S3):**
    - Zlutować pady `USB-OTG` na spodzie S3, zasilić S3 przez port `COM` (zasilacz 5 V / min. 1,5–2 A) i potwierdzić zasilanie C6 przez jeden kabelek USB-C ↔ USB-C.
    - Zaprojektować i wdrożyć **2×OTA + automatyczny rollback na ESP32-C6** (tablica partycji `ota_0`/`ota_1` po 1,5 MB na 4 MB Flash C6 + odbiornik OTA po istniejącym `UART1` `GPIO19/20 ↔ GPIO12/13` + endpoint `POST /api/gateway/update-rcp` w S3 korzystający z partycji `rcp_fw` 600 KB i podpisu ECDSA).
    - Jednorazowo wgrać bazowy obraz 2×OTA na C6 po USB przed zamknięciem obudowy.
  - **Alternatywnie — Wariant A (6 przewodów: 5V/GND + UART0 `GPIO16/17` + `EN` + `BOOT/GPIO9`):**
    - Włączyć oficjalny `esp_rcp_update` z krzemowego ROM bootloadera C6 oraz dodać sprzętowy reset C6 z pinu `EN` podpięty do `watchdog_alert` w `gateway_services.cpp`.

## 3. Dalszy rozwój aplikacji Android (`Matter2Supla Finder`)
- [ ] **Test zaktualizowanego APK na telefonie (`Matter2Supla-Finder-Android.apk`):**
  - Sprawdzić wielogodzinną stabilność z otwartym podglądem `LIVE` (5 s, Keep-Alive + `requestMutex`), sterowanie przekaźnikiem oraz skaner QR.
- [ ] **Wersja `v1.2.0` (opcjonalnie):**
  - Dodać zakładkę *Ustawienia i Aktualizacje*: zdalne wgrywanie `.m2s` (S3 i C6), przycisk **Rollback**, pobieranie kopii NVS oraz zmianę nazwy bramki / kanału Thread z aplikacji.

## 4. Procedura awaryjnego powrotu do `0.3.2-beta4` (Codex)
- Oryginalne, zweryfikowane binarki Codexa `0.3.2-beta4` (`30203`) są zabezpieczone w:
  1. `K:\AI PROJEKTY\matter2supla\beta\0.3.2-beta4-dev\build-artifacts\s3\`
  2. `M:\Matter2Supla-Gemini-Version\do-wgrania\powrot-do-codex-beta4\`
- **Powrót bez kabla:** przycisk *Przywróć poprzednią wersję (Rollback)* w WebUI (`POST /api/gateway/rollback`, `{"confirm":"ROLLBACK"}`) — wraca do `ota_0` (`0.3.2-beta4`) bez ruszania NVS i urządzeń (o ile sparowanych jest `<= 32` urządzeń).
- **Powrót przez USB (`COM16`):** wgranie `s3-n16r8-controller-app.bin` z `powrot-do-codex-beta4` do aktywnego slotu aplikacji bez modyfikacji tablicy partycji i NVS.
