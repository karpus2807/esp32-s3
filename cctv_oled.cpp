// cctv_oled.cpp — SH1106 128×64 OLED display with boot animation + page rotation
#include "cctv_oled.h"
#include "board_config.h"
#include "cctv_dht.h"
#include "cctv_pir.h"
#include "cctv_net.h"
#include "cctv_mqtt.h"
#include <WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <time.h>

// SH1106 128×64, I2C, full buffer (uses ~1KB PSRAM via heap).
static U8G2_SH1106_128X64_NONAME_F_HW_I2C *s_u8g2 = nullptr;
static bool     s_ok     = false;
static uint8_t  s_page   = 0;
static uint32_t s_overrideUntilMs = 0;
static char     s_msg[4][22];  // 4 lines × 21 chars + NUL

// ────────── helpers ──────────

static void clearBuf()   { s_u8g2->clearBuffer(); }
static void sendBuf()    { s_u8g2->sendBuffer(); }

static void drawCentered(int y, const char *txt) {
  int w = s_u8g2->getStrWidth(txt);
  s_u8g2->drawStr((128 - w) / 2, y, txt);
}

// ────────── boot animation ──────────

void cctv_oled_boot_animation() {
  if (!s_ok) return;

  // Frame 1 (0–2s): device name fade-in
  s_u8g2->setFont(u8g2_font_helvB10_tr);
  for (int contrast = 0; contrast <= 255; contrast += 15) {
    s_u8g2->setContrast(contrast);
    clearBuf();
    drawCentered(28, "ESP32-S3 CCTV");
    s_u8g2->setFont(u8g2_font_6x13_tr);
    drawCentered(48, "v2.0");
    s_u8g2->setFont(u8g2_font_helvB10_tr);
    sendBuf();
    vTaskDelay(pdMS_TO_TICKS(80));
  }
  vTaskDelay(pdMS_TO_TICKS(500));

  // Frame 2 (2–4s): initialising with progress dots
  s_u8g2->setFont(u8g2_font_6x13_tr);
  for (int dots = 1; dots <= 8; ++dots) {
    clearBuf();
    drawCentered(28, "Initialising");
    char bar[17] = {};
    for (int d = 0; d < dots && d < 16; ++d) bar[d] = '.';
    drawCentered(46, bar);
    sendBuf();
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  // Frame 3 (4–7s): connecting
  for (int i = 0; i < 6; ++i) {
    clearBuf();
    drawCentered(20, "Connecting...");
    String ethIp = cctv_eth_ip_string();
    String wfIp  = cctv_wifi_ip_string();
    if (ethIp.length()) {
      char buf[22];
      snprintf(buf, sizeof(buf), "LAN: %s", ethIp.c_str());
      drawCentered(38, buf);
    }
    if (wfIp.length()) {
      char buf[22];
      snprintf(buf, sizeof(buf), "WiFi: %s", wfIp.c_str());
      drawCentered(54, buf);
    }
    sendBuf();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  s_u8g2->setContrast(200);
}

// ────────── runtime pages ──────────

static void drawPageNetwork() {
  s_u8g2->setFont(u8g2_font_5x8_tr);
  String wfIp  = cctv_wifi_ip_string();
  String ethIp = cctv_eth_ip_string();
  String wfMac = WiFi.macAddress();
  // ETH MAC from cctv_net — use primary IP logic
  char lmac[20] = "N/A";
  // Try to show ETH MAC via Network lib
  extern String cctv_eth_mac_string();
  String ethMac = cctv_eth_mac_string();

  char line[26];
  snprintf(line, sizeof(line), "WiFi:%s", wfIp.length() ? wfIp.c_str() : "N/A");
  s_u8g2->drawStr(0, 10, line);

  snprintf(line, sizeof(line), "LAN: %s", ethIp.length() ? ethIp.c_str() : "N/A");
  s_u8g2->drawStr(0, 22, line);

  snprintf(line, sizeof(line), "WM:%s", wfMac.c_str());
  s_u8g2->drawStr(0, 34, line);

  snprintf(line, sizeof(line), "LM:%s", ethMac.length() ? ethMac.c_str() : "N/A");
  s_u8g2->drawStr(0, 46, line);

  // Bottom: SSID
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(line, sizeof(line), "SSID:%s", WiFi.SSID().c_str());
    s_u8g2->drawStr(0, 58, line);
  }
}

static void drawPageStatus() {
  s_u8g2->setFont(u8g2_font_5x8_tr);

  // Line 1: date + time
  struct tm ti;
  char line[26];
  if (getLocalTime(&ti, 50)) {
    snprintf(line, sizeof(line), "%04d-%02d-%02d  %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
  } else {
    snprintf(line, sizeof(line), "Time: not synced");
  }
  s_u8g2->drawStr(0, 10, line);

  // Line 2: temp + humidity
  if (cctv_dht_ok()) {
    snprintf(line, sizeof(line), "Temp:%.1fC  Hum:%.0f%%",
             cctv_dht_temperature(), cctv_dht_humidity());
  } else {
    snprintf(line, sizeof(line), "Temp:--  Hum:--");
  }
  s_u8g2->drawStr(0, 22, line);

  // Line 3: server status
  snprintf(line, sizeof(line), "MQTT:%s", cctv_mqtt_connected() ? "Connected" : "Disconn");
  s_u8g2->drawStr(0, 34, line);

  // Line 4: WiFi RSSI + LAN
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(line, sizeof(line), "WiFi:%ddBm", WiFi.RSSI());
  } else {
    snprintf(line, sizeof(line), "WiFi:N/A");
  }
  s_u8g2->drawStr(0, 46, line);

  snprintf(line, sizeof(line), "LAN:%s", cctv_eth_has_ip() ? "OK" : "N/A");
  s_u8g2->drawStr(64, 46, line);

  // Line 5: uptime
  uint32_t up = millis() / 1000;
  uint32_t d = up / 86400; up %= 86400;
  uint32_t h = up / 3600;  up %= 3600;
  uint32_t m = up / 60;
  snprintf(line, sizeof(line), "Up:%lud %luh %lum", (unsigned long)d, (unsigned long)h, (unsigned long)m);
  s_u8g2->drawStr(0, 58, line);
}

static void drawPageAlerts() {
  s_u8g2->setFont(u8g2_font_5x8_tr);
  char line[26];

  if (cctv_pir_alert()) {
    s_u8g2->drawStr(0, 10, "! MOTION DETECTED");
  } else {
    s_u8g2->drawStr(0, 10, "PIR: No motion");
  }

  if (cctv_dht_ok()) {
    float t = cctv_dht_temperature();
    snprintf(line, sizeof(line), "Temp: %.1f C", t);
    s_u8g2->drawStr(0, 22, line);
  } else {
    s_u8g2->drawStr(0, 22, "Temp: sensor err");
  }

  // Alert level
  extern int cctv_telemetry_alert_level();
  int al = cctv_telemetry_alert_level();
  const char *alStr = (al >= 3) ? "CRITICAL" : (al >= 2) ? "WARNING" : "NORMAL";
  snprintf(line, sizeof(line), "Alert: %s", alStr);
  s_u8g2->drawStr(0, 34, line);

  // Uptime
  uint32_t up = millis() / 1000;
  uint32_t d = up / 86400; up %= 86400;
  uint32_t h = up / 3600;  up %= 3600;
  uint32_t m = up / 60;
  snprintf(line, sizeof(line), "Up:%lud %luh %lum", (unsigned long)d, (unsigned long)h, (unsigned long)m);
  s_u8g2->drawStr(0, 46, line);

  // Free heap
  snprintf(line, sizeof(line), "Heap:%luK", (unsigned long)(ESP.getFreeHeap() / 1024));
  s_u8g2->drawStr(0, 58, line);
}

// ────────── override message ──────────

void cctv_oled_show_message(const char* l1, const char* l2, const char* l3, const char* l4) {
  if (!s_ok) return;
  s_overrideUntilMs = millis() + 4000;
  strncpy(s_msg[0], l1 ? l1 : "", 21); s_msg[0][21] = 0;
  strncpy(s_msg[1], l2 ? l2 : "", 21); s_msg[1][21] = 0;
  strncpy(s_msg[2], l3 ? l3 : "", 21); s_msg[2][21] = 0;
  strncpy(s_msg[3], l4 ? l4 : "", 21); s_msg[3][21] = 0;
}

static void drawOverride() {
  s_u8g2->setFont(u8g2_font_6x13_tr);
  for (int i = 0; i < 4; ++i) {
    if (s_msg[i][0]) s_u8g2->drawStr(0, 14 + i * 14, s_msg[i]);
  }
}

// ────────── main task ──────────

static void oledTask(void *) {
  const uint32_t kPageMs = 4000;
  for (;;) {
    clearBuf();
    if (millis() < s_overrideUntilMs) {
      drawOverride();
    } else {
      switch (s_page) {
        case 0: drawPageNetwork(); break;
        case 1: drawPageStatus();  break;
        case 2: drawPageAlerts();  break;
      }
      s_page = (s_page + 1) % 3;
    }
    sendBuf();
    vTaskDelay(pdMS_TO_TICKS(kPageMs));
  }
}

// ────────── public API ──────────

void cctv_oled_init() {
  Wire.begin(CCTV_OLED_SDA, CCTV_OLED_SCL);

  /* I2C scan to verify OLED is physically connected */
  bool found = false;
  for (uint8_t addr = 0x3C; addr <= 0x3D; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[OLED] I2C device found at 0x%02X (SDA=%d SCL=%d)\n", addr, CCTV_OLED_SDA, CCTV_OLED_SCL);
      found = true;
      break;
    }
  }
  if (!found) {
    Serial.printf("[OLED] No I2C device at 0x3C/0x3D (SDA=%d SCL=%d) — check wiring!\n", CCTV_OLED_SDA, CCTV_OLED_SCL);
    s_ok = false;
    return;
  }

  s_u8g2 = new U8G2_SH1106_128X64_NONAME_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE);
  if (!s_u8g2->begin()) {
    s_ok = false;
    delete s_u8g2;
    s_u8g2 = nullptr;
    return;
  }
  s_ok = true;
  s_u8g2->setContrast(200);
}

void cctv_oled_start_task() {
  if (!s_ok) return;
  xTaskCreatePinnedToCore(oledTask, "OLED", 4096, nullptr, 1, nullptr, 1);
}

bool cctv_oled_ok() { return s_ok; }

const char* cctv_oled_status_str() {
  return s_ok ? "OK" : "Not detected";
}
