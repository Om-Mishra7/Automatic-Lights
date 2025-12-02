#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <WebServer.h>
#include <ArduinoOTA.h>

/* ================= WIFI ================= */
String wifiSsid = "";
String wifiPassword = "";

/* ================= PINS ================= */
#define TRIG_PIN 5
#define ECHO_PIN 18
#define RELAY_PIN 26

/* ================= USER SETTINGS ================= */
int DETECTION_DISTANCE_CM = 135;
unsigned long MOTION_ON_TIME_MS = 120000;

int alwaysOnStart = 18;
int alwaysOnEnd = 22;
int motionStart = 0;
int motionEnd = 6;
int overrideMode = 0;

#define WIFI_TIMEOUT_MS 15000
#define WIFI_RETRY_INTERVAL 300000

/* ================= TIME CONFIG (IST) ================= */
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;
const int daylightOffset_sec = 0;

/* ================= TIME FALLBACK ================= */
unsigned long lastTimeSyncMillis = 0;
time_t lastUnixTime = 0;
bool timeValid = false;

/* ================= MOTION TIMER ================= */
unsigned long motionTimer = 0;
bool motionLightActive = false;

/* ================= WEB SERVER ================= */
WebServer server(80);
Preferences prefs;

/* ================= GLOBAL ================= */
float lastDistance = -1;
unsigned long lastWiFiAttempt = 0;
bool apActive = false;

/* ================= OTA SETUP ================= */
void setupOTA()
{
  ArduinoOTA.setHostname("AutoLightESP32");

  ArduinoOTA.onStart([]()
                     { Serial.println("OTA Update Start"); });

  ArduinoOTA.onEnd([]()
                   { Serial.println("OTA Update Complete"); });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { Serial.printf("OTA Progress: %u%%\n", (progress * 100) / total); });

  ArduinoOTA.onError([](ota_error_t error)
                     { Serial.printf("OTA Error[%u]\n", error); });

  ArduinoOTA.begin();
}

/* ================= SETUP ================= */
void setup()
{
  Serial.begin(115200);
  prefs.begin("AutoLight", false);
  loadSettings();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  WiFi.onEvent(WiFiEvent);
  connectWiFiAndSyncTime();

  setupOTA(); // ✅ OTA ENABLED
  setupWebServer();
}

/* ================= LOAD SETTINGS ================= */
void loadSettings()
{
  wifiSsid = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("pw", "");
  alwaysOnStart = prefs.getInt("aos", alwaysOnStart);
  alwaysOnEnd = prefs.getInt("aoe", alwaysOnEnd);
  motionStart = prefs.getInt("ms", motionStart);
  motionEnd = prefs.getInt("me", motionEnd);
  DETECTION_DISTANCE_CM = prefs.getInt("dist", DETECTION_DISTANCE_CM);
  MOTION_ON_TIME_MS = prefs.getULong("mtn", MOTION_ON_TIME_MS);
  overrideMode = prefs.getInt("ovr", overrideMode);
}

/* ================= WIFI EVENT ================= */
void WiFiEvent(arduino_event_id_t event)
{
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
  {
    Serial.println("WiFi Connected");
    apActive = false;
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
  {
    Serial.println("WiFi Disconnected");
    if (wifiSsid.length() == 0)
    {
      WiFi.softAP("AutoLight_Config");
      apActive = true;
    }
  }
}

/* ================= WIFI CONNECT ================= */
void connectWiFiAndSyncTime()
{
  WiFi.mode(WIFI_STA);

  if (wifiSsid.length() > 0)
  {
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  }
  else
  {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("AutoLight_Config");
    apActive = true;
    return;
  }

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttempt < WIFI_TIMEOUT_MS)
  {
    delay(300);
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("AutoLight_Config");
    apActive = true;
  }
}

/* ================= WIFI MAINTAIN ================= */
void maintainWiFi()
{
  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastWiFiAttempt > WIFI_RETRY_INTERVAL)
  {
    lastWiFiAttempt = millis();
    WiFi.disconnect();
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  }
}

/* ================= TIME ================= */
int getCurrentHour()
{
  struct tm timeinfo;
  if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo))
  {
    return timeinfo.tm_hour;
  }
  return -1;
}

/* ================= SENSOR ================= */
float getDistanceCM()
{
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 26000);
  if (duration == 0)
    return -1;
  return duration * 0.034 / 2;
}

float getFilteredDistance()
{
  float v[3];
  for (int i = 0; i < 3; i++)
  {
    v[i] = getDistanceCM();
    if (v[i] < 0)
      v[i] = 9999;
    delay(10);
  }
  if (v[0] > v[1])
  {
    float t = v[0];
    v[0] = v[1];
    v[1] = t;
  }
  if (v[1] > v[2])
  {
    float t = v[1];
    v[1] = v[2];
    v[2] = t;
  }
  if (v[0] > v[1])
  {
    float t = v[0];
    v[0] = v[1];
    v[1] = t;
  }
  return v[1] >= 9999 ? -1 : v[1];
}

/* ================= RELAY ================= */
void relayON() { digitalWrite(RELAY_PIN, LOW); }
void relayOFF() { digitalWrite(RELAY_PIN, HIGH); }

/* ================= WEB SERVER ================= */
void setupWebServer()
{

  server.on("/", []()
            {
    String page = "<h2>ESP32 AUTO LIGHT</h2>";
    page += "<a href='/wifi'><button>WiFi Setup</button></a><br><br>";
    page += "<a href='/distance'><button>Live Distance</button></a><br><br>";
    page += "<a href='/set'><button>Settings</button></a><br><br>";
    page += "<a href='/forgetwifi'><button>Forget WiFi</button></a><br><br>";
    server.send(200, "text/html", page); });

  server.on("/wifi", []()
            {
    String page = "<form action='/savewifi' method='POST'>";
    page += "SSID: <input name='ssid'><br>";
    page += "PASS: <input name='pw'><br>";
    page += "<input type='submit'></form>";
    server.send(200, "text/html", page); });

  server.on("/savewifi", HTTP_POST, []()
            {
    wifiSsid = server.arg("ssid");
    wifiPassword = server.arg("pw");
    prefs.putString("ssid", wifiSsid);
    prefs.putString("pw", wifiPassword);
    WiFi.disconnect();
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    server.send(200, "text/html", "Saved. <a href='/'>Back</a>"); });

  server.on("/forgetwifi", []()
            {
    prefs.clear();
    wifiSsid = "";
    wifiPassword = "";
    WiFi.disconnect();
    WiFi.softAP("AutoLight_Config");
    apActive = true;
    server.send(200, "text/html", "WiFi Cleared. <a href='/'>Back</a>"); });

  server.on("/set", []()
            {
    String page = "<form action='/savesettings' method='POST'>";
    page += "Always ON Start:<input name='aos'><br>";
    page += "Always ON End:<input name='aoe'><br>";
    page += "Motion Start:<input name='ms'><br>";
    page += "Motion End:<input name='me'><br>";
    page += "Distance:<input name='dist'><br>";
    page += "Time(ms):<input name='time'><br>";
    page += "<input type='submit'></form>";
    server.send(200, "text/html", page); });

  server.on("/savesettings", HTTP_POST, []()
            {
    alwaysOnStart = server.arg("aos").toInt();
    alwaysOnEnd   = server.arg("aoe").toInt();
    motionStart   = server.arg("ms").toInt();
    motionEnd     = server.arg("me").toInt();
    DETECTION_DISTANCE_CM = server.arg("dist").toInt();
    MOTION_ON_TIME_MS = server.arg("time").toInt();
    server.send(200, "text/html", "Saved. <a href='/'>Back</a>"); });

  server.on("/distance", []()
            {
    lastDistance = getFilteredDistance();
    server.send(200, "text/plain", String(lastDistance)); });

  server.begin();
}

/* ================= LOOP ================= */
void loop()
{
  ArduinoOTA.handle();
  server.handleClient();
  maintainWiFi();

  int hour = getCurrentHour();

  // FORCE MODES
  if (overrideMode == 1)
  {
    relayON();
    return;
  }
  if (overrideMode == 2)
  {
    relayOFF();
    return;
  }

  // ALWAYS ON MODE
  if (hour != -1 && hour >= alwaysOnStart && hour < alwaysOnEnd)
  {
    relayON();
    motionLightActive = false;
  }

  else if (hour == -1 || (hour >= motionStart && hour < motionEnd))
  {

    lastDistance = getFilteredDistance();

    if ((lastDistance > 0 && lastDistance < DETECTION_DISTANCE_CM) || lastDistance == -1)
    {
      relayON();
      motionTimer = millis();
      motionLightActive = true;
    }

    if (motionLightActive &&
        millis() - motionTimer >= MOTION_ON_TIME_MS)
    {
      relayOFF();
      motionLightActive = false;
    }
  }

  // OFF HOURS
  else
  {
    relayOFF();
    motionLightActive = false;
  }

  delay(300);
}
