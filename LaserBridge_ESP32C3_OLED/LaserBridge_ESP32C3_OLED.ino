/*
 * ESP32-C3 LaserBridge HYBRID
 *  - WebSocket Bridge für LaserGRBL (ws://IP:81/)
 *  - G-Code Upload & Dateiverwaltung
 *  - Offline-Druck ohne PC
 *  - 0.42" OLED Status
 *  - WiFiManager & OTA
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
  // Nur loggen wenn kein Job läuft
  if (jobState == JOB_RUNNING) return;
  
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

void processGrblResponse() {
  static String lineBuffer = "";
  
  while (LaserSerial.available()) {
    char c = LaserSerial.read();
    
    // Direkt an WebSocket durchleiten (Byte für Byte)
    if (wsLaserConnected) {
      uint8_t buf[1] = {(uint8_t)c};
      wsLaser.broadcastTXT(buf, 1);
    }
    
    bytesFromLaser++;
    laserActive = true;
    lastLaserActivity = millis();
    
    // Zeile sammeln für Log und Job-Verarbeitung
    if (c == '\n') {
      lineBuffer.trim();
      
      if (lineBuffer.length() > 0) {
        // Log (nur wenn kein WS verbunden und kein Job läuft)
        if (!wsLaserConnected && jobState != JOB_RUNNING && jobState != JOB_PRE_HOMING && jobState != JOB_POST_HOMING) {
          appendToLog("\n" + lineBuffer);
        }
        
        // Job-relevante Responses verarbeiten
        if (lineBuffer == "ok") {
          waitingForOk = false;
        }
        else if (lineBuffer.startsWith("error:")) {
          waitingForOk = false;
          Serial.println("[GRBL ERR] " + lineBuffer);
        }
        else if (lineBuffer.startsWith("ALARM:")) {
          if (jobState == JOB_RUNNING || jobState == JOB_PRE_HOMING || jobState == JOB_POST_HOMING) {
            jobState = JOB_ERROR;
            if (gcodeFile) gcodeFile.close();
            oledNeedsUpdate = true;
          }
          Serial.println("[GRBL ALARM] " + lineBuffer);
          appendToLog("\n" + lineBuffer);
        }
        else if (lineBuffer.startsWith("Grbl")) {
          Serial.println("[GRBL] " + lineBuffer);
          if (!wsLaserConnected) {
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
body{font-family:system-ui,sans-serif;background:#0a0a0a;color:#eee;padding:10px;max-width:800px;margin:0 auto}
.card{background:#1a1a1a;border-radius:8px;padding:14px;margin-bottom:10px}
h1{font-size:18px;margin-bottom:10px}
h2{font-size:13px;margin:10px 0 6px;color:#666;text-transform:uppercase;letter-spacing:1px}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
.stat{background:#252525;padding:6px;border-radius:4px;text-align:center}
.stat-label{font-size:9px;color:#555;text-transform:uppercase}
.stat-value{font-size:13px;font-weight:bold}
.ok{color:#4caf50}.warn{color:#ff9800}.err{color:#f44336}.idle{color:#666}
.progress{background:#252525;border-radius:4px;height:20px;margin:8px 0;overflow:hidden;position:relative}
.progress-fill{background:linear-gradient(90deg,#4caf50,#8bc34a);height:100%;transition:width 0.3s}
.progress-text{position:absolute;width:100%;text-align:center;line-height:20px;font-size:11px}
button{padding:6px 10px;border:none;border-radius:4px;cursor:pointer;font-size:11px;font-weight:500}
button:hover{filter:brightness(1.1)}
button:active{transform:scale(0.98)}
button:disabled{opacity:0.4;cursor:not-allowed}
.btn-green{background:#4caf50;color:#fff}
.btn-orange{background:#ff9800;color:#fff}
.btn-red{background:#f44336;color:#fff}
.btn-blue{background:#2196f3;color:#fff}
.btn-gray{background:#333;color:#aaa}
.btn-sm{padding:4px 6px;font-size:10px}
.row{display:flex;gap:6px;flex-wrap:wrap;margin:6px 0}
.upload-zone{border:2px dashed #333;border-radius:6px;padding:20px;text-align:center;cursor:pointer;font-size:12px}
.upload-zone:hover{border-color:#444;background:#151515}
.upload-zone.drag{border-color:#4caf50;background:#1a2f1a}
.file-item{display:flex;align-items:center;padding:8px;background:#252525;border-radius:4px;margin:4px 0;gap:8px;font-size:12px}
.file-item.selected{background:#2e4a2e;outline:1px solid #4caf50}
.file-info{flex:1;min-width:0}
.file-name{font-family:monospace;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.file-meta{font-size:10px;color:#555}
.storage-bar{background:#252525;border-radius:3px;height:6px;overflow:hidden}
.storage-fill{background:#2196f3;height:100%}
.storage-text{font-size:10px;color:#555;display:flex;justify-content:space-between;margin-top:4px}
#log{background:#000;color:#0f0;padding:8px;border-radius:4px;height:100px;font-family:monospace;font-size:10px;overflow-y:auto;white-space:pre-wrap}
input[type=text]{padding:6px;border-radius:4px;border:1px solid #333;background:#252525;color:#eee;font-family:monospace;font-size:12px;flex:1}
input[type=file]{display:none}
.section{border-top:1px solid #252525;padding-top:10px;margin-top:10px}
.job-file{font-size:11px;color:#666}
</style>
</head>
<body>

<div class="card">
  <h1>🔥 LaserBridge</h1>
  <div class="grid">
    <div class="stat"><div class="stat-label">IP</div><div class="stat-value" id="ip">-</div></div>
    <div class="stat"><div class="stat-label">WiFi</div><div class="stat-value" id="wifi">-</div></div>
    <div class="stat"><div class="stat-label">WS</div><div class="stat-value" id="ws">-</div></div>
    <div class="stat"><div class="stat-label">Status</div><div class="stat-value" id="job">-</div></div>
  </div>
  <div class="progress">
    <div class="progress-fill" id="progress" style="width:0%"></div>
    <div class="progress-text"><span id="line">0</span>/<span id="total">0</span> (<span id="pct">0</span>%)</div>
  </div>
  <div class="job-file">📄 <span id="jobFile">Keine Datei ausgewählt</span></div>
</div>

<div class="card">
  <h2>📁 Dateien</h2>
  <div class="upload-zone" id="dropzone">
    <input type="file" id="fileInput" accept=".gcode,.nc,.gc,.ngc">
    📤 G-Code hochladen (Drag & Drop)
  </div>
  <div id="uploadStatus" style="font-size:11px;margin:6px 0;color:#888"></div>
  <div id="fileList"></div>
  
  <div class="section">
    <h2>💾 Speicher</h2>
    <div class="storage-bar"><div class="storage-fill" id="storageFill" style="width:0%"></div></div>
    <div class="storage-text"><span id="storageUsed">-</span><span id="storageFree">-</span></div>
  </div>
</div>

<div class="card">
  <h2>🎮 Steuerung</h2>
  <div class="row">
    <button class="btn-green" id="btnStart" onclick="jobCmd('start')" disabled>▶ Start</button>
    <button class="btn-orange" id="btnPause" onclick="jobCmd('pause')" style="display:none">⏸ Pause</button>
    <button class="btn-green" id="btnResume" onclick="jobCmd('resume')" style="display:none">▶ Weiter</button>
    <button class="btn-red" onclick="jobCmd('stop')">⏹ Stop</button>
    <button class="btn-blue" onclick="grblCmd('unlock')">🔓 Unlock</button>
    <button class="btn-blue" onclick="grblCmd('home')">🏠 Home</button>
    <button class="btn-red" onclick="grblCmd('reset')">🔄 Reset</button>
  </div>
  
  <div class="section">
    <h2>⌨ G-Code</h2>
    <div class="row">
      <input type="text" id="gcode" placeholder="G0 X10 Y10" onkeydown="if(event.key==='Enter')sendGcode()">
      <button class="btn-blue" onclick="sendGcode()">Send</button>
    </div>
  </div>
  
  <div class="section">
    <div style="display:flex;justify-content:space-between;align-items:center">
      <h2>📋 Log</h2>
      <button class="btn-gray btn-sm" onclick="clearLog()">Leeren</button>
    </div>
    <div id="log">Warte auf Daten...</div>
  </div>
  
  <div class="section">
    <h2>⚙ System</h2>
    <div class="row">
      <button class="btn-orange btn-sm" onclick="if(confirm('WiFi Reset?'))grblCmd('wifireset')">WiFi Reset</button>
      <button class="btn-gray btn-sm" onclick="location.reload()">Refresh</button>
    </div>
  </div>
</div>

<script>
let selectedFile='';
let jobState=0;

function updateStatus(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    document.getElementById('ip').textContent=s.ip;
    document.getElementById('wifi').innerHTML=s.wifi?'<span class="ok">✓</span>':'<span class="err">✗</span>';
    document.getElementById('ws').innerHTML=s.ws?'<span class="ok">✓</span>':'<span class="idle">-</span>';
    
    const st=['IDLE','HOME','RUN','HOME','PAUSE','DONE','ERR'];
    const cl=['idle','warn','ok','warn','warn','ok','err'];
    jobState=s.job;
    document.getElementById('job').innerHTML='<span class="'+cl[s.job]+'">'+st[s.job]+'</span>';
    
    document.getElementById('line').textContent=s.line;
    document.getElementById('total').textContent=s.total;
    const pct=s.total>0?Math.round(s.line*100/s.total):0;
    document.getElementById('pct').textContent=pct;
    document.getElementById('progress').style.width=pct+'%';
    document.getElementById('jobFile').textContent=s.file||'Keine Datei ausgewählt';
    
    const active=s.job>=1&&s.job<=4;
    document.getElementById('btnStart').style.display=active?'none':'';
    document.getElementById('btnStart').disabled=!selectedFile;
    document.getElementById('btnPause').style.display=s.job===2?'':'none';
    document.getElementById('btnResume').style.display=s.job===4?'':'none';
  }).catch(()=>{});
}

function loadFiles(){
  fetch('/api/files').then(r=>r.json()).then(files=>{
    const list=document.getElementById('fileList');
    if(!files||files.length===0){
      list.innerHTML='<div style="color:#555;padding:10px;text-align:center">Keine Dateien</div>';
      return;
    }
    list.innerHTML=files.map(f=>`
      <div class="file-item${selectedFile===f.name?' selected':''}" onclick="selectFile('${f.name}')">
        <div class="file-info">
          <div class="file-name">${f.name}</div>
          <div class="file-meta">${f.lines} Zeilen · ${(f.size/1024).toFixed(1)}KB</div>
        </div>
        <button class="btn-green btn-sm" onclick="event.stopPropagation();playFile('${f.name}')">▶</button>
        <button class="btn-red btn-sm" onclick="event.stopPropagation();deleteFile('${f.name}')">🗑</button>
      </div>
    `).join('');
  }).catch(()=>{});
  
  fetch('/api/storage').then(r=>r.json()).then(s=>{
    const pct=Math.round(s.used*100/s.total);
    document.getElementById('storageFill').style.width=pct+'%';
    document.getElementById('storageUsed').textContent=(s.used/1024).toFixed(0)+'KB ('+pct+'%)';
    document.getElementById('storageFree').textContent=(s.free/1024).toFixed(0)+'KB frei';
  }).catch(()=>{});
}

function selectFile(n){
  selectedFile=n;
  document.querySelectorAll('.file-item').forEach(el=>el.classList.toggle('selected',el.querySelector('.file-name').textContent===n));
  document.getElementById('btnStart').disabled=false;
}

function playFile(n){
  if(jobState>=1&&jobState<=4){alert('Job läuft!');return;}
  selectedFile=n;
  jobCmd('start');
}

function deleteFile(n){
  if(!confirm('Löschen: '+n+'?'))return;
  fetch('/api/delete?file='+encodeURIComponent(n)).then(()=>{
    if(selectedFile===n)selectedFile='';
    loadFiles();
  });
}

const dz=document.getElementById('dropzone');
const fi=document.getElementById('fileInput');
dz.onclick=()=>fi.click();
dz.ondragover=e=>{e.preventDefault();dz.classList.add('drag');};
dz.ondragleave=()=>dz.classList.remove('drag');
dz.ondrop=e=>{e.preventDefault();dz.classList.remove('drag');if(e.dataTransfer.files.length)upload(e.dataTransfer.files[0]);};
fi.onchange=()=>{if(fi.files.length)upload(fi.files[0]);fi.value='';};

function upload(f){
  document.getElementById('uploadStatus').innerHTML='⏳ '+f.name;
  const fd=new FormData();fd.append('file',f);
  fetch('/api/upload',{method:'POST',body:fd}).then(r=>r.json()).then(d=>{
    document.getElementById('uploadStatus').innerHTML=d.ok?'✅ '+f.name+' ('+d.lines+' Zeilen)':'❌ Fehler';
    if(d.ok){selectedFile='/'+f.name;loadFiles();}
  }).catch(()=>document.getElementById('uploadStatus').innerHTML='❌ Fehler');
}

function jobCmd(a){
  let u='/api/job?action='+a;
  if(a==='start'&&selectedFile)u+='&file='+encodeURIComponent(selectedFile);
  fetch(u).then(()=>updateStatus());
}

function grblCmd(c){fetch('/api/grbl?cmd='+c);}

function sendGcode(){
  const i=document.getElementById('gcode');
  if(i.value.trim())fetch('/api/grbl?cmd=gcode&g='+encodeURIComponent(i.value.trim()));
  i.value='';
}

function clearLog(){
  fetch('/api/grbl?cmd=clearlog').then(()=>document.getElementById('log').textContent='');
}

function updateLog(){
  if(jobState>=1&&jobState<=3)return;
  fetch('/api/log').then(r=>r.text()).then(t=>{
    const l=document.getElementById('log');
    l.textContent=t.trim()||'Warte auf Daten...';
    l.scrollTop=l.scrollHeight;
  }).catch(()=>{});
}

loadFiles();
updateStatus();
setInterval(updateStatus,1000);
setInterval(updateLog,1500);
setInterval(loadFiles,20000);
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

void wsLaserEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      wsLaserConnected = true;
      IPAddress ip = wsLaser.remoteIP(num);
      Serial.printf("[WS] Client %u connected from %s\n", num, ip.toString().c_str());
      appendToLog("\n[WS connected]");
      oledNeedsUpdate = true;
      // KEIN Reset hier - LaserGRBL macht das selbst!
      break;
    }
      
    case WStype_DISCONNECTED:
      Serial.printf("[WS] Client %u disconnected\n", num);
      wsLaserConnected = wsLaser.connectedClients() > 0;
      appendToLog("\n[WS disconnected]");
      oledNeedsUpdate = true;
      break;
      
    case WStype_TEXT:
      if (length > 0) {
        bytesFromWs += length;
        laserActive = true;
        lastLaserActivity = millis();
        
        // Direkt an UART senden
        LaserSerial.write(payload, length);
        
        // Debug
        String cmd = String((char*)payload, length);
        cmd.trim();
        if (cmd.length() > 0 && cmd.length() < 80 && cmd != "?") {
          Serial.printf("[WS TX] %s\n", cmd.c_str());
        }
      }
      break;
      
    case WStype_BIN:
      if (length > 0) {
        bytesFromWs += length;
        laserActive = true;
        lastLaserActivity = millis();
        LaserSerial.write(payload, length);
        Serial.printf("[WS TX BIN] %d bytes\n", length);
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
  Serial.println("\n\n=== LaserBridge HYBRID ===");

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
  Serial.printf("WebSocket: OK (Port %d)\n", WS_LASER_PORT);
  Serial.println("  LaserGRBL: ws://" + WiFi.localIP().toString() + ":81/");

  Serial.println("\n=== READY ===\n");
  appendToLog("LaserBridge gestartet\n");
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
