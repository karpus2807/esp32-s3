#pragma once
// cctv_mqtt.h — MQTT client for ThingsBoard telemetry

#include <Arduino.h>

// Initialise MQTT (loads config from NVS). Call once from setup().
void cctv_mqtt_init();

// Start background task that maintains connection + publishes telemetry.
void cctv_mqtt_start_task();

// Publish a JSON payload immediately (thread-safe).
bool cctv_mqtt_publish(const char* json);

// Connection state.
bool cctv_mqtt_connected();
const char* cctv_mqtt_status_str();

// Runtime config (loaded from NVS, settable from web UI).
struct CctvMqttConfig {
  char server[64];
  uint16_t port;
  char token[64];       // ThingsBoard device access token
  char deviceId[32];    // friendly device name
  uint16_t pushIntervalS;  // telemetry push interval (seconds)
  float warnTemp;
  float criticalTemp;
  float alertTemp;
};

// Get/set config. After set, call save to persist to NVS.
CctvMqttConfig& cctv_mqtt_config();
void cctv_mqtt_load_config();
void cctv_mqtt_save_config();
