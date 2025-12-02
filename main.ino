#include <WiFi.h>
#include <time.h>
#include <WebServer.h>

/* ================= WIFI ================= */
const char* ssid = "<WiFi_SSID>";
const char* password = "<WiFi_PASSWORD>";

/* ================= PINS ================= */
#define TRIG_PIN   5
#define ECHO_PIN   18
#define RELAY_PIN  26

/* ================= USER SETTINGS ================= */
int DETECTION_DISTANCE_CM = 290;
unsigned long MOTION_ON_TIME_MS = 120000;

int alwaysOnStart = 18;
int alwaysOnEnd   = 22;

int motionStart = 0;
int motionEnd   = 6;

int overrideMode = 0;   // 0=AUTO, 1=FORCE ON, 2=FORCE OFF

#define WIFI_TIMEOUT_MS 15000
#define WIFI_RETRY_INTERVAL 30000

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

/* ================= WEB SERVER ================= */
WebServer server(80);

/* ================= GLOBAL DISTANCE CACHE ================= */
float lastDistance = -1;

/* ================= WIFI RECONNECT ================= */
unsigned long lastWiFiAttempt = 0;

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  connectWiFiAndSyncTime();
  setupWebServer();
}

/* ================= WIFI + TIME ================= */
void connectWiFiAndSyncTime() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttempt < WIFI_TIMEOUT_MS) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10000)) {
      lastUnixTime = mktime(&timeinfo);
      lastTimeSyncMillis = millis();
      timeValid = true;
      Serial.println("Time Synced");
    }
  }
}

/* ================= AUTO WIFI RECONNECT ================= */
void maintainWiFi() {
  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastWiFiAttempt > WIFI_RETRY_INTERVAL) {

    lastWiFiAttempt = millis();
    WiFi.disconnect();
    WiFi.begin(ssid, password);
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
  else return -1;
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
    String page = "<html><head><title>ESP32 Night Light</title>";
    page += "<script>";
    page += "function loadDist(){fetch('/distance').then(r=>r.text()).then(t=>{document.getElementById('dist').innerHTML = t;});}";
    page += "setInterval(loadDist,1000);</script></head><body>";

    page += "<h2>ESP32 Night Light Control</h2>";
    page += "<b>Live Distance:</b> <span id='dist'>---</span><br><br>";

    page += "<form action='/set'>";
    page += "Always ON Start: <input name='aos' value='" + String(alwaysOnStart) + "'><br>";
    page += "Always ON End: <input name='aoe' value='" + String(alwaysOnEnd) + "'><br><br>";

    page += "Motion Start: <input name='ms' value='" + String(motionStart) + "'><br>";
    page += "Motion End: <input name='me' value='" + String(motionEnd) + "'><br><br>";

    page += "Detection Distance: <input name='dist' value='" + String(DETECTION_DISTANCE_CM) + "'><br>";
    page += "Motion Time (sec): <input name='time' value='" + String(MOTION_ON_TIME_MS / 1000) + "'><br><br>";

    page += "<select name='ovr'><option value='0'>AUTO</option><option value='1'>FORCE ON</option><option value='2'>FORCE OFF</option></select><br><br>";
    page += "<input type='submit'></form></body></html>";
    server.send(200, "text/html", page);
  });

  server.on("/set", []() {
    alwaysOnStart = constrain(server.arg("aos").toInt(), 0, 23);
    alwaysOnEnd   = constrain(server.arg("aoe").toInt(), 0, 23);
    motionStart   = constrain(server.arg("ms").toInt(), 0, 23);
    motionEnd     = constrain(server.arg("me").toInt(), 0, 23);
    DETECTION_DISTANCE_CM = server.arg("dist").toInt();
    MOTION_ON_TIME_MS = server.arg("time").toInt() * 1000;
    overrideMode = server.arg("ovr").toInt();

    server.sendHeader("Location", "/");
    server.send(302);
  });

  server.on("/distance", []() {
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

  int hour = getCurrentHour();

  // ---------- FORCE MODES ----------
  if (overrideMode == 1) { relayON(); delay(200); return; }
  if (overrideMode == 2) { relayOFF(); delay(200); return; }

  // ---------- ALWAYS ON ----------
  if (hour >= alwaysOnStart && hour < alwaysOnEnd) {
    relayON();
    motionLightActive = false;
  }

  // ---------- MOTION SLOT (ONLY HERE SENSOR RUNS) ----------
  else if (hour >= motionStart && hour < motionEnd) {

    lastDistance = getDistanceCM();

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
