# TODO — Matter2Supla (Przekazanie dla Codex / kolejne kroki)

## 1. Weryfikacja sprzętowa wydania `0.3.3-gemini1` (Build `30301`)
- [ ] **Decyzja użytkownika o wgraniu `0.3.3-gemini1`:**
  - Obraz OTA `.m2s` (`M:\Matter2Supla-Gemini-Version\do-wgrania\Matter2Supla-0.3.3-gemini1-S3-N16R8-OTA.m2s`) lub obraz aplikacji USB `0x10000` nie był jeszcze wgrywany na fizyczną płytkę S3.
  - Przed wgraniem przez USB wykonać pełny backup NVS/PHY/otadata; przy wgraniu przez OTA z panelu obecna wersja `0.3.2-beta4` (`30203`) pozostanie w slocie `ota_0` jako gotowy obraz powrotny (`VALID`).
- [ ] **Pomiar zysku pamięci wewnętrznej DRAM na fizycznym S3 po starcie `0.3.3-gemini1`:**
  - Sprawdzić w `/api/gateway/diagnostics` (lub w zakładce Diagnostyka aplikacji Android) wzrost `memory.internal_free` oraz `memory.internal_minimum` po przeniesieniu tablic `device_telemetry` i `gateway_mqtt` do PSRAM (oczekiwany wzrost o ok. +25–28 KiB względem ~66 KiB w `beta4`).
- [ ] **Weryfikacja mDNS i Watchdoga subskrypcji na żywo:**
  - Potwierdzić dostępność `https://matter2supla.local/` oraz wykrywanie usługi `_matter2supla._tcp` w aplikacji Android.
  - Potwierdzić zachowanie `BridgeSubscription` (limit 3 prób `DefaultResubscribePolicy` + 180 s Zombie Watchdog) przy wymuszonym uśpieniu/wybudzeniu czujnika TIMMERFLOTTE i gniazdka GRILLPLATS.

## 2. Diagnostyka sprzętowa radia ESP32-C6 (CCA −70 dBm)
- [ ] **Separacja fizyczna i zasilanie płytki C6:**
  - Płaski poziom ok. −70 dBm na wszystkich kanałach 11–26 podczas wcześniejszej awarii CCA oraz fakt, że pomógł dopiero power-cycle C6 (a nie sam reset EN), wskazują na czynnik sprzętowy/zasilaniowy lub sprzężenie z anteną Wi-Fi S3.
  - Sprawdzić odsunięcie płytki C6 od anteny S3 (>10–15 cm) oraz stabilność linii zasilania 5 V / 3,3 V.
- [ ] **Opcjonalny sprzętowy klucz zasilania C6 (MOSFET sterowany z GPIO S3):**
  - Jeśli blokada CCA na C6 kiedykolwiek powróci, podłączyć sterowanie zasilaniem C6 do wolnego GPIO S3 i spiąć je z dodaną w `0.3.3-gemini1` flagą `radio.watchdog_alert` (`consecutive_cca_blocked >= 3 || consecutive_failed >= 3`), aby S3 mogła automatycznie wykonać power-cycle C6 (z limitem np. 1 raz na 15 min).

## 3. Aplikacja Android (`android-app/`)
- [ ] **Test terenowy wersji `1.1.0` (`LIVE` 2,5 s + skaner QR + poprawiona Diagnostyka):**
  - Potwierdzić na telefonie płynność automatycznego odświeżania co 2,5 s, skanowanie kodu QR `MT:...` aparatem oraz brak pustego miejsca w pierwszej karcie Diagnostyki.
- [ ] **Rozbudowa o zakładkę Ustawienia / OTA (opcjonalnie w `v1.2.0`):**
  - Dodać możliwość zmiany nazwy bramki, kanału Thread, pobrania kopii zapasowej oraz bezpośredniego przesłania pliku `.m2s` (OTA) i przycisku **Rollback** z poziomu natywnej aplikacji Android.

## 4. Procedura awaryjnego powrotu do `0.3.2-beta4` (Codex)
- Wszystkie oryginalne, zweryfikowane binarki Codexa `0.3.2-beta4` (`30203`) są zabezpieczone w dwóch miejscach:
  1. `K:\AI PROJEKTY\matter2supla\beta\0.3.2-beta4-dev\build-artifacts\s3\`
  2. `M:\Matter2Supla-Gemini-Version\do-wgrania\powrot-do-codex-beta4\`
- Sposoby powrotu:
  - **Bez kabla (po wgraniu przez OTA):** użyć przycisku **Przywróć poprzednią wersję (Rollback)** w panelu WebUI (`POST /api/gateway/rollback` z `{"confirm":"ROLLBACK"}`) — przełącza boot z powrotem na slot `ota_0` z `0.3.2-beta4`.
  - **Przez kabel USB (COM16):** wgrać `s3-n16r8-controller-app.bin` z `powrot-do-codex-beta4` pod adres aktywnego slotu aplikacji (`0x10000` dla `ota_0`), bez ruszania tablicy partycji, NVS, mapy kanałów i tożsamości SUPLA.
