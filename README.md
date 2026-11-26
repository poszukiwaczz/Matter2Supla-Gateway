# Matter2Supla

Mostek **Matter (Thread / Wi-Fi / LAN) → SUPLA Cloud + MQTT (Home Assistant Discovery)** dla tandemu
**ESP32-S3 N16R8 + ESP32-C6 RCP (UART1 2 Mb/s)** oraz natywna aplikacja Android **Matter2Supla Finder**.

---

## Instrukcje (zacznij tutaj)

- **[Gotowe pliki do pobrania (GitHub Releases)](../../releases)** — stabilna `v0.3.5` (po teście ~72 h) oraz `v0.4.3` (64 urz. + Single-USB + C6 2×OTA)
- **[Instrukcja wgrywania firmware (OTA / USB / Rollback)](docs/INSTRUKCJA-WGRYWANIA.md)**
- **[Instrukcja dodawania urządzeń Matter (Thread / Wi-Fi / LAN Multi-Admin)](docs/INSTRUKCJA-DODAWANIA-URZADZEN.md)**
- **[Zdalna aktualizacja C6 po jednym kablu USB-C (Single-USB OTA)](docs/SINGLE-USB-OTA.md)**

---

## Układ repozytorium

| Ścieżka / gałąź | Zawartość |
|---|---|
| **[`firmware/controller/`](firmware/controller/)** (gałąź `main`) | **Firmware ESP32-S3 (`0.4.3`, 64 urządzenia).** Oba warianty (Standard i Single-USB) buduje się z jednego drzewa przez Kconfig `CONFIG_M2S_SINGLE_USB_RCP_OTA`. |
| Gałąź **`release/0.3.5`** → `firmware/controller/` | **Stabilna gałąź konserwatywna ESP32-S3 (`0.3.5`, 32 urządzenia)** — zweryfikowana w teście ciągłym ~72 h. |
| **[`firmware/c6-rcp-ota/`](firmware/c6-rcp-ota/)** | **Firmware radia ESP32-C6 RCP (`1.1.0-c6-ota`)** z partycjami 2×OTA, weryfikacją Spinel i odbiornikiem OTA po UART1. |
| **[`android-app/`](android-app/)** | **Aplikacja Android (`Matter2Supla Finder`)** — skaner QR na żywo (CameraX + ZXing), wykrywanie bramki w LAN, sterowanie przekaźnikami i telemetria. |
| **[`scripts/`](scripts/)** | Skrypty budowania i podpisywania (`Build-Release.ps1`, `Build-Update.py`, `Build-C6-RcpOta.ps1`, `Build-RcpUpdate.py`) oraz testy panelu WWW (`Test-WebUI.cjs`, `Test-GatewayPanel.cjs`). |
| **[`docs/`](docs/)** | Instrukcje wgrywania, parowania urządzeń i konfiguracji Single-USB OTA. |

---

## Samodzielna kompilacja i testy (Windows + ESP-IDF 6.0)

```powershell
# Wydanie ESP32-S3 (build → podpis ECDSA → SHA-256 → manifest.json → RELEASE-NOTES.md)
.\scripts\Build-Release.ps1 -Variant standard        # wariant 64 urz. (standard)
.\scripts\Build-Release.ps1 -Variant singleusb       # wariant 64 urz. + zdalne OTA C6
.\scripts\Build-Release.ps1 -Variant standard -CompileOnly   # sama weryfikacja kompilacji

# Wydanie ESP32-C6 RCP (2xOTA)
.\scripts\Build-C6-RcpOta.ps1

# Testy panelu WWW na hoście
node .\scripts\Test-WebUI.cjs
node .\scripts\Test-GatewayPanel.cjs
```
