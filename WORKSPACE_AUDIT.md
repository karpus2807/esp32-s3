# ESP32-S3 CCTV Firmware — Workspace Audit Report

> Generated: 2026-04-15

---

## Quick Summary

| Category | Count |
|----------|-------|
| **Total project files** | ~53 |
| **Source files** (.cpp/.h/.ino) | 20 |
| **Fully Integrated** (active in build) | 17 modules (32 files) |
| **Available but Disabled** (code present, feature off) | 1 module (2 files) |
| **Orphan** (never referenced) | 1 file |
| **Config/Build/Docs** | ~18 files |
| **Dead functions** (defined, never called) | 4 |
| **Duplicated logic** | 3 copies of same WiFi status function |

---

## A) File-by-File Classification

### INTEGRATED (Active in Build Chain)

| # | File(s) | What It Provides |
|---|---------|------------------|
| 1 | `CameraWebServer.ino` | Main sketch: setup(), loop(), WiFi connect, SD init, AVI recording task, serial console, NTP/time sync, sensor warmup, JPEG capture |
| 2 | `app_httpd.cpp` | HTTP server + full dashboard web UI (embedded HTML/JS/CSS), all API endpoints, auth login, debug logging |
| 3 | `board_config.h` | Master configuration: camera model, JPEG quality, PSRAM, W5500 Ethernet pins, serial verbosity, DHT/PIR/OLED/MQTT defaults |
| 4 | `camera_pins.h` | DVP pin mappings for 15+ camera board models. Included by board_config.h |
| 5 | `avi_recorder.h` | Header-only AVI MJPEG writer class. Used by recordingTask() |
| 6 | `cctv_platform.cpp` / `.h` | Mutex primitives: camera_lock/unlock, wifi_lock/unlock/try_lock |
| 7 | `cctv_psram.h` | Header-only PSRAM detection (cctv_psram_app_ready, total/free bytes) |
| 8 | `cctv_net.cpp` / `.h` | W5500 Ethernet init, DHCP, static IP, link status, DNS fallback |
| 9 | `cctv_wifi_profiles.cpp` / `.h` | 3-slot WiFi profile manager (NVS load/save/failover) |
| 10 | `cctv_time_sync.cpp` / `.h` | HTTP world-clock sync, NVS wall-clock snapshot |
| 11 | `cctv_web_control.cpp` / `.h` | Serial/web console command dispatcher (wifiscan, devstatus, ethstatic, etc.) |
| 12 | `cctv_devstatus.cpp` / `.h` | Full device status dump (CPU, RAM, PSRAM, flash, WiFi, ETH, SD) |
| 13 | `cctv_osd.cpp` / `.h` | JPEG OSD timestamp overlay (active when CCTV_ENABLE_FRAME_OSD=1) |
| 14 | `cctv_dht.cpp` / `.h` | DHT11 temperature/humidity driver + background read task |
| 15 | `cctv_pir.cpp` / `.h` | PIR HC-SR501 ISR driver |
| 16 | `cctv_mqtt.cpp` / `.h` | MQTT/ThingsBoard client: config, connect, periodic telemetry |
| 17 | `cctv_telemetry.cpp` / `.h` | JSON telemetry builder (25+ fields), alert level computation |
| 18 | `cctv_ui_log.cpp` / `.h` | Thread-safe ring-buffer log for web console live debug |

### AVAILABLE BUT DISABLED (Code Present, Feature Off)

| File(s) | Status | Why Disabled |
|---------|--------|--------------|
| `cctv_oled.cpp` / `.h` | Compiles and links, but **functionally disabled** at runtime | GPIO 1 (I2C SDA) conflict with W5500 Ethernet SPI SCK. `cctv_oled_init()` is commented out in setup(). `cctv_oled_start_task()` is called but returns immediately (s_ok = false). Web API reads oled_ok()/oled_status_str() safely (returns false/"Not detected"). |

**To activate OLED:** Reassign ETH SCK to a different GPIO or disable Ethernet, then uncomment the two init lines in setup().

### ORPHAN (Never Referenced)

| File | Notes |
|------|-------|
| `pin_config.h` | Pure comment-only reference document. **Never #included** by any file. Documents same pin assignments that board_config.h and camera_pins.h already define. |

### CONFIG / BUILD / DOCS

| File | Purpose |
|------|---------|
| `sketch.yaml` | Arduino CLI profile (FQBN, libraries) |
| `partitions.csv` | Custom flash partition table |
| `ci.yml` | CI FQBN matrix for multi-target builds |
| `.arduino-cli.yaml` | Arduino CLI board manager config |
| `.gitignore` | Git ignore rules |
| `.clangd` | Clangd LSP config |
| `.vscode/c_cpp_properties.json` | VS Code IntelliSense config |
| `.vscode/settings.json` | VS Code workspace settings |
| `compile_commands.json` | Compilation database for clangd |
| `compile_flags.txt` | Clang compile flags fallback |
| `.theia/launch.json` | Theia IDE debug config (unused) |
| `FLASHING_INSTRUCTIONS.md` | Flashing documentation |
| `PLAN_v2.md` | Design plan documentation |
| `SERIAL_COMMANDS.txt` | Serial console command reference |
| `ESP32-S3-WROOM-1U-N16R8.pdf` | Datasheet |
| `backup/` (6 files) | Firmware backup binaries + source snapshot |

---

## B) Dead Code / Unused Functions

### Functions Defined But Never Called

| Function | File | Line | Notes |
|----------|------|------|-------|
| `configureSensorDefaults()` | CameraWebServer.ino | ~650 | **Completely dead.** Static function, never called. Sensor setup is done inline in setup() (lines 1112-1131) which duplicates the same logic. Safe to delete. |
| `cctv_pir_status_str()` | cctv_pir.cpp | ~33 | Declared in header, defined in .cpp, but never called from any other file. Compare to cctv_dht_status_str() which IS called. |
| `cctv_pir_clear_alert()` | cctv_pir.cpp | ~26 | Declared in header but never called anywhere. PIR alert flag is read but never reset — stays true until reboot. **Likely a missing integration** (MQTT push should probably clear it). |
| `cctv_oled_show_message()` | cctv_oled.cpp | ~204 | Declared in header but never called from any file outside its own .cpp. Even if OLED were enabled, no code sends override messages. |

### Duplicated Logic (3 copies of same function)

| Function | Location | Notes |
|----------|----------|-------|
| `wifiStatusStr()` | CameraWebServer.ino:178 | static, maps wl_status_t to string |
| `wifi_status_str()` | cctv_web_control.cpp:53 | static, same logic |
| `wl_status_text()` | app_httpd.cpp | static, same logic |
| **Recommendation:** Consolidate into one shared function in cctv_platform.h/cpp ||

---

## C) Recommendations

### Files Safe to Remove/Rename

| Action | File | Rationale |
|--------|------|-----------|
| **DELETE or rename to .md** | `pin_config.h` | Orphan — never included, pure documentation masquerading as header |
| **DELETE** | `.theia/launch.json` | Theia IDE config; no effect on Arduino builds |
| **DELETE** | `.codex` | Codex IDE metadata; no build impact |
| **REVIEW** | `backup/` directory | Binary firmware backups; useful for recovery but not part of build. Consider moving to a separate archive. |

### Code Cleanup

| Action | Details |
|--------|---------|
| **Delete** `configureSensorDefaults()` | Dead function in CameraWebServer.ino (~lines 650-670). Sensor init is already done inline in setup(). |
| **Consolidate** WiFi status-to-string | Merge 3 copies into one shared function (e.g., in cctv_platform.h). |
| **Integrate** `cctv_pir_clear_alert()` | Wire it into MQTT push callback or web UI "acknowledge" button. Otherwise remove from header. |
| **Integrate** `cctv_pir_status_str()` | Add to IoT panel in web UI (like DHT status is shown). Otherwise remove. |
| **OLED status** | Either: (a) comment out all OLED references if permanently disabled, or (b) add a board_config.h toggle `#define CCTV_OLED_ENABLED 0` to cleanly disable at compile time. |

### Future Integration Opportunities

| Feature | Status | Effort |
|---------|--------|--------|
| **OLED display** | Full code ready, GPIO conflict blocks it | Needs hardware GPIO reassignment |
| **PIR alert acknowledge** | `cctv_pir_clear_alert()` exists but unwired | Small — add UI button + wire call |
| **PIR status string** | `cctv_pir_status_str()` exists but unwired | Small — add to IoT web panel |
| **OLED override messages** | `cctv_oled_show_message()` exists but unwired | Medium — add callers for boot/error/alert |

---

## D) Integration Dependency Map

```
CameraWebServer.ino (entry point)
├── board_config.h → camera_pins.h
├── avi_recorder.h
├── cctv_platform.h/.cpp (mutexes)
├── cctv_psram.h
├── cctv_net.h/.cpp (Ethernet)
├── cctv_wifi_profiles.h/.cpp (WiFi profiles)
├── cctv_time_sync.h/.cpp (NTP/HTTP time)
├── cctv_web_control.h/.cpp (console commands)
│   ├── cctv_devstatus.h/.cpp
│   └── cctv_wifi_profiles, cctv_net, cctv_time_sync
├── cctv_dht.h/.cpp (DHT11 sensor)
├── cctv_pir.h/.cpp (PIR motion)
├── cctv_oled.h/.cpp (OLED — DISABLED)
├── cctv_mqtt.h/.cpp (MQTT client)
├── cctv_telemetry.h/.cpp (telemetry builder)
├── cctv_osd.h/.cpp (frame overlay)
└── cctv_ui_log.h/.cpp (debug log buffer)

app_httpd.cpp (HTTP server)
├── All of the above (reads status from every module)
├── Auth login/logout/session (built-in)
└── Full embedded web dashboard
```

---

**End of audit.**
