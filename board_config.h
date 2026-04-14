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
// AVI segment frame rate (with OSD re-encode, 8–10 is smoother than 12 on ESP32-S3).
#ifndef CCTV_RECORD_FPS
#define CCTV_RECORD_FPS 12
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
// OSD stamp interval: stamp every Nth frame (1=every, 9=once/sec at 9fps, 12=once/sec at 12fps).
// Reduces CPU: JPEG decode+encode only on stamped frames.
#ifndef CCTV_OSD_STAMP_EVERY_N
#define CCTV_OSD_STAMP_EVERY_N CCTV_RECORD_FPS
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
#define CCTV_ETH_PIN_SCK 21
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
// W5500 internal PHY: 0 on most modules; try 1 if link never comes (see Serial [ETH] lines).
#ifndef CCTV_ETH_PHY_ADDR
#define CCTV_ETH_PHY_ADDR 0
#endif
// Optional: GPIO tied to W5500 RST (active low). -1 = none. Many boards need RST high — use 10k to 3V3 or this pin.
#ifndef CCTV_ETH_PIN_RST
#define CCTV_ETH_PIN_RST (-1)
#endif
// Dupont wires: 10–14 MHz is safer than 20.
#ifndef CCTV_ETH_SPI_MHZ
#define CCTV_ETH_SPI_MHZ 12
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

#endif  // BOARD_CONFIG_H
