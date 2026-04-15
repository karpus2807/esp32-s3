// cctv_telemetry.cpp — Collect all 25+ telemetry fields into JSON
#include "cctv_telemetry.h"
#include "cctv_dht.h"
#include "cctv_pir.h"
#include "cctv_mqtt.h"
#include "cctv_net.h"
#include "board_config.h"
#include <WiFi.h>
#include <ETH.h>
#include <esp_system.h>
#include <esp_chip_info.h>

// ────────── ETH MAC string ──────────

String cctv_eth_mac_string() {
#if CCTV_USE_ETH_W5500
  uint8_t mac[6] = {0};
  ETH.macAddress(mac);
  char buf[20];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
#else
  return String("N/A");
#endif
}

// ────────── alert level ──────────

int cctv_telemetry_alert_level() {
  if (!cctv_dht_ok()) return 1;
  float t = cctv_dht_temperature();
  const CctvMqttConfig &cfg = cctv_mqtt_config();
  if (t >= cfg.criticalTemp) return 3;
  if (t >= cfg.warnTemp)     return 2;
  return 1;
}

// ────────── JSON builder ──────────

String cctv_telemetry_build_json() {
  const CctvMqttConfig &cfg = cctv_mqtt_config();

  // Chip info
  esp_chip_info_t ci;
  esp_chip_info(&ci);

  String j;
  j.reserve(768);
  j = "{";

  // Sensor data
  if (cctv_dht_ok()) {
    j += "\"temperature\":";   j += String(cctv_dht_temperature(), 1);
    j += ",\"humidity\":";     j += String(cctv_dht_humidity(), 1);
  } else {
    j += "\"temperature\":null,\"humidity\":null";
  }

  // PIR
  j += ",\"pir_alert\":";     j += cctv_pir_alert() ? "true" : "false";

  // Alert
  j += ",\"alert_level\":";   j += String(cctv_telemetry_alert_level());
  j += ",\"alert_temp\":";    j += String(cfg.alertTemp, 1);
  j += ",\"warn_temp\":";     j += String(cfg.warnTemp, 1);
  j += ",\"critical_temp\":"; j += String(cfg.criticalTemp, 1);

  // Device identity
  j += ",\"device_id\":\"";   j += cfg.deviceId; j += "\"";
  j += ",\"firmware\":\"v2.0\"";

  // Chip
  j += ",\"chip_model\":\"ESP32-S3\"";
  j += ",\"chip_revision\":"; j += String(ci.revision);
  j += ",\"cpu_freq\":";      j += String(getCpuFrequencyMhz());
  j += ",\"flash_size\":";    j += String((unsigned long)ESP.getFlashChipSize());

  // Heap
  j += ",\"heap_free\":";     j += String((unsigned long)ESP.getFreeHeap());
  j += ",\"heap_min\":";      j += String((unsigned long)ESP.getMinFreeHeap());
  j += ",\"memory_status\":"; j += String((unsigned long)ESP.getFreeHeap());

  // Uptime
  j += ",\"uptime\":";        j += String((unsigned long)(millis() / 1000));

  // BT status
  j += ",\"bt_status\":\"enabled\"";

  // Network
  String wfIp  = cctv_wifi_ip_string();
  String ethIp = cctv_eth_ip_string();
  j += ",\"ip_address_wifi\":\"";  j += wfIp.length()  ? wfIp  : "N/A"; j += "\"";
  j += ",\"ip_address_lan\":\"";   j += ethIp.length() ? ethIp : "N/A"; j += "\"";
  j += ",\"mac_address_wifi\":\""; j += WiFi.macAddress(); j += "\"";
  j += ",\"mac_address_lan\":\"";  j += cctv_eth_mac_string(); j += "\"";

  // WiFi
  if (WiFi.status() == WL_CONNECTED) {
    j += ",\"wifi_ssid\":\"";    j += WiFi.SSID(); j += "\"";
    j += ",\"wifi_rssi\":";      j += String(WiFi.RSSI());
    j += ",\"wifi_channel\":";   j += String(WiFi.channel());
  } else {
    j += ",\"wifi_ssid\":\"N/A\",\"wifi_rssi\":0,\"wifi_channel\":0";
  }

  // Server
  j += ",\"server_status\":\"";
  j += cctv_mqtt_connected() ? "connected" : "disconnected";
  j += "\"";

  j += "}";
  return j;
}
