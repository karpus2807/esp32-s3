// cctv_mqtt.cpp — MQTT client for ThingsBoard
#include "cctv_mqtt.h"
#include "cctv_telemetry.h"
#include "cctv_pir.h"
#include "cctv_net.h"
#include "board_config.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <Preferences.h>

static WiFiClient   s_tcp;
static PubSubClient s_mqtt(s_tcp);
static CctvMqttConfig s_cfg;
static bool   s_connected = false;
static const char* s_status = "Not started";
static SemaphoreHandle_t s_mtx = nullptr;

// ────────── NVS load/save ──────────

void cctv_mqtt_load_config() {
  Preferences p;
  p.begin("iot", true);
  strlcpy(s_cfg.server,   p.getString("mqSrv",  CCTV_MQTT_DEFAULT_SERVER).c_str(), sizeof(s_cfg.server));
  s_cfg.port            = p.getUShort("mqPort",  CCTV_MQTT_DEFAULT_PORT);
  strlcpy(s_cfg.token,    p.getString("mqTok",   "").c_str(), sizeof(s_cfg.token));
  strlcpy(s_cfg.deviceId, p.getString("mqDev",   "esp32-s3-cctv").c_str(), sizeof(s_cfg.deviceId));
  s_cfg.pushIntervalS   = p.getUShort("mqIntv",  CCTV_MQTT_DEFAULT_INTERVAL_S);
  s_cfg.warnTemp        = p.getFloat("mqWarnT",  40.0f);
  s_cfg.criticalTemp    = p.getFloat("mqCritT",  50.0f);
  s_cfg.alertTemp       = p.getFloat("mqAlrtT",  35.0f);
  p.end();
}

void cctv_mqtt_save_config() {
  Preferences p;
  p.begin("iot", false);
  p.putString("mqSrv",   s_cfg.server);
  p.putUShort("mqPort",  s_cfg.port);
  p.putString("mqTok",   s_cfg.token);
  p.putString("mqDev",   s_cfg.deviceId);
  p.putUShort("mqIntv",  s_cfg.pushIntervalS);
  p.putFloat("mqWarnT",  s_cfg.warnTemp);
  p.putFloat("mqCritT",  s_cfg.criticalTemp);
  p.putFloat("mqAlrtT",  s_cfg.alertTemp);
  p.end();
}

CctvMqttConfig& cctv_mqtt_config() { return s_cfg; }

// ────────── connect / reconnect ──────────

static bool mqttConnect() {
  if (s_cfg.token[0] == '\0') {
    s_status = "No token configured";
    return false;
  }
  if (!cctv_has_ip_for_internet()) {
    s_status = "No network";
    return false;
  }
  s_mqtt.setServer(s_cfg.server, s_cfg.port);
  s_mqtt.setBufferSize(1024);

  // ThingsBoard: username = device token, password empty, clientId = deviceId
  if (s_mqtt.connect(s_cfg.deviceId, s_cfg.token, "")) {
    s_connected = true;
    s_status    = "Connected";
    return true;
  }
  s_connected = false;
  int rc = s_mqtt.state();
  static char errBuf[32];
  snprintf(errBuf, sizeof(errBuf), "Fail rc=%d", rc);
  s_status = errBuf;
  return false;
}

// ────────── publish ──────────

bool cctv_mqtt_publish(const char* json) {
  if (!s_mqtt.connected()) return false;
  if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
  bool ok = s_mqtt.publish("v1/devices/me/telemetry", json, false);
  if (s_mtx) xSemaphoreGive(s_mtx);
  return ok;
}

// ────────── background task ──────────

static void mqttTask(void *) {
  uint32_t lastPushMs = 0;
  bool pirWasAlert = false;

  for (;;) {
    // Reconnect loop
    if (!s_mqtt.connected()) {
      s_connected = false;
      s_status = "Reconnecting...";
      mqttConnect();
      if (!s_mqtt.connected()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
    }

    s_mqtt.loop();

    const uint32_t now = millis();

    // Immediate push on PIR rising edge
    bool pirNow = cctv_pir_alert();
    if (pirNow && !pirWasAlert) {
      String json = cctv_telemetry_build_json();
      cctv_mqtt_publish(json.c_str());
      lastPushMs = now;
    }
    pirWasAlert = pirNow;

    // Periodic telemetry push
    const uint32_t intervalMs = (uint32_t)s_cfg.pushIntervalS * 1000u;
    if (intervalMs > 0 && (now - lastPushMs >= intervalMs)) {
      String json = cctv_telemetry_build_json();
      cctv_mqtt_publish(json.c_str());
      lastPushMs = now;
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ────────── public API ──────────

void cctv_mqtt_init() {
  s_mtx = xSemaphoreCreateMutex();
  cctv_mqtt_load_config();
}

void cctv_mqtt_start_task() {
  xTaskCreatePinnedToCore(mqttTask, "MQTT", 8192, nullptr, 2, nullptr, 0);
}

bool cctv_mqtt_connected() { return s_connected && s_mqtt.connected(); }

const char* cctv_mqtt_status_str() { return s_status; }
