# Matter2Supla — Wersja Single-USB (`0.4.1-singleusb` + C6 `2×OTA` RCP)

**Pełna zdalna aktualizacja obu płytek (ESP32-S3 i ESP32-C6) z panelu WWW bramki po JEDNYM kabelku USB-C ↔ USB-C — bez żadnych dodatkowych przewodów `EN`/`BOOT`!**

---

## 1. Połączenie sprzętowe (1 kabelek USB-C ↔ USB-C)

1. **Zlutuj zworkę `USB-OTG` (IN-OUT) na spodzie płytki ESP32-S3 N16R8.**
   - Zwiera ona diodę na gnieździe `USB` (`OTG`) płytki S3, dzięki czemu szyna `5V` z portu `COM` w S3 zasila bezpośrednio płytkę C6.
2. **Zasilacz 5 V (min. `5V / 1.5–2A`)** podłączasz wyłącznie do portu **`COM` (`UART`)** na płytce **ESP32-S3**.
3. **Krótki, zwykły kabel danych USB-C ↔ USB-C (2.0, bez chipa e-Marker PD)** spina gniazdo **`USB` (`OTG`)** na S3 z gniazdem **`USB`** na C6:
   - Płynie nim jednocześnie zasilanie `5V / GND` dla C6 oraz magistrala `UART1 2 Mb/s` (`S3 TX19/RX20` ↔ `C6 RX12/TX13`).
4. **WAŻNA ZASADA BEZPIECZEŃSTWA:** Po zlutowaniu zworki `USB-OTG` **nigdy nie podłączaj drugiego zasilacza ani komputera do płytki C6**, gdy obie płytki są spięte kablem USB-C ↔ USB-C! (Jeśli kiedyś chcesz podpiąć C6 do komputera, najpierw rozepnij kabelek S3 ↔ C6.)

---

## 2. Jak działa zdalna aktualizacja C6 po samym kablu UART1 (Opcja B — 2×OTA + Rollback)

### Tablica partycji ESP32-C6 (`partitions.csv`, działa na C6 4 MB i 8 MB)
| Partycja | Offset | Rozmiar | Rola |
|---|---|---|---|
| `nvs` | `0x9000` | `24 KB` | Pamięć nieulotna |
| `phy_init` | `0xf000` | `4 KB` | Kalibracja RF |
| `otadata` | `0x10000` | `8 KB` | Wybór aktywnego slotu OTA + stan `PENDING_VERIFY` |
| `ota_0` | `0x20000` | `1536 KB` | Pierwszy slot aplikacji RCP (~320 KB zajęte) |
| `ota_1` | `0x1A0000` | `1536 KB` | Drugi slot aplikacji RCP (dla zdalnego OTA i rollbacku) |

### Przebieg zdalnej aktualizacji C6 z panelu WWW S3
1. W zakładce **Aktualizacja i kopie** w panelu WWW wybierasz podpisany pakiet **`Matter2Supla-RCP-C6-*.m2s`** (ten sam przycisk co dla aktualizacji S3 — WebUI automatycznie rozpoznaje nagłówek **`M2SRCP1`** dla C6 vs **`M2SOTA3`** dla S3!).
2. **S3 weryfikuje podpis ECDSA P-256 i sumę SHA-256**, zapisuje obraz C6 w swojej wbudowanej partycji buforowej **`rcp_fw` (600 KB)** z nagłówkiem `PENDING` i wykonuje restart S3.
3. **Na starcie S3 (przed uruchomieniem OpenThread/Matter):**
   - `gateway::apply_pending_rcp_update()` otwiera `UART1 (TX19/RX20 @ 2 Mb/s)`.
   - S3 wysyła standardową ramkę HDLC **Spinel `CMD_RESET(SPINEL_RESET_PLATFORM)`** → działające C6 natychmiast wykonuje `esp_restart()`.
   - Po restarcie C6 wchodzi w **750 ms okno startowe OTA** (`rcp_uart_ota_boot_window()`, które przy normalnym starcie natychmiast się zamyka po pierwszym bajcie Spinel `0x7E` — 0 ms opóźnienia!).
   - S3 puka ramką `M2SO / RCP_PKT_KNOCK`, C6 odpowiada `RCP_PKT_READY`, a S3 przesyła obraz blokami po `1024 B` z kontrolą **CRC32** i ponawianiem (ok. **2–3 sekundy** dla ~320 KB!).
   - C6 zapisuje obraz do **nieaktywnego slotu `ota_0`/`ota_1`**, sprawdza pełne **SHA-256** całego obrazu, wywołuje `esp_ota_end()` + `esp_ota_set_boot_partition()` i restartuje się do nowej wersji.
4. **Automatyczny Rollback na C6 (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`):**
   - Po starcie nowej wersji C6 uruchamia OpenThread i wątek `rcp_ota_confirm_task`, który po **30 sekundach** stabilnej pracy bez awarii zatwierdza nowy slot (`esp_ota_mark_app_valid_cancel_rollback()`).
   - Jeśli nowe firmware C6 zawiesiłoby się lub wpadło w panikę — bootloader C6 **samoczynnie cofnie się do poprzedniego sprawnego slotu `ota_0`/`ota_1`!**
   - Wynik ostatniej aktualizacji C6 jest widoczny w `/api/gateway/diagnostics` w polu `s3_c6.rcp_ota_status` (`idle` / `done:<wersja>` / `failed...`).

---

## 3. Jednorazowe przygotowanie przed montażem w docelowym miejscu

1. **Jednorazowo po USB z komputera wgraj na ESP32-C6 obraz fabryczny 2×OTA (`0x0`):**
   - Plik: `do-wgrania\Matter2Supla-RCP-C6-1.0.0-2xOTA-FACTORY-0x0.bin`
   - Komenda (na porcie COM płytki C6, z rozpiętym kablem S3 ↔ C6):
     ```powershell
     python -m esptool --chip esp32c6 -p COMx -b 460800 write-flash 0x0 Matter2Supla-RCP-C6-1.0.0-2xOTA-FACTORY-0x0.bin
     ```
2. **Wgraj na ESP32-S3 firmware `0.4.1-singleusb` (`FW_BUILD 40101`):**
   - Zdalnie przez panel WWW (plik `do-wgrania\Matter2Supla-0.4.1-singleusb-S3-N16R8-OTA.m2s`) albo po USB (`USB-UPDATE-0x10000.bin`).
3. **Od tej chwili każdą kolejną aktualizację S3 (`M2SOTA3`) i C6 (`M2SRCP1`) wgrywasz już wyłącznie zdalnie przez panel WWW bramki!**

---

## 4. Zawartość podfolderu `single-usb-rcp-ota/`
- `firmware-c6-rcp-ota/` — kompletne źródła ESP32-C6 RCP z tablicą `2×OTA`, `rcp_uart_ota.c`, `rcp_uart_ota_proto.h` i automatycznym rollbackiem.
- `firmware-s3-controller/` — kompletne źródła ESP32-S3 (`0.4.1-singleusb`, oparte na zaudytowanej `0.4.0-scale64` Build `40002`) z `rcp_uart_ota_host.cpp`, trasą `/api/gateway/update-rcp` i ujednoliconym uploaderem `.m2s` w `www/gateway.js`.
- `scripts/` — `Build-C6-RcpOta.ps1`, `Build-S3-SingleUsb.ps1`, `Build-RcpUpdate.py` (podpisywanie pakietów C6 ECDSA).
- `do-wgrania/` — skompilowane i podpisane gotowe binarki dla S3 i C6 + `SHA256SUMS.txt`.
