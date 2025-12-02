#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <WebServer.h>

/* ================= WIFI ================= */
// WiFi credentials (loaded from preferences at startup if available)
String wifiSsid = "";
String wifiPassword = "";

/* ================= PINS ================= */
#define TRIG_PIN   5
#define ECHO_PIN   18
#define RELAY_PIN  26

/* ================= USER SETTINGS ================= */
int DETECTION_DISTANCE_CM = 135;
unsigned long MOTION_ON_TIME_MS = 120000;

int alwaysOnStart = 18;
int alwaysOnEnd   = 22;

int motionStart = 0;
int motionEnd   = 6;

int overrideMode = 0;   // 0=AUTO, 1=FORCE ON, 2=FORCE OFF

#define WIFI_TIMEOUT_MS 15000
// Retry WiFi every 5 minutes when offline (milliseconds)
#define WIFI_RETRY_INTERVAL 300000

/* ================= TIME CONFIG (IST) ================= */
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800;
const int   daylightOffset_sec = 0;

/* ================= TIME FALLBACK ================= */
unsigned long lastTimeSyncMillis = 0;
time_t lastUnixTime = 0;
bool timeValid = false;

/* ================= MOTION TIMER ================= */
unsigned long motionTimer = 0;
bool motionLightActive = false;

// Log status throttling
unsigned long lastNoTimeLogMillis = 0;

/* ================= WEB SERVER ================= */
WebServer server(80);
Preferences prefs;

/* ================= GLOBAL DISTANCE CACHE ================= */
float lastDistance = -1;

/* ================= WIFI RECONNECT ================= */
unsigned long lastWiFiAttempt = 0;
bool apActive = false;
// When the device cannot connect to WiFi, it will start a local AP named
// "AutoLight_Config" so users can connect and configure it; it will also
// periodically (every WIFI_RETRY_INTERVAL) attempt to reconnect to the
// configured SSID and resync time.

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  prefs.begin("AutoLight", false);
  loadSettings();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  // LED status using built-in LED (GPIO 2 for many ESP32 boards)
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // off (assuming active LOW)

  WiFi.onEvent(WiFiEvent);
  connectWiFiAndSyncTime();
  setupWebServer();
}

// ================= PREFERENCES =================
void loadSettings() {
  // WiFi
  String s = prefs.getString("ssid", "");
  if (s.length() > 0) wifiSsid = s;
  String p = prefs.getString("pw", "");
  if (p.length() > 0) wifiPassword = p;

  // UI / motion settings
  alwaysOnStart = prefs.getInt("aos", alwaysOnStart);
  alwaysOnEnd   = prefs.getInt("aoe", alwaysOnEnd);
  motionStart   = prefs.getInt("ms", motionStart);
  motionEnd     = prefs.getInt("me", motionEnd);
  DETECTION_DISTANCE_CM = prefs.getInt("dist", DETECTION_DISTANCE_CM);
  MOTION_ON_TIME_MS = prefs.getULong("mtn", MOTION_ON_TIME_MS);
  overrideMode = prefs.getInt("ovr", overrideMode);

  unsigned long savedUnix = prefs.getULong("lastUnix", 0);
  if (savedUnix != 0) {
    lastUnixTime = (time_t)savedUnix;
    lastTimeSyncMillis = millis();
    timeValid = true;
    Serial.println("Loaded last unix time from preferences.");
  }
}

void saveSettings() {
  prefs.putInt("aos", alwaysOnStart);
  prefs.putInt("aoe", alwaysOnEnd);
  prefs.putInt("ms", motionStart);
  prefs.putInt("me", motionEnd);
  prefs.putInt("dist", DETECTION_DISTANCE_CM);
  prefs.putULong("mtn", MOTION_ON_TIME_MS);
  prefs.putInt("ovr", overrideMode);
}

// Save the last synced unix epoch
void persistLastUnix(time_t unixTime) {
  prefs.putULong("lastUnix", (unsigned long)unixTime);
}

// ================= WIFI EVENT HANDLER =================
void WiFiEvent(WiFiEvent_t event) {
  if (event == SYSTEM_EVENT_STA_GOT_IP) {
    Serial.println("Event: WiFi connected (GOT_IP)");
    // Stop AP if it was running
    if (apActive) {
      WiFi.softAPdisconnect(true);
      apActive = false;
    }
    // Sync time
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10000)) {
      lastUnixTime = mktime(&timeinfo);
      lastTimeSyncMillis = millis();
      timeValid = true;
      Serial.println("Time Synced (event)");
      persistLastUnix(lastUnixTime);
    }
  } else if (event == SYSTEM_EVENT_STA_DISCONNECTED) {
    Serial.println("Event: WiFi disconnected");
    // start AP fallback for provisioning only when we have no saved SSID
    if (wifiSsid.length() == 0) {
      WiFi.softAP("AutoLight_Config");
      apActive = true;
    }
  }
}

// ================= LED STATUS =================
unsigned long lastLedToggle = 0;
bool ledState = false;
void updateLed() {
  unsigned long now = millis();
  bool relayOn = (digitalRead(RELAY_PIN) == LOW); // relayON writes LOW

  // Priority: relay on (fast blink), WiFi connected steady ON, AP slow blink, else off
  if (relayOn) {
    // fast blink
    if (now - lastLedToggle > 200) {
      ledState = !ledState;
      digitalWrite(LED_BUILTIN, ledState ? LOW : HIGH);
      lastLedToggle = now;
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    // steady on
    digitalWrite(LED_BUILTIN, LOW);
    return;
  }

  if (apActive) {
    // slow blink
    if (now - lastLedToggle > 700) {
      ledState = !ledState;
      digitalWrite(LED_BUILTIN, ledState ? LOW : HIGH);
      lastLedToggle = now;
    }
    return;
  }

  // off
  digitalWrite(LED_BUILTIN, HIGH);
}

/* ================= WIFI + TIME ================= */
void connectWiFiAndSyncTime() {
  WiFi.mode(WIFI_STA);
  // Only attempt to connect if SSID not empty
  if (wifiSsid.length() > 0) {
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  } else {
    Serial.println("No saved SSID; starting in AP mode for provisioning.");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("AutoLight_Config");
    apActive = true;
    lastWiFiAttempt = millis();
    return;
  }

  unsigned long startAttempt = millis();

  Serial.print("Attempting to connect to WiFi");
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttempt < WIFI_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10000)) {
      lastUnixTime = mktime(&timeinfo);
      lastTimeSyncMillis = millis();
      timeValid = true;
      Serial.println("Time Synced");
      // Persist last synced time
      prefs.putULong("lastUnix", (unsigned long)lastUnixTime);
    }
    // Stop AP mode if it was active
    if (apActive) {
      WiFi.softAPdisconnect(true);
      apActive = false;
    }
  } else {
    // Failed to connect; create a local AP so device still functions and
    // provides a way to connect directly for configuration.
    Serial.println("WiFi not available. Starting AP fallback.");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("AutoLight_Config");
    apActive = true;
    // mark last attempt so maintainWiFi() waits until next retry interval
    lastWiFiAttempt = millis();
  }
}

/* ================= AUTO WIFI RECONNECT ================= */
void maintainWiFi() {
  // Only try reconnecting periodically when not connected
    if (WiFi.status() != WL_CONNECTED &&
      millis() - lastWiFiAttempt > WIFI_RETRY_INTERVAL) {

    lastWiFiAttempt = millis();
    Serial.println("Attempting periodic WiFi reconnect...");
    WiFi.disconnect();
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  }

  // If we just connected, sync time if possible
  if (WiFi.status() == WL_CONNECTED && !timeValid) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10000)) {
      lastUnixTime = mktime(&timeinfo);
      lastTimeSyncMillis = millis();
      timeValid = true;
      Serial.println("Time synced after reconnection");
      // Persist last time and clear AP
      prefs.putULong("lastUnix", (unsigned long)lastUnixTime);
      if (apActive) { WiFi.softAPdisconnect(true); apActive = false; }
    }
  }
}

/* ================= TIME FUNCTIONS ================= */
time_t getCurrentUnixTime() {
  if (!timeValid) return 0;
  return lastUnixTime + ((millis() - lastTimeSyncMillis) / 1000);
}

int getCurrentHour() {
  struct tm timeinfo;

  if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo, 10)) {
    lastUnixTime = mktime(&timeinfo);
    lastTimeSyncMillis = millis();
    timeValid = true;
    return timeinfo.tm_hour;
  } 
  else if (timeValid) {
    time_t now = getCurrentUnixTime();
    struct tm *t = localtime(&now);
    return t->tm_hour;
  } 
  else return -1; // unknown hour (no time available)
}

// Simple median filter for distance readings to reduce false positives
float getFilteredDistance() {
  float values[3];
  for (int i = 0; i < 3; i++) {
    values[i] = getDistanceCM();
    delay(5);
    if (values[i] < 0) values[i] = 9999; // treat invalid as far
  }
  // sort 3 values (simple bubble sort)
  if (values[0] > values[1]) { float t = values[0]; values[0] = values[1]; values[1] = t; }
  if (values[1] > values[2]) { float t = values[1]; values[1] = values[2]; values[2] = t; }
  if (values[0] > values[1]) { float t = values[0]; values[0] = values[1]; values[1] = t; }
  return values[1] >= 9999 ? -1 : values[1];
}

/* ================= ULTRASONIC ================= */
float getDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 26000);
  if (duration == 0) return -1;
  return duration * 0.034 / 2;
}

/* ================= RELAY ================= */
void relayON()  { digitalWrite(RELAY_PIN, LOW);  }
void relayOFF() { digitalWrite(RELAY_PIN, HIGH); }

/* ================= WEB SERVER ================= */
void setupWebServer() {

  server.on("/", []() {
    int hour = getCurrentHour();
    String timeText;
    if (hour == -1) timeText = "Time: Unknown";
    else timeText = "Current Hour: " + String(hour) + ":00";
    String page = "<html><head><title>ESP32 Night Light</title>";
    page += "<script>";
    page += "function loadDist(){fetch('/distance').then(r=>r.text()).then(t=>{document.getElementById('dist').innerHTML = t;});}";
    page += "setInterval(loadDist,1000);</script></head><body>";

    page += "<h2>ESP32 Night Light Control</h2>";
    if (WiFi.status() == WL_CONNECTED) {
      page += "<b>WiFi:</b> Connected to " + String(WiFi.SSID()) + "<br>";
    } else {
      page += "<b>WiFi:</b> Offline (AP Mode) - Connect to 'AutoLight_Config' to configure<br>";
    }
    page += "<a href=\"/wifi\">WiFi Provisioning</a> | <a href=\"/forgetwifi\">Forget WiFi</a><br><br>";
    page += "<b>Live Distance:</b> <span id='dist'>---</span><br><br>";
    page += "<b>System:</b> " + timeText + "<br><br>";

    page += "<form action='/set'>";
    page += "Always ON Start: <input name='aos' value='" + String(alwaysOnStart) + "'><br>";
    page += "Always ON End: <input name='aoe' value='" + String(alwaysOnEnd) + "'><br><br>";

    page += "Motion Start: <input name='ms' value='" + String(motionStart) + "'><br>";
    page += "Motion End: <input name='me' value='" + String(motionEnd) + "'><br><br>";

    page += "Detection Distance: <input name='dist' value='" + String(DETECTION_DISTANCE_CM) + "'><br>";
    page += "Motion Time (sec): <input name='time' value='" + String(MOTION_ON_TIME_MS / 1000) + "'><br><br>";

    page += "<select name='ovr'><option value='0'" + String((overrideMode==0)?" selected":"") + ">AUTO</option>";
    page += "<option value='1'" + String((overrideMode==1)?" selected":"") + ">FORCE ON</option>";
    page += "<option value='2'" + String((overrideMode==2)?" selected":"") + ">FORCE OFF</option></select><br><br>";
    page += "<input type='submit'></form></body></html>";
    server.send(200, "text/html", page);
  });

  // WiFi prov page
  server.on("/wifi", []() {
    String page = "<html><body><h2>WiFi Provisioning</h2>";
    page += "<form action='/savewifi' method='POST'>";
    page += "SSID: <input name='ssid' value='" + wifiSsid + "'><br>";
    page += "Password: <input name='pw' value='" + wifiPassword + "'><br>";
    page += "<input type='submit' value='Save and Connect'></form></body></html>";
    server.send(200, "text/html", page);
  });

  server.on("/savewifi", HTTP_POST, []() {
    if (server.hasArg("ssid") && server.hasArg("pw")) {
      String s = server.arg("ssid");
      String p = server.arg("pw");
      if (s.length() > 0) {
        wifiSsid = s; wifiPassword = p;
        prefs.putString("ssid", wifiSsid);
        prefs.putString("pw", wifiPassword);
        server.send(200, "text/html", "Saved credentials and attempting to connect. <a href='/'>Back</a>");
        Serial.println("Saved WiFi credentials via provisioning UI.");
        // Attempt connect
        WiFi.disconnect();
        WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
        lastWiFiAttempt = millis();
        delay(1000);
        return;
      }
    }
    server.send(400, "text/html", "Invalid input. <a href='/wifi'>Try again</a>");
  });

  server.on("/forgetwifi", []() {
    prefs.remove("ssid");
    prefs.remove("pw");
    wifiSsid = "";
    wifiPassword = "";
    WiFi.disconnect();
    WiFi.softAP("AutoLight_Config");
    apActive = true;
    server.send(200, "text/html", "WiFi credentials cleared; AP started. <a href='/'>Back</a>");
  });

  server.on("/set", []() {
    alwaysOnStart = constrain(server.arg("aos").toInt(), 0, 23);
    alwaysOnEnd   = constrain(server.arg("aoe").toInt(), 0, 23);
    motionStart   = constrain(server.arg("ms").toInt(), 0, 23);
    motionEnd     = constrain(server.arg("me").toInt(), 0, 23);
    DETECTION_DISTANCE_CM = server.arg("dist").toInt();
    MOTION_ON_TIME_MS = server.arg("time").toInt() * 1000;
    overrideMode = server.arg("ovr").toInt();
    saveSettings();

    server.sendHeader("Location", "/");
    server.send(302);
  });

  server.on("/distance", []() {
    String status = (WiFi.status() == WL_CONNECTED) ? String("WiFi: ") + String(WiFi.SSID()) : String("WiFi: Offline (AP)");
    if (motionLightActive)
      server.send(200, "text/plain", String(lastDistance) + " cm");
    else
      server.send(200, "text/plain", "IDLE");
  });

  server.begin();
}

/* ================= MAIN LOOP ================= */
void loop() {
  server.handleClient();
  maintainWiFi();
  updateLed();

  int hour = getCurrentHour();

  // If hour == -1 (time not available), we still want motion detection to work.
  // Treat hour == -1 as "unknown" — in that case, don't enable Always-ON
  // mode, but allow motion-based activation.
  if (hour == -1) {
    // Avoid serial flooding — print only every 30s
    if (millis() - lastNoTimeLogMillis > 30000) {
      Serial.println("No time available; using motion-only mode until WiFi/time sync.");
      lastNoTimeLogMillis = millis();
    }
  }

  // ---------- FORCE MODES ----------
  if (overrideMode == 1) { relayON(); delay(200); return; }
  if (overrideMode == 2) { relayOFF(); delay(200); return; }

  // ---------- ALWAYS ON ----------
  if (hour != -1 && hour >= alwaysOnStart && hour < alwaysOnEnd) {
    relayON();
    motionLightActive = false;
  }

  // ---------- MOTION SLOT (ONLY HERE SENSOR RUNS) ----------
  // If the hour is unknown, allow motion detection to operate regardless
  else if (hour == -1 || (hour >= motionStart && hour < motionEnd)) {

    lastDistance = getFilteredDistance();

    if (lastDistance > 0 && lastDistance < DETECTION_DISTANCE_CM) {
      relayON();
      motionLightActive = true;
      motionTimer = millis();
    }

    if (motionLightActive &&
        millis() - motionTimer >= MOTION_ON_TIME_MS) {
      relayOFF();
      motionLightActive = false;
    }
  }

  // ---------- OFF HOURS ----------
  else {
    relayOFF();
    motionLightActive = false;
    lastDistance = -1;
  }

  delay(300);
}
