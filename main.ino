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

/* ================= TIME CONFIG (IST) ================= */
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;
const int daylightOffset_sec = 0;

/* ================= GLOBAL ================= */
WebServer server(80);
Preferences prefs;
float lastDistance = -1;
bool motionLightActive = false;
unsigned long motionTimer = 0;
bool apActive = false;

/* ================= OTA ================= */
void setupOTA()
{
  ArduinoOTA.setHostname("AutoLightESP32");
  ArduinoOTA.begin();
}

/* ================= PREFERENCES ================= */
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

void saveSettings()
{
  prefs.putInt("aos", alwaysOnStart);
  prefs.putInt("aoe", alwaysOnEnd);
  prefs.putInt("ms", motionStart);
  prefs.putInt("me", motionEnd);
  prefs.putInt("dist", DETECTION_DISTANCE_CM);
  prefs.putULong("mtn", MOTION_ON_TIME_MS);
  prefs.putInt("ovr", overrideMode);
}

/* ================= WIFI ================= */
void WiFiEvent(arduino_event_id_t event)
{
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
  {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
  {
    if (wifiSsid.length() == 0)
    {
      WiFi.softAP("AutoLight_Config");
      apActive = true;
    }
  }
}

void connectWiFiAndSyncTime()
{
  WiFi.mode(WIFI_STA);
  if (wifiSsid.length() > 0)
  {
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  }
  else
  {
    WiFi.softAP("AutoLight_Config");
    apActive = true;
  }
}

/* ================= TIME ================= */
String getTimeString()
{
  struct tm timeinfo;
  if (getLocalTime(&timeinfo))
  {
    char buf[20];
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    return String(buf);
  }
  return "No Time";
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

  // DASHBOARD
  server.on("/", []()
            {
    String page = R"rawliteral(
<html>
<head>
<style>
body{background:#111;color:#0f0;font-family:Arial;text-align:center}
.card{background:#222;padding:20px;margin:10px;border-radius:10px}
.btn{padding:10px 20px;margin:8px;border-radius:8px}
</style>
</head>
<body>
<h2>ESP32 AUTO LIGHT</h2>
<div class="card">
  Status: <b><span id="status">--</span></b><br>
  Distance: <b><span id="dist">--</span> cm</b><br>
  Time: <b><span id="time">--</span></b>
</div>
<a href="/wifi"><button class="btn">WiFi</button></a>
<a href="/set"><button class="btn">Settings</button></a>
<a href="/forgetwifi"><button class="btn">Reset WiFi</button></a>

<script>
function update(){
 fetch('/api/status').then(r=>r.json()).then(d=>{
   document.getElementById('dist').innerText=d.distance;
   document.getElementById('time').innerText=d.time;
   document.getElementById('status').innerText=d.status;
 });
}
setInterval(update,1000);update();
</script>
</body>
</html>
)rawliteral";
    server.send(200,"text/html",page); });

  // API
  server.on("/api/status", []()
            {
    String status = motionLightActive ? "ON" : "WAITING";
    server.send(200,"application/json",
      "{\"distance\":"+String(lastDistance)+",\"time\":\""+getTimeString()+"\",\"status\":\""+status+"\"}"); });

  // WIFI PAGE
  server.on("/wifi", []()
            {
    String page="<h2>WiFi Setup</h2><form action='/savewifi' method='POST'>"
                "SSID:<input name='ssid' value='"+wifiSsid+"'><br>"
                "PASS:<input name='pw' value='"+wifiPassword+"'><br>"
                "<input type='submit'></form><a href='/'>Back</a>";
    server.send(200,"text/html",page); });

  server.on("/savewifi", HTTP_POST, []()
            {
    wifiSsid=server.arg("ssid");
    wifiPassword=server.arg("pw");
    prefs.putString("ssid",wifiSsid);
    prefs.putString("pw",wifiPassword);
    WiFi.disconnect(); WiFi.begin(wifiSsid.c_str(),wifiPassword.c_str());
    server.send(200,"text/html","Saved <a href='/'>Back</a>"); });

  server.on("/forgetwifi", []()
            {
    prefs.remove("ssid"); prefs.remove("pw");
    WiFi.disconnect(); WiFi.softAP("AutoLight_Config");
    server.send(200,"text/html","Reset Done <a href='/'>Back</a>"); });

  // SETTINGS
  server.on("/set", []()
            {
    String page="<form action='/savesettings' method='POST'>"
    "Always ON Start:<input name='aos' value='"+String(alwaysOnStart)+"'><br>"
    "Always ON End:<input name='aoe' value='"+String(alwaysOnEnd)+"'><br>"
    "Motion Start:<input name='ms' value='"+String(motionStart)+"'><br>"
    "Motion End:<input name='me' value='"+String(motionEnd)+"'><br>"
    "Distance:<input name='dist' value='"+String(DETECTION_DISTANCE_CM)+"'><br>"
    "Time(ms):<input name='time' value='"+String(MOTION_ON_TIME_MS)+"'><br>"
    "<input type='submit'></form>";
    server.send(200,"text/html",page); });

  server.on("/savesettings", HTTP_POST, []()
            {
    alwaysOnStart=server.arg("aos").toInt();
    alwaysOnEnd=server.arg("aoe").toInt();
    motionStart=server.arg("ms").toInt();
    motionEnd=server.arg("me").toInt();
    DETECTION_DISTANCE_CM=server.arg("dist").toInt();
    MOTION_ON_TIME_MS=server.arg("time").toInt();
    saveSettings();
    server.sendHeader("Location","/");
    server.send(302); });

  server.begin();
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

  WiFi.onEvent(WiFiEvent);
  connectWiFiAndSyncTime();
  setupOTA();
  setupWebServer();
}

/* ================= LOOP ================= */
void loop()
{
  ArduinoOTA.handle();
  server.handleClient();

  lastDistance = getFilteredDistance();

  if ((lastDistance > 0 && lastDistance < DETECTION_DISTANCE_CM) || lastDistance == -1)
  {
    relayON();
    motionLightActive = true;
    motionTimer = millis();
  }

  if (motionLightActive && millis() - motionTimer >= MOTION_ON_TIME_MS)
  {
    relayOFF();
    motionLightActive = false;
  }

  delay(200);
}
