#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  PIN CONFIGURATION — ESP32-S3-WROOM-1U-N16R8 (OceanLabz CCTV Board)
// ═══════════════════════════════════════════════════════════════════════════
//
//  Board : OceanLabz ESP32-S3 WROOM-1(-U)-N16R8 + OV3660 + TF (SDMMC 1-bit)
//  Chip  : ESP32-S3 (dual-core Xtensa LX7, 240 MHz)
//  Flash : 16 MB QIO
//  PSRAM : 8 MB OPI
//
//  All GPIO assignments in one place.  Other headers (#include "board_config.h",
//  "camera_pins.h") still carry the actual #define — this file is REFERENCE ONLY,
//  so you can see the full pin-map at a glance.
//
// ───────────────────────────────────────────────────────────────────────────
//  MODULE            PIN         GPIO    ACTIVE DIR   NOTES
// ───────────────────────────────────────────────────────────────────────────
//
//  ── Camera (OV3660 DVP 8-bit) ──────────────────────────────────────────
//  XCLK              GPIO 15     15      OUT          Master clock to sensor
//  SIOD (I2C SDA)    GPIO 4       4      I/O          SCCB data
//  SIOC (I2C SCL)    GPIO 5       5      OUT          SCCB clock
//  D0 (Y2)           GPIO 11     11      IN           Pixel data bit 0
//  D1 (Y3)           GPIO 9       9      IN           Pixel data bit 1
//  D2 (Y4)           GPIO 8       8      IN           Pixel data bit 2
//  D3 (Y5)           GPIO 10     10      IN           Pixel data bit 3
//  D4 (Y6)           GPIO 12     12      IN           Pixel data bit 4
//  D5 (Y7)           GPIO 18     18      IN           Pixel data bit 5
//  D6 (Y8)           GPIO 17     17      IN           Pixel data bit 6
//  D7 (Y9)           GPIO 16     16      IN           Pixel data bit 7
//  VSYNC             GPIO 6       6      IN           Frame sync
//  HREF              GPIO 7       7      IN           Line sync
//  PCLK              GPIO 13     13      IN           Pixel clock
//  PWDN              —           -1      —            Not connected
//  RESET             —           -1      —            Not connected
//
//  ── SD Card (SDMMC 1-bit mode) ─────────────────────────────────────────
//  CLK               GPIO 39     39      OUT          SD clock
//  CMD               GPIO 38     38      I/O          SD command
//  D0                GPIO 40     40      I/O          SD data line 0
//
//  ── W5500 Ethernet (SPI) ───────────────────────────────────────────────
//  SCK               GPIO 1       1      OUT          SPI clock (was 21, dead — testing)
//  MOSI              GPIO 47     47      OUT          SPI master-out
//  MISO              GPIO 48     48      IN           SPI master-in
//  CS                GPIO 14     14      OUT          Chip select (active LOW)
//  RST               GPIO 3       3      OUT          Hardware reset (active LOW)
//  PHY_ADDR          —            0      —            Internal PHY address
//  SPI Speed         —           12 MHz  —            Safe for dupont wires
//
//  ── DHT11 Temperature & Humidity Sensor ────────────────────────────────
//  DATA              GPIO 42     42      I/O          1-Wire data (10K pull-up to 3V3)
//
//  ── PIR HC-SR501 Motion Sensor ─────────────────────────────────────────
//  DATA              GPIO 41     41      IN           Digital HIGH on motion (ISR RISING)
//
//  ── SH1106 128×64 OLED (I2C) — DISABLED (GPIO 1 used as ETH SCK for testing)
//  SDA               GPIO 1       1      I/O          I2C data  ← TEMP: used as ETH SCK
//  SCL               GPIO 45     45      OUT          I2C clock (was GPIO 2, dead)
//
// ───────────────────────────────────────────────────────────────────────────
//  GPIO SUMMARY (sorted)
// ───────────────────────────────────────────────────────────────────────────
//
//  GPIO  1  — ETH W5500 SCK (testing, was OLED SDA)
//  GPIO  2  — DEAD (was OLED SCL)
//  GPIO  3  — ETH W5500 RST
//  GPIO  4  — Camera SCCB SDA (I2C)
//  GPIO  5  — Camera SCCB SCL (I2C)
//  GPIO  6  — Camera VSYNC
//  GPIO  7  — Camera HREF
//  GPIO  8  — Camera D2 (Y4)
//  GPIO  9  — Camera D1 (Y3)
//  GPIO 10  — Camera D3 (Y5)
//  GPIO 11  — Camera D0 (Y2)
//  GPIO 12  — Camera D4 (Y6)
//  GPIO 13  — Camera PCLK
//  GPIO 14  — ETH W5500 CS (SPI)
//  GPIO 15  — Camera XCLK
//  GPIO 16  — Camera D7 (Y9)
//  GPIO 17  — Camera D6 (Y8)
//  GPIO 18  — Camera D5 (Y7)
//  GPIO 19  — USB D- (reserved)
//  GPIO 20  — USB D+ (reserved)
//  GPIO 21  — DEAD (was ETH W5500 SCK)
//  GPIO 38  — SD CMD
//  GPIO 39  — SD CLK
//  GPIO 40  — SD D0
//  GPIO 41  — PIR DATA
//  GPIO 42  — DHT11 DATA
//  GPIO 47  — ETH W5500 MOSI (SPI)
//  GPIO 48  — ETH W5500 MISO (SPI)
//
// ───────────────────────────────────────────────────────────────────────────
//  FREE GPIOs (usable for future addons)
// ───────────────────────────────────────────────────────────────────────────
//
//  GPIO 43  — Free (default UART TX)
//  GPIO 44  — Free (default UART RX)
//  GPIO 45  — OLED SCL (I2C) (strapping pin)
//  GPIO 46  — Free (strapping pin — boot mode; use with care)
//
// ═══════════════════════════════════════════════════════════════════════════
