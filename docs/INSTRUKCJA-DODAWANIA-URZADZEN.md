# Instrukcja dodawania urządzeń Matter do bramki Matter2Supla

Bramka obsługuje trzy sposoby dodawania urządzeń Matter: **Thread**, **Wi-Fi** oraz **LAN (Multi-Admin)**. Wszystko odbywa się lokalnie — kod QR jest dekodowany w Twojej przeglądarce lub w aplikacji Android, bez wysyłania zdjęć na zewnątrz.

---

## 1. Który tryb wybrać? (szybka ściągawka)

| Sytuacja | Wybierz w panelu | Skąd wziąć kod? |
|---|---|---|
| Nowe urządzenie **Matter over Thread** (np. czujnik bateryjny, gniazdko Thread) | **Matter over Thread** | Naklejka / instrukcja urządzenia (`MT:...` lub 11/21 cyfr) |
| Nowe lub zresetowane urządzenie **Matter over Wi-Fi** (jeszcze nie zna Twojego Wi-Fi) | **Matter over Wi-Fi** | Naklejka / instrukcja urządzenia + **SSID i hasło routera 2,4 GHz** |
| Urządzenie **już dodane** do Home Assistanta, Apple Home lub Google Home (albo podpięte po Ethernet) | **Urządzenie już w sieci lokalnej** (`lan`) | **Tymczasowy kod udostępniania (Multi-Admin)** z tamtej aplikacji *(lub naklejka, jeśli urządzenie ma już LAN, ale nie było jeszcze parowane)* |

---

## 2. Jak podać kod Matter (QR lub ręczny)

- **W panelu WWW bramki (`https://<IP-bramki>` → zakładka „Dodaj”):**
  - Kliknij **„Wybierz zdjęcie lub aparat”** — na telefonie otworzy się aparat, zrób ostre zdjęcie kodu QR z etykiety (cały kod w kadrze). Przeglądarka sama odczyta kod `MT:...` i automatycznie rozpocznie parowanie.
  - Jeśli wolisz wpisać kod ręcznie, rozwiń **„Mam kod — wpiszę go ręcznie”**, wpisz `MT:...` albo 11/21 cyfr (ze spacjami lub myślnikami — bramka sama je wyczyści) i kliknij **„Rozpocznij parowanie”**.
- **W aplikacji Android (`Matter2Supla Finder`):**
  - Działa **skaner na żywo z aparatu** — wystarczy wycelować kamerą w kod QR.

---

## 3. Dodawanie krok po kroku

### Scenariusz A: Matter over Thread
1. Upewnij się w panelu, że sieć Thread jest aktywna (rola `leader` lub `router`).
2. Włącz tryb parowania na urządzeniu (wg instrukcji producenta). Jeśli to czujnik bateryjny, wybudź go przyciskiem tuż przed parowaniem.
3. W zakładce **Dodaj** wybierz **„Matter over Thread”**.
4. Zeskanuj kod QR (lub wpisz kod ręcznie) i poczekaj na komunikat o dodaniu (zwykle 15–60 s).

### Scenariusz B: Matter over Wi-Fi (nowe urządzenie Wi-Fi)
> **Ważne:** podczas pierwszego parowania urządzenie musi znajdować się w **zasięgu Bluetooth (BLE)** bramki ESP32-S3 (najlepiej 1–3 m), a sieć Wi-Fi musi działać w paśmie **2,4 GHz** (tej samej sieci lokalnej, w której jest bramka).

1. Włącz tryb parowania na urządzeniu Wi-Fi (po resecie fabrycznym zwykle miga dioda).
2. W zakładce **Dodaj** wybierz **„Matter over Wi-Fi”** — pojawią się dwa dodatkowe pola.
3. Wpisz **nazwę sieci Wi-Fi (SSID)** i **hasło Twojego routera 2,4 GHz**.
4. Zeskanuj kod QR z naklejki urządzenia lub wpisz kod ręcznie i kliknij **„Rozpocznij parowanie”**.
5. Bramka połączy się z urządzeniem po Bluetooth, przekaże mu dane Wi-Fi, a po chwili urządzenie przełączy się na Twój router i pojawi na liście **Urządzenia**.

### Scenariusz C: Urządzenie już w sieci lokalnej (`lan` / Multi-Admin)
Ten tryb pozwala dodać urządzenie, które **już działa w innym systemie** (urządzenie będzie wtedy działać jednocześnie w obu systemach!).

1. Wygeneruj **tymczasowy kod parowania** w systemie, w którym urządzenie już działa:
   - **Home Assistant:** *Ustawienia* → *Urządzenia oraz usługi* → **Matter** → *Urządzenia* → wybierz urządzenie → **„Udostępnij urządzenie”** (*Share device*). Kod ważny 15 minut.
   - **Apple Home (Dom):** otwórz kafel urządzenia → ikona koła zębatego (*Ustawienia akcesorium*) → **„Włącz tryb parowania”** → skopiuj kod.
   - **Google Home:** otwórz urządzenie → ikona koła zębatego → **„Połączone aplikacje i usługi Matter”** → **„Połącz aplikacje i usługi”** → **„Użyj kodu parowania”**.
2. W panelu bramki w zakładce **Dodaj** wybierz **„Urządzenie już w sieci lokalnej”**.
   - *Uwaga:* po wybraniu tej opcji nie pojawiają się żadne dodatkowe pola ani lista urządzeń — to normalne.
3. Wpisz wygenerowany kod ręczny (lub zeskanuj kod QR z ekranu tamtej aplikacji) i kliknij **„Rozpocznij parowanie”**.
4. Bramka sama odnajdzie urządzenie w sieci LAN po **mDNS** (bez Bluetooth i bez podawania hasła Wi-Fi).

---

## 4. Co dzieje się po dodaniu urządzenia?

1. Urządzenie pojawia się w zakładce **Urządzenia** z nadanym numerem `node`.
2. Bramka automatycznie odczytuje obsługiwane funkcje (klastry Matter: przekaźnik On/Off, temperatura, wilgotność, licznik energii) i wystawia je jako kanały w **SUPLA Cloud** oraz (jeśli włączone) w **MQTT**.
3. W zakładce **Urządzenia** możesz zmienić nazwę urządzenia oraz podejrzeć jego klastry (**„Sprawdź klastry”**) i bieżącą telemetrię.

---

## 5. Najczęstsze problemy i szybkie rozwiązania

- **„Kod nie przeszedł walidacji”** — sprawdź, czy przepisano wszystkie 11 lub 21 cyfr bez literówki (kod ma sumę kontrolną).
- **Nie znajduje urządzenia w trybie `lan`** — jeśli urządzenie było już sparowane z HA / Apple / Google, **kod z naklejki nie zadziała**; użyj tymczasowego kodu z *Udostępnij urządzenie* (Multi-Admin). Upewnij się też, że router nie blokuje ruchu mDNS między urządzeniami w Wi-Fi (wyłączona izolacja klientów / *AP Isolation*).
- **Nie znajduje urządzenia w trybie `Matter over Wi-Fi`** — przysuń urządzenie bliżej bramki (zasięg Bluetooth) i upewnij się, że wpisujesz sieć **2,4 GHz** (większość urządzeń Matter Wi-Fi nie obsługuje 5 GHz).
- **Czujnik bateryjny Thread nie kończy parowania** — wybudź go przyciskiem na obudowie i upewnij się, że okno parowania urządzenia nie wygasło.
- **Nie udało się odczytać zdjęcia QR** — zrób zdjęcie z bliższej odległości, przy dobrym świetle, bez odblasku na naklejce, albo wpisz 11-cyfrowy kod spod QR ręcznie.
