#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <time.h>
#include <WebServer.h>
#include <ArduinoOTA.h>

/* ================= WIFI ================= */
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

/* Override Mode:
   0 = AUTO
   1 = FORCE ON
   2 = FORCE OFF
*/
int overrideMode = 0;

/* ================= TIME CONFIG (IST) ================= */
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800;
const int   daylightOffset_sec = 0;

/* ================= GLOBAL ================= */
WebServer server(80);
Preferences prefs;
float lastDistance = -1;
bool motionLightActive = false;
unsigned long motionTimer = 0;
bool apActive = false;
bool timeSynced = false;

/* ================= OTA ================= */
void setupOTA() {
  ArduinoOTA.setHostname("AutoLightESP32");
  ArduinoOTA.begin();
}

/* ================= PREFERENCES ================= */
void loadSettings() {
  wifiSsid = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("pw", "");
  alwaysOnStart = prefs.getInt("aos", alwaysOnStart);
  alwaysOnEnd   = prefs.getInt("aoe", alwaysOnEnd);
  motionStart   = prefs.getInt("ms", motionStart);
  motionEnd     = prefs.getInt("me", motionEnd);
  DETECTION_DISTANCE_CM = prefs.getInt("dist", DETECTION_DISTANCE_CM);
  MOTION_ON_TIME_MS = prefs.getULong("mtn", MOTION_ON_TIME_MS);
  overrideMode = prefs.getInt("ovr", overrideMode);
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

/* ================= WIFI ================= */
void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("AutoLight_Config");
  apActive = true;
}

void connectWiFi() {
  if (wifiSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    apActive = false;
  } else {
    startAP();
  }
}

/* ================= TIME ================= */
int getHour() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) return timeinfo.tm_hour;
  return -1;
}

String getTimeString() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buf[20];
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    return String(buf);
  }
  return "No Time";
}

bool inWindow(int start, int end, int hour) {
  if (hour < 0) return false;
  if (start <= end) return hour >= start && hour < end;
  return hour >= start || hour < end;
}

/* ================= SENSOR ================= */
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

float getFilteredDistance() {
  float v[3];
  for (int i = 0; i < 3; i++) {
    v[i] = getDistanceCM();
    if (v[i] < 0) v[i] = 9999;
  }
  if (v[0] > v[1]) { float t=v[0]; v[0]=v[1]; v[1]=t; }
  if (v[1] > v[2]) { float t=v[1]; v[1]=v[2]; v[2]=t; }
  if (v[0] > v[1]) { float t=v[0]; v[0]=v[1]; v[1]=t; }
  return v[1] >= 9999 ? -1 : v[1];
}

/* ================= RELAY ================= */
void relayON()  { digitalWrite(RELAY_PIN, LOW);  }
void relayOFF() { digitalWrite(RELAY_PIN, HIGH); }

/* ================= UI ================= */
const char* baseStyle = R"rawliteral(
<style>
body{background:#020617;color:#e5e7eb;font-family:Arial;margin:0;text-align:center}
.card{background:#020617;padding:16px;margin:12px;border-radius:14px;
box-shadow:0 0 20px rgba(34,197,94,0.4);border:1px solid #22c55e}
.btn{padding:10px 18px;margin:6px;border-radius:20px;background:#22c55e;
border:none;font-weight:bold}
input{width:90%;padding:10px;border-radius:8px;border:1px solid #4b5563;background:#020617;color:white}
small{color:#a1a1aa}
</style>
)rawliteral";

/* ================= WEB SERVER ================= */
void setupWebServer() {

  /* DASHBOARD */
  server.on("/", []() {
    String page = R"rawliteral(
<html><head><title>Auto Light</title>)rawliteral";
    page += baseStyle;
    page += R"rawliteral(</head><body>

<div class="card">
<h2>AUTO LIGHT</h2>
Status: <b id="status">--</b><br>
Distance: <b id="dist">--</b> cm<br>
Time: <b id="time">--</b><br>
Mode: <b id="mode">--</b>
</div>

<div class="card">
<button class="btn" onclick="setMode(0)">AUTO</button>
<button class="btn" onclick="setMode(1)">FORCE ON</button>
<button class="btn" onclick="setMode(2)">FORCE OFF</button>
</div>

<div class="card">
<button class="btn" onclick="location.href='/wifi'">WiFi</button>
<button class="btn" onclick="location.href='/set'">Settings</button>
<button class="btn" onclick="location.href='/forgetwifi'">Reset WiFi</button>
</div>

<script>
function setMode(m){fetch('/api/override?mode='+m);}
function update(){
fetch('/api/status').then(r=>r.json()).then(d=>{
status.innerText=d.status;
dist.innerText=d.distance;
time.innerText=d.time;
mode.innerText=d.mode;
});
}
setInterval(update,1000);update();
</script>

</body></html>
)rawliteral";
    server.send(200, "text/html", page);
  });

  /* STATUS API */
  server.on("/api/status", []() {
    String state = motionLightActive ? "ON" : "OFF";
    String modeStr = (overrideMode==0)?"AUTO":(overrideMode==1)?"FORCE ON":"FORCE OFF";

    server.send(200,"application/json",
      "{\"distance\":"+String(lastDistance)+
      ",\"time\":\""+getTimeString()+
      "\",\"status\":\""+state+
      "\",\"mode\":\""+modeStr+"\"}");
  });

  /* OVERRIDE API */
  server.on("/api/override", []() {
    if (server.hasArg("mode")) {
      overrideMode = server.arg("mode").toInt();
      prefs.putInt("ovr", overrideMode);
    }
    server.send(200, "text/plain", "OK");
  });

  /* WIFI PAGE */
  server.on("/wifi", []() {
    String page = R"rawliteral(
<html><head><title>WiFi</title>)rawliteral";
    page += baseStyle;
    page += R"rawliteral(</head><body>

<div class="card">
<h2>WiFi Setup</h2>
<form method="POST" action="/savewifi">
SSID:<br><input name="ssid"><br>
PASS:<br><input name="pw" type="password"><br><br>
<input type="submit" class="btn">
</form>
<button class="btn" onclick="location.href='/'">Back</button>
</div>

</body></html>
)rawliteral";
    server.send(200, "text/html", page);
  });

  server.on("/savewifi", HTTP_POST, []() {
    wifiSsid = server.arg("ssid");
    wifiPassword = server.arg("pw");
    prefs.putString("ssid", wifiSsid);
    prefs.putString("pw", wifiPassword);
    WiFi.disconnect(true);
    delay(300);
    connectWiFi();
    server.sendHeader("Location","/");
    server.send(302);
  });

  server.on("/forgetwifi", []() {
    prefs.remove("ssid");
    prefs.remove("pw");
    wifiSsid="";
    wifiPassword="";
    WiFi.disconnect(true);
    startAP();
    server.sendHeader("Location","/");
    server.send(302);
  });

  /* SETTINGS PAGE */
  server.on("/set", []() {
    String page = "<html><head>"+String(baseStyle)+"</head><body><div class='card'>";
    page += "<form method='POST' action='/savesettings'>";
    page += "Always ON Start (0-23):<br><input name='aos' value='"+String(alwaysOnStart)+"'><br>";
    page += "Always ON End (0-23):<br><input name='aoe' value='"+String(alwaysOnEnd)+"'><br>";
    page += "Motion Start (0-23):<br><input name='ms' value='"+String(motionStart)+"'><br>";
    page += "Motion End (0-23):<br><input name='me' value='"+String(motionEnd)+"'><br>";
    page += "Distance (cm):<br><input name='dist' value='"+String(DETECTION_DISTANCE_CM)+"'><br>";
    page += "Motion Time (ms):<br><input name='time' value='"+String(MOTION_ON_TIME_MS)+"'><br><br>";
    page += "<input type='submit' class='btn'></form>";
    page += "<button class='btn' onclick=\"location.href='/'\">Back</button></div></body></html>";
    server.send(200,"text/html",page);
  });

  server.on("/savesettings", HTTP_POST, []() {
    alwaysOnStart=server.arg("aos").toInt();
    alwaysOnEnd=server.arg("aoe").toInt();
    motionStart=server.arg("ms").toInt();
    motionEnd=server.arg("me").toInt();
    DETECTION_DISTANCE_CM=server.arg("dist").toInt();
    MOTION_ON_TIME_MS=server.arg("time").toInt();
    saveSettings();
    server.sendHeader("Location","/");
    server.send(302);
  });

  server.begin();
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  prefs.begin("AutoLight", false);
  loadSettings();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  relayOFF();

  connectWiFi();
  setupOTA();
  setupWebServer();
}

/* ================= LOOP ================= */
void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  if (WiFi.status() == WL_CONNECTED && !timeSynced) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    timeSynced = true;
  }

  int hour = getHour();
  bool alwaysON = inWindow(alwaysOnStart, alwaysOnEnd, hour);
  bool motionAllowed = inWindow(motionStart, motionEnd, hour);

  lastDistance = getFilteredDistance();

  if (overrideMode == 1) {
    relayON();
    motionLightActive = true;
  }
  else if (overrideMode == 2) {
    relayOFF();
    motionLightActive = false;
  }
  else {
    if (alwaysON) {
      relayON();
      motionLightActive = true;
    }
    else if (motionAllowed && lastDistance > 0 && lastDistance < DETECTION_DISTANCE_CM) {
      relayON();
      motionLightActive = true;
      motionTimer = millis();
    }
    else if (motionLightActive && millis() - motionTimer >= MOTION_ON_TIME_MS) {
      relayOFF();
      motionLightActive = false;
    }
    else if (!alwaysON && !motionAllowed) {
      relayOFF();
      motionLightActive = false;
    }
  }

  delay(200);
}
