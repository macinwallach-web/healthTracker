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
