#include <M5Stack.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>




#include <M5Stack.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// --- Konfiguracja Wi-Fi ---
// --- Konfiguracja Wi-Fi ---
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

// --- Konfiguracja Firebase (healthtracker-b7c01) ---
const char* PROJECT_ID    = "";
const char* API_KEY       = "";
const char* DEVICE_EMAIL  = "";
const char* DEVICE_PASS   = "";
const char* COLLECTION    = "weight_records";
// --- Konfiguracja wagi (HX711 / tensometr) ---
const int DOUT_PIN = 22; // Pin danych (Data Out)
const int SCK_PIN  = 21; // Pin zegara (Clock)

// Współczynnik kalibracji przeliczający na kilogramy
const float CALIBRATION_FACTOR = 26670.0;

long offset = 0;
float g_lastWeightKg = 0.0f;
String g_idToken = "";
unsigned long g_tokenExpiresAtMs = 0;

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

  String secStr = json.substring(firstQuote + 1, secondQuote);
  unsigned long sec = secStr.toInt();
  return (sec > 0) ? sec : 3600;
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.printf("[WiFi] Laczenie z %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Polaczono, IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("[WiFi] Nie udalo sie polaczyc.");
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

  Serial.printf("[Auth] HTTP %d\n", httpCode);
  if (httpCode < 200 || httpCode >= 300) {
    Serial.println("[Auth] Blad logowania: " + resp);
    return false;
  }

  String token = extractJsonString(resp, "idToken");
  unsigned long expiresSec = extractExpiresInSeconds(resp);

  if (token.length() == 0) {
    Serial.println("[Auth] Brak idToken w odpowiedzi.");
    return false;
  }

  g_idToken = token;
  unsigned long lifetimeMs = (expiresSec > 60) ? ((expiresSec - 60) * 1000UL) : (expiresSec * 1000UL);
  g_tokenExpiresAtMs = millis() + lifetimeMs;

  Serial.println("[Auth] Zalogowano poprawnie.");
  return true;
}

bool ensureValidIdToken() {
  if (g_idToken.length() > 0 && millis() < g_tokenExpiresAtMs) {
    return true;
  }
  return signInWithEmailPassword();
}

bool sendWeightToFirestore(float weightKgValue) {
  Serial.printf("[Firestore] Wysylanie pomiaru: %.3f kg...\n", weightKgValue);

  if (!ensureWiFiConnected()) return false;
  if (!ensureValidIdToken()) return false;

  WiFiClientSecure client;
  client.setInsecure();

  String url = "https://firestore.googleapis.com/v1/projects/" + String(PROJECT_ID) +
               "/databases/(default)/documents/" + String(COLLECTION);

  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + g_idToken);

  String payload = "{";
  payload += "\"fields\": {";
  payload += "\"weight\": {\"doubleValue\": " + String(weightKgValue, 3) + "},";
  payload += "\"unit\": {\"stringValue\": \"kg\"},";
  payload += "\"uptime_ms\": {\"integerValue\": \"" + String(millis()) + "\"}";
  payload += "}";
  payload += "}";

  int httpCode = http.POST(payload);
  String resp = http.getString();
  http.end();

  Serial.printf("[Firestore] HTTP %d\n", httpCode);
  if (httpCode == 401 || httpCode == 403) {
    g_idToken = "";
  }
  if (httpCode < 200 || httpCode >= 300) {
    Serial.println("[Firestore] Blad zapisu: " + resp);
    return false;
  }

  Serial.println("[Firestore] Zapisano pomyslnie.");
  return true;
}

// Bezpieczna funkcja odczytu odporna na przerwania systemowe
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
    if (digitalRead(DOUT_PIN)) {
      count++;
    }
  }

  digitalWrite(SCK_PIN, HIGH);
  delayMicroseconds(1);
  count ^= 0x800000;
  digitalWrite(SCK_PIN, LOW);
  delayMicroseconds(1);

  interrupts();

  return count;
}

// Funkcja odrzucająca błędne odczyty i uśredniająca poprawne
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

  if (validSamples == 0) return 0;
  return sum / validSamples;
}

// Pomocnicza funkcja do wykonywania kalibracji z komunikatem
void performCalibration() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(YELLOW, BLACK);
  M5.Lcd.setTextSize(3);
  M5.Lcd.drawCentreString("KALIBRACJA", 160, 105, 1);

  Serial.println("[Scale] Kalibracja zera za 2 s...");
  delay(2000);

  offset = getStableReading(15);
  Serial.printf("[Scale] Nowy offset: %ld\n", offset);
  M5.Lcd.fillScreen(BLACK);
}

void setup() {
  M5.begin();
  M5.Power.begin();
  Serial.begin(115200);

  pinMode(DOUT_PIN, INPUT);
  pinMode(SCK_PIN, OUTPUT);
  digitalWrite(SCK_PIN, LOW);

  // 1) Kalibracja po uruchomieniu z 2-sekundowym opóźnieniem
  performCalibration();

  // 2) Połączenie z Wi-Fi i logowanie do Firebase (status tylko w Serial Monitorze)
  ensureWiFiConnected();
  signInWithEmailPassword();
}

void loop() {
  M5.update();

  long rawValue = getStableReading(5);

  if (rawValue == 0) {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.drawCentreString("Blad czujnika!", 160, 105, 1);
    Serial.println("[Scale] Blad odczytu HX710 (rawValue == 0).");
    delay(500);
    M5.Lcd.fillScreen(BLACK);
    return;
  }

  long adjustedValue = rawValue - offset;
  float weightKg = (float)adjustedValue / CALIBRATION_FACTOR;

  if (fabs(weightKg) < 0.001f) {
    weightKg = 0.000f;
  }
  g_lastWeightKg = weightKg;

  char weightStr[16];
  sprintf(weightStr, "%.3f kg", weightKg);

  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextSize(5);

  char paddedStr[20];
  sprintf(paddedStr, " %s ", weightStr);
  M5.Lcd.drawCentreString(paddedStr, 160, 100, 1);

  // Przycisk A: wysyła aktualny pomiar do Firestore
  if (M5.BtnA.wasPressed()) {
    sendWeightToFirestore(g_lastWeightKg);
  }

  // Przycisk B: ponowne tarowanie / kalibracja zera
  if (M5.BtnB.wasPressed()) {
    performCalibration();
  }

  delay(100);
}
