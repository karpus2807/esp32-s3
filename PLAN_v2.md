# CCTV Firmware v2.0 — Addon Integration Plan

> Baseline: v1.0 (stable, tagged, pushed to GitHub)
> Board: ESP32-S3-WROOM-1U-N16R8 (16MB Flash, 8MB OPI PSRAM, OV3660)

---

## 1. Goal

Extend the existing CCTV firmware with:
- **DHT11** temperature & humidity sensor
- **PIR HC-SR501** motion detection
- **SH1106 128×64 OLED** status display (I2C)
- **MQTT → ThingsBoard** telemetry push (self-hosted server)
- **Web UI "IoT" tab** for configuration & live data

All addon features run alongside existing camera recording, streaming, WiFi/Ethernet
networking, and SD card storage without affecting v1.0 stability.

---

## 2. Hardware Pin Assignment

| Module            | Signal | GPIO | Notes                          |
|-------------------|--------|------|--------------------------------|
| SH1106 OLED       | SDA    | 1    | I2C data                       |
| SH1106 OLED       | SCL    | 2    | I2C clock                      |
| DHT11 Sensor       | DATA   | 42   | Temperature & humidity          |
| PIR HC-SR501       | DATA   | 41   | Motion detection (digital HIGH) |

All 4 GPIOs confirmed free — no conflict with camera bus, SD_MMC, W5500 Ethernet, or Flash.

---

## 3. ThingsBoard MQTT Integration

### Connection
- **Server**: user-configurable (default: `thingsboard.ipserver.in`)
- **Port**: user-configurable (default: `1883`)
- **Topic**: `v1/devices/me/telemetry`
- **Auth**: MQTT username = device access token (no password needed)
- **QoS**: 1

### Example (mosquitto_pub equivalent)
```
mosquitto_pub -d -q 1 \
  -h thingsboard.ipserver.in -p 1883 \
  -t v1/devices/me/telemetry \
  -u "<device_token>" \
  -m '{"temperature":25}'
```

### Telemetry Fields (25 keys)

| Field              | Source           | Type      | Notes                              |
|--------------------|------------------|-----------|------------------------------------|
| `temperature`       | DHT11            | float     | °C, read every 5s                  |
| `humidity`          | DHT11            | float     | %, read every 5s                   |
| `pir_alert`         | PIR HC-SR501     | bool      | true on motion detect              |
| `alert_level`       | computed         | int       | 1=normal / 2=warn / 3=critical     |
| `alert_temp`        | user config      | float     | alert threshold °C                 |
| `warn_temp`         | user config      | float     | warning threshold °C               |
| `critical_temp`     | user config      | float     | critical threshold °C              |
| `device_id`         | user config      | string    | unique device name                 |
| `firmware`          | compile-time     | string    | "v2.0"                             |
| `chip_model`        | ESP32 API        | string    | "ESP32-S3"                         |
| `chip_revision`     | ESP32 API        | int       | silicon revision                   |
| `cpu_freq`          | ESP32 API        | int       | MHz                                |
| `flash_size`        | ESP32 API        | int       | bytes                              |
| `heap_free`         | ESP32 API        | int       | bytes, current free heap           |
| `heap_min`          | ESP32 API        | int       | bytes, all-time minimum free heap  |
| `memory_status`     | computed         | int       | free memory in bytes               |
| `uptime`            | millis()         | int       | seconds since boot                 |
| `bt_status`         | ESP32 API        | string    | BT enabled, name="esp32-s3"       |
| `ip_address_wifi`   | WiFi.localIP()   | string    | WiFi IPv4                          |
| `ip_address_lan`    | Ethernet IP      | string    | LAN IPv4                           |
| `mac_address_wifi`  | WiFi.macAddress()| string    | WiFi MAC                           |
| `mac_address_lan`   | ETH MAC          | string    | Ethernet MAC                       |
| `wifi_ssid`         | WiFi.SSID()      | string    | connected SSID                     |
| `wifi_rssi`         | WiFi.RSSI()      | int       | dBm                                |
| `wifi_channel`      | WiFi.channel()   | int       | RF channel                         |
| `server_status`     | MQTT client      | string    | "connected" / "disconnected"       |

### Push Interval
- Default: every **10 seconds** (configurable via web UI)
- Motion alert (`pir_alert`) pushed immediately on trigger

---

## 4. OLED Display Design

### Boot Sequence (7 seconds)
1. **Frame 1 (0–2s)**: Logo/device name animation (fade-in or scroll)
2. **Frame 2 (2–4s)**: "Initializing..." with progress dots
3. **Frame 3 (4–7s)**: "Connecting..." → show IP as soon as available

### Runtime Pages (auto-rotate every 4 seconds)

**Page 1 — Network**
```
┌──────────────────────────┐
│ WiFi: 192.168.1.100      │
│ LAN:  10.0.0.50          │
│ W-MAC: AA:BB:CC:DD:EE:FF │
│ L-MAC: 11:22:33:44:55:66 │
└──────────────────────────┘
```

**Page 2 — Status**
```
┌──────────────────────────┐
│ 2026-04-14   15:30:42    │
│ Temp: 28.5°C  Hum: 65%  │
│ Server: Connected ✓      │
│ WiFi: -45dBm  LAN: OK   │
└──────────────────────────┘
```

**Page 3 — Alerts** (only if active)
```
┌──────────────────────────┐
│ ⚠ MOTION DETECTED        │
│ ⚠ TEMP HIGH: 45.2°C      │
│ Alert Level: CRITICAL    │
│ Uptime: 2d 5h 30m       │
└──────────────────────────┘
```

### Display Constraints
- SH1106: 128×64 pixels, ~21 chars × 4 lines (6×13 font) or ~25 chars × 8 lines (5×8 font)
- All text must fit — no overflow, no scrolling needed

---

## 5. Web UI — New "IoT" Tab

### Config Section (saved to NVS)
- ThingsBoard Server URL
- ThingsBoard Port
- Device Access Token
- Device ID (friendly name)
- Telemetry push interval (seconds)
- Temperature thresholds: alert, warn, critical (°C)

### Live Data Section (auto-refresh)
- Current temperature & humidity
- PIR motion status
- Alert level
- MQTT server connection status (connected/disconnected/reconnecting)
- All module statuses (OLED, DHT11, PIR, MQTT)
- Last telemetry push timestamp

---

## 6. New Source Files

| File                  | Purpose                                           |
|-----------------------|---------------------------------------------------|
| `cctv_dht.h/cpp`      | DHT11 driver: init, read temp/humidity, error handling |
| `cctv_pir.h/cpp`      | PIR motion: GPIO ISR, debounce, alert state        |
| `cctv_oled.h/cpp`     | SH1106 OLED: I2C init, boot animation, page rotation |
| `cctv_mqtt.h/cpp`     | MQTT client: connect, publish telemetry, auto-reconnect |
| `cctv_telemetry.h/cpp`| Collect all 25 fields into JSON, manage push timing |

### Modifications to Existing Files
- `board_config.h` — new pin defines, MQTT defaults, feature toggles
- `CameraWebServer.ino` — launch addon tasks in setup()
- `app_httpd.cpp` — add IoT tab HTML + API endpoints (`/iot`, `/api/iot`, `/api/iot/save`)
- `cctv_web_control.cpp` — serial commands for IoT config

---

## 7. Implementation Phases

### Phase 1 — Sensors + OLED (hardware layer)
- Install libraries: PubSubClient, U8g2, DHTesp
- Create `cctv_dht.h/cpp` — GPIO 42, 5s read interval, FreeRTOS task
- Create `cctv_pir.h/cpp` — GPIO 41, ISR + debounce
- Create `cctv_oled.h/cpp` — I2C GPIO 1/2, boot animation, page rotation

### Phase 2 — Telemetry collector
- Create `cctv_telemetry.h/cpp` — gather all 25 fields into JSON string
- Add NVS load/save for IoT config (server, port, token, thresholds)

### Phase 3 — MQTT / ThingsBoard
- Create `cctv_mqtt.h/cpp` — PubSubClient, connect with token, auto-reconnect
- Periodic telemetry push (default 10s)
- Immediate push on PIR motion alert

### Phase 4 — Web UI "IoT" tab
- Add HTML/JS tab in `app_httpd.cpp`
- API endpoints: GET `/api/iot` (live data + config), POST `/api/iot/save`
- Live refresh with Page Visibility API (same pattern as existing dashboard)

### Phase 5 — Integration & Testing
- Wire all tasks into `CameraWebServer.ino` setup()
- Test: sensor readings, OLED display, MQTT publish, web UI
- Memory/CPU impact check
- Tag as v2.0, push to GitHub

---

## 8. Libraries Required

| Library       | Purpose            | Install Command                          |
|---------------|--------------------|------------------------------------------|
| PubSubClient  | MQTT client        | `arduino-cli lib install "PubSubClient"` |
| U8g2          | SH1106 OLED        | `arduino-cli lib install "U8g2"`         |
| DHTesp        | DHT11 sensor       | `arduino-cli lib install "DHTesp"`       |

Wire (I2C) is built into ESP32 Arduino core — no install needed.

---

## 9. FreeRTOS Task Layout (v2.0)

| Task              | Core | Priority | Stack   | RAM Type  |
|-------------------|------|----------|---------|-----------|
| Recorder          | 1    | 3        | 20 KB   | Internal  |
| WiFiSTA           | 1    | 2        | 8 KB    | Internal  |
| HTTPD (control)   | 0    | 6        | 4 KB    | Internal  |
| HTTPD (stream)    | 1    | 6        | 4 KB    | Internal  |
| SerialConsole     | 0    | 1        | 8 KB    | Internal  |
| **MqttTask** (new)| 0    | 2        | 8 KB    | PSRAM OK  |
| **SensorTask**(new)| 0   | 1        | 4 KB    | PSRAM OK  |
| **OledTask** (new)| 1    | 1        | 4 KB    | PSRAM OK  |

MQTT/Sensor/OLED tasks are low priority and use PSRAM stacks where safe
(no NVS writes from these tasks — config saves go through HTTPD which has internal stack).

---

## 10. Risk & Mitigation

| Risk                           | Mitigation                                    |
|--------------------------------|-----------------------------------------------|
| MQTT blocking camera stream    | Separate task, low priority, non-blocking      |
| I2C OLED contention            | Only one task uses I2C (OledTask)              |
| DHT11 timing-sensitive         | DHTesp handles ESP32 timing internally         |
| Flash NVS from wrong stack     | IoT config saves route through HTTPD (internal)|
| Heap exhaustion with MQTT buf  | 512-byte MQTT buffer, PSRAM for JSON build     |
| PIR false triggers             | Software debounce (2s cooldown)                |
