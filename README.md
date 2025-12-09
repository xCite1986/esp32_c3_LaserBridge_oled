# ESP32-C3 LaserBridge

WiFi-Bridge für Laser-Engraver mit GRBL-Firmware. Ermöglicht kabellose Steuerung über LaserGRBL via WebSocket.

![ESP32-C3 OLED](https://img.shields.io/badge/ESP32--C3-OLED%200.42%22-blue)
![License](https://img.shields.io/badge/license-MIT-green)
![GRBL](https://img.shields.io/badge/GRBL-1.1-orange)

## Features

- **WebSocket-Bridge** für LaserGRBL (Port 81)
- **Web-UI** mit Live-Log, Status und Steuerbuttons (Port 80)
- **OTA-Updates** – Firmware kabellos aktualisieren
- **WiFiManager** – WLAN-Konfiguration über Captive Portal
- **0.42" OLED** – zeigt IP, WebSocket- und UART-Status

## Hardware

### Benötigt

- ESP32-C3 mit 0.42" OLED (72x40, SSD1306)
  https://de.aliexpress.com/item/1005008222593807.html?spm=a2g0o.order_list.order_list_main.36.77365c5fJ63Lfl&gatewayAdapt=glo2deu
- Laser-Engraver mit GRBL-Controller (z.B. ACMER S1, Ortur, etc., sollte auf jedem GRBL-fähigen Lasergravierer mit einem CH340G-Controller funktionieren)

### Pinbelegung

| ESP32-C3 | Laser-Board (CH340G) |
|----------|----------------------|
| GPIO20 (RX) | Pin 3 (RXD) |
| GPIO21 (TX) | Pin 2 (TXD) |
| GND | GND |

![ESP32-C3 Pinout](https://github.com/xCite1986/esp32_c3_LaserBridge_oled/blob/main/images/esp32_c3_oled_pinout.avif)

### Verkabelung am CH340G

```
CH340G (SOIC-16, Kerbe links)
        ┌───U───┐
   GND -│1    16│- VCC
   TXD -│2    15│- ..      
   RXD -│3    14│- ..     
        │  ...  │
        └───────┘
```
Als Alternativer GND-Pin kann auch Pin4/6 von der Pingruppe rechts neben dem Reset-Button verwendet werden.

![ESP32-C3 connection](https://github.com/xCite1986/esp32_c3_LaserBridge_oled/blob/main/images/board_esp32_c3.jpg)
![GND connection](https://github.com/xCite1986/esp32_c3_LaserBridge_oled/blob/main/images/board_gnd_pin.jpg)
![ESP32-C3 connection](https://github.com/xCite1986/esp32_c3_LaserBridge_oled/blob/main/images/board_tx_rx.jpg)

> **Hinweis:** USB-Kabel vom Laser abstecken wenn WiFi verwendet wird!

## Installation

### 1. Arduino IDE vorbereiten

Benötigte Libraries über den Bibliotheksverwalter installieren:

- **WiFiManager** von tzapu
- **WebSockets** von Markus Sattler
- **U8g2** von Oliver Kraus

### 2. Board-Einstellungen

| Einstellung | Wert |
|-------------|------|
| Board | ESP32C3 Dev Module |
| USB CDC On Boot | Enabled |
| CPU Frequency | 160MHz |
| Flash Size | 4MB |

### 3. Flashen

1. Sketch öffnen und auf ESP32-C3 hochladen
2. Beim ersten Start öffnet sich ein Access Point: `LaserBridge-Setup`
3. Mit dem AP verbinden und WLAN-Zugangsdaten eingeben
4. ESP startet neu und verbindet sich mit dem WLAN

## Verwendung

### LaserGRBL verbinden

1. In LaserGRBL: **Grbl** → **Verbindung** → **ESP/ESP8266 WebSocket**
2. Adresse eingeben: `ws://[IP-ADRESSE]:81/`
3. Verbinden

### Web-UI

Browser öffnen: `http://[IP-ADRESSE]/`

![WEB-UI](https://github.com/xCite1986/esp32_c3_LaserBridge_oled/blob/main/images/web_ui.png)

Features:
- **Status** – WiFi, WebSocket, UART-Aktivität
- **Steuerung** – Unlock, Homing, Feed Hold, Soft Reset
- **G-Code senden** – Direkte Befehle an den Laser
- **Live-Log** – Echtzeit-Kommunikation via WebSocket

### OTA-Update

In Arduino IDE:
1. **Werkzeuge** → **Port** → **laserbridge** (oder IP-Adresse)
2. Sketch hochladen

## Konfiguration

Im Code anpassbar:

```cpp
const char* HOSTNAME = "laserbridge";      // mDNS Hostname
const char* AP_NAME = "LaserBridge-Setup"; // AP-Name für WiFi-Setup

static const int OLED_SDA = 5;             // I2C Pins für OLED
static const int OLED_SCL = 6;

static const int LASER_RX_PIN = 20;        // UART Pins
static const int LASER_TX_PIN = 21;
static const uint32_t LASER_BAUD = 115200; // Baudrate (meist 115200)
```

## Troubleshooting

### Keine Verbindung zum Laser

- USB-Kabel vom Laser abstecken
- Verkabelung prüfen (RX↔TX gekreuzt?)
- Baudrate überprüfen (manche Boards: 9600)

### LaserGRBL trennt sofort

- Laser eingeschaltet? (12V + grüner Knopf)
- Im Log sollte `Grbl 1.1f` erscheinen

### WiFi-Reset

- Im Web-UI: "WiFi Reset" Button
- Oder: Boot-Taste 10 Sekunden halten

## Getestete Hardware

- **ESP32-C3** mit 0.42" OLED (72x40)
- **ACMER S1** 6W Laser Engraver (MST MINI V1.0 Board)

## Lizenz

MIT License – siehe [LICENSE](LICENSE)

## Credits

Erstellt mit Unterstützung von Claude (Anthropic).
