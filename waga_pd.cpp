#include <M5Stack.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include "config.h"

// --- Konfiguracja wagi (HX710 / HX711) ---
const int DOUT_PIN = 22;
const int SCK_PIN  = 21;
const float CALIBRATION_FACTOR = 26670.0f;

// --- Progi detekcji i stabilizacji (w kg) ---
const float MIN_BAG_WEIGHT_KG   = 0.500f; // Minimalna masa worka ze świeżym płynem (500 g)
const float ZERO_THRESHOLD_KG   = 0.030f; // Tolerancja zera (30 g)
const float DRAIN_START_KG      = 0.050f; // Próg wykrycia początku drenażu (50 g)
const float STABILITY_WINDOW_KG = 0.020f; // Okno stabilizacji pomiaru (±5 g; zmień na 0.100f jeśli ma być 0.1 kg)
const float FLUSH_DELTA_KG      = 0.040f; // Próg zakończenia płukania układu (20 g; zmień na 0.0002f dla 0.2 g lub 0.200f dla 0.2 kg)

// --- Konfiguracja czasu NTP (Polska) ---
const char* NTP_SERVER = "pool.ntp.org";
const char* TZ_INFO    = "CET-1CEST,M3.5.0,M10.5.0/3";

// --- Maszyna stanów cyklu dializy ---
enum CycleState {
  STATE_TARE = 0,
  STATE_WEIGH_FRESH,
  STATE_WAIT_ZERO,
  STATE_DRAINING,
  STATE_FLUSHING
};

CycleState g_state = STATE_TARE;
Preferences prefs;

long offset = 0;
float g_lastWeightKg = 0.0f;

// Zmienne śledzące stabilizację i czasy
float g_refStableWeight = 0.0f;
unsigned long g_stableSinceMs = 0;
bool g_isStabilizing = false;

// Zmienne drenażu i płukania
bool g_drainStarted = false;
unsigned long g_drainStartMs = 0;
unsigned long g_drainDurationSec = 0;
float g_drainBaseWeightKg = 0.0f;
float g_flushStartWeightKg = 0.0f;

// Pamięć poprzedniego worka do obliczeń UF
float g_prevFreshWeightKg = 0.0f;
bool g_hasPrevFresh = false;

// Autoryzacja Firebase
String g_idToken = "";
unsigned long g_tokenExpiresAtMs = 0;

// --- Dźwięk powiadomienia ---
void playBeep(int freq, int durationMs, int repeats = 1) {
  for (int i = 0; i < repeats; i++) {
    M5.Speaker.tone(freq, durationMs);
    delay(durationMs + 80);
  }
  M5.Speaker.mute();
}

// --- Czas NTP ---
String getIsoTimestampUTC() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) return "";
  time_t now;
  time(&now);
  struct tm* utcTime = gmtime(&now);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", utcTime);
  return String(buf);
}

String getLocalTimeFormatted() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 50)) return "--:--:--";
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  return String(buf);
}

// --- Parsowanie JSON i Auth Firebase ---
String extractJsonString(const String& json, const String& key) {
  String pattern = "\"" + key + "\"";
  int keyPos = json.indexOf(pattern);
  if (keyPos == -1) return "";
  int colonPos = json.indexOf(':', keyPos + pattern.length());
  if (colonPos == -1) return "";
  int firstQuote = json.indexOf('"', colonPos + 1);
  if (firstQuote == -1) return "";
  int secondQuote = json.indexOf('"', firstQuote + 1);
  if (secondQuote == -1) return "";
  return json.substring(firstQuote + 1, secondQuote);
}

unsigned long extractExpiresInSeconds(const String& json) {
  const String key = "\"expiresIn\"";
  int keyPos = json.indexOf(key);
  if (keyPos == -1) return 3600;
  int colonPos = json.indexOf(':', keyPos + key.length());
  if (colonPos == -1) return 3600;
  int firstQuote = json.indexOf('"', colonPos + 1);
  if (firstQuote == -1) return 3600;
  int secondQuote = json.indexOf('"', firstQuote + 1);
  if (secondQuote == -1) return 3600;
  unsigned long sec = json.substring(firstQuote + 1, secondQuote).toInt();
  return (sec > 0) ? sec : 3600;
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(TZ_INFO, NTP_SERVER);
    return true;
  }
  return false;
}

bool signInWithEmailPassword() {
  if (!ensureWiFiConnected()) return false;
  WiFiClientSecure client;
  client.setInsecure();

  String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + String(API_KEY);
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  String body = "{\"email\":\"" + String(DEVICE_EMAIL) +
                "\",\"password\":\"" + String(DEVICE_PASS) +
                "\",\"returnSecureToken\":true}";

  int httpCode = http.POST(body);
  String resp = http.getString();
  http.end();

  if (httpCode < 200 || httpCode >= 300) return false;
  String token = extractJsonString(resp, "idToken");
  unsigned long expiresSec = extractExpiresInSeconds(resp);
  if (token.length() == 0) return false;

  g_idToken = token;
  g_tokenExpiresAtMs = millis() + ((expiresSec > 60) ? ((expiresSec - 60) * 1000UL) : (expiresSec * 1000UL));
  return true;
}

bool ensureValidIdToken() {
  if (g_idToken.length() > 0 && millis() < g_tokenExpiresAtMs) return true;
  return signInWithEmailPassword();
}

// --- Zapis do Firestore (obsługuje typ: fresh lub drain z UF i czasem przepływu) ---
bool sendRecordToFirestore(const String& eventType, float weightKg, float ufKg, unsigned long durationSec, float flowRateGPerMin) {
  if (!ensureWiFiConnected() || !ensureValidIdToken()) return false;

  WiFiClientSecure client;
  client.setInsecure();

  String url = "https://firestore.googleapis.com/v1/projects/" + String(PROJECT_ID) +
               "/databases/(default)/documents/" + String(COLLECTION);

  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + g_idToken);

  String isoTime = getIsoTimestampUTC();

  String payload = "{\"fields\": {";
  payload += "\"event_type\": {\"stringValue\": \"" + eventType + "\"},";
  payload += "\"weight_kg\": {\"doubleValue\": " + String(weightKg, 3) + "},";
  payload += "\"weight_g\": {\"integerValue\": \"" + String((int)round(weightKg * 1000.0f)) + "\"}";

  if (eventType == "drain") {
    payload += ",\"drain_duration_sec\": {\"integerValue\": \"" + String(durationSec) + "\"}";
    payload += ",\"flow_rate_g_min\": {\"doubleValue\": " + String(flowRateGPerMin, 1) + "}";
    if (g_hasPrevFresh) {
      payload += ",\"prev_fresh_kg\": {\"doubleValue\": " + String(g_prevFreshWeightKg, 3) + "}";
      payload += ",\"uf_kg\": {\"doubleValue\": " + String(ufKg, 3) + "}";
      payload += ",\"uf_ml\": {\"integerValue\": \"" + String((int)round(ufKg * 1000.0f)) + "\"}";
    }
  }

  if (isoTime.length() > 0) {
    payload += ",\"recorded_at\": {\"timestampValue\": \"" + isoTime + "\"}";
  }
  payload += "}}";

  int httpCode = http.POST(payload);
  http.end();

  if (httpCode == 401 || httpCode == 403) g_idToken = "";
  return (httpCode >= 200 && httpCode < 300);
}

// --- Odczyt z HX710/HX711 ---
unsigned long readHX710_safe() {
  unsigned long count = 0;
  noInterrupts();
  unsigned long startTime = millis();
  while (digitalRead(DOUT_PIN)) {
    if (millis() - startTime > 20) {
      interrupts();
      return 0;
    }
  }
  for (int i = 0; i < 24; i++) {
    digitalWrite(SCK_PIN, HIGH);
    delayMicroseconds(1);
    count = count << 1;
    digitalWrite(SCK_PIN, LOW);
    delayMicroseconds(1);
    if (digitalRead(DOUT_PIN)) count++;
  }
  digitalWrite(SCK_PIN, HIGH);
  delayMicroseconds(1);
  count ^= 0x800000;
  digitalWrite(SCK_PIN, LOW);
  delayMicroseconds(1);
  interrupts();
  return count;
}

long getStableReading(int samples) {
  long sum = 0;
  int validSamples = 0;
  for (int i = 0; i < samples; i++) {
    unsigned long raw = readHX710_safe();
    if (raw > 0 && raw < 16777215) {
      sum += raw;
      validSamples++;
    }
    delay(5);
  }
  return (validSamples == 0) ? 0 : (sum / validSamples);
}

void resetStabilityTracker() {
  g_isStabilizing = false;
  g_stableSinceMs = 0;
}

void performTare() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(YELLOW, BLACK);
  M5.Lcd.setTextSize(3);
  M5.Lcd.drawCentreString("1. TAROWANIE", 160, 95, 1);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawCentreString("Oproznij wage...", 160, 135, 1);
  delay(1500);
  offset = getStableReading(20);
  resetStabilityTracker();
  M5.Lcd.fillScreen(BLACK);
}

// --- Rysowanie ekranu dla aktualnego kroku ---
void drawStatusScreen(float weightKg, const char* stepTitle, uint16_t headerColor, const char* instruction, const char* extraInfo = "") {
  M5.Lcd.fillRect(0, 0, 320, 32, headerColor);
  M5.Lcd.setTextColor(WHITE, headerColor);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(8, 8);
  M5.Lcd.print(stepTitle);
  M5.Lcd.setCursor(215, 8);
  M5.Lcd.print(getLocalTimeFormatted());

  M5.Lcd.setTextColor(CYAN, BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawCentreString(instruction, 160, 44, 1);

  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextSize(5);
  char wStr[20];
  sprintf(wStr, " %.3f kg ", weightKg);
  M5.Lcd.drawCentreString(wStr, 160, 88, 1);

  M5.Lcd.setTextColor(YELLOW, BLACK);
  M5.Lcd.setTextSize(2);
  char infoBuf[42];
  sprintf(infoBuf, "%-30s", extraInfo);
  M5.Lcd.drawCentreString(infoBuf, 160, 148, 1);

  // Dolny pasek awaryjny
  M5.Lcd.drawFastHLine(0, 205, 320, DARKGREY);
  M5.Lcd.setTextColor(LIGHTGREY, BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawCentreString("[TARA]", 55, 214, 1);
  M5.Lcd.drawCentreString("[POMIN]", 160, 214, 1);
  M5.Lcd.drawCentreString("[RESET]", 268, 214, 1);
}

void setup() {
  M5.begin();
  M5.Power.begin();
  Serial.begin(115200);

  pinMode(DOUT_PIN, INPUT);
  pinMode(SCK_PIN, OUTPUT);
  digitalWrite(SCK_PIN, LOW);

  // Wczytanie poprzedniej masy świeżego worka z pamięci Flash (NVS)
  prefs.begin("dialysis", false);
  g_hasPrevFresh = prefs.getBool("has_prev", false);
  g_prevFreshWeightKg = prefs.getFloat("prev_fresh", 0.0f);

  ensureWiFiConnected();
  signInWithEmailPassword();

  // Krok 1: Tarowanie startowe
  performTare();
  g_state = STATE_WEIGH_FRESH;
}

void loop() {
  M5.update();

  long rawValue = getStableReading(5);
  if (rawValue == 0) {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.drawCentreString("Blad czujnika!", 160, 105, 1);
    delay(500);
    M5.Lcd.fillScreen(BLACK);
    return;
  }

  float weightKg = (float)(rawValue - offset) / CALIBRATION_FACTOR;
  if (fabs(weightKg) < 0.001f) weightKg = 0.000f;
  g_lastWeightKg = weightKg;

  // Ręczne sterowanie awaryjne (przyciski A / B / C)
  if (M5.BtnA.wasPressed()) {
    performTare();
  }
  if (M5.BtnB.wasPressed()) {
    // Ręczne przejście do kolejnego stanu w razie potrzeby
    resetStabilityTracker();
    g_state = (CycleState)((g_state + 1) % 5);
    M5.Lcd.fillScreen(BLACK);
  }
  if (M5.BtnC.wasPressed()) {
    // Pełny reset cyklu od kroku 1
    performTare();
    g_drainStarted = false;
    g_state = STATE_WEIGH_FRESH;
  }

  // --- LOGIKA MASZYNY STANÓW ---
  switch (g_state) {

    // KROK 1: Tarowanie (gdy wywołane z cyklu)
    case STATE_TARE: {
      performTare();
      g_state = STATE_WEIGH_FRESH;
      break;
    }

    // KROK 2: Ważenie świeżego płynu do ustabilizowania i zapis w dzienniku
    case STATE_WEIGH_FRESH: {
      char extra[40] = "Zawies swiezy worek (>0.5kg)";
      if (weightKg >= MIN_BAG_WEIGHT_KG) {
        if (!g_isStabilizing || fabs(weightKg - g_refStableWeight) > STABILITY_WINDOW_KG) {
          g_refStableWeight = weightKg;
          g_stableSinceMs = millis();
          g_isStabilizing = true;
        }
        unsigned long elapsed = millis() - g_stableSinceMs;
        sprintf(extra, "Stabilizacja: %.1f / 3.0 s", elapsed / 1000.0f);

        if (elapsed >= 3000) {
          // Zapis świeżego worka w chmurze
          M5.Lcd.fillScreen(BLACK);
          M5.Lcd.setTextColor(YELLOW, BLACK);
          M5.Lcd.setTextSize(3);
          M5.Lcd.drawCentreString("Zapis swiezego...", 160, 105, 1);

          bool ok = sendRecordToFirestore("fresh", weightKg, 0.0f, 0, 0.0f);
          playBeep(1200, 150, ok ? 1 : 3);

          // UWAGA: Zapisujemy masę świeżego worka. Jeśli w pamięci był już poprzedni
          // świeży worek (z poprzedniej wymiany), zachowujemy go dla nadchodzącego drenażu (Krok 3),
          // a nowy zapisujemy jako "next_fresh", który stanie się "prev_fresh" po drenażu!
          prefs.putFloat("next_fresh", weightKg);
          prefs.putBool("has_next", true);

          resetStabilityTracker();
          M5.Lcd.fillScreen(BLACK);
          g_state = STATE_WAIT_ZERO;
          break;
        }
      } else {
        resetStabilityTracker();
      }
      drawStatusScreen(weightKg, "2. SWIEZY PLYN", DARKGREEN, "Wazenie nowego worka", extra);
      break;
    }

    // KROK 3a: Oczekiwanie na powrót wagi do ~0 po zdjęciu świeżego worka
    case STATE_WAIT_ZERO: {
      char extra[40] = "Zdejmij worek -> oczekiwanie 0";
      if (fabs(weightKg) <= ZERO_THRESHOLD_KG) {
        if (!g_isStabilizing) {
          g_stableSinceMs = millis();
          g_isStabilizing = true;
        }
        if (millis() - g_stableSinceMs >= 1500) {
          // Zero ustabilizowane - przechodzimy do czuwania na start drenażu
          g_drainBaseWeightKg = weightKg;
          g_drainStarted = false;
          resetStabilityTracker();
          M5.Lcd.fillScreen(BLACK);
          g_state = STATE_DRAINING;
          break;
        }
      } else {
        resetStabilityTracker();
      }
      drawStatusScreen(weightKg, "3. GOTOWOSC DRENAZU", NAVY, "Oczekiwanie na ~0.000 kg", extra);
      break;
    }

    // KROK 3b: Pomiar drenowania, czasu przepływu, stabilizacja + 5s odczekania, obliczenie UF
    case STATE_DRAINING: {
      float drainedMassKg = weightKg - g_drainBaseWeightKg;
      char extra[42] = "Oczekiwanie na wyplyw...";

      if (!g_drainStarted) {
        if (drainedMassKg >= DRAIN_START_KG) {
          g_drainStarted = true;
          g_drainStartMs = millis();
          resetStabilityTracker();
        }
      } else {
        unsigned long elapsedSec = (millis() - g_drainStartMs) / 1000UL;
        float flowRate = (elapsedSec > 0) ? ((drainedMassKg * 1000.0f) / (elapsedSec / 60.0f)) : 0.0f;

        if (!g_isStabilizing || fabs(weightKg - g_refStableWeight) > STABILITY_WINDOW_KG) {
          g_refStableWeight = weightKg;
          g_stableSinceMs = millis();
          g_isStabilizing = true;
          sprintf(extra, "Czas: %lus | %.0f g/min", elapsedSec, flowRate);
        } else {
          // Waga ustabilizowana - odliczamy 5 sekund po stabilizacji
          unsigned long postStableMs = millis() - g_stableSinceMs;
          sprintf(extra, "Koniec za: %.1fs (%.0fg/m)", max(0.0f, (5000.0f - postStableMs) / 1000.0f), flowRate);

          if (postStableMs >= 5000) {
            g_drainDurationSec = elapsedSec;
            float finalFlowRate = (g_drainDurationSec > 0) ? ((drainedMassKg * 1000.0f) / (g_drainDurationSec / 60.0f)) : 0.0f;

            // Obliczenie ultrafiltracji: aktualny drenaż minus poprzedni świeży worek z pkt 2
            float ufKg = 0.0f;
            if (g_hasPrevFresh) {
              ufKg = drainedMassKg - g_prevFreshWeightKg;
            }

            M5.Lcd.fillScreen(BLACK);
            M5.Lcd.setTextColor(YELLOW, BLACK);
            M5.Lcd.setTextSize(3);
            M5.Lcd.drawCentreString("Zapis drenazu...", 160, 85, 1);
            if (g_hasPrevFresh) {
              char ufBuf[32];
              sprintf(ufBuf, "UF: %+.0f ml", ufKg * 1000.0f);
              M5.Lcd.drawCentreString(ufBuf, 160, 125, 1);
            }

            bool ok = sendRecordToFirestore("drain", drainedMassKg, ufKg, g_drainDurationSec, finalFlowRate);
            playBeep(1500, 200, ok ? 2 : 3);

            // Teraz aktualny świeży worek (z kroku 2) staje się "poprzednim" dla kolejnego cyklu!
            if (prefs.getBool("has_next", false)) {
              g_prevFreshWeightKg = prefs.getFloat("next_fresh", 0.0f);
              g_hasPrevFresh = true;
              prefs.putFloat("prev_fresh", g_prevFreshWeightKg);
              prefs.putBool("has_prev", true);
            }

            // Przejście do kroku 4 (czyszczenie układu / płukanie drenu)
            g_flushStartWeightKg = weightKg;
            resetStabilityTracker();
            M5.Lcd.fillScreen(BLACK);
            g_state = STATE_FLUSHING;
            break;
          }
        }
      }
      drawStatusScreen(drainedMassKg, "3. DRENOWANIE", MAROON, g_drainStarted ? "Pomiar przeplywu..." : "Podlacz drenaz", extra);
      break;
    }

    // KROK 4: Czyszczenie układu - sygnał dźwiękowy po wzroście masy o FLUSH_DELTA_KG
    case STATE_FLUSHING: {
      float flushDeltaKg = weightKg - g_flushStartWeightKg;
      char extra[42];
      sprintf(extra, "Przyrost: %.1f / %.1f g", flushDeltaKg * 1000.0f, FLUSH_DELTA_KG * 1000.0f);

      if (flushDeltaKg >= FLUSH_DELTA_KG) {
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setTextColor(GREEN, BLACK);
        M5.Lcd.setTextSize(3);
        M5.Lcd.drawCentreString("PLUKANIE OK!", 160, 95, 1);
        M5.Lcd.setTextSize(2);
        M5.Lcd.drawCentreString("Rozpocznij wplyw plynu", 160, 135, 1);

        // Sygnał dźwiękowy kończący czyszczenie układu
        playBeep(2000, 300, 3);
        delay(2000);

        // Powrót do kroku 1/2 na kolejny cykl wymiany
        performTare();
        g_drainStarted = false;
        g_state = STATE_WEIGH_FRESH;
        break;
      }

      drawStatusScreen(weightKg, "4. CZYSZCZENIE UKLADU", PURPLE, "Plukanie drenu...", extra);
      break;
    }
  }

  delay(80);
}
