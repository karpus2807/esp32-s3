#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

//
// WARNING!!! PSRAM IC required for UXGA resolution and high JPEG quality
//            Ensure ESP32 Wrover Module or other board with PSRAM is selected
//            Partial images will be transmitted if image exceeds buffer size
//
//            You must select partition scheme from the board menu that has at least 3MB APP space.

// ===================
// Select camera model (must match your PCB DVP wiring)
// ===================
//
// OceanLabz ESP32-S3 WROOM-1(-U)-N16R8 + OV3660 + TF (SDMMC 1-bit): GPIO39 CLK, 38 CMD, 40 D0
// USB-UART: CH343P on the second Type-C. Same camera bus as ESP32-S3-EYE reference.
//
// **Arduino Tools → PSRAM:** use **"OPI PSRAM"** (FQBN …PSRAM=opi…), NOT "QSPI PSRAM"
// (…PSRAM=enabled…). N16R8 = 8 MB **octal** PSRAM; QSPI mode leaves SPIRAM heap size = 0.
//
//#define CAMERA_MODEL_WROVER_KIT // Has PSRAM
//#define CAMERA_MODEL_ESP32S3_EYE
#define CAMERA_MODEL_OCEANLABZ_ESP32S3
//#define CAMERA_MODEL_M5STACK_PSRAM // Has PSRAM
//#define CAMERA_MODEL_M5STACK_V2_PSRAM // M5Camera version B Has PSRAM
//#define CAMERA_MODEL_M5STACK_WIDE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_ESP32CAM // No PSRAM
//#define CAMERA_MODEL_M5STACK_UNITCAM // No PSRAM
//#define CAMERA_MODEL_M5STACK_CAMS3_UNIT  // Has PSRAM
//#define CAMERA_MODEL_AI_THINKER // Has PSRAM
//#define CAMERA_MODEL_TTGO_T_JOURNAL // No PSRAM
//#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM
// ** Espressif Internal Boards **
//#define CAMERA_MODEL_ESP32_CAM_BOARD
//#define CAMERA_MODEL_ESP32S2_CAM_BOARD
//#define CAMERA_MODEL_ESP32S3_CAM_LCD
//#define CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3 // Has PSRAM
//#define CAMERA_MODEL_DFRobot_Romeo_ESP32S3 // Has PSRAM
#include "camera_pins.h"

// ─── CCTV tuning (override before #include "board_config.h" if needed) ───
// JPEG quality: 1–63 on OV sensors; higher = more compression, smaller files, faster Wi‑Fi.
#ifndef CCTV_JPEG_QUALITY
#define CCTV_JPEG_QUALITY 12
#endif
// Move normal malloc allocations to PSRAM earlier (bytes and above).
// Lower threshold = more pressure moved away from internal RAM.
#ifndef CCTV_EXTMEM_MALLOC_THRESHOLD
#define CCTV_EXTMEM_MALLOC_THRESHOLD 16
#endif
// Camera FB count when PSRAM is available. Higher count smooths stream but uses
// more driver/buffer resources; 6 is a safer balance for ETH + camera workload.
#ifndef CCTV_CAMERA_FB_COUNT_PSRAM
#define CCTV_CAMERA_FB_COUNT_PSRAM 6
#endif
// AVI segment frame rate (with OSD re-encode, 8–10 is smoother than 12 on ESP32-S3).
#ifndef CCTV_RECORD_FPS
#define CCTV_RECORD_FPS 8
#endif

// HTTP JSON time source (JSON with "unixtime" or "epoch"). Default unless NVS thDis=1 (timeurl off).
#ifndef CCTV_WORLD_TIME_HTTP_URL
#define CCTV_WORLD_TIME_HTTP_URL "http://worldtimeapi.org/api/ip"
#endif
// Burn date/time into JPEGs. Master switch; see OSD_ON_* below.
#ifndef CCTV_ENABLE_FRAME_OSD
#define CCTV_ENABLE_FRAME_OSD 1
#endif
// Live /stream and /mjpeg: OSD doubles CPU per frame — default off for smooth preview.
#ifndef CCTV_OSD_ON_STREAM
#define CCTV_OSD_ON_STREAM 0
#endif
// AVI recording and SD snapshot: keep timestamp on evidence files.
#ifndef CCTV_OSD_ON_RECORDING
#define CCTV_OSD_ON_RECORDING 1
#endif
#ifndef CCTV_OSD_ON_CAPTURE
#define CCTV_OSD_ON_CAPTURE 1
#endif
// Re-encode quality after OSD (higher = smaller/faster; camera uses CCTV_JPEG_QUALITY).
#ifndef CCTV_OSD_JPEG_QUALITY
#define CCTV_OSD_JPEG_QUALITY 18
#endif
// SD AVI recording only: OSD bar ke baad MJPEG encode — lower = sharper / larger files.
// Stream / /capture still use CCTV_OSD_JPEG_QUALITY above.
#ifndef CCTV_OSD_JPEG_QUALITY_RECORD
#define CCTV_OSD_JPEG_QUALITY_RECORD 14
#endif
// OSD stamp interval (reserved for future stream stamping). Recording uses *_RECORD below.
#ifndef CCTV_OSD_STAMP_EVERY_N
#define CCTV_OSD_STAMP_EVERY_N 12
#endif
// AVI recording: stamp every N **written** frames (must not use frameIdx — catch-up skips
// indices and timestamps vanished). Default 1 = every frame (best evidence consistency).
// Increase to 3–6 only if CPU cannot keep up.
#ifndef CCTV_OSD_STAMP_EVERY_N_RECORD
#define CCTV_OSD_STAMP_EVERY_N_RECORD 1
#endif

// W5500 SPI Ethernet (initial setup + web UI over LAN without WiFi)
//
// Link speed: the W5500 integrated PHY is 10/100 Mbps only — it will never negotiate
// 1000 Mbps. A "Gigabit" patch cable is fine, but the port LED / switch UI will still
// show 100M when talking to this chip. Throughput is further capped by the SPI bus
// (CCTV_ETH_SPI_MHZ) and ESP32 CPU, not by using Cat6 vs Cat5e.
//
#ifndef CCTV_USE_ETH_W5500
#define CCTV_USE_ETH_W5500 1
#endif
#ifndef CCTV_ETH_PIN_SCK
#define CCTV_ETH_PIN_SCK 1     // GPIO 21 dead — permanently on GPIO 1
#endif
#ifndef CCTV_ETH_PIN_MOSI
#define CCTV_ETH_PIN_MOSI 47
#endif
// MISO toggles at SPI speed — if a LED is routed to this GPIO it will flicker (normal for MISO).
#ifndef CCTV_ETH_PIN_MISO
#define CCTV_ETH_PIN_MISO 48
#endif
#ifndef CCTV_ETH_PIN_CS
#define CCTV_ETH_PIN_CS 14
#endif
// W5500 INT (interrupt) pin — connect to ESP32 for low-latency LAN
#ifndef CCTV_ETH_PIN_INT
#define CCTV_ETH_PIN_INT 45
#endif
// W5500 internal PHY: 0 on most modules; try 1 if link never comes (see Serial [ETH] lines).
#ifndef CCTV_ETH_PHY_ADDR
#define CCTV_ETH_PHY_ADDR 0
#endif
// Optional: GPIO tied to W5500 RST (active low). -1 = none. Many boards need RST high — use 10k to 3V3 or this pin.
#ifndef CCTV_ETH_PIN_RST
#define CCTV_ETH_PIN_RST 3
#endif
// W5500 max 33 MHz. Dupont wires unstable above 14. 25 caused 10% loss.
#ifndef CCTV_ETH_SPI_MHZ
#define CCTV_ETH_SPI_MHZ 14
#endif
// First DHCP wait at boot (ms). Slow routers / STP may need 20–30s.
#ifndef CCTV_ETH_DHCP_WAIT_MS
#define CCTV_ETH_DHCP_WAIT_MS 25000
#endif
// Blocking wait inside setup() only — keep small so the web server comes up quickly on LAN.
// After this window, httpd is started; `EthDhcpPoll` / link wait continues in a background task.
#ifndef CCTV_ETH_BOOT_DHCP_WAIT_MS
#define CCTV_ETH_BOOT_DHCP_WAIT_MS 2000u
#endif
// Bare "ethstatic" (no arguments) — fixed layout for direct laptop ↔ board cable.
#ifndef CCTV_ETH_STATIC_DEFAULT_IP
#define CCTV_ETH_STATIC_DEFAULT_IP "10.0.0.20"
#endif
#ifndef CCTV_ETH_STATIC_DEFAULT_MASK
#define CCTV_ETH_STATIC_DEFAULT_MASK "255.255.255.0"
#endif
#ifndef CCTV_ETH_STATIC_DEFAULT_GW
#define CCTV_ETH_STATIC_DEFAULT_GW "10.0.0.1"
#endif
#ifndef CCTV_ETH_STATIC_DEFAULT_DNS
#define CCTV_ETH_STATIC_DEFAULT_DNS "8.8.8.8"
#endif

// Serial: 0 = quiet boot + no Serial command task (use web "Device control" panel).
#ifndef CCTV_SERIAL_VERBOSE
#define CCTV_SERIAL_VERBOSE 0
#endif
#ifndef CCTV_ENABLE_SERIAL_CONSOLE
#define CCTV_ENABLE_SERIAL_CONSOLE 0
#endif
// When full serial console is OFF: tiny task accepts devstatus + Ethernet lines (ethdhcp / ethstatic …).
// Ignored if CCTV_ENABLE_SERIAL_CONSOLE is 1 (one Serial reader only).
#ifndef CCTV_ENABLE_ETH_SERIAL_MINI
#define CCTV_ENABLE_ETH_SERIAL_MINI 1
#endif

// ─── Addon modules (v2.0) ───────────────────────────────────────────────
// DHT11 temperature & humidity sensor
#ifndef CCTV_DHT_PIN
#define CCTV_DHT_PIN 42
#endif
#ifndef CCTV_DHT_READ_INTERVAL_MS
#define CCTV_DHT_READ_INTERVAL_MS 5000
#endif

// PIR HC-SR501 motion sensor (must not overlap camera_pins.h for your board; OceanLabz: 41 is free)
#ifndef CCTV_PIR_PIN
#define CCTV_PIR_PIN 41
#endif
#ifndef CCTV_PIR_DEBOUNCE_MS
#define CCTV_PIR_DEBOUNCE_MS 2000
#endif
// Serial: print "PIR: motion detected" / "PIR: no motion" when GPIO changes (poll ~100ms). 0 = off.
#ifndef CCTV_PIR_SERIAL_INTERVAL_MS
#define CCTV_PIR_SERIAL_INTERVAL_MS 1
#endif
// Also print a status line every N ms (raw pin + filtered motion) so the monitor is never silent. 0 = edges only.
#ifndef CCTV_PIR_SERIAL_STATUS_MS
#define CCTV_PIR_SERIAL_STATUS_MS 5000
#endif

// MQTT / ThingsBoard defaults
#ifndef CCTV_MQTT_DEFAULT_SERVER
#define CCTV_MQTT_DEFAULT_SERVER "thingsboard.ipserver.in"
#endif
#ifndef CCTV_MQTT_DEFAULT_PORT
#define CCTV_MQTT_DEFAULT_PORT 1883
#endif
#ifndef CCTV_MQTT_DEFAULT_INTERVAL_S
#define CCTV_MQTT_DEFAULT_INTERVAL_S 10
#endif

#endif  // BOARD_CONFIG_H
