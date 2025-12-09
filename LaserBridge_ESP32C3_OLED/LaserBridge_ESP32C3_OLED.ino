/*
 * ESP32-C3 LaserBridge (für 0.42" OLED)
 *  - UART <-> WebSocket Bridge für LaserGRBL (ws://IP:81/)
 *  - HTTP Web-UI auf Port 80 mit Status, Live-Log und Steuerbuttons
 *  - 0.42" OLED (72x40, SSD1306, I2C) zeigt IP & Status
 *  - WiFiManager für flexible WLAN-Konfiguration
 *  - OTA-Update Support
 *  - Live-Log via WebSocket (kein Polling)
 *
 * Benötigt:
 *  - esp32 Arduino Core
 *  - WebSockets (Markus Sattler)
 *  - U8g2 (Oliver Kraus)
 *  - WiFiManager (tzapu)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <U8g2lib.h>
#include <Wire.h>

// ====================== KONFIG ======================

// Hostname für OTA und mDNS
const char* HOSTNAME = "laserbridge";

// WiFiManager AP-Name (wenn kein WLAN konfiguriert)
const char* AP_NAME = "LaserBridge-Setup";

// OLED Pins (typisch für ESP32-C3 0.42" OLED Board)
static const int OLED_SDA = 5;
static const int OLED_SCL = 6;

// UART zum Laser
// ESP32-C3:  GPIO21 = RX, GPIO20 = TX
static const int LASER_RX_PIN = 20;   // ESP empfängt hier -> TX des Lasers
static const int LASER_TX_PIN = 21;   // ESP sendet hier  -> RX des Lasers
static const uint32_t LASER_BAUD = 115200;

// WebSocket Ports
static const uint16_t WS_LASER_PORT = 81;  // für LaserGRBL
static const uint16_t WS_UI_PORT    = 82;  // für Web-UI Live-Log

// ====================================================

HardwareSerial LaserSerial(1);   // UART1

WebServer httpServer(80);
WebSocketsServer wsLaser(WS_LASER_PORT);
WebSocketsServer wsUi(WS_UI_PORT);

// 0.42" OLED: 72x40 Pixel, SSD1306
U8G2_SSD1306_72X40_ER_F_HW_I2C oled(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

WiFiManager wifiManager;

// Statusvariablen
bool wifiConnected        = false;
bool wsLaserConnected     = false;
bool wsUiConnected        = false;
bool laserActive          = false;
uint32_t lastLaserActivity = 0;
uint32_t bytesFromWs       = 0;
uint32_t bytesFromLaser    = 0;

// Für event-basiertes OLED-Update
bool oledNeedsUpdate       = true;
bool lastWifiState         = false;
bool lastWsLaserState      = false;
bool lastLaserActiveState  = false;

// Log-Puffer
String serialLog;
const size_t MAX_LOG = 4096;

// ===================== OLED-Helfer ===================

void oledInit() {
  Wire.begin(OLED_SDA, OLED_SCL);
  oled.begin();
  oled.setFont(u8g2_font_7x13_tf);  // Größere Schrift als Standard
  oled.clearBuffer();
  oled.sendBuffer();
}

void oledShowStatus() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x13_tf);  // Größere Schrift (7x13)
  
  // Zeile 1: Letzte 2 Oktette der IP
  String line1;
  if (wifiConnected) {
    IPAddress ip = WiFi.localIP();
    line1 = String(ip[2]) + "." + String(ip[3]);
  } else {
    line1 = "No WiFi";
  }
  
  // Zeile 2: WebSocket + UART kompakt
  String line2 = wsLaserConnected ? "WS OK" : "WS --";
  
  // Zeile 3: Laser-Aktivität
  String line3 = laserActive ? "ACTIVE" : "IDLE";
  
  // Zeichnen (72x40, Font 13px hoch)
  oled.drawStr(0, 13, line1.c_str());
  oled.drawStr(0, 26, line2.c_str());
  oled.drawStr(0, 39, line3.c_str());
  
  oled.sendBuffer();
  oledNeedsUpdate = false;
}

void oledShowMessage(const char* msg1, const char* msg2 = nullptr) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x13_tf);
  oled.drawStr(0, 16, msg1);
  if (msg2) oled.drawStr(0, 32, msg2);
  oled.sendBuffer();
}

void oledShowProgress(const char* title, int percent) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x13_tf);
  oled.drawStr(0, 12, title);
  
  // Fortschrittsbalken
  oled.drawFrame(0, 18, 72, 10);
  int barWidth = (percent * 68) / 100;
  oled.drawBox(2, 20, barWidth, 6);
  
  // Prozent
  String pct = String(percent) + "%";
  oled.drawStr(22, 38, pct.c_str());
  
  oled.sendBuffer();
}

// Prüft ob sich Status geändert hat
void checkOledUpdate() {
  if (wifiConnected != lastWifiState ||
      wsLaserConnected != lastWsLaserState ||
      laserActive != lastLaserActiveState) {
    
    lastWifiState = wifiConnected;
    lastWsLaserState = wsLaserConnected;
    lastLaserActiveState = laserActive;
    oledNeedsUpdate = true;
  }
}

// ================ Log-Helfer ========================

void appendToLog(const String& s) {
  serialLog += s;
  if (serialLog.length() > MAX_LOG) {
    serialLog.remove(0, serialLog.length() - MAX_LOG);
  }
  
  // Live an alle UI-WebSocket-Clients senden
  if (wsUiConnected) {
    String temp = s;  // Lokale Kopie - broadcastTXT braucht non-const
    wsUi.broadcastTXT(temp);
  }
}

// ================ OTA Setup =========================

void setupOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "FW" : "FS";
    Serial.println("OTA Start: " + type);
    oledShowMessage("OTA...", type.c_str());
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA Ende");
    oledShowMessage("OTA OK!", "Reboot..");
    delay(500);
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int percent = (progress / (total / 100));
    static int lastPercent = -1;
    if (percent != lastPercent) {
      lastPercent = percent;
      oledShowProgress("OTA", percent);
    }
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]\n", error);
    oledShowMessage("OTA ERR!", "Retry...");
    delay(2000);
    oledNeedsUpdate = true;
  });
  
  ArduinoOTA.begin();
  Serial.println("OTA bereit.");
}

// ================ Web-UI / HTTP =====================

const char MAIN_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>LaserBridge</title>
<style>
body { font-family: system-ui, sans-serif; background:#111; color:#eee; margin:0; }
.wrap { max-width:900px; margin:20px auto; padding:0 10px; }
.card { background:#1f1f1f; border-radius:8px; padding:16px; margin-bottom:16px; box-shadow:0 2px 6px rgba(0,0,0,0.6);}
h1 { font-size:22px; margin:0 0 8px; }
h2 { font-size:18px; margin:0 0 8px; }
.status-row { margin:4px 0; }
.label { font-weight:bold; display:inline-block; width:140px; }
.badge { display:inline-block; padding:2px 8px; border-radius:4px; font-size:12px; }
.ok { background:#2e7d32; }
.fail { background:#c62828; }
.idle { background:#555; }
button { margin:4px 4px 4px 0; padding:6px 10px; border-radius:4px; border:none; background:#1976d2; color:#fff; cursor:pointer; }
button.danger { background:#d32f2f; }
button.warn { background:#ff9800; }
button:active { transform:translateY(1px); }
#log { background:#000; color:#0f0; padding:8px; border-radius:4px; min-height:200px; font-family:monospace; white-space:pre-wrap; overflow-y:auto; max-height:400px; font-size:13px;}
#cmd { width:75%; padding:6px; font-family:monospace; border-radius:4px; border:1px solid #444; background:#222; color:#eee; }
.ws-status { font-size:11px; color:#888; margin-top:8px; }
.ws-status.connected { color:#4caf50; }
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <h1>ESP32-C3 LaserBridge</h1>
    <div class="status-row">
      <span class="label">IP-Adresse:</span>
      <span id="ip">-</span>
    </div>
    <div class="status-row">
      <span class="label">WiFi:</span>
      <span id="wifi" class="badge idle">-</span>
    </div>
    <div class="status-row">
      <span class="label">WS LaserGRBL:</span>
      <span id="ws" class="badge idle">-</span>
    </div>
    <div class="status-row">
      <span class="label">UART Aktiv:</span>
      <span id="uart" class="badge idle">-</span>
    </div>
    <div class="status-row">
      <span class="label">Bytes von PC:</span>
      <span id="bws">0</span>
    </div>
    <div class="status-row">
      <span class="label">Bytes vom Laser:</span>
      <span id="blas">0</span>
    </div>
    <p style="margin-top:10px;font-size:12px;color:#bbb;">
      LaserGRBL: Verbindung als <code>ESP/ESP8266 WebSocket</code> zu
      <code>ws://<span class="ip-placeholder">IP</span>:81/</code>
    </p>
  </div>

  <div class="card">
    <h2>Steuerung</h2>
    <button onclick="sendCmd('unlock')">Unlock ($X)</button>
    <button onclick="sendCmd('home')">Homing ($H)</button>
    <button onclick="sendCmd('start')">Cycle Start (~)</button>
    <button onclick="sendCmd('hold')">Feed Hold (!)</button>
    <button class="danger" onclick="sendCmd('reset')">Soft Reset (0x18)</button>
    <br><br>
    <input id="cmd" type="text" placeholder="G-Code z.B. G0 X10 Y10" onkeydown="if(event.key==='Enter')sendGcode()" />
    <button onclick="sendGcode()">Senden</button>
    <br><br>
    <button class="warn" onclick="sendCmd('wifireset')">WiFi Reset</button>
    <span style="font-size:11px;color:#888;margin-left:8px;">Löscht gespeicherte WLAN-Daten</span>
  </div>

  <div class="card">
    <h2>Live-Log</h2>
    <div id="log"></div>
    <div id="wsStatus" class="ws-status">WebSocket: Verbinde...</div>
    <button onclick="clearLog()" style="margin-top:8px;">Log leeren</button>
    <button onclick="downloadLog()" style="margin-top:8px;">Log speichern</button>
  </div>
</div>

<script>
const logDiv = document.getElementById('log');
const wsStatusDiv = document.getElementById('wsStatus');
let logWs = null;
let logContent = '';

function cls(el, c) { el.className = 'badge ' + c; }

function updateStatus() {
  fetch('/status')
    .then(r => r.json())
    .then(s => {
      document.getElementById('ip').textContent = s.ip;
      document.getElementById('bws').textContent = s.bytes_from_ws;
      document.getElementById('blas').textContent = s.bytes_from_laser;
      document.querySelectorAll('.ip-placeholder').forEach(el => el.textContent = s.ip);

      const wifi = document.getElementById('wifi');
      wifi.textContent = s.wifi ? 'CONNECTED' : 'DISCONNECTED';
      cls(wifi, s.wifi ? 'ok' : 'fail');

      const ws = document.getElementById('ws');
      ws.textContent = s.ws ? 'CONNECTED' : 'IDLE';
      cls(ws, s.ws ? 'ok' : 'idle');

      const uart = document.getElementById('uart');
      uart.textContent = s.uart ? 'ACTIVE' : 'IDLE';
      cls(uart, s.uart ? 'ok' : 'idle');
    })
    .catch(e => console.error('Status fetch error:', e));
}

function connectLogWebSocket() {
  const wsUrl = 'ws://' + location.hostname + ':82/';
  logWs = new WebSocket(wsUrl);
  
  logWs.onopen = () => {
    wsStatusDiv.textContent = 'WebSocket: Verbunden';
    wsStatusDiv.className = 'ws-status connected';
    fetch('/log').then(r => r.text()).then(t => {
      logContent = t;
      logDiv.textContent = logContent;
      logDiv.scrollTop = logDiv.scrollHeight;
    });
  };
  
  logWs.onmessage = (evt) => {
    logContent += evt.data;
    if (logContent.length > 50000) {
      logContent = logContent.slice(-40000);
    }
    logDiv.textContent = logContent;
    logDiv.scrollTop = logDiv.scrollHeight;
  };
  
  logWs.onclose = () => {
    wsStatusDiv.textContent = 'WebSocket: Getrennt - Reconnect in 3s...';
    wsStatusDiv.className = 'ws-status';
    setTimeout(connectLogWebSocket, 3000);
  };
  
  logWs.onerror = (err) => {
    console.error('WebSocket error:', err);
    logWs.close();
  };
}

function sendCmd(type) {
  fetch('/api/cmd?type=' + encodeURIComponent(type))
    .then(r => r.json())
    .then(d => { if (d.msg) alert(d.msg); })
    .catch(e => console.error(e));
}

function sendGcode() {
  const input = document.getElementById('cmd');
  const g = input.value.trim();
  if (!g) return;
  fetch('/api/cmd?type=gcode&g=' + encodeURIComponent(g))
    .catch(e => console.error(e));
  input.value = '';
  input.focus();
}

function clearLog() {
  logContent = '';
  logDiv.textContent = '';
  fetch('/api/cmd?type=clearlog');
}

function downloadLog() {
  const blob = new Blob([logContent], {type: 'text/plain'});
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = 'laserbridge_log_' + new Date().toISOString().slice(0,19).replace(/:/g,'-') + '.txt';
  a.click();
  URL.revokeObjectURL(url);
}

connectLogWebSocket();
updateStatus();
setInterval(updateStatus, 2000);
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  httpServer.send_P(200, "text/html", MAIN_page);
}

void handleStatus() {
  String json = "{";
  json += "\"ip\":\"";
  json += wifiConnected ? WiFi.localIP().toString() : "0.0.0.0";
  json += "\",\"wifi\":";
  json += wifiConnected ? "true" : "false";
  json += ",\"ws\":";
  json += wsLaserConnected ? "true" : "false";
  json += ",\"uart\":";
  json += laserActive ? "true" : "false";
  json += ",\"bytes_from_ws\":";
  json += bytesFromWs;
  json += ",\"bytes_from_laser\":";
  json += bytesFromLaser;
  json += "}";
  httpServer.send(200, "application/json", json);
}

void handleLog() {
  httpServer.send(200, "text/plain", serialLog);
}

void handleCmd() {
  String type = httpServer.hasArg("type") ? httpServer.arg("type") : "";
  String g    = httpServer.hasArg("g")    ? httpServer.arg("g")    : "";
  String msg  = "";

  if (type == "unlock") {
    LaserSerial.print("$X\n");
    appendToLog("\n>> $X");
  } else if (type == "home") {
    LaserSerial.print("$H\n");
    appendToLog("\n>> $H");
  } else if (type == "start") {
    LaserSerial.print("~");
    appendToLog("\n>> ~ (Cycle Start)");
  } else if (type == "hold") {
    LaserSerial.print("!");
    appendToLog("\n>> ! (Feed Hold)");
  } else if (type == "reset") {
    LaserSerial.write(0x18);
    appendToLog("\n>> 0x18 (Soft Reset)");
  } else if (type == "gcode" && g.length() > 0) {
    LaserSerial.print(g);
    if (!g.endsWith("\n")) LaserSerial.print("\n");
    appendToLog("\n>> " + g);
  } else if (type == "clearlog") {
    serialLog = "";
  } else if (type == "wifireset") {
    msg = "WiFi-Daten gelöscht. ESP startet neu...";
    appendToLog("\n>> WiFi Reset");
    httpServer.send(200, "application/json", "{\"ok\":true,\"msg\":\"" + msg + "\"}");
    delay(500);
    wifiManager.resetSettings();
    ESP.restart();
    return;
  }

  httpServer.send(200, "application/json", "{\"ok\":true}");
}

// ================ WebSocket-Handler ==================

void wsLaserEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      wsLaserConnected = true;
      IPAddress ip = wsLaser.remoteIP(num);
      Serial.printf("[WS81] Client %u connected from %s\n", num, ip.toString().c_str());
      appendToLog("\n[LaserGRBL connected]");
      oledNeedsUpdate = true;
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("[WS81] Client %u disconnected\n", num);
      wsLaserConnected = wsLaser.connectedClients() > 0;
      appendToLog("\n[LaserGRBL disconnected]");
      oledNeedsUpdate = true;
      break;

    case WStype_TEXT:
      if (length > 0) {
        bytesFromWs += length;
        laserActive = true;
        lastLaserActivity = millis();
        LaserSerial.write(payload, length);
        
        // Nur loggen wenn kein Realtime-Command (?, !, ~, 0x18 etc.)
        // Diese werden ständig gesendet und spammen das Log
        if (length == 1) {
          char c = (char)payload[0];
          if (c == '?' || c == '!' || c == '~' || c == 0x18) {
            // Realtime-Commands nicht loggen
            break;
          }
        }
        
        String msg = String((char*)payload, length);
        msg.trim();
        if (msg.length() > 0) {
          appendToLog("\n>> " + msg);
        }
      }
      break;
    default:
      break;
  }
}

void wsUiEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      wsUiConnected = true;
      IPAddress ip = wsUi.remoteIP(num);
      Serial.printf("[WS82] UI Client %u connected from %s\n", num, ip.toString().c_str());
      break;
    }
    case WStype_DISCONNECTED:
      wsUiConnected = wsUi.connectedClients() > 0;
      Serial.printf("[WS82] UI Client %u disconnected\n", num);
      break;

    case WStype_TEXT:
      if (length > 0) {
        LaserSerial.write(payload, length);
        bytesFromWs += length;
        laserActive = true;
        lastLaserActivity = millis();
        
        String msg = String((char*)payload, length);
        msg.trim();
        if (msg.length() > 0) {
          appendToLog("\n>> " + msg);
        }
      }
      break;
    default:
      break;
  }
}

// ======================= SETUP ========================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nESP32-C3 LaserBridge startet...");

  oledInit();
  oledShowMessage("LaserBridge", "Starting...");

  // WiFiManager konfigurieren
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setConnectTimeout(20);
  
  wifiManager.setAPCallback([](WiFiManager *myWiFiManager) {
    Serial.println("Config Portal gestartet");
    oledShowMessage("WiFi Setup", AP_NAME);
  });

  Serial.println("WiFiManager startet...");
  oledShowMessage("WiFi...", "");
  
  if (wifiManager.autoConnect(AP_NAME)) {
    wifiConnected = true;
    Serial.print("WLAN OK, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    wifiConnected = false;
    Serial.println("WLAN-Verbindung fehlgeschlagen.");
  }
  
  WiFi.setHostname(HOSTNAME);
  oledShowStatus();

  // UART zum Laser
  LaserSerial.begin(LASER_BAUD, SERIAL_8N1, LASER_RX_PIN, LASER_TX_PIN);
  Serial.println("Laser-UART gestartet.");

  // OTA Setup
  if (wifiConnected) {
    setupOTA();
  }

  // HTTP-Server
  httpServer.on("/", handleRoot);
  httpServer.on("/status", handleStatus);
  httpServer.on("/log", handleLog);
  httpServer.on("/api/cmd", handleCmd);
  httpServer.begin();
  Serial.println("HTTP-Server auf Port 80 gestartet.");

  // WebSockets
  wsLaser.begin();
  wsLaser.onEvent(wsLaserEvent);
  Serial.printf("WebSocket (Laser) auf Port %u gestartet.\n", WS_LASER_PORT);

  wsUi.begin();
  wsUi.onEvent(wsUiEvent);
  Serial.printf("WebSocket (UI) auf Port %u gestartet.\n", WS_UI_PORT);

  Serial.println("=== LaserBridge bereit ===");
  appendToLog("LaserBridge gestartet\n");
}

// ======================== LOOP ========================

void loop() {
  ArduinoOTA.handle();
  httpServer.handleClient();
  wsLaser.loop();
  wsUi.loop();

  // Daten vom Laser
  if (LaserSerial.available() > 0) {
    uint8_t buf[128];
    size_t len = LaserSerial.readBytes(buf, sizeof(buf));
    if (len > 0) {
      bytesFromLaser += len;
      laserActive = true;
      lastLaserActivity = millis();

      String s = String((char*)buf, len);
      appendToLog(s);

      wsLaser.broadcastTXT((char*)buf, len);
    }
  }

  // Aktivität timeouten
  if (laserActive && millis() - lastLaserActivity > 3000) {
    laserActive = false;
  }

  // Event-basiertes OLED-Update
  checkOledUpdate();
  if (oledNeedsUpdate) {
    oledShowStatus();
  }

  // WiFi-Reconnect Check
  static uint32_t lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    bool currentWifi = (WiFi.status() == WL_CONNECTED);
    if (currentWifi != wifiConnected) {
      wifiConnected = currentWifi;
      oledNeedsUpdate = true;
      if (wifiConnected) {
        Serial.println("WiFi reconnected: " + WiFi.localIP().toString());
      } else {
        Serial.println("WiFi connection lost!");
      }
    }
  }
}
