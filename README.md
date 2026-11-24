# PD Monitor — Automatyczny Dziennik Dializy Otrzewnowej (IoT + Cloud)

System telemetryczny wspomagający pacjenta na manualnej dializie otrzewnowej (**CAPD**). Urządzenie oparte na **M5Stack Core** oraz tensometrze z przetwornikiem **HX710/HX711** automatyzuje ważenie worków dializacyjnych, mierzy czas i tempo odpływu z cewnika, oblicza bilans ultrafiltracji netto (**UF**) oraz synchronizuje wyniki w czasie rzeczywistym z bazą **Firebase Firestore** i panelem webowym.

---

## Kluczowe funkcje

* **Automatyczna maszyna stanów (FSM) na M5Stack:**
  1. **Tarowanie (`STATE_TARE`)** — kalibracja zera wagi przed cyklem.
  2. **Ważenie świeżego płynu (`STATE_WEIGH_FRESH`)** — wykrycie zawieszenia worka, detekcja stabilizacji pomiaru, zapis masy brutto w chmurze oraz w pamięci nieulotnej Flash (**NVS / `Preferences`**) na potrzeby kolejnego cyklu.
  3. **Oczekiwanie na zero (`STATE_WAIT_ZERO`)** — automatyczne wykrycie zdjęcia świeżego worka i kompensacja dryftu zera przed drenażem.
  4. **Pomiar drenażu i przepływu (`STATE_DRAINING`)** — pomiar czasu wypływu, wyliczenie średniego tempa przepływu ($\text{g/min}$) jako wskaźnika drożności cewnika, odczekanie 5 s po stabilizacji oraz obliczenie ultrafiltracji netto:
     $$\text{UF}_{\text{netto}} = W_{\text{drenaż}} - W_{\text{poprzedni\_świeży}}$$
  5. **Czyszczenie układu (`STATE_FLUSHING`)** — detekcja przyrostu masy podczas płukania drenu (*flush before fill*) zakończona sygnalizacją dźwiękową.
* **Synchronizacja czasu NTP:** Automatyczne pobieranie czasu UTC z `pool.ntp.org` i zapis znaczników `timestampValue` w Firestore.
* **Panel pacjenta i statystyki (`index.html` / `stats.htm`):**
  * Podgląd bieżącego bilansu 24h, średniej dobowej UF (7 dni vs 30 dni) oraz drożności cewnika.
  * Agregacja tygodniowa (ISO Week) i miesięczna z wykresami słupkowo-liniowymi (**Chart.js**).
  * Wbudowany generator danych symulacyjnych (30 dni, 4 wymiany/dobę, pacjent 85 kg, $\text{UF} \approx 1000\text{ ml/d}$) oraz eksport historii do pliku **CSV** dla lekarza nefrologa.

---

## Architektura systemu
[Tensometr + HX710/HX711] ──(GPIO 21/22)──▶ [M5Stack Core (ESP32)]
│
HTTPS REST API + Firebase Auth
▼
[Firebase Firestore]
│
Modular SDK v10 (onSnapshot)
▼
[Firebase Hosting (Web Dashboard)]


---

## Sprzęt i podłączenie

| Komponent | Pin / Interfejs M5Stack | Uwagi |
| :--- | :--- | :--- |
| **M5Stack Core** | ESP32 + LCD + Buzzer | Zasilanie USB-C lub wbudowany akumulator |
| **HX710 / HX711 `DOUT`** | `GPIO 22` | Linia danych przetwornika tensometrycznego |
| **HX710 / HX711 `SCK`** | `GPIO 21` | Linia zegarowa przetwornika |

### Sterowanie awaryjne (przyciski M5Stack)
* **Przycisk A (Lewy):** Ręczne tarowanie wagi (`performTare()`).
* **Przycisk B (Środkowy):** Ręczne przejście do kolejnego kroku maszyny stanów.
* **Przycisk C (Prawy):** Pełny reset bieżącego cyklu do kroku 1.

---

## Struktura dokumentów w Firebase Firestore

Każdy zapis trafia do kolekcji zdefiniowanej w `COLLECTION` (domyślnie `weights`):

```json
{
  "event_type": "drain",
  "solution": "2.27%",
  "weight_kg": 2.490,
  "weight_g": 2490,
  "prev_fresh_kg": 2.130,
  "uf_kg": 0.360,
  "uf_ml": 360,
  "drain_duration_sec": 905,
  "flow_rate_g_min": 165.1,
  "recorded_at": "2026-11-24T06:30:00Z"
}

Konfiguracja i wdrożenie
1. Firmware (M5Stack)

    Otwórz projekt w Arduino IDE / PlatformIO z zainstalowaną biblioteką M5Stack.

    Utwórz plik config.h w katalogu ze szkicem na podstawie poniższego szablonu (nie commituj config.h do publicznego repozytorium):
    C++

    #pragma once
    const char* WIFI_SSID     = "TWOJE_WIFI";
    const char* WIFI_PASSWORD = "HASLO_WIFI";
    const char* API_KEY       = "FIREBASE_WEB_API_KEY";
    const char* PROJECT_ID    = "TWOJ_PROJECT_ID";
    const char* COLLECTION    = "weights";
    const char* DEVICE_EMAIL  = "pacjent@example.com";
    const char* DEVICE_PASS   = "HASLO_KONTA_FIREBASE";

    Dostosuj stałe progowe (CALIBRATION_FACTOR, STABILITY_WINDOW_KG, FLUSH_DELTA_KG) do użytej belki tensometrycznej i wgraj kod na M5Stack.

2. Firebase Firestore i Authentication

    W konsoli Firebase włącz Authentication → Sign-in method → Email/Password i utwórz konto urządzenia/pacjenta.

    W Firestore Database → Rules ustaw reguły dostępu dla zalogowanych użytkowników:
    JavaScript

    rules_version = '2';
    service cloud.firestore {
      match /databases/{database}/documents {
        match /{document=**} {
          allow read, write: if request.auth != null;
        }
      }
    }

3. Hosting WWW (GitHub Actions → Firebase Hosting)

    W pliku stats.htm / index.html uzupełnij obiekt firebaseConfig danymi swojej aplikacji webowej z ustawień projektu Firebase.

    W ustawieniach repozytorium GitHub (Settings → Secrets and variables → Actions) dodaj sekret FIREBASE_SERVICE_ACCOUNT z kluczem JSON konta usługi Firebase.

    Każdy push na gałąź main automatycznie publikuje aktualną wersję panelu na https://<PROJECT_ID>.web.app.

Zastrzeżenie

Projekt ma charakter demonstracyjny i edukacyjny (prototyp telemetryczny DIY). Nie stanowi certyfikowanego wyrobu medycznego — wszelkie decyzje terapeutyczne oraz zmiany schematu wymian należy konsultować z lekarzem prowadzącym.
