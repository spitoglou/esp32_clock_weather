/*
  ESP32-WROOM-32 NTP Clock + Weather + WiFi Config Portal
  --------------------------------------------------------
  - TM1637: HH:MM with blinking colon (continues running even in AP mode)
  - SSD1306 OLED: date, temperature, condition, wind, WiFi info
  - Weather from Open-Meteo (free, no API key)
  - Captive-portal WiFi setup at first boot or when saved creds fail
  - Auto-fallback to portal after 10 min offline mid-run
  - Onboard LED blinks at 1 Hz while in AP/setup mode
  - Hold BOOT button on power-on to force re-configuration

  Hardware:
    TM1637  — CLK=GPIO19, DIO=GPIO18
    SSD1306 — I2C SDA=GPIO21, SCL=GPIO22
    LED     — GPIO2 (onboard, active HIGH on most WROOM-32 dev boards)

  Libraries (Library Manager):
    - TM1637Display
    - Adafruit SSD1306   (pulls in GFX + BusIO)
    - ArduinoJson        (v7+)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TM1637Display.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
const int LED_PIN = LED_BUILTIN;   // GPIO2 on standard WROOM-32 dev boards

// ---------- Timezone (Greece) ----------
const char* TZ_INFO = "EET-2EEST,M3.5.0/3,M10.5.0/4";

// ---------- Weather (Athens) ----------
const float LATITUDE  = 37.9838f;
const float LONGITUDE = 23.7275f;
const unsigned long WEATHER_INTERVAL_MS = 15UL * 60UL * 1000UL;
const unsigned long WEATHER_RETRY_MS    = 60UL * 1000UL;
const unsigned long WEATHER_FIRST_DELAY_MS = 3000UL;   // settle DNS/NTP after WiFi up

// ---------- TM1637 ----------
#define TM_CLK 19
#define TM_DIO 18
TM1637Display segDisplay(TM_CLK, TM_DIO);

// Brightness schedule (TM1637 only). Levels 0..7; 7 = max.
static const uint8_t BRIGHT_DAY     = 7;
static const uint8_t BRIGHT_NIGHT   = 3;
static const int     DAY_START_HOUR = 7;    // inclusive
static const int     DAY_END_HOUR   = 22;   // exclusive

// ---------- OLED ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Adafruit GFX default font is 6x8 px at size 1; multiply by textSize
static const int GFX_CHAR_W   = 6;
static const int GFX_CHAR_H   = 8;
// Degree-circle + gap to 'C' glyph at text size 3 (see drawDegreeAndC)
static const int DEG_GAP_S3   = 8;

// ---------- WiFi reconnect tuning ----------
const unsigned long WIFI_BOOT_TIMEOUT_MS   = 20000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
const unsigned long OFFLINE_THRESHOLD_MS   = 10UL * 60UL * 1000UL;
const unsigned long AP_LED_BLINK_MS        = 500;   // half-period (1 Hz overall)
const unsigned long COLON_BLINK_MS         = 500;   // half-period (1 Hz overall)

// ---------- Config portal ----------
const int        CONFIG_BUTTON = 0;
const byte       DNS_PORT      = 53;
const IPAddress  AP_IP(192, 168, 4, 1);
WebServer        server(80);
DNSServer        dnsServer;
Preferences      prefs;
String           apSsid;
String           savedSsid;
String           savedPass;

// ---------- State ----------
unsigned long lastReconnectAttempt = 0;
unsigned long lastConnectedAt      = 0;
int           lastSecondRendered   = -1;
unsigned long lastNoTimeRender     = 0;
unsigned long lastWeatherAttempt   = 0;
unsigned long weatherIntervalNow   = 0;
unsigned long lastScanStartedAt    = 0;
const unsigned long SCAN_CACHE_MS  = 30000;

struct WeatherData {
  // current
  bool  valid       = false;
  float temperature = 0;
  int   humidity    = 0;
  float windSpeed   = 0;
  int   code        = -1;

  // daily — index 0 = today, 1 = tomorrow, 2 = day after
  bool  dailyValid   = false;
  float dailyMax[3]  = {0, 0, 0};
  float dailyMin[3]  = {0, 0, 0};
  int   dailyCode[3] = {-1, -1, -1};
  char  todaySunrise[6] = {0};   // "HH:MM"
  char  todaySunset[6]  = {0};

  // hourly precipitation probability — 12 entries starting at hourlyStartHour
  bool    hourlyValid       = false;
  uint8_t precipProb[12]    = {0};
  int     hourlyStartHour   = 0;
} weather;

// ---------- OLED page rotation ----------
enum Page {
  PAGE_MAIN = 0,
  PAGE_FORECAST,
  PAGE_PRECIP,
  PAGE_SUN,
  PAGE_SYSTEM,
  PAGE_COUNT
};
static const unsigned long PAGE_DURATIONS_MS[PAGE_COUNT] = {
  10000UL,   // MAIN
   6000UL,   // FORECAST
   6000UL,   // PRECIP
   6000UL,   // SUN
   6000UL,   // SYSTEM
};
static Page          currentPage   = PAGE_MAIN;
static unsigned long pageEnteredAt = 0;

void runConfigPortal();   // forward decl

// ============================================================================
//                               DISPLAY INIT
// ============================================================================
void initSegment() {
  segDisplay.setBrightness(BRIGHT_DAY);
  segDisplay.clear();
}

void initOLED() {
  Wire.begin(21, 22);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 not found"));
    while (true) delay(100);
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println(F("Booting..."));
  oled.display();
}

// ============================================================================
//                          CREDENTIALS PERSISTENCE
// ============================================================================
void loadCredentials() {
  prefs.begin("clockcfg", true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();
}

void saveCredentials(const String& ssid, const String& pass) {
  prefs.begin("clockcfg", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

String makeApSsid() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return String("ESP32-Clock-") + mac.substring(8);
}

// ============================================================================
//                         WIFI CONNECTION (saved creds)
// ============================================================================
bool connectSavedWiFi(unsigned long timeoutMs) {
  if (savedSsid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());

  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.println(F("Connecting to:"));
  oled.println(savedSsid);
  oled.display();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void manageWiFi() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    lastConnectedAt = now;
    return;
  }

  if (now - lastConnectedAt > OFFLINE_THRESHOLD_MS) {
    Serial.println(F("Offline > threshold — dropping into config portal"));
    runConfigPortal();
  }

  if (now - lastReconnectAttempt < WIFI_RETRY_INTERVAL_MS) return;

  Serial.println(F("WiFi down — retrying..."));
  WiFi.disconnect();
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  lastReconnectAttempt = now;
}

// ============================================================================
//                       TM1637 — HH:MM with blinking colon
// ============================================================================
uint8_t segBrightnessFor(int hour) {
  return (hour >= DAY_START_HOUR && hour < DAY_END_HOUR)
         ? BRIGHT_DAY : BRIGHT_NIGHT;
}

void renderSegment(bool haveTime, const struct tm& t, bool colonOn) {
  if (haveTime) {
    segDisplay.setBrightness(segBrightnessFor(t.tm_hour));
    int value = t.tm_hour * 100 + t.tm_min;
    uint8_t mask = colonOn ? 0b01000000 : 0;
    segDisplay.showNumberDecEx(value, mask, true);
  } else {
    const uint8_t dashes[] = { 0x40, 0x40, 0x40, 0x40 };
    segDisplay.setSegments(dashes);
  }
}

// ============================================================================
//                            CONFIG PORTAL — HTML
// ============================================================================
String htmlEscape(const String& s) {
  String r;
  r.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '<':  r += "&lt;";   break;
      case '>':  r += "&gt;";   break;
      case '&':  r += "&amp;";  break;
      case '"':  r += "&quot;"; break;
      case '\'': r += "&#39;";  break;
      default:   r += c;
    }
  }
  return r;
}

const char PAGE_CSS[] PROGMEM = R"=====(
* { box-sizing: border-box; }
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  max-width: 480px; margin: 0 auto; padding: 20px;
  background: #f5f5f7; color: #1d1d1f; }
h1 { font-size: 1.5em; margin-bottom: 4px; }
.subtitle { color: #666; font-size: 0.9em; margin-top: 0; margin-bottom: 24px; }
fieldset { border: none; padding: 0; margin: 0 0 16px 0;
  background: white; border-radius: 12px; overflow: hidden; }
legend { font-size: 0.85em; color: #666; padding: 8px 12px; }
.network { display: flex; align-items: center; padding: 12px;
  border-bottom: 1px solid #eee; cursor: pointer; }
.network:last-child { border-bottom: none; }
.network input[type=radio] { margin-right: 12px; }
.ssid { flex: 1; font-weight: 500; word-break: break-all; }
.lock { color: #888; margin-right: 8px; font-size: 0.9em; }
.rssi { color: #888; font-size: 0.85em; }
label.field { display: block; margin: 12px 0 4px; font-size: 0.9em; color: #444; }
input[type=text], input[type=password] { width: 100%; padding: 12px;
  border: 1px solid #ddd; border-radius: 8px; font-size: 16px; background: white; }
button { width: 100%; padding: 14px; background: #0066cc; color: white;
  border: none; border-radius: 8px; font-size: 16px; font-weight: 600;
  margin-top: 16px; cursor: pointer; }
button:active { background: #004499; }
.hint { font-size: 0.85em; color: #666; margin-top: 16px; }
a.cancel { display: block; text-align: center; margin-top: 12px;
  color: #666; font-size: 0.9em; text-decoration: none; padding: 10px; }
a.cancel:hover { color: #000; }
)=====";

void ensureScan() {
  int s = WiFi.scanComplete();
  if (s == WIFI_SCAN_RUNNING) return;
  unsigned long now = millis();
  if (s >= 0 && now - lastScanStartedAt < SCAN_CACHE_MS) return;
  if (s >= 0) WiFi.scanDelete();   // discard stale results before re-scanning
  Serial.println(F("Starting async WiFi scan..."));
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false,
                    /*passive=*/false, /*max_ms_per_chan=*/300);
  lastScanStartedAt = now;
}

void handleRoot() {
  ensureScan();
  int n = WiFi.scanComplete();
  bool scanReady = (n >= 0);

  String html;
  html.reserve(4096);
  html += F("<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (!scanReady) html += F("<meta http-equiv='refresh' content='2'>");
  html += F("<title>ESP32 Clock Setup</title><style>");
  html += FPSTR(PAGE_CSS);
  html += F("</style></head><body>"
            "<h1>WiFi Setup</h1><p class='subtitle'>ESP32 NTP Clock</p>"
            "<form action='/save' method='POST'>"
            "<fieldset><legend>Available networks</legend>");

  if (!scanReady) {
    html += F("<div style='padding:14px;color:#888'>Scanning&hellip;</div>");
  } else if (n == 0) {
    html += F("<div style='padding:14px;color:#888'>No networks found. "
              "<a href='/'>Rescan</a></div>");
  } else {
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      int rssi    = WiFi.RSSI(i);
      bool secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      html += F("<label class='network'>");
      html += F("<input type='radio' name='ssid' value='");
      html += htmlEscape(ssid);
      html += F("'>");
      html += F("<span class='ssid'>");
      html += htmlEscape(ssid);
      html += F("</span>");
      html += F("<span class='lock'>");
      html += secure ? F("&#128274;") : F("");
      html += F("</span>");
      html += F("<span class='rssi'>");
      html += rssi;
      html += F(" dBm</span></label>");
    }
  }

  html += F("</fieldset>"
            "<label class='field' for='manual'>Or enter SSID manually:</label>"
            "<input type='text' id='manual' name='manual_ssid' placeholder='Hidden network'>"
            "<label class='field' for='pass'>Password:</label>"
            "<input type='password' id='pass' name='pass' placeholder='Leave blank for open network'>"
            "<button type='submit'>Save &amp; Connect</button>"
            "<p class='hint'>The device will reboot and try the new credentials.</p>"
            "</form><a class='cancel' href='/cancel'>Cancel &amp; reboot</a>"
            "</body></html>");

  server.send(200, "text/html", html);
  // Cached scan results are kept for SCAN_CACHE_MS so rapid reloads don't re-scan
}

void handleSave() {
  String ssid = server.arg("ssid");
  if (ssid.length() == 0) ssid = server.arg("manual_ssid");
  String pass = server.arg("pass");
  ssid.trim();
  pass.trim();

  if (ssid.length() == 0) {
    server.send(400, "text/html",
      "<h2>SSID required</h2><p><a href='/'>Back</a></p>");
    return;
  }
  if (ssid.length() > 32) {
    server.send(400, "text/html",
      "<h2>SSID too long (max 32)</h2><p><a href='/'>Back</a></p>");
    return;
  }
  if (pass.length() > 63) {
    server.send(400, "text/html",
      "<h2>Password too long (max 63)</h2><p><a href='/'>Back</a></p>");
    return;
  }

  saveCredentials(ssid, pass);

  String body = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>Saved</title><style>body{font-family:sans-serif;"
                  "max-width:480px;margin:40px auto;padding:20px;text-align:center;}"
                  "</style></head><body><h1>Saved!</h1>"
                  "<p>The device will reboot and try connecting to <b>");
  body += htmlEscape(ssid);
  body += F("</b>.</p><p>You can disconnect from the setup network now.</p>"
            "</body></html>");
  server.send(200, "text/html", body);

  Serial.println(F("Credentials saved — rebooting..."));
  delay(1500);
  ESP.restart();
}

void handleNotFound() {
  server.sendHeader("Location", String("http://") + AP_IP.toString(), true);
  server.send(302, "text/plain", "");
}

void handleCancel() {
  server.send(200, "text/html",
    F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Rebooting</title><style>body{font-family:sans-serif;"
      "max-width:480px;margin:40px auto;padding:20px;text-align:center;}"
      "</style></head><body><h1>Rebooting...</h1>"
      "<p>The device will restart and reuse the previously saved network "
      "(if any).</p></body></html>"));
  Serial.println(F("Cancel pressed — rebooting..."));
  delay(800);
  ESP.restart();
}

void renderConfigPortalOLED() {
  oled.clearDisplay();
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  oled.println(F("== SETUP MODE =="));

  oled.setCursor(0, 12);
  oled.print(F("WiFi: "));
  oled.println(apSsid);

  oled.setCursor(0, 26);
  oled.println(F("Then open:"));
  oled.setCursor(0, 36);
  oled.print(F("http://"));
  oled.println(AP_IP);

  oled.setCursor(0, 56);
  oled.print(F("Clients: "));
  oled.print(WiFi.softAPgetStationNum());

  oled.display();
}

void runConfigPortal() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_AP_STA);   // STA enabled so background scans don't break the AP
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid.c_str());

  dnsServer.start(DNS_PORT, "*", AP_IP);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/cancel", HTTP_GET, handleCancel);
  server.onNotFound(handleNotFound);
  server.begin();

  ensureScan();   // kick off async scan so the captive page lands fast

  Serial.print(F("Config portal up: AP \""));
  Serial.print(apSsid);
  Serial.print(F("\" at http://"));
  Serial.println(AP_IP);

  unsigned long lastOledUpdate    = 0;
  unsigned long lastLedToggle     = 0;
  unsigned long lastColonToggle   = 0;
  bool          ledOn             = false;
  bool          colonOn           = false;
  bool          segShowingDashes  = false;

  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();

    unsigned long now = millis();

    // (a) Onboard LED — 1 Hz blink to advertise AP/setup mode
    if (now - lastLedToggle > AP_LED_BLINK_MS) {
      ledOn = !ledOn;
      digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
      lastLedToggle = now;
    }

    // (b) Segment clock keeps running on the free-running system clock
    struct tm t = {};
    bool haveTime = getLocalTime(&t, 5);

    if (now - lastColonToggle >= COLON_BLINK_MS) {
      colonOn = !colonOn;
      lastColonToggle = now;
      if (haveTime) {
        renderSegment(true, t, colonOn);
        segShowingDashes = false;
      }
    }
    if (!haveTime && !segShowingDashes) {
      // No NTP sync ever happened — show dashes once, leave them
      renderSegment(false, t, false);
      segShowingDashes = true;
    }

    // OLED — refresh once per second
    if (now - lastOledUpdate > 1000) {
      renderConfigPortalOLED();
      lastOledUpdate = now;
    }

    delay(2);
  }
}

// ============================================================================
//                                    TIME
// ============================================================================
void initTime() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  setenv("TZ", TZ_INFO, 1);
  tzset();
}

// ============================================================================
//                                  WEATHER
// ============================================================================
const char* weatherDescription(int code) {
  switch (code) {
    case 0:                     return "Clear";
    case 1:                     return "Mainly clear";
    case 2:                     return "Partly cloudy";
    case 3:                     return "Overcast";
    case 45: case 48:           return "Fog";
    case 51: case 53: case 55:  return "Drizzle";
    case 56: case 57:           return "Frz drizzle";
    case 61:                    return "Light rain";
    case 63:                    return "Rain";
    case 65:                    return "Heavy rain";
    case 66: case 67:           return "Frz rain";
    case 71:                    return "Light snow";
    case 73:                    return "Snow";
    case 75:                    return "Heavy snow";
    case 77:                    return "Snow grains";
    case 80: case 81:           return "Showers";
    case 82:                    return "Hvy showers";
    case 85: case 86:           return "Snow shwrs";
    case 95:                    return "T-storm";
    case 96: case 99:           return "T-storm hail";
    default:                    return "Unknown";
  }
}

// Short form (<=7 chars) used by the 3-day forecast page
const char* weatherDescriptionShort(int code) {
  switch (code) {
    case 0: case 1:             return "Clear";
    case 2:                     return "PtCloud";
    case 3:                     return "Cloudy";
    case 45: case 48:           return "Fog";
    case 51: case 53: case 55:  return "Drizzle";
    case 56: case 57:           return "FzDrzl";
    case 61:                    return "LtRain";
    case 63:                    return "Rain";
    case 65:                    return "HvRain";
    case 66: case 67:           return "FzRain";
    case 71:                    return "LtSnow";
    case 73:                    return "Snow";
    case 75:                    return "HvSnow";
    case 77:                    return "Grains";
    case 80: case 81:           return "Showers";
    case 82:                    return "HvShwrs";
    case 85: case 86:           return "SnShwrs";
    case 95:                    return "Storm";
    case 96: case 99:           return "Storm+";
    default:                    return "?";
  }
}

// Copies "HH:MM" out of an Open-Meteo "YYYY-MM-DDTHH:MM" timestamp.
// `out` must hold at least 6 bytes.
static void extractTimeOfDay(const char* iso, char* out) {
  if (!iso || strlen(iso) < 16) { out[0] = 0; return; }
  memcpy(out, iso + 11, 5);
  out[5] = 0;
}

// "06:14" + "20:39" -> "14h 25m" (wraps over midnight if needed).
static void formatDayLength(const char* sunrise, const char* sunset,
                            char* out, size_t outSize) {
  if (strlen(sunrise) < 5 || strlen(sunset) < 5) {
    snprintf(out, outSize, "--");
    return;
  }
  int srM = (sunrise[0]-'0')*600 + (sunrise[1]-'0')*60
          + (sunrise[3]-'0')*10  + (sunrise[4]-'0');
  int ssM = (sunset[0]-'0')*600  + (sunset[1]-'0')*60
          + (sunset[3]-'0')*10   + (sunset[4]-'0');
  int diff = ssM - srM;
  if (diff < 0) diff += 24 * 60;
  snprintf(out, outSize, "%dh %02dm", diff / 60, diff % 60);
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10);   // seconds — caps the TLS handshake
  HTTPClient http;
  http.setTimeout(10000);

  String url = String("https://api.open-meteo.com/v1/forecast?latitude=")
             + String(LATITUDE, 4) + "&longitude=" + String(LONGITUDE, 4)
             + "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
             + "&daily=sunrise,sunset,temperature_2m_max,temperature_2m_min,weather_code"
             + "&hourly=precipitation_probability"
             + "&forecast_days=3"
             + "&forecast_hours=12"
             + "&timezone=auto";

  if (!http.begin(client, url)) {
    Serial.println(F("Weather: http.begin failed"));
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("Weather HTTP error: %d\n", code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print(F("Weather JSON error: "));
    Serial.println(err.c_str());
    return false;
  }

  JsonObject current = doc["current"];
  weather.temperature = current["temperature_2m"]      | 0.0f;
  weather.humidity    = current["relative_humidity_2m"]| 0;
  weather.windSpeed   = current["wind_speed_10m"]      | 0.0f;
  weather.code        = current["weather_code"]        | -1;
  weather.valid       = true;

  // Daily (next 3 days)
  JsonObject daily = doc["daily"];
  JsonArray dMax  = daily["temperature_2m_max"];
  JsonArray dMin  = daily["temperature_2m_min"];
  JsonArray dCode = daily["weather_code"];
  JsonArray dSr   = daily["sunrise"];
  JsonArray dSs   = daily["sunset"];
  if (!dMax.isNull() && !dMin.isNull() && !dCode.isNull() && dMax.size() >= 3) {
    for (int i = 0; i < 3; i++) {
      weather.dailyMax[i]  = dMax[i]  | 0.0f;
      weather.dailyMin[i]  = dMin[i]  | 0.0f;
      weather.dailyCode[i] = dCode[i] | -1;
    }
    if (!dSr.isNull() && dSr.size() > 0) {
      extractTimeOfDay(dSr[0] | "", weather.todaySunrise);
      extractTimeOfDay(dSs[0] | "", weather.todaySunset);
    }
    weather.dailyValid = true;
  }

  // Hourly precipitation probability — find entry matching "now" hour, then
  // copy the next 12 values. With forecast_hours=12 the array is already
  // aligned, but this lookup also handles longer arrays defensively.
  JsonObject hourly = doc["hourly"];
  JsonArray  hTime  = hourly["time"];
  JsonArray  hProb  = hourly["precipitation_probability"];
  if (!hTime.isNull() && !hProb.isNull() && hProb.size() > 0) {
    int startIdx = 0;
    struct tm tNow = {};
    if (getLocalTime(&tNow, 100)) {
      char target[14];
      strftime(target, sizeof(target), "%Y-%m-%dT%H", &tNow);
      for (size_t i = 0; i < hTime.size(); i++) {
        const char* tStr = hTime[i] | "";
        if (strncmp(tStr, target, 13) == 0) { startIdx = (int)i; break; }
      }
    }
    int available = (int)hProb.size() - startIdx;
    int n = available < 12 ? available : 12;
    for (int i = 0; i < 12; i++) {
      weather.precipProb[i] = (i < n) ? (uint8_t)(hProb[startIdx + i] | 0) : 0;
    }
    const char* t0 = hTime[startIdx] | "";
    if (strlen(t0) >= 13) {
      weather.hourlyStartHour = (t0[11]-'0')*10 + (t0[12]-'0');
    }
    weather.hourlyValid = true;
  }

  Serial.printf("Weather: %.1f°C, %s, %d%% RH, %.1f km/h\n",
                weather.temperature, weatherDescription(weather.code),
                weather.humidity, weather.windSpeed);
  if (weather.dailyValid) {
    Serial.printf("Daily: today %.0f/%.0f, tomorrow %.0f/%.0f, sunrise %s sunset %s\n",
                  weather.dailyMax[0], weather.dailyMin[0],
                  weather.dailyMax[1], weather.dailyMin[1],
                  weather.todaySunrise, weather.todaySunset);
  }
  return true;
}

void manageWeather() {
  if (millis() - lastWeatherAttempt < weatherIntervalNow) return;

  if (WiFi.status() != WL_CONNECTED) {
    lastWeatherAttempt = millis();
    weatherIntervalNow = WEATHER_RETRY_MS;
    return;
  }

  bool ok = fetchWeather();
  lastWeatherAttempt = millis();
  weatherIntervalNow = ok ? WEATHER_INTERVAL_MS : WEATHER_RETRY_MS;
}

// ============================================================================
//                         OLED — date + weather + WiFi
// ============================================================================
void drawDegreeAndC(int x, int y, uint8_t textSize) {
  int r = (textSize >= 3) ? 2 : 1;
  oled.drawCircle(x + r, y + r, r, SSD1306_WHITE);
  oled.setTextSize(textSize);
  oled.setCursor(x + r * 2 + 2, y);
  oled.print('C');
}

void renderOLED(bool haveTime, const struct tm& t) {
  oled.clearDisplay();

  // Top: date + humidity
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  if (haveTime) {
    char dateStr[20];
    strftime(dateStr, sizeof(dateStr), "%a %d %b %Y", &t);
    oled.print(dateStr);
  } else {
    oled.print(F("Syncing..."));
  }
  if (weather.valid) {
    char humStr[8];
    snprintf(humStr, sizeof(humStr), "%d%%", weather.humidity);
    int humW = strlen(humStr) * GFX_CHAR_W;
    oled.setCursor(SCREEN_WIDTH - humW, 0);
    oled.print(humStr);
  }

  // Middle: big temperature
  const int BIG_W = GFX_CHAR_W * 3;        // size-3 glyph width
  oled.setTextSize(3);
  if (weather.valid) {
    char tempStr[8];
    int tempInt = (int)round(weather.temperature);
    snprintf(tempStr, sizeof(tempStr), "%d", tempInt);
    int numW   = strlen(tempStr) * BIG_W;
    int totalW = numW + DEG_GAP_S3 + BIG_W;   // digits + degree symbol + 'C'
    int startX = (SCREEN_WIDTH - totalW) / 2;
    oled.setCursor(startX, 16);
    oled.print(tempStr);
    drawDegreeAndC(startX + numW + 2, 16, 3);
  } else {
    int placeholderW = 2 * BIG_W;
    int totalW       = placeholderW + DEG_GAP_S3 + BIG_W;
    int startX       = (SCREEN_WIDTH - totalW) / 2;
    oled.setCursor(startX, 16);
    oled.print(F("--"));
    drawDegreeAndC(startX + placeholderW + 2, 16, 3);
  }

  // Lower: condition + wind
  oled.setTextSize(1);
  oled.setCursor(0, 44);
  oled.print(weather.valid ? weatherDescription(weather.code) : "Loading...");
  if (weather.valid) {
    char windStr[12];
    snprintf(windStr, sizeof(windStr), "%.0f km/h", weather.windSpeed);
    int windW = strlen(windStr) * GFX_CHAR_W;
    oled.setCursor(SCREEN_WIDTH - windW, 44);
    oled.print(windStr);
  }

  // Bottom: WiFi info, with countdown to portal when offline
  oled.setCursor(0, 56);
  if (WiFi.status() == WL_CONNECTED) {
    oled.print(WiFi.localIP());
    char rssi[10];
    snprintf(rssi, sizeof(rssi), "%ddBm", WiFi.RSSI());
    int rssiW = strlen(rssi) * GFX_CHAR_W;
    oled.setCursor(SCREEN_WIDTH - rssiW, 56);
    oled.print(rssi);
  } else {
    unsigned long offlineMs = millis() - lastConnectedAt;
    unsigned long remainMs  = (offlineMs >= OFFLINE_THRESHOLD_MS)
                              ? 0 : (OFFLINE_THRESHOLD_MS - offlineMs);
    int remainSec = remainMs / 1000;
    char buf[24];
    snprintf(buf, sizeof(buf), "Retry %d:%02d", remainSec / 60, remainSec % 60);
    oled.print(buf);
    oled.setCursor(SCREEN_WIDTH - 5 * GFX_CHAR_W, 56);
    oled.print(F("->cfg"));
  }

  oled.display();
}

// ============================================================================
//                       OLED — additional informational pages
// ============================================================================
void renderForecastPage(bool haveTime, const struct tm& t) {
  oled.clearDisplay();
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  oled.print(F("3-day forecast"));

  if (!weather.dailyValid) {
    oled.setCursor(0, 28);
    oled.print(F("Loading..."));
    oled.display();
    return;
  }

  static const char* const DOWS[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  for (int i = 0; i < 3; i++) {
    int y = 16 + i * 14;

    oled.setCursor(0, y);
    if (haveTime) oled.print(DOWS[(t.tm_wday + i) % 7]);
    else          oled.print(F("---"));

    char tempStr[12];
    snprintf(tempStr, sizeof(tempStr), "%2d/%2d",
             (int)round(weather.dailyMax[i]),
             (int)round(weather.dailyMin[i]));
    oled.setCursor(28, y);
    oled.print(tempStr);

    oled.setCursor(74, y);
    oled.print(weatherDescriptionShort(weather.dailyCode[i]));
  }

  oled.display();
}

void renderPrecipPage() {
  oled.clearDisplay();
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  oled.print(F("Precip prob 12h"));

  if (!weather.hourlyValid) {
    oled.setCursor(0, 28);
    oled.print(F("Loading..."));
    oled.display();
    return;
  }

  const int BAR_W      = 8;
  const int BAR_PITCH  = 10;
  const int BAR_LEFT   = 4;
  const int BAR_BOTTOM = 54;
  const int BAR_MAX_H  = 38;

  for (int i = 0; i < 12; i++) {
    int x    = BAR_LEFT + i * BAR_PITCH;
    int prob = weather.precipProb[i];
    int h    = (prob * BAR_MAX_H) / 100;
    if (h < 1 && prob > 0) h = 1;
    if (h > 0) oled.fillRect(x, BAR_BOTTOM - h, BAR_W, h, SSD1306_WHITE);
    // 1px baseline tick so empty hours still show
    oled.drawPixel(x + BAR_W / 2, BAR_BOTTOM, SSD1306_WHITE);
  }

  // Footer labels: start hour at left, "+12h" at right
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:00", weather.hourlyStartHour);
  oled.setCursor(0, 56);
  oled.print(buf);
  oled.setCursor(SCREEN_WIDTH - 4 * GFX_CHAR_W, 56);
  oled.print(F("+12h"));

  oled.display();
}

void renderSunPage() {
  oled.clearDisplay();
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  oled.print(F("Sun"));

  if (!weather.dailyValid || weather.todaySunrise[0] == 0) {
    oled.setCursor(0, 28);
    oled.print(F("Loading..."));
    oled.display();
    return;
  }

  oled.setCursor(0, 20);
  oled.print(F("Sunrise   "));
  oled.print(weather.todaySunrise);

  oled.setCursor(0, 34);
  oled.print(F("Sunset    "));
  oled.print(weather.todaySunset);

  char dayLen[12];
  formatDayLength(weather.todaySunrise, weather.todaySunset,
                  dayLen, sizeof(dayLen));
  oled.setCursor(0, 48);
  oled.print(F("Day len   "));
  oled.print(dayLen);

  oled.display();
}

void renderSystemPage() {
  oled.clearDisplay();
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  oled.print(F("System"));

  unsigned long s = millis() / 1000;
  int days  = s / 86400; s %= 86400;
  int hours = s / 3600;  s %= 3600;
  int mins  = s / 60;
  int secs  = s % 60;

  char buf[24];

  // Body rows start at y=16 so nothing straddles the yellow/blue boundary
  // on bicolor SSD1306 panels (yellow band = rows 0..15).
  oled.setCursor(0, 16);
  oled.print(F("Up   "));
  if (days > 0) snprintf(buf, sizeof(buf), "%dd %02d:%02d:%02d", days, hours, mins, secs);
  else          snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours, mins, secs);
  oled.print(buf);

  oled.setCursor(0, 26);
  oled.print(F("IP   "));
  if (WiFi.status() == WL_CONNECTED) oled.print(WiFi.localIP());
  else                               oled.print(F("--"));

  oled.setCursor(0, 36);
  oled.print(F("RSSI "));
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(buf, sizeof(buf), "%d dBm", WiFi.RSSI());
    oled.print(buf);
  } else {
    oled.print(F("--"));
  }

  oled.setCursor(0, 46);
  oled.print(F("Heap "));
  snprintf(buf, sizeof(buf), "%u KB free", (unsigned)(ESP.getFreeHeap() / 1024));
  oled.print(buf);

  oled.setCursor(0, 56);
  oled.print(F("SSID "));
  oled.print(savedSsid);

  oled.display();
}

void renderOLEDDispatch(bool haveTime, const struct tm& t) {
  switch (currentPage) {
    case PAGE_MAIN:     renderOLED(haveTime, t);         break;
    case PAGE_FORECAST: renderForecastPage(haveTime, t); break;
    case PAGE_PRECIP:   renderPrecipPage();              break;
    case PAGE_SUN:      renderSunPage();                 break;
    case PAGE_SYSTEM:   renderSystemPage();              break;
    default: break;
  }
}

// ============================================================================
//                            ARDUINO ENTRY POINTS
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(CONFIG_BUTTON, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);   // off by default; only blinks during AP mode

  initSegment();
  initOLED();

  loadCredentials();
  apSsid = makeApSsid();

  delay(50);
  bool forceConfig = (digitalRead(CONFIG_BUTTON) == LOW);
  if (forceConfig) {
    Serial.println(F("BOOT button held — entering config portal"));
    runConfigPortal();
  }

  if (!connectSavedWiFi(WIFI_BOOT_TIMEOUT_MS)) {
    Serial.println(F("Could not connect with saved creds — entering config portal"));
    runConfigPortal();
  }

  Serial.print(F("WiFi connected: "));
  Serial.println(WiFi.localIP());

  unsigned long now = millis();
  lastReconnectAttempt = now;
  lastConnectedAt      = now;
  lastWeatherAttempt   = now;
  weatherIntervalNow   = WEATHER_FIRST_DELAY_MS;
  pageEnteredAt        = now;

  initTime();
}

void loop() {
  manageWiFi();
  manageWeather();

  static bool          colonOn         = false;
  static unsigned long lastColonToggle = 0;

  struct tm t;
  bool haveTime = getLocalTime(&t, 5);

  unsigned long now = millis();
  if (now - lastColonToggle >= COLON_BLINK_MS) {
    colonOn = !colonOn;
    lastColonToggle = now;
    if (haveTime) renderSegment(true, t, colonOn);
  }

  // Advance OLED page after its duration elapses
  bool pageChanged = false;
  if (now - pageEnteredAt >= PAGE_DURATIONS_MS[currentPage]) {
    currentPage = (Page)((currentPage + 1) % PAGE_COUNT);
    pageEnteredAt = now;
    pageChanged = true;
  }

  if (haveTime) {
    if (t.tm_sec != lastSecondRendered || pageChanged) {
      lastSecondRendered = t.tm_sec;
      renderOLEDDispatch(true, t);
    }
  } else {
    if (now - lastNoTimeRender > 1000 || pageChanged) {
      lastNoTimeRender = now;
      struct tm dummy = {};
      renderSegment(false, dummy, false);
      renderOLEDDispatch(false, dummy);
    }
  }

  delay(20);
}
