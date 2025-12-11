/*
 * ESP32-C3 LaserBridge HYBRID v2
 *  - WebSocket Bridge für LaserGRBL (ws://IP:81/)
 *  - G-Code Upload & Dateiverwaltung
 *  - Offline-Druck ohne PC
 *  - 0.42" OLED Status
 *  - WiFiManager & OTA
 *
 * v2 Änderungen:
 *  - Robusteres G-Code Parsing (multi-line, CR/LF handling)
 *  - UI ohne Tabs, alles auf einer Seite
 *  - Größere UI-Elemente
 *  - Größeres Log-Fenster
 *
 * Benötigt:
 *  - esp32 Arduino Core
 *  - WebSockets (Markus Sattler)
 *  - U8g2 (Oliver Kraus)
 *  - WiFiManager (tzapu)
 *  - SPIFFS (im Core enthalten)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <SPIFFS.h>

// ====================== KONFIG ======================

const char* HOSTNAME = "laserbridge";
const char* AP_NAME = "LaserBridge-Setup";

// OLED Pins
static const int OLED_SDA = 5;
static const int OLED_SCL = 6;

// UART zum Laser
static const int LASER_RX_PIN = 20;
static const int LASER_TX_PIN = 21;
static const uint32_t LASER_BAUD = 115200;

// WebSocket Port
static const uint16_t WS_LASER_PORT = 81;

// ====================================================

HardwareSerial LaserSerial(1);
WebServer httpServer(80);
WebSocketsServer wsLaser(WS_LASER_PORT);
U8G2_SSD1306_72X40_ER_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
WiFiManager wifiManager;

// Status
bool wifiConnected = false;
bool wsLaserConnected = false;
bool laserActive = false;
uint32_t lastLaserActivity = 0;
uint32_t bytesFromWs = 0;
uint32_t bytesFromLaser = 0;

// OLED Update
bool oledNeedsUpdate = true;
bool lastWifiState = false;
bool lastWsLaserState = false;
bool lastLaserActiveState = false;

// Job Status
enum JobState { JOB_IDLE, JOB_PRE_HOMING, JOB_RUNNING, JOB_POST_HOMING, JOB_PAUSED, JOB_COMPLETE, JOB_ERROR };
JobState jobState = JOB_IDLE;
File gcodeFile;
String currentJobFile = "";
String pendingJobFile = "";
uint32_t totalLines = 0;
uint32_t currentLine = 0;
bool waitingForOk = false;
uint32_t lastCommandTime = 0;
const uint32_t COMMAND_TIMEOUT = 30000;
const uint32_t HOMING_TIMEOUT = 60000;

// WebSocket Input Buffer für robustes Parsing
String wsInputBuffer = "";

// Log
String serialLog;
const size_t MAX_LOG = 2048;

// ===================== OLED =========================

void oledInit() {
  Wire.begin(OLED_SDA, OLED_SCL);
  oled.begin();
  oled.setFont(u8g2_font_7x13_tf);
  oled.clearBuffer();
  oled.sendBuffer();
}

void oledShowStatus() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x13_tf);
  
  // Zeile 1: IP
  String line1;
  if (wifiConnected) {
    IPAddress ip = WiFi.localIP();
    line1 = String(ip[2]) + "." + String(ip[3]);
  } else {
    line1 = "No WiFi";
  }
  
  // Zeile 2 & 3: Status abhängig vom Job
  String line2, line3;
  if (jobState == JOB_PRE_HOMING || jobState == JOB_POST_HOMING) {
    line2 = "HOMING";
    line3 = jobState == JOB_PRE_HOMING ? "PRE" : "POST";
  } else if (jobState == JOB_RUNNING || jobState == JOB_PAUSED) {
    int pct = totalLines > 0 ? (currentLine * 100 / totalLines) : 0;
    line2 = (jobState == JOB_RUNNING) ? "RUN" : "PAUSE";
    line3 = String(pct) + "%";
  } else if (wsLaserConnected) {
    line2 = "WS OK";
    line3 = laserActive ? "ACTIVE" : "IDLE";
  } else {
    line2 = "WS --";
    line3 = (jobState == JOB_COMPLETE) ? "DONE" : "IDLE";
  }
  
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
  oled.drawFrame(0, 18, 72, 10);
  oled.drawBox(2, 20, (percent * 68) / 100, 6);
  String pct = String(percent) + "%";
  oled.drawStr(22, 38, pct.c_str());
  oled.sendBuffer();
}

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

// ===================== LOG ==========================

void appendToLog(const String& s) {
  // Nicht loggen wenn Job läuft oder WebSocket-Client verbunden ist
  if (jobState == JOB_RUNNING || wsLaserConnected) return;
  
  serialLog += s;
  if (serialLog.length() > MAX_LOG) {
    serialLog.remove(0, serialLog.length() - MAX_LOG);
  }
}

// ===================== GRBL =========================

void grblReset() {
  LaserSerial.write(0x18);
  delay(100);
  waitingForOk = false;
}

void grblUnlock() {
  LaserSerial.println("$X");
}

void grblHome() {
  LaserSerial.println("$H");
}

bool grblSendLine(const String& line) {
  LaserSerial.println(line);
  waitingForOk = true;
  lastCommandTime = millis();
  return true;
}

// Robuste G-Code Zeile senden - entfernt CR, validiert Format
void grblSendGcodeLine(const String& rawLine) {
  String line = rawLine;
  
  // Alle CR entfernen
  line.replace("\r", "");
  line.trim();
  
  // Leere Zeilen ignorieren
  if (line.length() == 0) return;
  
  // Realtime Commands (?, !, ~) direkt senden ohne newline
  if (line.length() == 1) {
    char c = line.charAt(0);
    if (c == '?' || c == '!' || c == '~') {
      LaserSerial.write(c);
      return;
    }
  }
  
  // Ctrl+X (Reset) direkt senden
  if (line.length() == 1 && line.charAt(0) == 0x18) {
    LaserSerial.write(0x18);
    return;
  }
  
  // Normale G-Code Zeile mit LF senden
  LaserSerial.print(line);
  LaserSerial.write('\n');
  
  // Debug (nur non-realtime, kurze commands)
  if (line.length() < 60 && line != "?") {
    Serial.printf("[WS->GRBL] %s\n", line.c_str());
  }
}

void processGrblResponse() {
  static String lineBuffer = "";
  
  while (LaserSerial.available()) {
    char c = LaserSerial.read();
    
    bytesFromLaser++;
    laserActive = true;
    lastLaserActivity = millis();
    
    // Zeile sammeln
    if (c == '\n') {
      // Komplette Zeile an WebSocket senden (mit \n)
      if (wsLaserConnected && lineBuffer.length() > 0) {
        String toSend = lineBuffer + "\n";
        wsLaser.broadcastTXT(toSend);
      }
      
      lineBuffer.trim();
      
      if (lineBuffer.length() > 0) {
        // Log (nur wenn kein Job läuft)
        if (jobState != JOB_RUNNING) {
          appendToLog("\n" + lineBuffer);
        }
        
        // Job-relevante Responses verarbeiten
        if (lineBuffer == "ok") {
          waitingForOk = false;
        }
        else if (lineBuffer.startsWith("error:")) {
          waitingForOk = false;
          Serial.println("GRBL Error: " + lineBuffer);
        }
        else if (lineBuffer.startsWith("ALARM:")) {
          if (jobState == JOB_RUNNING) {
            jobState = JOB_ERROR;
            if (gcodeFile) gcodeFile.close();
            oledNeedsUpdate = true;
          }
          Serial.println("GRBL Alarm: " + lineBuffer);
        }
        else if (lineBuffer.startsWith("Grbl")) {
          Serial.println("GRBL: " + lineBuffer);
          if (jobState != JOB_RUNNING) {
            appendToLog("\n" + lineBuffer);
          }
        }
      }
      lineBuffer = "";
    } else if (c != '\r') {
      lineBuffer += c;
    }
  }
}

// ===================== FILE HELPERS =================

uint32_t countLines(const char* filename) {
  File f = SPIFFS.open(filename, "r");
  if (!f) return 0;
  
  uint32_t count = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0 && !line.startsWith(";") && !line.startsWith("(")) {
      count++;
    }
  }
  f.close();
  return count;
}

String getStorageInfo() {
  size_t total = SPIFFS.totalBytes();
  size_t used = SPIFFS.usedBytes();
  size_t free = total - used;
  
  String info = "{\"total\":" + String(total);
  info += ",\"used\":" + String(used);
  info += ",\"free\":" + String(free);
  info += ",\"percent\":" + String((used * 100) / total) + "}";
  return info;
}

String getFileList() {
  String json = "[";
  File root = SPIFFS.open("/");
  if (!root) {
    return "[]";
  }
  
  File file = root.openNextFile();
  bool first = true;
  
  while (file) {
    if (!file.isDirectory()) {
      String name = String(file.name());
      // Ensure name starts with /
      if (!name.startsWith("/")) name = "/" + name;
      
      String nameLower = name;
      nameLower.toLowerCase();
      
      if (nameLower.endsWith(".gcode") || nameLower.endsWith(".nc") || nameLower.endsWith(".gc") || nameLower.endsWith(".ngc")) {
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + name + "\"";
        json += ",\"size\":" + String(file.size());
        json += ",\"lines\":" + String(countLines(name.c_str())) + "}";
      }
    }
    file = root.openNextFile();
  }
  root.close();
  json += "]";
  Serial.println("Files: " + json);
  return json;
}

// ===================== JOB CONTROL ==================

void startJob(const String& filename) {
  if (jobState == JOB_RUNNING || jobState == JOB_PRE_HOMING || jobState == JOB_POST_HOMING) return;
  
  String path = filename.startsWith("/") ? filename : "/" + filename;
  
  // Prüfen ob Datei existiert
  if (!SPIFFS.exists(path)) {
    appendToLog("\nERR: File not found");
    jobState = JOB_ERROR;
    return;
  }
  
  pendingJobFile = path;
  currentJobFile = filename;
  totalLines = countLines(path.c_str());
  currentLine = 0;
  
  // Pre-Homing starten
  jobState = JOB_PRE_HOMING;
  waitingForOk = true;
  lastCommandTime = millis();
  
  appendToLog("\n>> $H (Pre-Homing)");
  LaserSerial.println("$H");
  
  oledNeedsUpdate = true;
}

void pauseJob() {
  if (jobState == JOB_RUNNING) {
    LaserSerial.write('!');
    jobState = JOB_PAUSED;
    oledNeedsUpdate = true;
  }
}

void resumeJob() {
  if (jobState == JOB_PAUSED) {
    LaserSerial.write('~');
    jobState = JOB_RUNNING;
    oledNeedsUpdate = true;
  }
}

void stopJob() {
  grblReset();
  if (gcodeFile) gcodeFile.close();
  jobState = JOB_IDLE;
  currentLine = 0;
  totalLines = 0;
  currentJobFile = "";
  pendingJobFile = "";
  waitingForOk = false;
  oledNeedsUpdate = true;
}

void processJob() {
  // Pre-Homing Phase
  if (jobState == JOB_PRE_HOMING) {
    if (!waitingForOk) {
      // Homing fertig, Job starten
      gcodeFile = SPIFFS.open(pendingJobFile, "r");
      if (!gcodeFile) {
        appendToLog("\nERR: Cannot open file");
        jobState = JOB_ERROR;
        return;
      }
      appendToLog("\nJob: " + currentJobFile);
      appendToLog("\nLines: " + String(totalLines));
      jobState = JOB_RUNNING;
      oledNeedsUpdate = true;
    } else if (millis() - lastCommandTime > HOMING_TIMEOUT) {
      appendToLog("\nHoming Timeout!");
      jobState = JOB_ERROR;
      oledNeedsUpdate = true;
    }
    return;
  }
  
  // Post-Homing Phase
  if (jobState == JOB_POST_HOMING) {
    if (!waitingForOk) {
      // Homing fertig
      jobState = JOB_COMPLETE;
      appendToLog("\nJob complete!");
      oledNeedsUpdate = true;
    } else if (millis() - lastCommandTime > HOMING_TIMEOUT) {
      appendToLog("\nPost-Homing Timeout!");
      jobState = JOB_COMPLETE;  // Trotzdem als fertig markieren
      oledNeedsUpdate = true;
    }
    return;
  }
  
  // Running Phase
  if (jobState != JOB_RUNNING) return;
  
  if (waitingForOk) {
    if (millis() - lastCommandTime > COMMAND_TIMEOUT) {
      appendToLog("\nTimeout!");
      jobState = JOB_ERROR;
      gcodeFile.close();
      oledNeedsUpdate = true;
    }
    return;
  }
  
  while (gcodeFile.available()) {
    String line = gcodeFile.readStringUntil('\n');
    line.trim();
    
    // Skip empty lines and comments
    if (line.length() == 0) continue;
    if (line.startsWith(";")) continue;
    if (line.startsWith("(")) continue;
    
    // Remove inline comments
    int pos = line.indexOf(';');
    if (pos > 0) line = line.substring(0, pos);
    pos = line.indexOf('(');
    if (pos > 0) line = line.substring(0, pos);
    line.trim();
    
    if (line.length() == 0) continue;
    
    grblSendLine(line);
    currentLine++;
    
    // Update OLED every 20 lines
    if (currentLine % 20 == 0) {
      oledNeedsUpdate = true;
    }
    return;
  }
  
  // End of file - Post-Homing starten
  gcodeFile.close();
  
  jobState = JOB_POST_HOMING;
  waitingForOk = true;
  lastCommandTime = millis();
  
  appendToLog("\n>> $H (Post-Homing)");
  LaserSerial.println("$H");
  oledNeedsUpdate = true;
}

// ===================== OTA ==========================

void setupOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword("laserbridge");
  
  ArduinoOTA.onStart([]() {
    if (jobState == JOB_RUNNING) stopJob();
    oledShowMessage("OTA...");
  });
  
  ArduinoOTA.onEnd([]() {
    oledShowMessage("OTA OK!", "Reboot..");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static int lastPct = -1;
    int pct = progress * 100 / total;
    if (pct != lastPct) {
      lastPct = pct;
      oledShowProgress("OTA", pct);
    }
  });
  
  ArduinoOTA.begin();
}

// ===================== WEB UI =======================

const char MAIN_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>LaserBridge</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#0a0a0a;color:#eee;padding:12px;max-width:800px;margin:0 auto}
.card{background:#1a1a1a;border-radius:10px;padding:16px;margin-bottom:14px}
h1{font-size:22px;margin-bottom:14px;display:flex;align-items:center;gap:10px}
h2{font-size:15px;margin:16px 0 10px;color:#888;text-transform:uppercase;letter-spacing:1px}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.stat{background:#252525;padding:10px;border-radius:6px;text-align:center}
.stat-label{font-size:11px;color:#666;text-transform:uppercase}
.stat-value{font-size:16px;font-weight:bold;margin-top:3px}
.ok{color:#4caf50}.warn{color:#ff9800}.err{color:#f44336}.idle{color:#888}
.progress{background:#252525;border-radius:6px;height:28px;margin:12px 0;overflow:hidden;position:relative}
.progress-fill{background:linear-gradient(90deg,#4caf50,#8bc34a);height:100%;transition:width 0.3s}
.progress-text{position:absolute;width:100%;text-align:center;line-height:28px;font-size:14px;font-weight:500}
button{padding:12px 18px;border:none;border-radius:6px;cursor:pointer;font-size:14px;font-weight:600;transition:all 0.2s}
button:hover{filter:brightness(1.15);transform:translateY(-1px)}
button:active{transform:scale(0.98)}
button:disabled{opacity:0.5;cursor:not-allowed;transform:none}
.btn-green{background:#4caf50;color:#fff}
.btn-orange{background:#ff9800;color:#fff}
.btn-red{background:#f44336;color:#fff}
.btn-blue{background:#2196f3;color:#fff}
.btn-gray{background:#444;color:#fff}
.btn-sm{padding:8px 12px;font-size:12px}
.upload-zone{border:2px dashed #444;border-radius:10px;padding:35px;text-align:center;cursor:pointer;transition:all 0.2s;font-size:16px}
.upload-zone:hover{border-color:#666;background:#1f1f1f}
.upload-zone.drag{border-color:#4caf50;background:#1a2f1a}
.file-item{display:flex;align-items:center;padding:12px;background:#252525;border-radius:8px;margin:8px 0;gap:12px}
.file-item.selected{background:#2e4a2e;border:2px solid #4caf50}
.file-icon{font-size:24px}
.file-info{flex:1;min-width:0}
.file-name{font-family:monospace;font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.file-meta{font-size:12px;color:#666;margin-top:3px}
.file-actions{display:flex;gap:6px}
.storage-bar{background:#252525;border-radius:6px;height:10px;margin-top:8px;overflow:hidden}
.storage-fill{background:linear-gradient(90deg,#2196f3,#03a9f4);height:100%;transition:width 0.3s}
.storage-text{font-size:12px;color:#666;margin-top:6px;display:flex;justify-content:space-between}
#log{background:#000;color:#0f0;padding:12px;border-radius:6px;height:300px;font-family:'Consolas','Monaco',monospace;font-size:12px;overflow-y:auto;white-space:pre-wrap;line-height:1.4}
.control-group{display:flex;flex-wrap:wrap;gap:8px;margin:10px 0}
input[type=text]{padding:12px;border-radius:6px;border:1px solid #444;background:#252525;color:#eee;font-family:monospace;font-size:14px;width:100%}
input[type=text]:focus{outline:none;border-color:#2196f3}
.input-row{display:flex;gap:8px;margin:10px 0}
.input-row input{flex:1}
.empty-state{text-align:center;padding:40px;color:#555;font-size:15px}
.job-info{font-size:13px;color:#888;margin-top:8px}
input[type=file]{display:none}
.log-header{display:flex;justify-content:space-between;align-items:center}
.section{margin-top:20px}
.divider{height:1px;background:#333;margin:20px 0}
.modal-overlay{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.8);z-index:1000;align-items:center;justify-content:center}
.modal-overlay.show{display:flex}
.modal{background:#1a1a1a;border-radius:12px;padding:24px;max-width:400px;width:90%;text-align:center}
.modal h3{margin-bottom:16px;font-size:18px}
.modal p{color:#888;margin-bottom:20px;font-size:14px}
.modal-buttons{display:flex;gap:12px;justify-content:center}
</style>
</head>
<body>

<!-- WiFi Reset Modal -->
<div class="modal-overlay" id="wifiModal">
  <div class="modal">
    <h3>⚠️ WiFi zurücksetzen?</h3>
    <p>Das Gerät startet neu und öffnet einen Access Point zur Neukonfiguration. Die aktuelle WiFi-Verbindung wird gelöscht.</p>
    <div class="modal-buttons">
      <button class="btn-gray" onclick="closeWifiModal()">Abbrechen</button>
      <button class="btn-orange" onclick="confirmWifiReset()">Ja, zurücksetzen</button>
    </div>
  </div>
</div>

<div class="card">
  <h1>🔥 LaserBridge</h1>
  <div class="grid">
    <div class="stat"><div class="stat-label">IP</div><div class="stat-value" id="ip">-</div></div>
    <div class="stat"><div class="stat-label">WiFi</div><div class="stat-value" id="wifi">-</div></div>
    <div class="stat"><div class="stat-label">WebSocket</div><div class="stat-value" id="ws">-</div></div>
    <div class="stat"><div class="stat-label">Status</div><div class="stat-value" id="job">-</div></div>
  </div>
  <div class="progress">
    <div class="progress-fill" id="progress" style="width:0%"></div>
    <div class="progress-text"><span id="progressText">0%</span></div>
  </div>
  <div class="job-info">📄 <span id="jobFile">-</span> | Zeile <span id="line">0</span> / <span id="total">0</span></div>
</div>

<div class="card">
  <h2>📤 G-Code Upload</h2>
  <div class="upload-zone" id="dropzone">
    <input type="file" id="fileInput" accept=".gcode,.nc,.gc,.ngc">
    📤 G-Code Datei hochladen<br>
    <small style="color:#666">Drag & Drop oder Klicken</small>
  </div>
  <div id="uploadStatus" style="font-size:13px;margin:10px 0;min-height:24px"></div>
  
  <div class="divider"></div>
  
  <h2>📂 Gespeicherte Dateien</h2>
  <div id="fileList"></div>
  
  <div class="divider"></div>
  
  <h2>💾 Speicher</h2>
  <div class="storage-bar"><div class="storage-fill" id="storageFill" style="width:0%"></div></div>
  <div class="storage-text">
    <span id="storageUsed">-</span>
    <span id="storageFree">-</span>
  </div>
</div>

<div class="card">
  <h2>▶ Job Steuerung</h2>
  <div class="control-group">
    <button class="btn-green" id="btnStart" onclick="jobCmd('start')" disabled>▶ Start</button>
    <button class="btn-orange" id="btnPause" onclick="jobCmd('pause')">⏸ Pause</button>
    <button class="btn-green" id="btnResume" onclick="jobCmd('resume')" style="display:none">▶ Weiter</button>
    <button class="btn-red" onclick="jobCmd('stop')">⏹ Stop</button>
  </div>
  
  <div class="divider"></div>
  
  <h2>🔧 GRBL Befehle</h2>
  <div class="control-group">
    <button class="btn-blue" onclick="grblCmd('unlock')">🔓 Unlock ($X)</button>
    <button class="btn-blue" onclick="grblCmd('home')">🏠 Home ($H)</button>
    <button class="btn-red" onclick="grblCmd('reset')">🔄 Reset</button>
  </div>
  
  <div class="divider"></div>
  
  <h2>⌨ G-Code senden</h2>
  <div class="input-row">
    <input type="text" id="gcode" placeholder="z.B. G0 X10 Y10" onkeydown="if(event.key==='Enter')sendGcode()">
    <button class="btn-blue" onclick="sendGcode()">Senden</button>
  </div>
  
  <div class="divider"></div>
  
  <div class="log-header">
    <h2>📋 Log</h2>
    <button class="btn-gray btn-sm" onclick="clearLog()">🗑 Leeren</button>
  </div>
  <div id="log">Warte auf Daten...</div>
  
  <div class="divider"></div>
  
  <h2>⚙ System</h2>
  <div class="control-group">
    <button class="btn-orange" onclick="showWifiModal()">📶 WiFi Reset</button>
    <button class="btn-gray" onclick="loadFiles()">🔄 Refresh</button>
  </div>
</div>

<script>
let selectedFile = '';
let currentJobState = 0;

// WiFi Reset Modal
function showWifiModal() {
  document.getElementById('wifiModal').classList.add('show');
}

function closeWifiModal() {
  document.getElementById('wifiModal').classList.remove('show');
}

function confirmWifiReset() {
  closeWifiModal();
  grblCmd('wifireset');
}

// Schließen bei Klick außerhalb
document.getElementById('wifiModal').onclick = function(e) {
  if (e.target === this) closeWifiModal();
};

function updateStatus() {
  fetch('/api/status').then(r=>r.json()).then(s => {
    document.getElementById('ip').textContent = s.ip;
    document.getElementById('wifi').innerHTML = s.wifi ? '<span class="ok">OK</span>' : '<span class="err">✗</span>';
    document.getElementById('ws').innerHTML = s.ws ? '<span class="ok">OK</span>' : '<span class="idle">-</span>';
    
    // States: 0=IDLE, 1=PRE_HOMING, 2=RUNNING, 3=POST_HOMING, 4=PAUSED, 5=COMPLETE, 6=ERROR
    const states = ['IDLE','HOMING','RUN','HOMING','PAUSE','DONE','ERROR'];
    const colors = ['idle','warn','ok','warn','warn','ok','err'];
    currentJobState = s.job;
    document.getElementById('job').innerHTML = '<span class="'+colors[s.job]+'">'+states[s.job]+'</span>';
    
    document.getElementById('line').textContent = s.line;
    document.getElementById('total').textContent = s.total;
    document.getElementById('jobFile').textContent = s.file || 'Keine Datei';
    
    const pct = s.total > 0 ? Math.round(s.line * 100 / s.total) : 0;
    document.getElementById('progress').style.width = pct + '%';
    document.getElementById('progressText').textContent = pct + '%';
    
    // Button states - Job aktiv wenn 1, 2, 3 oder 4
    const jobActive = (s.job >= 1 && s.job <= 4);
    document.getElementById('btnStart').style.display = jobActive ? 'none' : '';
    document.getElementById('btnStart').disabled = !selectedFile;
    document.getElementById('btnPause').style.display = s.job === 2 ? '' : 'none';
    document.getElementById('btnResume').style.display = s.job === 4 ? '' : 'none';
  }).catch(e => console.error('Status error:', e));
}

function loadFiles() {
  fetch('/api/files').then(r=>r.json()).then(files => {
    const list = document.getElementById('fileList');
    
    if (!files || files.length === 0) {
      list.innerHTML = '<div class="empty-state">📭 Keine G-Code Dateien vorhanden</div>';
      return;
    }
    
    list.innerHTML = files.map(f => {
      const isSelected = selectedFile === f.name;
      return `
        <div class="file-item ${isSelected ? 'selected' : ''}" data-file="${f.name}">
          <div class="file-icon">📄</div>
          <div class="file-info">
            <div class="file-name">${f.name}</div>
            <div class="file-meta">${f.lines} Zeilen • ${(f.size/1024).toFixed(1)} KB</div>
          </div>
          <div class="file-actions">
            <button class="btn-green btn-sm" onclick="event.stopPropagation();playFile('${f.name}')" title="Drucken">▶</button>
            <button class="btn-red btn-sm" onclick="event.stopPropagation();deleteFile('${f.name}')" title="Löschen">🗑</button>
          </div>
        </div>
      `;
    }).join('');
    
    list.querySelectorAll('.file-item').forEach(item => {
      item.onclick = () => selectFile(item.dataset.file);
    });
    
  }).catch(e => {
    console.error('Files error:', e);
    document.getElementById('fileList').innerHTML = '<div class="empty-state">❌ Fehler beim Laden</div>';
  });
  
  fetch('/api/storage').then(r=>r.json()).then(s => {
    const pct = Math.round((s.used / s.total) * 100);
    document.getElementById('storageFill').style.width = pct + '%';
    document.getElementById('storageUsed').textContent = 'Belegt: ' + (s.used/1024).toFixed(1) + ' KB (' + pct + '%)';
    document.getElementById('storageFree').textContent = 'Frei: ' + (s.free/1024).toFixed(1) + ' KB';
  }).catch(e => console.error('Storage error:', e));
}

function selectFile(name) {
  selectedFile = name;
  document.querySelectorAll('.file-item').forEach(item => {
    item.classList.toggle('selected', item.dataset.file === name);
  });
  document.getElementById('btnStart').disabled = false;
}

function playFile(name) {
  // Job aktiv wenn 1=PRE_HOMING, 2=RUN, 3=POST_HOMING, 4=PAUSED
  if (currentJobState >= 1 && currentJobState <= 4) {
    alert('Ein Job läuft bereits!');
    return;
  }
  selectedFile = name;
  jobCmd('start');
}

function deleteFile(name) {
  if (!confirm('Datei löschen?\n' + name)) return;
  fetch('/api/delete?file=' + encodeURIComponent(name)).then(r=>r.json()).then(d => {
    if (d.ok) {
      if (selectedFile === name) selectedFile = '';
      loadFiles();
    } else {
      alert('Löschen fehlgeschlagen');
    }
  });
}

// Upload
const dropzone = document.getElementById('dropzone');
const fileInput = document.getElementById('fileInput');
const uploadStatus = document.getElementById('uploadStatus');

dropzone.onclick = () => fileInput.click();
dropzone.ondragover = e => { e.preventDefault(); dropzone.classList.add('drag'); };
dropzone.ondragleave = () => dropzone.classList.remove('drag');
dropzone.ondrop = e => {
  e.preventDefault();
  dropzone.classList.remove('drag');
  if (e.dataTransfer.files.length) uploadFile(e.dataTransfer.files[0]);
};
fileInput.onchange = () => { if (fileInput.files.length) uploadFile(fileInput.files[0]); fileInput.value=''; };

function uploadFile(file) {
  uploadStatus.innerHTML = '⏳ Upload: ' + file.name + '...';
  const formData = new FormData();
  formData.append('file', file);
  
  fetch('/api/upload', { method: 'POST', body: formData })
    .then(r => r.json())
    .then(d => {
      if (d.ok) {
        uploadStatus.innerHTML = '✅ <b>' + file.name + '</b> - ' + d.lines + ' Zeilen';
        selectedFile = '/' + file.name;
        loadFiles();
      } else {
        uploadStatus.innerHTML = '❌ ' + (d.error || 'Upload fehlgeschlagen');
      }
    })
    .catch(e => uploadStatus.innerHTML = '❌ ' + e);
}

function jobCmd(action) {
  let url = '/api/job?action=' + action;
  if (action === 'start' && selectedFile) {
    url += '&file=' + encodeURIComponent(selectedFile);
  }
  fetch(url).then(() => { updateStatus(); loadFiles(); });
}

function grblCmd(cmd) {
  fetch('/api/grbl?cmd=' + cmd).then(() => updateStatus());
}

function sendGcode() {
  const input = document.getElementById('gcode');
  const g = input.value.trim();
  if (!g) return;
  fetch('/api/grbl?cmd=gcode&g=' + encodeURIComponent(g));
  input.value = '';
}

function clearLog() {
  fetch('/api/grbl?cmd=clearlog').then(() => {
    document.getElementById('log').textContent = 'Log geleert.';
  });
}

function updateLog() {
  // Kein Log-Update während aktivem Job (1=PRE_HOMING, 2=RUN, 3=POST_HOMING)
  if (currentJobState >= 1 && currentJobState <= 3) return;
  fetch('/api/log').then(r=>r.text()).then(t => {
    const log = document.getElementById('log');
    if (t && t.trim().length > 0) {
      log.textContent = t;
    } else {
      log.textContent = 'Warte auf Daten...';
    }
    log.scrollTop = log.scrollHeight;
  }).catch(()=>{});
}

// Init
loadFiles();
updateStatus();
setInterval(updateStatus, 1000);
setInterval(updateLog, 1500);
setInterval(loadFiles, 15000);
</script>
</body>
</html>
)rawliteral";

// =================== HTTP HANDLERS ==================

void handleRoot() {
  httpServer.send_P(200, "text/html", MAIN_page);
}

void handleStatus() {
  String json = "{\"ip\":\"" + (wifiConnected ? WiFi.localIP().toString() : String("0.0.0.0")) + "\"";
  json += ",\"wifi\":" + String(wifiConnected ? "true" : "false");
  json += ",\"ws\":" + String(wsLaserConnected ? "true" : "false");
  json += ",\"job\":" + String((int)jobState);
  json += ",\"line\":" + String(currentLine);
  json += ",\"total\":" + String(totalLines);
  json += ",\"file\":\"" + currentJobFile + "\"";
  json += ",\"active\":" + String(laserActive ? "true" : "false");
  json += "}";
  httpServer.send(200, "application/json", json);
}

void handleFiles() {
  httpServer.send(200, "application/json", getFileList());
}

void handleStorage() {
  httpServer.send(200, "application/json", getStorageInfo());
}

void handleLog() {
  httpServer.send(200, "text/plain", serialLog);
}

void handleUpload() {
  HTTPUpload& upload = httpServer.upload();
  static File uploadFile;
  
  if (upload.status == UPLOAD_FILE_START) {
    String filename = "/" + upload.filename;
    uploadFile = SPIFFS.open(filename, "w");
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
  }
}

void handleUploadComplete() {
  String filename = "/" + httpServer.upload().filename;
  uint32_t lines = countLines(filename.c_str());
  httpServer.send(200, "application/json", "{\"ok\":true,\"lines\":" + String(lines) + "}");
}

void handleDelete() {
  String file = httpServer.arg("file");
  if (!file.startsWith("/")) file = "/" + file;
  
  if (SPIFFS.remove(file)) {
    httpServer.send(200, "application/json", "{\"ok\":true}");
  } else {
    httpServer.send(200, "application/json", "{\"ok\":false,\"error\":\"Delete failed\"}");
  }
}

void handleJob() {
  String action = httpServer.arg("action");
  String file = httpServer.arg("file");
  
  if (action == "start" && file.length() > 0) {
    startJob(file);
  } else if (action == "pause") {
    pauseJob();
  } else if (action == "resume") {
    resumeJob();
  } else if (action == "stop") {
    stopJob();
  }
  
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

void handleGrbl() {
  String cmd = httpServer.arg("cmd");
  String g = httpServer.arg("g");
  
  if (cmd == "unlock") {
    grblUnlock();
    appendToLog("\n>> $X");
  } else if (cmd == "home") {
    grblHome();
    appendToLog("\n>> $H");
  } else if (cmd == "reset") {
    grblReset();
    appendToLog("\n>> Reset");
  } else if (cmd == "gcode" && g.length() > 0) {
    LaserSerial.println(g);
    appendToLog("\n>> " + g);
  } else if (cmd == "clearlog") {
    serialLog = "";
    httpServer.send(200, "application/json", "{\"ok\":true}");
    return;
  } else if (cmd == "wifireset") {
    httpServer.send(200, "application/json", "{\"ok\":true}");
    delay(500);
    wifiManager.resetSettings();
    ESP.restart();
    return;
  }
  
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

// ================== WEBSOCKET =======================

// Verarbeitet gepufferte WebSocket-Daten zeilenweise
void processWsBuffer() {
  // Zuerst: Realtime-Commands am Anfang des Buffers sofort verarbeiten
  while (wsInputBuffer.length() > 0) {
    char c = wsInputBuffer.charAt(0);
    
    // Realtime Commands (?, !, ~, Ctrl+X) direkt senden
    if (c == '?' || c == '!' || c == '~' || c == 0x18) {
      LaserSerial.write(c);
      wsInputBuffer.remove(0, 1);
      continue;
    }
    
    // Keine weiteren Realtime-Commands am Anfang
    break;
  }
  
  // Dann: Normale Zeilen verarbeiten
  while (true) {
    // Suche nach Zeilenende (LF oder CRLF)
    int lfPos = wsInputBuffer.indexOf('\n');
    if (lfPos < 0) break;  // Keine komplette Zeile vorhanden
    
    // Zeile extrahieren
    String line = wsInputBuffer.substring(0, lfPos);
    wsInputBuffer.remove(0, lfPos + 1);
    
    // CR am Ende entfernen falls vorhanden
    if (line.endsWith("\r")) {
      line.remove(line.length() - 1);
    }
    
    // Zeile verarbeiten
    grblSendGcodeLine(line);
  }
  
  // Nochmal Realtime-Commands prüfen (könnten nach einer Zeile kommen)
  while (wsInputBuffer.length() > 0) {
    char c = wsInputBuffer.charAt(0);
    if (c == '?' || c == '!' || c == '~' || c == 0x18) {
      LaserSerial.write(c);
      wsInputBuffer.remove(0, 1);
      continue;
    }
    break;
  }
  
  // Buffer-Überlauf verhindern
  if (wsInputBuffer.length() > 256) {
    Serial.println("[WS] Buffer overflow, flushing");
    grblSendGcodeLine(wsInputBuffer);
    wsInputBuffer = "";
  }
}

void wsLaserEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      wsLaserConnected = true;
      wsInputBuffer = "";  // Buffer leeren bei neuer Verbindung
      IPAddress ip = wsLaser.remoteIP(num);
      String logMsg = "\n[WS] Client " + String(num) + " from " + ip.toString();
      // Direkt zum Log (nicht via appendToLog, da wsLaserConnected jetzt true ist)
      serialLog += logMsg;
      Serial.println(logMsg);
      oledNeedsUpdate = true;
      
      // Soft-Reset senden um GRBL-Banner zu triggern - LaserGRBL erwartet das!
      delay(50);
      LaserSerial.write(0x18);
      serialLog += "\n[WS] Sent GRBL reset";
      break;
    }
      
    case WStype_DISCONNECTED: {
      String logMsg = "\n[WS] Client " + String(num) + " disconnected";
      serialLog += logMsg;  // Direkt zum Log
      Serial.printf("[WS] Client %u disconnected (remaining: %d)\n", num, wsLaser.connectedClients());
      wsLaserConnected = wsLaser.connectedClients() > 0;
      wsInputBuffer = "";  // Buffer leeren
      oledNeedsUpdate = true;
      break;
    }
      
    case WStype_ERROR: {
      serialLog += "\n[WS] ERROR client " + String(num);
      Serial.printf("[WS] Client %u error!\n", num);
      break;
    }
      
    case WStype_TEXT:
      if (length > 0) {
        bytesFromWs += length;
        laserActive = true;
        lastLaserActivity = millis();
        
        // Daten zum Buffer hinzufügen
        wsInputBuffer += String((char*)payload, length);
        
        // Buffer verarbeiten
        processWsBuffer();
      }
      break;
      
    case WStype_BIN:
      if (length > 0) {
        bytesFromWs += length;
        laserActive = true;
        lastLaserActivity = millis();
        
        // Binärdaten: Byte für Byte prüfen auf Realtime-Commands
        for (size_t i = 0; i < length; i++) {
          uint8_t b = payload[i];
          // Realtime commands direkt durchreichen
          if (b == '?' || b == '!' || b == '~' || b == 0x18) {
            LaserSerial.write(b);
          } else {
            // Andere Bytes zum Buffer hinzufügen
            wsInputBuffer += (char)b;
          }
        }
        processWsBuffer();
      }
      break;
      
    default:
      break;
  }
}

// ===================== SETUP ========================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== LaserBridge HYBRID v2 ===");

  oledInit();
  oledShowMessage("Starting...");

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Failed!");
    oledShowMessage("SPIFFS ERR");
    delay(3000);
    ESP.restart();
    return;
  }
  Serial.println("SPIFFS: OK");
  Serial.printf("  Total: %d bytes\n", SPIFFS.totalBytes());
  Serial.printf("  Used:  %d bytes\n", SPIFFS.usedBytes());

  // WiFiManager
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setConnectTimeout(20);
  wifiManager.setAPCallback([](WiFiManager *wm) {
    oledShowMessage("WiFi Setup", AP_NAME);
  });

  oledShowMessage("WiFi...");
  
  if (wifiManager.autoConnect(AP_NAME)) {
    wifiConnected = true;
    Serial.println("WiFi: OK");
    Serial.print("  IP: ");
    Serial.println(WiFi.localIP());
  } else {
    wifiConnected = false;
    Serial.println("WiFi: FAILED");
  }
  
  WiFi.setHostname(HOSTNAME);
  oledShowStatus();

  // UART
  LaserSerial.begin(LASER_BAUD, SERIAL_8N1, LASER_RX_PIN, LASER_TX_PIN);
  LaserSerial.setTimeout(10);
  Serial.println("UART: OK");

  // OTA
  if (wifiConnected) {
    setupOTA();
    Serial.println("OTA: OK");
  }

  // HTTP
  httpServer.on("/", handleRoot);
  httpServer.on("/api/status", handleStatus);
  httpServer.on("/api/files", handleFiles);
  httpServer.on("/api/storage", handleStorage);
  httpServer.on("/api/log", handleLog);
  httpServer.on("/api/upload", HTTP_POST, handleUploadComplete, handleUpload);
  httpServer.on("/api/delete", handleDelete);
  httpServer.on("/api/job", handleJob);
  httpServer.on("/api/grbl", handleGrbl);
  httpServer.begin();
  Serial.println("HTTP: OK (Port 80)");

  // WebSocket
  wsLaser.begin();
  wsLaser.onEvent(wsLaserEvent);
  // Heartbeat mit sehr langen Intervallen (LaserGRBL unterstützt kein Ping/Pong)
  wsLaser.enableHeartbeat(300000, 60000, 99);  // 5min ping, 1min timeout, 99 retries
  Serial.printf("WebSocket: OK (Port %d)\n", WS_LASER_PORT);
  Serial.println("  LaserGRBL: ws://" + WiFi.localIP().toString() + ":81/");

  Serial.println("\n=== READY ===\n");
  appendToLog("LaserBridge v2 gestartet\n");
}

// ===================== LOOP =========================

void loop() {
  // WebSocket hat höchste Priorität
  wsLaser.loop();
  
  // GRBL Responses lesen und verarbeiten
  processGrblResponse();
  
  // Job verarbeiten (inkl. Homing)
  processJob();
  
  // HTTP Requests
  httpServer.handleClient();
  
  // OTA (nur wenn komplett idle)
  if (jobState == JOB_IDLE && !wsLaserConnected) {
    ArduinoOTA.handle();
  }
  
  // Activity timeout
  if (laserActive && millis() - lastLaserActivity > 3000) {
    laserActive = false;
    oledNeedsUpdate = true;
  }
  
  // OLED Update
  static uint32_t lastOled = 0;
  bool jobActive = (jobState == JOB_PRE_HOMING || jobState == JOB_RUNNING || jobState == JOB_POST_HOMING);
  if (jobActive) {
    // Während Job: alle 2 Sekunden
    if (millis() - lastOled > 2000) {
      lastOled = millis();
      oledShowStatus();
    }
  } else {
    // Sonst: event-basiert
    checkOledUpdate();
    if (oledNeedsUpdate) oledShowStatus();
  }
  
  // WiFi check
  static uint32_t lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();
    bool newWifiState = (WiFi.status() == WL_CONNECTED);
    if (newWifiState != wifiConnected) {
      wifiConnected = newWifiState;
      oledNeedsUpdate = true;
      Serial.println(wifiConnected ? "WiFi reconnected" : "WiFi lost");
    }
  }
  
  // Kleine Pause um CPU zu entlasten
  yield();
}
