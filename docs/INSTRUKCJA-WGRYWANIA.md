# Instrukcja wgrywania firmware i odzyskiwania bramki

## 1. Jakie pliki są dostępne po kompilacji?

| Typ pliku | Kiedy używać | Czy kasuje ustawienia / urządzenia? |
|---|---|---|
| `*-OTA.m2s` | **Zwykła aktualizacja przez panel WWW** (*Aktualizacja i kopie*) | **Nie** — zachowuje NVS, Wi-Fi, Thread, SUPLA i urządzenia; ma automatyczny rollback |
| `*-APP.bin` | Wgranie samej aplikacji przez **USB (`esptool`)** pod adres konkretnego slotu (`ota_0` `0x4b0000`, `ota_1` `0x8b0000` lub `factory` `0x10000`) | **Nie** — nie rusza tablicy partycji ani NVS |
| `*-FULL-FLASH-0x0.bin` | **Wyłącznie na nową / czystą płytkę** (od adresu `0x0`: bootloader + partycje + `otadata` + aplikacja) | **Tak** (nadpisuje cały flash od `0x0`) |

---

## 2. Którą wersję wybrać?

- **`0.3.5` (gałąź `release/0.3.5`, 32 urządzenia):** sprawdzona, stabilna wersja po teście ciągłym **~72 h** (0 wycieków RAM, 0 rozłączeń Wi-Fi, 0 restartów). Zalecana do codziennej pracy.
- **`0.4.3` (gałąź `main`, 64 urządzenia):** wszystko z `0.3.5` + skalowanie stosu Matter/OpenThread do 64 urządzeń.
- **`0.4.3-singleusb` (gałąź `main`, Kconfig `CONFIG_M2S_SINGLE_USB_RCP_OTA=y`):** jak `0.4.3` + obsługa zdalnej aktualizacji radia ESP32-C6 po jednym kablu USB-C ↔ USB-C (szczegóły: [SINGLE-USB-OTA.md](SINGLE-USB-OTA.md)).

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
