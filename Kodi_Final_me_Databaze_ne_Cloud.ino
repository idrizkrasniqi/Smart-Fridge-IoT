// esp32_smart_fridge_with_webui_and_localdb.ino
// Enhanced ESP32 Smart Fridge sketch — Mobile-friendly UI + remote threshold controls
// - Stylish, responsive web UI with live MJPEG feed, detection button, sensor dashboard, local log viewer
// - Local NDJSON log database stored in LittleFS (/logs.ndjson)
// - API endpoints: /api/status, /api/settings (GET/POST), /proxy_detect, /logs, /logs/clear
// - TFT screen updates show: temperature, humidity, weight, delta, door, pending action, last detection, IP, time, fan speed & threshold, LED status
// - Fan speed ramps up automatically after a configurable temperature threshold (changeable from the browser)
// - Text color on TFT changes based on temperature relative to fan threshold: blue below (threshold - 4), green ideal (threshold - 4 to threshold), red above threshold
// - Added LED status to API and web UI
// - Update Firebase when fan changes
// - Handle NaN sensor readings gracefully
// - Replaced sliders with number inputs for fan threshold and weight tolerance
// - Use a variable to track LED state instead of digitalRead, to avoid ESP32 issue where digitalRead on output pins may return incorrect values
// - Added toggle button for live stream on website
// - Handle comma as decimal separator in settings inputs for regions using comma
// - Prevent overwriting input values during refresh if input is focused
// - Adapted to provided database structure: Use real Unix timestamps for logs, improved stock iteration, added Pending boolean to States
// - Added real-time listening to Firebase for Settings changes (fanThreshold, tolerance) to sync from external changes

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Firebase_ESP_Client.h>
#include "HX711.h"
#include "DHT.h"
#include <time.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include "addons/TokenHelper.h"  // Provides the token generation process info.

// Forward declarations for safeFirebaseSet functions (to fix declaration errors)
void safeFirebaseSetString(const String &path, const String &value);
void safeFirebaseSetFloat(const String &path, float value);
void safeFirebaseSetInt(const String &path, int value);
void safeFirebaseSetBool(const String &path, bool value);

// Forward declaration for readAndUpdateDHT
void readAndUpdateDHT();

// --------- USER CONFIG ----------
String LAPTOP_IP = "";  // Will be loaded from prefs or prompted via serial
// -------------------------------------------------------

#define WIFI_SSID "Dizi Dizi"
#define WIFI_PASSWORD "044302585"
#define API_KEY "AIzaSyAA9mExgniNEuR2QRiYnv-IjjnUVJOzqlM"
#define DATABASE_URL "https://smart-fridge-24931-default-rtdb.europe-west1.firebasedatabase.app/"

FirebaseData fbdo;
FirebaseData stream;  // For real-time streaming (settings)
FirebaseData detectionStream;  // For real-time detection results
FirebaseAuth auth;
FirebaseConfig config;

WebServer server(80);

HX711 scale;
#define DOUT 16
#define CLK 17

#define DHTPIN 32
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

#define DOOR_PIN 27
#define LED_PIN 26
#define FAN_PIN 25
#define BACKLIGHT_PIN 33

TFT_eSPI tft = TFT_eSPI();
#define TOUCH_CS 21
#define TOUCH_IRQ 22
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

Preferences prefs;

float lastWeight = 0.0;
float tolerance = 5.0;  // grams (or same units as scale.get_units)
bool waitingForProduct = false;
float deltaWeight = 0.0;
String pendingAction = "";
unsigned long lastDHTRead = 0;
const unsigned long DHT_INTERVAL = 10000;
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;
int lastDoorState = -1;
String lastDoorStateStr = "Mbyllur";
float temperature = NAN;  // Initialize to NaN
float humidity = NAN;     // Initialize to NaN
unsigned long lastScreenUpdate = 0;
const unsigned long SCREEN_UPDATE_INTERVAL = 500;  // Faster refresh: 500ms

String lastDetection = "—";

// Fan control threshold (configurable via web UI)
float fanTempThreshold = 26.5;  // default °C
float editFanThreshold = 26.5;  // Temporary edit value for TFT

// PWM for ESP32 core 3.x
const uint32_t PWM_FREQ = 5000;
const uint8_t PWM_RES = 8;  // 8-bit resolution (0-255)
int currentFanSpeed = 0;
int lastFanSpeed = -1;  // To detect changes

bool ledOn = false; // Track LED state

// Touch calibration variables (loaded from prefs)
uint16_t TS_MINX;
uint16_t TS_MAXX;
uint16_t TS_MINY;
uint16_t TS_MAXY;

// Button positions (assuming 320x240 landscape, rotation 1)
const int SET_Y = 180;  // Adjusted
const int NUM_X = 70, NUM_W = 50, NUM_H = 20;
const int PLUS_X = 130, PLUS_W = 30, PLUS_H = 20;
const int MINUS_X = 170, MINUS_W = 30, MINUS_H = 20;
const int SAVE_X = 210, SAVE_W = 50, SAVE_H = 20;

// Screen layout constants (smaller text: size 1 for most)
const int TEXT_SIZE_LARGE = 2;
const int TEXT_SIZE_SMALL = 1;
const int LINE_HEIGHT_SMALL = 12;
const int LINE_HEIGHT_LARGE = 24;
const int LABEL_X = 0;  // Back to 0 since legend removed
const int VALUE_X = 70;  // Adjusted for smaller text
const int CLEAR_WIDTH = 320 - VALUE_X;  // Adjusted

// Y positions for each line (closer spacing for smaller text)
const int Y_TITLE = 0;
const int Y_TIME = 24;
const int Y_HUM = 40;
const int Y_TEMP = 52;
const int Y_WEIGHT = 64;
const int Y_DELTA = 76;
const int Y_DOOR = 88;
const int Y_PENDING = 100;
const int Y_LASTDET = 112;
const int Y_LED = 124;
const int Y_FANSPD = 136;
const int Y_FANTHR = 148;
const int Y_IP = 160;
const int Y_SETTHR = SET_Y;
const int Y_LOGS = 0;  // Removed or adjust if needed

// Previous values for selective updates
bool screenInitialized = false;
float prevTemperature = NAN;
float prevHumidity = NAN;
float prevWeight = 0.0;
float prevDelta = 0.0;
String prevDoor = "";
String prevPending = "";
String prevLastDet = "";
String prevIP = "";
int prevLogs = 0;
float prevFanThr = 0.0;
float prevEditFanThr = 0.0;
int prevFanSpd = 0;
bool prevLed = false;
String prevTimeStr = "";
uint16_t prevTextColor = TFT_WHITE;  // For temp color changes

// Dynamic temperature thresholds based on fanTempThreshold
float getTempMin() {
  return fanTempThreshold - 4.0;
}  // Below: blue (cold)
float getTempIdealMin() {
  return fanTempThreshold - 4.0;
}
float getTempIdealMax() {
  return fanTempThreshold;
}  // Ideal up to threshold: green
// Above threshold: red (warm)

String laptopStreamURL() {
  return "http://" + LAPTOP_IP + "/stream";
}
String laptopDetectURL() {
  return "http://" + LAPTOP_IP + "/trigger_photo";
}

// ---------- Helper functions ----------

bool initLocalDB() {
  if (!LittleFS.begin(true)) {
    Serial.println("❌ LittleFS mount failed");
    return false;
  }
  if (!LittleFS.exists("/logs.ndjson")) {
    File f = LittleFS.open("/logs.ndjson", "w");
    if (f) f.close();
  }
  return true;
}

void appendLogToLocalDB(const String &jsonLine) {
  File f = LittleFS.open("/logs.ndjson", "a");
  if (!f) {
    Serial.println("❌ Unable to open log file for append");
    return;
  }
  f.println(jsonLine);
  f.flush();
  f.close();
}

String readAllLogsAsJsonArray() {
  if (!LittleFS.exists("/logs.ndjson")) return "[]";
  File f = LittleFS.open("/logs.ndjson", "r");
  if (!f) return "[]";
  String out = "[";
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (!first) out += ",";
    out += line;
    first = false;
  }
  out += "]";
  f.close();
  return out;
}

void clearLocalLogs() {
  File f = LittleFS.open("/logs.ndjson", "w");
  if (f) f.close();
}

int getLocalLogCount() {
  if (!LittleFS.exists("/logs.ndjson")) return 0;
  File f = LittleFS.open("/logs.ndjson", "r");
  if (!f) return 0;
  int count = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) count++;
  }
  f.close();
  return count;
}

// ---------- New: request photo trigger from laptop (calls /trigger_photo) ----------
String requestDetectionFromLaptop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ WiFi not connected - cannot contact laptop");
    return "Error:NoWiFi";
  }

  HTTPClient http;
  String url = "https://" + LAPTOP_IP + "/trigger_photo";  // Use HTTPS, no port, correct path
  Serial.println("Request URL: " + url);  // Debug: Print the exact URL being requested
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // Handle any redirects automatically
  http.addHeader("ngrok-skip-browser-warning", "true");    // Bypass ngrok free-tier warning page

  int httpCode = http.GET();
  if (httpCode <= 0) {
    Serial.println("❌ HTTP GET failed, code: " + String(httpCode));
    http.end();
    return "Error:HTTPFailed";
  }
  String response = http.getString();
  http.end();
  response.trim();
  Serial.println("📩 Laptop photo trigger response: " + response);
  if (response.length() == 0) return String("Error:EmptyResponse");
  return response;
}


// ---------- product processing ----------
void processDetectedProduct(const String &product) {
  String topName = product;
  if (product.startsWith("{")) {
    int idx = product.indexOf("\"name\":");
    if (idx >= 0) {
      int quote1 = product.indexOf('"', idx + 7);
      int quote2 = product.indexOf('"', quote1 + 1);
      if (quote1 >= 0 && quote2 > quote1) {
        topName = product.substring(quote1 + 1, quote2);
      }
    } else {
      if (product.indexOf("\"detections\":[]") >= 0 || product.indexOf("\"detections\": []") >= 0) {
        topName = "No product detected";
      }
    }
  }

  time_t now;
  struct tm timeinfo;
  bool timeValid = getLocalTime(&timeinfo);
  if (timeValid) {
    now = mktime(&timeinfo);
  } else {
    now = (time_t)millis();  // Fallback to millis if time not synced
  }

  if (topName == "No product detected" || topName.startsWith("Error")) {
    Serial.println("⚠ Product detection failed or no product: " + topName);
    String log = "{\"ts\":" + String((unsigned long)now) + ",\"product\":\"" + topName + "\",\"delta\":" + String(deltaWeight, 3) + ",\"action\":\"" + pendingAction + "\"}";
    appendLogToLocalDB(log);

    waitingForProduct = false;
    pendingAction = "";
    deltaWeight = 0.0;
    safeFirebaseSetBool("/SmartFridge/States/Pending", false);
    return;
  }

  Serial.println("📩 Produkt nga laptop: " + topName);
  String log = "{\"ts\":" + String((unsigned long)now) + ",\"product\":\"" + topName + "\",\"delta\":" + String(deltaWeight, 3) + ",\"action\":\"" + pendingAction + "\"}";
  appendLogToLocalDB(log);

  String logPath = "/SmartFridge/Logs/log_" + String((unsigned long)now);
  safeFirebaseSetString(logPath + "/product", topName);
  safeFirebaseSetFloat(logPath + "/delta", deltaWeight);
  safeFirebaseSetString(logPath + "/action", pendingAction);
  safeFirebaseSetInt(logPath + "/timestamp", (int)now);

  String stockPath = "/SmartFridge/Stock/" + topName;
  float stockWeight = 0.0;
  int quantity = 0;

  if (!Firebase.RTDB.getFloat(&fbdo, stockPath + "/weight_total")) {
    safeFirebaseSetFloat(stockPath + "/weight_total", 0.0);
    safeFirebaseSetInt(stockPath + "/quantity", 0);
  } else {
    stockWeight = fbdo.floatData();
    if (Firebase.RTDB.getInt(&fbdo, stockPath + "/quantity")) {
      quantity = fbdo.intData();
    } else {
      safeFirebaseSetInt(stockPath + "/quantity", 0);
    }
  }

  if (pendingAction == "Shtuar") {
    stockWeight += deltaWeight;
    quantity += 1;
  } else if (pendingAction == "Hequr") {
    stockWeight += deltaWeight;
    quantity = max(0, quantity - 1);
  }

  safeFirebaseSetFloat(stockPath + "/weight_total", stockWeight);
  safeFirebaseSetInt(stockPath + "/quantity", quantity);

  Serial.println("✅ Logs dhe Stock u përditësuan: " + pendingAction + ", Produkt: " + topName);

  lastDetection = topName;
  waitingForProduct = false;
  pendingAction = "";
  deltaWeight = 0.0;
  safeFirebaseSetBool("/SmartFridge/States/Pending", false);
}

// ---------- HTTP handler: responsive HTML with threshold controls ----------
void handleRoot() {
  String html = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Smart Fridge — Live</title>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;600;700;800&display=swap" rel="stylesheet">
<style>
:root{--bg1:#071023;--bg2:#0b1020;--accent:#8b5cf6;--card:rgba(255,255,255,0.03);--muted:rgba(255,255,255,0.7)}
*{box-sizing:border-box;font-family:Inter,system-ui,Segoe UI,Roboto,Arial}
body{margin:0;background:linear-gradient(180deg,var(--bg1),var(--bg2));color:#e6eef8;min-height:100vh}
.container{max-width:1100px;margin:14px auto;padding:12px;display:grid;grid-template-columns:1fr 360px;gap:14px}
.card{background:var(--card);padding:12px;border-radius:12px;border:1px solid rgba(255,255,255,0.03)}
.header{display:flex;gap:12px;align-items:center;justify-content:space-between}
.controls{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.btn{background:linear-gradient(90deg,var(--accent),#5b21b6);border:none;color:white;padding:8px 12px;border-radius:10px;font-weight:700}
.muted{color:var(--muted);font-size:13px}
.small{font-size:13px}
.sensor-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px}
.sensor{background:rgba(255,255,255,0.02);padding:8px;border-radius:8px}
.sensor .val{font-weight:800;font-size:18px}
#logList{max-height:260px;overflow:auto;margin-top:8px}
#stockList{max-height:260px;overflow:auto;margin-top:8px}
.settings{display:flex;flex-direction:column;gap:8px}
.range-row{display:flex;gap:8px;align-items:center}
.range-row input[type=number]{flex:1}
.footer{font-size:12px;color:var(--muted);text-align:center;margin-top:8px}
@media (max-width:980px){.container{grid-template-columns:1fr;padding:8px}.header{flex-direction:column;align-items:flex-start}.controls{width:100%}.sensor-grid{grid-template-columns:1fr}}
</style>
</head>
<body>
<div class="container">
  <div class="card">
    <div class="header">
      <div>
        <h2 style="margin:0">Smart Fridge — Live</h2>
        <div class="muted small">Auto detection on weight changes</div>
      </div>
      <div class="controls">
        <div class="muted" id="conn">Connecting...</div>
      </div>
    </div>

    <div style="height:10px"></div>
    <div class="sensor-grid">
      <div class="sensor"><div class="muted">Top detection</div><div id="topLabel" class="val">—</div></div>
      <div class="sensor"><div class="muted">IP</div><div id="espIP" class="val">__ESP_IP__</div></div>
      <div class="sensor"><div class="muted">Raw response</div><pre id="rawResp" style="white-space:pre-wrap;max-height:120px;overflow:auto;background:rgba(0,0,0,0.06);padding:6px;border-radius:6px"></pre></div>
      <div class="sensor"><div class="muted">Last detection</div><div id="lastDetect" class="val">—</div></div>
    </div>
    <div class="footer">Open this page from your phone on the same Wi‑Fi: <strong>http://<span id="footerIP">__ESP_IP__</span></strong></div>
  </div>

  <div class="card">
    <h3 style="margin:0 0 8px 0">Stock Inventory</h3>
    <div class="sensor-grid" id="stockList"></div>
    <div style="height:8px"></div>
    <div style="display:flex;gap:8px"><button class="btn" id="clearStock">Clear Stock</button></div>
  </div>

  <div class="card">
    <h3 style="margin:0 0 8px 0">Sensors & Settings</h3>
    <div class="sensor-grid">
      <div class="sensor"><div class="muted">Humidity</div><div id="hum" class="val">-- %</div></div>
      <div class="sensor"><div class="muted">Temperature</div><div id="temp" class="val">-- °C</div></div>
      <div class="sensor"><div class="muted">Weight</div><div id="weight" class="val">--</div></div>
      <div class="sensor"><div class="muted">Δ (delta)</div><div id="delta" class="val">--</div></div>
      <div class="sensor"><div class="muted">Door</div><div id="door" class="val">--</div></div>
      <div class="sensor"><div class="muted">Pending</div><div id="pending" class="val">--</div></div>
      <div class="sensor"><div class="muted">LED</div><div id="led" class="val">--</div></div>
      <div class="sensor"><div class="muted">Fan State</div><div id="fanState" class="val">--</div></div>
      <div class="sensor"><div class="muted">Fan Speed</div><div id="fanSpeed" class="val">--</div></div>
    </div>

    <div style="height:8px"></div>
    <div class="settings">
      <div class="muted">Fan temperature threshold (°C) — fan ramps from 0 → 255 after this temperature</div>
      <div class="range-row"><input id="fanInput" type="number" min="15" max="40" step="0.1"><div style="width:64px;text-align:right" id="fanVal">26.5°C</div></div>

      <div class="muted">Weight tolerance (units of scale) — sensitivity to detect add/remove</div>
      <div class="range-row"><input id="tolInput" type="number" min="0" max="100" step="0.5"><div style="width:64px;text-align:right" id="tolVal">5</div></div>

      <div style="display:flex;gap:8px"><button class="btn" id="saveSettings">Save settings</button><button class="btn" id="resetSettings">Reset Defaults</button></div>
    </div>

    <div style="height:12px"></div>
    <div class="muted">Local logs (<span id="logCount">0</span>)</div>
    <div id="logList"></div>
    <div style="height:8px"></div>
    <div style="display:flex;gap:8px"><button class="btn" id="clearLogs">Clear Local Logs</button></div>
  </div>
</div>

<script>
const conn = document.getElementById('conn');
const espIP = document.getElementById('espIP');
const footerIP = document.getElementById('footerIP');
const topLabel = document.getElementById('topLabel');
const rawResp = document.getElementById('rawResp');
const tempEl = document.getElementById('temp');
const humEl = document.getElementById('hum');
const weightEl = document.getElementById('weight');
const deltaEl = document.getElementById('delta');
const doorEl = document.getElementById('door');
const pendingEl = document.getElementById('pending');
const lastDetectEl = document.getElementById('lastDetect');
const logCountEl = document.getElementById('logCount');
const logList = document.getElementById('logList');
const clearLogs = document.getElementById('clearLogs');
const fanInput = document.getElementById('fanInput');
const fanVal = document.getElementById('fanVal');
const tolInput = document.getElementById('tolInput');
const tolVal = document.getElementById('tolVal');
const saveSettings = document.getElementById('saveSettings');
const resetSettings = document.getElementById('resetSettings');
const ledEl = document.getElementById('led');
const fanStateEl = document.getElementById('fanState');
const fanSpeedEl = document.getElementById('fanSpeed');
const stockList = document.getElementById('stockList');
const clearStock = document.getElementById('clearStock');

async function fetchStatus(){
  try{
    const r = await fetch('/api/status'); if (!r.ok) { conn.innerText='Offline'; return; }
    const j = await r.json(); conn.innerText='Online'; espIP.innerText = j.ip; footerIP.innerText = j.ip; tempEl.innerText = j.temperature + ' °C'; humEl.innerText = j.humidity + ' %'; weightEl.innerText = parseFloat(j.weight).toFixed(2); deltaEl.innerText = parseFloat(j.delta).toFixed(2); doorEl.innerText = j.door; pendingEl.innerText = j.pendingAction || '--'; lastDetectEl.innerText = j.lastDetection || '—'; logCountEl.innerText = j.localLogCount || 0; 
    if (j.fanThreshold && !fanInput.matches(':focus')) { 
      fanInput.value = j.fanThreshold; 
      fanVal.innerText = parseFloat(j.fanThreshold).toFixed(1) + '°C'; 
    } 
    if (j.tolerance!==undefined && !tolInput.matches(':focus')) { 
      tolInput.value = j.tolerance; 
      tolVal.innerText = j.tolerance; 
    } 
    ledEl.innerText = j.led; fanStateEl.innerText = j.fanSpeed > 0 ? 'On' : 'Off'; fanSpeedEl.innerText = j.fanSpeed; }
  catch(e){ conn.innerText='Offline'; }
}

async function fetchLogs(){
  try{
    const r = await fetch('/logs'); if (!r.ok) return; const arr = await r.json(); logList.innerHTML=''; for (let i=arr.length-1;i>=0 && i>=arr.length-20;i--){ const it = arr[i]; const div = document.createElement('div'); div.style.padding='8px'; div.style.borderBottom='1px solid rgba(255,255,255,0.04)'; div.innerHTML = `<div style='font-weight:700'>${it.product}</div><div class='muted' style='font-size:12px'>${new Date(it.ts).toLocaleString()} Δ=${it.delta} act=${it.action}</div>`; logList.appendChild(div); } }
  catch(e){}
}

async function fetchStock(){
  try{
    const r = await fetch('/api/stock'); if (!r.ok) return; const arr = await r.json(); let html = ''; for(let item of arr){ html += `<div class="sensor"><div class="muted">${item.name}</div><div class="val">${item.quantity} (${item.weight.toFixed(2)}g)</div></div>`; } stockList.innerHTML = html || '<div class="muted">No stock items</div>'; }
  catch(e){}
}

clearLogs.addEventListener('click', async ()=>{ await fetch('/logs/clear',{method:'POST'}); await fetchLogs(); await fetchStatus(); });

clearStock.addEventListener('click', async ()=>{ await fetch('/api/stock/clear',{method:'POST'}); await fetchStock(); });

fanInput.addEventListener('input', ()=>{ 
  let v = parseFloat(fanInput.value.replace(',', '.')); 
  if(!isNaN(v)) fanVal.innerText = v.toFixed(1) + '°C'; 
});
tolInput.addEventListener('input', ()=>{ 
  let v = parseFloat(tolInput.value.replace(',', '.')); 
  if(!isNaN(v)) tolVal.innerText = v.toFixed(1); 
});

saveSettings.addEventListener('click', async ()=>{
  let fanThresh = parseFloat(fanInput.value.replace(',', '.'));
  let tol = parseFloat(tolInput.value.replace(',', '.'));
  if(isNaN(fanThresh)) fanThresh = 26.5;
  if(isNaN(tol)) tol = 5.0;
  const payload = {fanThreshold: fanThresh, tolerance: tol};
  await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});
  await fetchStatus();
});

resetSettings.addEventListener('click', async ()=>{
  await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({reset:true})});
  await fetchStatus();
});

setInterval(fetchStatus,1500);
setInterval(fetchLogs,4000);
setInterval(fetchStock,5000);
window.addEventListener('load', ()=>{ fetchStatus(); fetchLogs(); fetchStock(); });
</script>
</body>
</html>
)rawliteral";

  String finalHtml = html;
  finalHtml.replace("__ESP_IP__", WiFi.localIP().toString());
  server.send(200, "text/html", finalHtml);
}

// -------- minimal proxy endpoint: lets browser call /proxy_detect (same origin) and micro-proxy to laptop /detect
void handleProxyDetect() {
  String resp = requestDetectionFromLaptop();
  if (resp.startsWith("{") || resp.startsWith("[")) {
    server.send(200, "application/json", resp);
  } else {
    String out = "{\"status\":\"ok\",\"detections\":[]}";
    if (resp.startsWith("Error") || resp == "No product detected") {
      server.send(200, "application/json", out);
    } else {
      String esc = resp;
      esc.replace("\"", "\\\"");
      String j = "{\"status\":\"ok\",\"detections\":[{\"name\":\"" + esc + "\",\"conf\":1.0}]}";
      server.send(200, "application/json", j);
    }
  }
}

// -------- proxy endpoint for stream: proxies /stream from laptop
void handleProxyStream() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "text/plain", "WiFi not connected");
    return;
  }

  HTTPClient http;
  String url = laptopStreamURL();
  http.begin(url);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    server.send(502, "text/plain", "Failed to connect to laptop stream");
    http.end();
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Type", "multipart/x-mixed-replace; boundary=frame");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200);

  WiFiClient *stream = http.getStreamPtr();
  while (http.connected() && server.client().connected()) {
    if (stream->available()) {
      uint8_t buf[512];
      size_t len = stream->readBytes(buf, sizeof(buf));
      if (len > 0) {
        server.client().write(buf, len);
      }
    }
    delay(1);  // yield to avoid watchdog
  }

  http.end();
}

// -------- API: status and logs --------
void handleApiStatus() {
  float w = scale.get_units(5);
  String tempStr = isnan(temperature) ? "NaN" : String(temperature, 2);
  String humStr = isnan(humidity) ? "NaN" : String(humidity, 2);
  String out = "{";
  out += "\"temperature\":" + tempStr + ",";
  out += "\"humidity\":" + humStr + ",";
  out += "\"weight\":" + String(w, 3) + ",";
  out += "\"delta\":" + String(deltaWeight, 3) + ",";
  out += "\"door\":\"" + lastDoorStateStr + "\",";
  out += "\"pendingAction\":\"" + pendingAction + "\",";
  out += "\"lastDetection\":\"" + lastDetection + "\",";
  out += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  out += "\"localLogCount\":" + String(getLocalLogCount()) + ",";
  out += "\"fanThreshold\":" + String(fanTempThreshold, 2) + ",";
  out += "\"fanSpeed\":" + String(currentFanSpeed) + ",";
  out += "\"led\":\"" + String(ledOn ? "Ndezur" : "Fikur") + "\"";
  out += "}";
  server.send(200, "application/json", out);
}

void handleLogsList() {
  String arr = readAllLogsAsJsonArray();
  server.send(200, "application/json", arr);
}

void handleLogsClear() {
  if (server.method() == HTTP_POST) {
    clearLocalLogs();
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(405, "text/plain", "Method not allowed");
  }
}

// Adapted API: stock inventory using proper FirebaseJson iteration
void handleApiStock() {
  if (!Firebase.ready()) {
    server.send(200, "application/json", "[]");
    return;
  }

  FirebaseJson json;
  String path = "/SmartFridge/Stock";
  if (!Firebase.RTDB.getJSON(&fbdo, path, &json)) {
    server.send(200, "application/json", "[]");
    return;
  }

  String out = "[";
  bool first = true;
  size_t len = json.iteratorBegin();
  for (size_t i = 0; i < len; i++) {
    FirebaseJson::IteratorValue val = json.valueAt(i);
    if (val.type == FirebaseJson::JSON_OBJECT) {
      String key = val.key;
      FirebaseJson subJson;
      subJson.setJsonData(val.value);
      FirebaseJsonData data;
      float weight = 0.0;
      if (subJson.get(data, "weight_total")) {
        weight = data.doubleValue;
      }
      int quantity = 0;
      if (subJson.get(data, "quantity")) {
        quantity = data.intValue;
      }
      if (!first) out += ",";
      out += "{\"name\":\"" + key + "\",\"quantity\":" + String(quantity) + ",\"weight\":" + String(weight, 2) + "}";
      first = false;
    }
  }
  out += "]";
  json.iteratorEnd();
  server.send(200, "application/json", out);
}

// New: Clear stock in Firebase
void handleClearStock() {
  if (server.method() == HTTP_POST) {
    if (Firebase.ready()) {
      if (Firebase.RTDB.deleteNode(&fbdo, "/SmartFridge/Stock")) {
        server.send(200, "application/json", "{\"ok\":true}");
      } else {
        server.send(500, "application/json", "{\"error\":\"Failed to clear stock\"}");
      }
    } else {
      server.send(503, "application/json", "{\"error\":\"Firebase not ready\"}");
    }
  } else {
    server.send(405, "text/plain", "Method not allowed");
  }
}

// Settings endpoints (GET/POST)
String getValueFromJson(const String &body, const String &key) {
  int idx = body.indexOf('\"' + key + '\"');
  if (idx < 0) return "";
  int colon = body.indexOf(':', idx);
  if (colon < 0) return "";
  int start = colon + 1;
  while (start < (int)body.length() && isSpace(body[start])) start++;
  int end = start;
  // read until comma or brace
  while (end < (int)body.length() && body[end] != ',' && body[end] != '}') end++;
  String val = body.substring(start, end);
  val.trim();
  // remove quotes
  if (val.startsWith("\"") && val.endsWith("\"")) val = val.substring(1, val.length() - 1);
  return val;
}

void handleGetSettings() {
  String out = "{";
  out += "\"fanThreshold\":" + String(fanTempThreshold, 2) + ",";
  out += "\"tolerance\":" + String(tolerance, 2);
  out += "}";
  server.send(200, "application/json", out);
}

void handlePostSettings() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method not allowed");
    return;
  }
  String body = "";
  if (server.hasArg("plain")) body = server.arg("plain");
  body.trim();
  if (body.indexOf("reset") >= 0) {
    fanTempThreshold = 26.5;
    tolerance = 5.0;
    prefs.putFloat("fan_thresh", fanTempThreshold);
    prefs.putFloat("tolerance", tolerance);
    safeFirebaseSetFloat("/SmartFridge/Settings/fanThreshold", fanTempThreshold);
    safeFirebaseSetInt("/SmartFridge/Settings/tolerance", (int)tolerance);
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  String ft = getValueFromJson(body, "fanThreshold");
  String tol = getValueFromJson(body, "tolerance");
  if (ft.length()) {
    fanTempThreshold = ft.toFloat();
    prefs.putFloat("fan_thresh", fanTempThreshold);
    safeFirebaseSetFloat("/SmartFridge/Settings/fanThreshold", fanTempThreshold);
  }
  if (tol.length()) {
    tolerance = tol.toFloat();
    prefs.putFloat("tolerance", tolerance);
    safeFirebaseSetInt("/SmartFridge/Settings/tolerance", (int)tolerance);
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// -------- other server handler (kept simple)
void handleFallback() {
  server.send(404, "text/plain", "Not found");
}

// ---------- Safe Firebase Set Functions ----------
void safeFirebaseSetString(const String &path, const String &value) {
  if (Firebase.ready() && Firebase.RTDB.setString(&fbdo, path, value)) {
    Serial.println("✅ Set string to " + path + ": " + value);
  } else {
    Serial.println("❌ Failed to set string to " + path + ": " + fbdo.errorReason());
  }
}

void safeFirebaseSetFloat(const String &path, float value) {
  if (Firebase.ready() && Firebase.RTDB.setFloat(&fbdo, path, value)) {
    Serial.println("✅ Set float to " + path + ": " + String(value));
  } else {
    Serial.println("❌ Failed to set float to " + path + ": " + fbdo.errorReason());
  }
}

void safeFirebaseSetInt(const String &path, int value) {
  if (Firebase.ready() && Firebase.RTDB.setInt(&fbdo, path, value)) {
    Serial.println("✅ Set int to " + path + ": " + String(value));
  } else {
    Serial.println("❌ Failed to set int to " + path + ": " + fbdo.errorReason());
  }
}

void safeFirebaseSetBool(const String &path, bool value) {
  if (Firebase.ready() && Firebase.RTDB.setBool(&fbdo, path, value)) {
    Serial.println("✅ Set bool to " + path + ": " + String(value));
  } else {
    Serial.println("❌ Failed to set bool to " + path + ": " + fbdo.errorReason());
  }
}

// ---------- rest of your original sketch (setup/loop, DHT, scale, Firebase initialization) ----------
void updateFirebaseStates(String doorStr, String ledStr, String fanStr, int fanSpd) {
  String basePath = "/SmartFridge/States";
  safeFirebaseSetString(basePath + "/Door", doorStr);
  safeFirebaseSetString(basePath + "/LED", ledStr);
  safeFirebaseSetString(basePath + "/Fan/state", fanStr);
  safeFirebaseSetInt(basePath + "/Fan/speed", fanSpd);
  safeFirebaseSetBool(basePath + "/Pending", waitingForProduct || pendingAction.length() > 0);
  Serial.println("✅ Gjendjet u përditësuan në Firebase (ose u provuan).");
}

void clearValueArea(int y, int height) {
  tft.fillRect(VALUE_X, y, CLEAR_WIDTH, height, TFT_BLACK);
}

String getTimeStr() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeStr[30];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S %d/%m/%Y", &timeinfo);
    return String(timeStr);
  } else {
    return "Koha: N/A";
  }
}

uint16_t getTempColor() {
  if (isnan(temperature)) return TFT_WHITE;
  if (temperature < getTempMin()) return TFT_BLUE;
  if (temperature >= getTempIdealMin() && temperature <= getTempIdealMax()) return TFT_GREEN;
  return TFT_RED;
}

void updateScreen() {
  bool thresholdChanged = (fanTempThreshold != prevFanThr);
  bool tempChanged = (temperature != prevTemperature) || thresholdChanged;
  uint16_t textColor = getTempColor();
  bool colorChanged = (textColor != prevTextColor) && tempChanged;

  // Get current values
  String timeStr = getTimeStr();
  float w = scale.get_units(5);
  int logCount = getLocalLogCount();

  // Update time (always, since it changes)
  if (timeStr != prevTimeStr) {
    clearValueArea(Y_TIME, LINE_HEIGHT_SMALL);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_TIME);
    tft.print(timeStr);
    prevTimeStr = timeStr;
  }

  // Update humidity
  if (humidity != prevHumidity) {
    clearValueArea(Y_HUM, LINE_HEIGHT_SMALL);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_HUM);
    tft.print(isnan(humidity) ? "--" : String(humidity));
    tft.print(" %");
    prevHumidity = humidity;
  }

  // Update temperature
  if (tempChanged || colorChanged) {
    clearValueArea(Y_TEMP, LINE_HEIGHT_SMALL);
    tft.setTextColor(textColor);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_TEMP);
    tft.print(isnan(temperature) ? "--" : String(temperature));
    tft.print(" C");
    prevTemperature = temperature;
    prevTextColor = textColor;
  }

  // Update weight
  if (w != prevWeight) {
    clearValueArea(Y_WEIGHT, LINE_HEIGHT_SMALL);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_WEIGHT);
    tft.print(w);
    tft.print(" g");
    prevWeight = w;
  }

  // Update delta
  if (deltaWeight != prevDelta) {
    clearValueArea(Y_DELTA, LINE_HEIGHT_SMALL);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_DELTA);
    tft.print(deltaWeight);
    tft.print(" g");
    prevDelta = deltaWeight;
  }

  // Update door
  if (lastDoorStateStr != prevDoor) {
    clearValueArea(Y_DOOR, LINE_HEIGHT_SMALL);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_DOOR);
    tft.print(lastDoorStateStr);
    prevDoor = lastDoorStateStr;
  }

  // Update pending
  String currPending = pendingAction.length() ? pendingAction : "-";
  if (currPending != prevPending) {
    clearValueArea(Y_PENDING, LINE_HEIGHT_SMALL);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_PENDING);
    tft.print(currPending);
    prevPending = currPending;
  }

  // Update last detection
  if (lastDetection != prevLastDet) {
    clearValueArea(Y_LASTDET, LINE_HEIGHT_SMALL);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_LASTDET);
    tft.print(lastDetection);
    prevLastDet = lastDetection;
  }

  // Update LED
  if (ledOn != prevLed) {
    clearValueArea(Y_LED, LINE_HEIGHT_SMALL);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_LED);
    tft.print(ledOn ? "Ndezur" : "Fikur");
    prevLed = ledOn;
  }

  // Update fan speed
  if (currentFanSpeed != prevFanSpd) {
    clearValueArea(Y_FANSPD, LINE_HEIGHT_SMALL);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_FANSPD);
    tft.print(currentFanSpeed);
    prevFanSpd = currentFanSpeed;
  }

  // Update fan threshold
  if (fanTempThreshold != prevFanThr) {
    clearValueArea(Y_FANTHR, LINE_HEIGHT_SMALL);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_FANTHR);
    tft.print(String(fanTempThreshold, 1));
    tft.print(" C");
    prevFanThr = fanTempThreshold;
  }

  // Update IP
  String currIP = WiFi.localIP().toString();
  if (currIP != prevIP) {
    clearValueArea(Y_IP, LINE_HEIGHT_SMALL);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(VALUE_X, Y_IP);
    tft.print(currIP);
    prevIP = currIP;
  }

  // Update set threshold (edit value)
  if (editFanThreshold != prevEditFanThr) {
    tft.fillRect(NUM_X + 1, Y_SETTHR + 1, NUM_W - 2, NUM_H - 2, TFT_BLACK);  // Clear inside rect
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);
    tft.setCursor(NUM_X + 5, Y_SETTHR + 6);
    tft.print(String(editFanThreshold, 1));
    prevEditFanThr = editFanThreshold;
  }
}

void handleRootPost() {
  if (server.method() == HTTP_POST && server.hasArg("plain")) {
    String product = server.arg("plain");
    processDetectedProduct(product);
    server.send(200, "text/plain", String("U pranua: ") + product);
  } else {
    handleRoot();
  }
}

void readAndUpdateDHT() {
  lastDHTRead = millis();
  float newHumidity = dht.readHumidity();
  float newTemperature = dht.readTemperature();
  if (isnan(newHumidity) || isnan(newTemperature)) {
    Serial.println("⚠ Failed to read from DHT sensor!");
    return;  // Don't update if invalid
  }
  humidity = newHumidity;
  temperature = newTemperature;

  String envPath = "/SmartFridge/Environment";
  safeFirebaseSetFloat(envPath + "/humidity", humidity);
  safeFirebaseSetFloat(envPath + "/temperature", temperature);

  // Updated Fan control logic:
  // - Below min (blue): speed = 0
  // - Ideal (green): low speed (e.g., 64, quiet)
  // - Above threshold (red): ramp from 128 to 255 over +8°C
  float tempMin = getTempMin();
  float tempIdealMax = getTempIdealMax();
  if (temperature < tempMin) {
    currentFanSpeed = 0;
  } else if (temperature >= tempMin && temperature <= tempIdealMax) {
    currentFanSpeed = 64;  // Quiet low speed
  } else {
    float over = temperature - tempIdealMax;  // degrees above threshold
    int speed = 128 + (int)((over / 8.0) * 127.0);  // Ramp from 128 to 255 over +8°C
    if (speed > 255) speed = 255;
    currentFanSpeed = speed;
  }
  ledcWrite(FAN_PIN, currentFanSpeed);

  // Update Firebase if fan speed changed
  if (currentFanSpeed != lastFanSpeed) {
    updateFirebaseStates(lastDoorStateStr, ledOn ? "Ndezur" : "Fikur", currentFanSpeed > 0 ? "On" : "Off", currentFanSpeed);
    lastFanSpeed = currentFanSpeed;
  }
}

void calibrateTouch() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);

  // Prompt for top-left corner
  tft.drawCircle(10, 10, 5, TFT_RED);
  tft.setCursor(20, 20);
  tft.println("Touch top-left corner");

  TS_Point p1;
  while (true) {
    if (ts.touched()) {
      p1 = ts.getPoint();
      while (ts.touched()) delay(10);  // Wait for release
      break;
    }
    delay(10);
  }

  tft.fillScreen(TFT_BLACK);

  // Prompt for bottom-right corner
  tft.drawCircle(310, 230, 5, TFT_RED);
  tft.setCursor(200, 200);
  tft.println("Touch bottom-right corner");

  TS_Point p2;
  while (true) {
    if (ts.touched()) {
      p2 = ts.getPoint();
      while (ts.touched()) delay(10);  // Wait for release
      break;
    }
    delay(10);
  }

  // Set min/max with margin
  TS_MINX = min(p1.x, p2.x) - 100;
  TS_MAXX = max(p1.x, p2.x) + 100;
  TS_MINY = min(p1.y, p2.y) - 100;
  TS_MAXY = max(p1.y, p2.y) + 100;

  // Save to preferences
  prefs.putUShort("ts_minx", TS_MINX);
  prefs.putUShort("ts_maxx", TS_MAXX);
  prefs.putUShort("ts_miny", TS_MINY);
  prefs.putUShort("ts_maxy", TS_MAXY);
  prefs.putBool("calibrated", true);

  tft.fillScreen(TFT_BLACK);
  tft.setCursor(50, 100);
  tft.println("Calibration complete");
  delay(2000);
}

void drawButtons() {
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(TEXT_SIZE_SMALL);
  tft.setCursor(LABEL_X, Y_SETTHR);
  tft.print("SetThr: ");
  tft.drawRect(NUM_X, Y_SETTHR, NUM_W, NUM_H, TFT_WHITE);
  tft.drawRect(PLUS_X, Y_SETTHR, PLUS_W, PLUS_H, TFT_WHITE);
  tft.setCursor(PLUS_X + 10, Y_SETTHR + 6);
  tft.print("+");
  tft.drawRect(MINUS_X, Y_SETTHR, MINUS_W, MINUS_H, TFT_WHITE);
  tft.setCursor(MINUS_X + 10, Y_SETTHR + 6);
  tft.print("-");
  tft.drawRect(SAVE_X, Y_SETTHR, SAVE_W, SAVE_H, TFT_WHITE);
  tft.setCursor(SAVE_X + 5, Y_SETTHR + 6);
  tft.print("Save");
}

// Firebase stream callback for settings
void streamCallback(FirebaseStream data) {
  String path = data.dataPath();
  if (data.dataTypeEnum() == fb_esp_rtdb_data_type_float) {
    if (path == "/fanThreshold") {
      fanTempThreshold = data.floatData();
      editFanThreshold = fanTempThreshold;
      prefs.putFloat("fan_thresh", fanTempThreshold);
      Serial.println("Updated fanThreshold from Firebase: " + String(fanTempThreshold));
    } else if (path == "/tolerance") {
      tolerance = data.floatData();
      prefs.putFloat("tolerance", tolerance);
      Serial.println("Updated tolerance from Firebase: " + String(tolerance));
    }
  }
}

// Firebase stream callback for detection results
void detectionCallback(FirebaseStream data) {
  if (data.dataPath() == "/product" && data.dataTypeEnum() == fb_esp_rtdb_data_type_string && waitingForProduct && data.stringData().length() > 0) {
    String product = data.stringData();
    processDetectedProduct(product);
    // Clear the pending detection node
    Firebase.RTDB.deleteNode(&fbdo, "/SmartFridge/PendingDetection");
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("Stream timeout, resuming...");
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  pinMode(DOOR_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  ledOn = false;

  prefs.begin("smartfridge", false);
  // load persisted settings
  fanTempThreshold = prefs.getFloat("fan_thresh", 26.5);
  editFanThreshold = fanTempThreshold;  // Initialize edit value
  tolerance = prefs.getFloat("tolerance", 5.0);

  // Load or prompt for LAPTOP_IP with 30s timeout
  String previousIP = prefs.getString("laptop_ip", "192.168.0.14");  // Default IP
  LAPTOP_IP = previousIP;
  Serial.println("Enter new Laptop IP (e.g., 192.168.0.14) or press Enter to keep " + previousIP + ": ");
  unsigned long startTime = millis();
  bool ipUpdated = false;
  while (millis() - startTime < 30000) {
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input.length() > 0) {
        LAPTOP_IP = input;
        prefs.putString("laptop_ip", LAPTOP_IP);
        Serial.println("Saved new Laptop IP: " + LAPTOP_IP);
      } else {
        Serial.println("Keeping previous IP: " + LAPTOP_IP);
      }
      ipUpdated = true;
      break;
    }
    delay(100);
  }
  if (!ipUpdated) {
    Serial.println("Timeout: Using previous IP: " + LAPTOP_IP);
  }

  // Setup PWM for ESP32 core 3.x
  ledcAttach(FAN_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(BACKLIGHT_PIN, PWM_FREQ, PWM_RES);
  ledcWrite(BACKLIGHT_PIN, 128);  // Reduce backlight to 128 (half) to avoid full white; adjust if needed

  tft.begin();
  tft.setRotation(1);
  ts.begin();
  ts.setRotation(1);

  // Force recalibration for debugging
  calibrateTouch();

  // Load calibration values (after calibration)
  TS_MINX = prefs.getUShort("ts_minx", 300);
  TS_MAXX = prefs.getUShort("ts_maxx", 3800);
  TS_MINY = prefs.getUShort("ts_miny", 200);
  TS_MAXY = prefs.getUShort("ts_maxy", 3700);

  tft.fillScreen(TFT_BLACK);

  // Draw static labels and buttons once
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(TEXT_SIZE_LARGE);
  tft.setCursor(LABEL_X, Y_TITLE);
  tft.println("Smart Fridge");

  tft.setTextSize(TEXT_SIZE_SMALL);

  tft.setCursor(LABEL_X, Y_HUM);
  tft.print("Hum: ");

  tft.setCursor(LABEL_X, Y_TEMP);
  tft.print("Temp: ");

  tft.setCursor(LABEL_X, Y_WEIGHT);
  tft.print("Weigh: ");

  tft.setCursor(LABEL_X, Y_DELTA);
  tft.print("Delta: ");

  tft.setCursor(LABEL_X, Y_DOOR);
  tft.print("Door: ");

  tft.setCursor(LABEL_X, Y_PENDING);
  tft.print("Pending: ");

  tft.setCursor(LABEL_X, Y_LASTDET);
  tft.print("LastDet: ");

  tft.setCursor(LABEL_X, Y_LED);
  tft.print("LED: ");

  tft.setCursor(LABEL_X, Y_FANSPD);
  tft.print("Fan Spd: ");

  tft.setCursor(LABEL_X, Y_FANTHR);
  tft.print("Fan Thr: ");

  tft.setCursor(LABEL_X, Y_IP);
  tft.print("IP: ");

  drawButtons();

  screenInitialized = true;

  if (!initLocalDB()) Serial.println("Warning: local DB init failed");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi: " + WiFi.localIP().toString());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  unsigned long ntpStartTime = millis();
  while (!getLocalTime(&timeinfo) && (millis() - ntpStartTime < 15000)) { delay(200); }
  if (getLocalTime(&timeinfo)) Serial.println("Time synced");

  // Firebase init
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;  // see addons/TokenHelper.h

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("✅ Anonymous authentication successful");
  } else {
    Serial.printf("❌ Anonymous sign-up failed: %s\n", config.signer.signupError.message.c_str());
  }
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Load settings from Firebase if available
  if (Firebase.RTDB.getFloat(&fbdo, "/SmartFridge/Settings/fanThreshold")) {
    fanTempThreshold = fbdo.floatData();
    prefs.putFloat("fan_thresh", fanTempThreshold);
  }
  if (Firebase.RTDB.getInt(&fbdo, "/SmartFridge/Settings/tolerance")) {
    tolerance = fbdo.intData();
    prefs.putFloat("tolerance", tolerance);
  }

  // Set up real-time stream for Settings
  if (!Firebase.RTDB.beginStream(&stream, "/SmartFridge/Settings")) {
    Serial.println("Could not begin settings stream: " + stream.errorReason());
  }
  Firebase.RTDB.setStreamCallback(&stream, streamCallback, streamTimeoutCallback);

  // Set up real-time stream for PendingDetection
  if (!Firebase.RTDB.beginStream(&detectionStream, "/SmartFridge/PendingDetection")) {
    Serial.println("Could not begin detection stream: " + detectionStream.errorReason());
  }
  Firebase.RTDB.setStreamCallback(&detectionStream, detectionCallback, streamTimeoutCallback);

  scale.begin(DOUT, CLK);
  scale.set_scale(435.0);
  scale.tare();
  lastWeight = scale.get_units(10);

  dht.begin();

  // register HTTP handlers
  server.on("/", HTTP_GET, handleRoot);
  server.on("/proxy_detect", HTTP_GET, handleProxyDetect);
  server.on("/proxy_stream", HTTP_GET, handleProxyStream);
  server.on("/post", HTTP_POST, handleRootPost);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/stock", HTTP_GET, handleApiStock);
  server.on("/api/stock/clear", HTTP_POST, handleClearStock);
  server.on("/logs", HTTP_GET, handleLogsList);
  server.on("/logs/clear", HTTP_POST, handleLogsClear);
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/settings", HTTP_POST, handlePostSettings);
  server.onNotFound(handleFallback);
  server.begin();
  Serial.println("HTTP server started at " + WiFi.localIP().toString());
  updateFirebaseStates("Mbyllur", "Fikur", "Off", 0);
  updateScreen();
}

struct DebugDot {
  int x;
  int y;
  unsigned long time;
};

const int MAX_DOTS = 10;
DebugDot debugDots[MAX_DOTS];
int numDebugDots = 0;

// ---------- Loop ----------
void loop() {
  server.handleClient();

  int doorState = digitalRead(DOOR_PIN);
  if (doorState != lastDoorState) {
    String doorStr = (doorState == HIGH) ? "Hapur" : "Mbyllur";  // Assume HIGH = open; adjust if hardware is different
    String ledStr = (doorState == HIGH) ? "Ndezur" : "Fikur";
    ledOn = (doorState == HIGH);
    digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
    if (doorStr != lastDoorStateStr) {
      updateFirebaseStates(doorStr, ledStr, currentFanSpeed > 0 ? "On" : "Off", currentFanSpeed);
      lastDoorStateStr = doorStr;
    }
    lastDoorState = doorState;
    updateScreen();  // Immediate update
  }

  if (millis() - lastDHTRead > DHT_INTERVAL) {
    readAndUpdateDHT();
  }

  if (doorState == HIGH) {  // HIGH = open
    float currentWeight = scale.get_units(5);
    if (!waitingForProduct) {
      float diff = currentWeight - lastWeight;
      if (abs(diff) > tolerance) {  // Strictly > 5.0 grams for detection
        deltaWeight = diff;
        pendingAction = (diff > 0) ? "Shtuar" : "Hequr";
        waitingForProduct = true;
        safeFirebaseSetBool("/SmartFridge/States/Pending", true);
        Serial.println("Weight change detected: " + String(deltaWeight) + " -> " + pendingAction);

        // Trigger photo capture on laptop
        String resp = requestDetectionFromLaptop();

        // Set pending detection in Firebase for Colab to process
        if (resp.indexOf("Error") < 0) {
          time_t now;
          struct tm timeinfo;
          bool timeValid = getLocalTime(&timeinfo);
          if (timeValid) {
            now = mktime(&timeinfo);
          } else {
            now = (time_t)millis();
          }
          safeFirebaseSetFloat("/SmartFridge/PendingDetection/delta", deltaWeight);
          safeFirebaseSetString("/SmartFridge/PendingDetection/action", pendingAction);
          safeFirebaseSetInt("/SmartFridge/PendingDetection/timestamp", (int)now);
          // Colab will set /product after ~120s
        } else {
          // If trigger failed, reset
          waitingForProduct = false;
          pendingAction = "";
          deltaWeight = 0.0;
          safeFirebaseSetBool("/SmartFridge/States/Pending", false);
        }

        // update lastWeight after handling
        lastWeight = currentWeight;
      }
    }
  }

  // Handle touch for fan threshold editing
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    // Map touch coordinates using calibrated values (reversed for rotation 1)
    int16_t screen_x = map(p.x, TS_MINX, TS_MAXX, tft.width(), 0);
    int16_t screen_y = map(p.y, TS_MINY, TS_MAXY, tft.height(), 0);

    // Print to serial for debugging
    Serial.printf("Touch raw: x=%d y=%d z=%d -> screen: x=%d y=%d\n", p.x, p.y, p.z, screen_x, screen_y);

    // Draw a red circle at the mapped touch position for visual debugging
    if (numDebugDots < MAX_DOTS) {
      debugDots[numDebugDots].x = screen_x;
      debugDots[numDebugDots].y = screen_y;
      debugDots[numDebugDots].time = millis();
      tft.fillCircle(screen_x, screen_y, 3, TFT_RED);
      numDebugDots++;
    }

    // Check + button
    if (screen_x > PLUS_X && screen_x < PLUS_X + PLUS_W && screen_y > Y_SETTHR && screen_y < Y_SETTHR + PLUS_H) {
      Serial.println("Plus button pressed");
      editFanThreshold += 0.1;
      if (editFanThreshold > 40.0) editFanThreshold = 40.0;  // Max limit
      updateScreen();  // Redraw
    }

    // Check - button
    if (screen_x > MINUS_X && screen_x < MINUS_X + MINUS_W && screen_y > Y_SETTHR && screen_y < Y_SETTHR + MINUS_H) {
      Serial.println("Minus button pressed");
      editFanThreshold -= 0.1;
      if (editFanThreshold < 15.0) editFanThreshold = 15.0;  // Min limit
      updateScreen();  // Redraw
    }

    // Check Save button (tick-like save)
    if (screen_x > SAVE_X && screen_x < SAVE_X + SAVE_W && screen_y > Y_SETTHR && screen_y < Y_SETTHR + SAVE_H) {
      Serial.println("Save button pressed");
      fanTempThreshold = editFanThreshold;
      prefs.putFloat("fan_thresh", fanTempThreshold);
      safeFirebaseSetFloat("/SmartFridge/Settings/fanThreshold", fanTempThreshold);
      Serial.println("Saved new fan threshold: " + String(fanTempThreshold));
      updateScreen();  // Redraw
    }

    delay(200);  // Debounce touch
  }

  // Erase debug red dots after 3 seconds
  bool forceButtonRedraw = false;
  for (int i = 0; i < numDebugDots; ) {
    if (millis() - debugDots[i].time > 3000) {
      tft.fillCircle(debugDots[i].x, debugDots[i].y, 3, TFT_BLACK);
      if (debugDots[i].y >= Y_SETTHR && debugDots[i].y <= Y_SETTHR + NUM_H) {
        forceButtonRedraw = true;
      }
      // Shift array to remove erased dot
      for (int j = i; j < numDebugDots - 1; j++) {
        debugDots[j] = debugDots[j + 1];
      }
      numDebugDots--;
    } else {
      i++;
    }
  }

  if (forceButtonRedraw) {
    drawButtons();
    // Redraw the edit threshold value inside the num box
    tft.fillRect(NUM_X + 1, Y_SETTHR + 1, NUM_W - 2, NUM_H - 2, TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(TEXT_SIZE_SMALL);

    
    tft.setCursor(NUM_X + 5, Y_SETTHR + 6);
    tft.print(String(editFanThreshold, 1));
    // No need to update prevEditFanThr here, as it's force redraw
  }

  if (millis() - lastScreenUpdate > SCREEN_UPDATE_INTERVAL) {
    updateScreen();
    lastScreenUpdate = millis();
  }
}