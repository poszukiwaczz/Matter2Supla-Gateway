# Instrukcja wgrywania firmware i odzyskiwania bramki

## 1. Jakie pliki są dostępne po kompilacji?

| Typ pliku | Kiedy używać | Czy kasuje ustawienia / urządzenia? |
|---|---|---|
| `*-OTA.m2s` | **Zwykła aktualizacja przez panel WWW** (*Aktualizacja i kopie*) | **Nie** — zachowuje NVS, Wi-Fi, Thread, SUPLA i urządzenia; ma automatyczny rollback |
| `*-APP.bin` | Wgranie samej aplikacji przez **USB (`esptool`)** pod adres konkretnego slotu (`ota_0` `0x4b0000`, `ota_1` `0x8b0000` lub `factory` `0x10000`) | **Nie** — nie rusza tablicy partycji ani NVS |
| `*-FULL-FLASH-0x0.bin` | **Wyłącznie na nową / czystą płytkę** (od adresu `0x0`: bootloader + partycje + `otadata` + aplikacja) | **Tak** (nadpisuje cały flash od `0x0`) |

---

## 2. Którą wersję i parę plików S3 + C6 wybrać?

| Wydanie | Plik ESP32-S3 (czysta płytka `0x0`) | Plik ESP32-C6 (czysta płytka `0x0`) | Uwagi |
|---|---|---|---|
| **Stabilna `0.3.5`** (32 urz., test ~72 h) | `Matter2Supla-0.3.5-S3-N16R8-FULL-FLASH-0x0.bin` | `Matter2Supla-C6-4MB-FACTORY.bin` *(lub `8MB`, z `IDF-6.1.1`)* | Zalecana do codziennej pracy. Wymaga sprawnego połączenia S3↔C6 już przy starcie. |
| **`0.4.5` standard** (64 urz.) | `Matter2Supla-0.4.5-S3-N16R8-FULL-FLASH-0x0.bin` | `Matter2Supla-C6-4MB-FACTORY.bin` *(lub `1.1.1-c6-ota`)* | Wi-Fi i panel WWW (`Matter-GW-XXXX` / `https://192.168.4.1`) startują **nawet bez podłączonego C6**; po podłączeniu C6 Thread wstaje automatycznie bez restartu S3. |
| **`0.4.5-singleusb`** (64 urz. + zdalne OTA C6) | `Matter2Supla-0.4.5-singleusb-S3-N16R8-FULL-FLASH-0x0.bin` | `Matter2Supla-RCP-C6-1.1.1-c6-ota-FULL-FLASH-0x0.bin` *(później aktualizacje przez `*-OTA.m2s`)* | Zdalne OTA radia C6 po jednym kablu USB-C ↔ USB-C (szczegóły: [SINGLE-USB-OTA.md](SINGLE-USB-OTA.md)) + niezależny start Wi-Fi/panelu bez C6. |

> **Połączenie sprzętowe S3 ↔ C6:** obie płytki łączy się kablem **USB-C z liniami danych (D+/D-)** wpiętym w **natywne gniazda USB** (S3 `GPIO19`/`GPIO20` ↔ C6 `GPIO12`/`GPIO13`, `UART1 2 Mb/s`). Zwykły kabel „tylko do ładowania” nie zadziała. Do podglądu logów na komputerze służy osobny port **UART/COM (`115200 8N1`)** na S3.

---

## 3. Aktualizacja ESP32-S3 przez panel WWW (OTA — zalecana)

1. Zaloguj się do panelu bramki: `https://<IP-bramki>`.
2. Wejdź w zakładkę **Aktualizacja i kopie** i najpierw pobierz **kopię zapasową NVS**.
3. W sekcji aktualizacji wskaż podpisany plik `Matter2Supla-<wersja>-S3-N16R8-OTA.m2s` i kliknij **Wgraj**.
4. Po restarcie bramka uruchomi się z drugiego slotu OTA (`ota_0` ↔ `ota_1`) i po **90 s** (przy sprawnym Wi-Fi, SUPLA/Matter i C6) automatycznie zatwierdzi nową wersję jako `VALID`. W razie awarii cofnie się sama do poprzedniego slotu.
5. Po aktualizacji odśwież panel przeglądarki skrótem **`Ctrl+F5`**.

---

## 4. Samodzielna kompilacja ze źródeł (Windows + ESP-IDF 6.0)

```powershell
# ESP32-S3 — wariant standard (64 urz.)
.\scripts\Build-Release.ps1 -Variant standard

# ESP32-S3 — wariant single-USB (64 urz. + zdalne OTA C6)
.\scripts\Build-Release.ps1 -Variant singleusb

# Sama weryfikacja kompilacji (bez tworzenia paczki i bez zużycia FW_BUILD)
.\scripts\Build-Release.ps1 -Variant standard -CompileOnly

# ESP32-C6 RCP (wariant 2xOTA)
.\scripts\Build-C6-RcpOta.ps1
```

Gotowe paczki trafiają do `build-artifacts/<wersja>-b<build>/` (poza gitem).

---

## 5. Powrót do poprzedniej wersji (Rollback / `factory`)

- **Z panelu WWW:** zakładka *Aktualizacja i kopie* → przycisk **Rollback** (wymaga wpisania `ROLLBACK`). Bramka przełączy się na poprzedni potwierdzony slot `ota_0`/`ota_1`, a jeśli ten nie jest `VALID` — na bezpiecznik **`factory` (`0x10000`, `0.3.2-beta4`)**.
- **Po kablu USB (`esptool`):** wgranie `*-APP.bin` pod adres aktywnego slotu (`0x4b0000` dla `ota_0`, `0x8b0000` dla `ota_1`, `0x10000` dla `factory`) bez ruszania NVS (`0x9000`).
