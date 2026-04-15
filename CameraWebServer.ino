#include "esp_camera.h"
#include <WiFi.h>
#include "esp_eap_client.h"
#include "esp_wifi.h"
#include <FS.h>
#include <SD_MMC.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <time.h>
#include <vector>
#include <algorithm>

#include "board_config.h"
#include "avi_recorder.h"
#include "cctv_osd.h"
#include "cctv_platform.h"
#include "cctv_net.h"
#include "cctv_psram.h"
#include "cctv_wifi_profiles.h"
#include "cctv_time_sync.h"
#include "cctv_web_control.h"
#include "cctv_dht.h"
#include "cctv_pir.h"
#include "cctv_oled.h"
#include "cctv_mqtt.h"
#include "cctv_telemetry.h"
// Library includes for Arduino CLI dependency resolution
#include <DHTesp.h>
#include <U8g2lib.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "esp_heap_caps.h"
#include <esp_task_wdt.h>
#include <sdkconfig.h>

#define DBG_PORT Serial
#if CCTV_SERIAL_VERBOSE
#define SDBGf(...) Serial.printf(__VA_ARGS__)
#define SDBGln(x) Serial.println(x)
#else
#define SDBGf(...) ((void)0)
#define SDBGln(x) ((void)0)
#endif

#if defined(CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION) && CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION && __has_include("freertos/idf_additions.h")
#include "freertos/idf_additions.h"
#define CCTV_TASK_CAPS 1
#else
#define CCTV_TASK_CAPS 0
#endif

static bool cctv_task_create_caps(TaskFunction_t fn,
                                 const char *name,
                                 uint32_t stack_bytes,
                                 void *param,
                                 UBaseType_t prio,
                                 BaseType_t core,
                                 uint32_t caps,
                                 TaskHandle_t *out_handle) {
#if CCTV_TASK_CAPS
  if (xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, param, prio, out_handle, core, caps) == pdPASS) {
    return true;
  }
#endif
  (void)caps;
  return xTaskCreatePinnedToCore(fn, name, stack_bytes, param, prio, out_handle, core) == pdPASS;
}

static void cctv_apply_wifi_throughput_tuning() {
#if CONFIG_ESP_WIFI_ENABLED
  (void)esp_wifi_set_protocol(WIFI_IF_STA,
                               WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  (void)esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
  // AMPDU: aggregate multiple frames into one WiFi transmission — higher throughput.
  wifi_config_t wc = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
  }
#endif
}

static void ntp_housekeeping_timer_cb(void *arg) {
  (void)arg;
  cctv_poll_ntp_housekeeping();
}
// SD_MMC 1-bit (must match PCB; OceanLabz N16R8 CAM boards use 39/38/40)
#define SD_CLK_PIN 39
#define SD_CMD_PIN 38
#define SD_D0_PIN 40

// WiFi config (persisted via NVS)
String   g_wifiSsid        = "";
String   g_wifiPass        = "";
bool     g_wifiEnterprise  = false;   // true = WPA/WPA2/WPA3-Enterprise (PEAP/MSCHAPV2)
String   g_wifiIdentity    = "";      // EAP identity / LDAP username
String   g_wifiEapPass     = "";      // EAP password (LDAP password)
String   g_timeHttpUrl;                // world-clock HTTP (NVS key timeHttp); empty = disabled
String   g_wifiStatus      = "Not connected";
bool     g_wifiAutoConnect = true;     // true = auto-connect WiFi (default ON)
// AP mode permanently disabled — WiFi setup via web dashboard (/wifi) or optional serial console

bool g_sdReady = false;
String g_lastSavedPath;
String g_sdStatusMessage = "Not initialized";
String g_cameraStatusMessage = "Booting";
String g_recordingStatus = "Initializing";
String g_recordingFile;


bool g_sdInitPending = true;
uint8_t g_sdInitAttempts = 0;
uint8_t g_sdInitFailures = 0;

namespace {
const char *kNtpServer = "pool.ntp.org";
const char *kTimezone = "IST-5:30";
uint32_t g_fileCounter = 0;
uint32_t g_lastAutoSaveMs = 0;
uint32_t g_bootMillis = 0;
bool g_ntpSyncPending = false;
bool g_ntpSyncDone = false;
uint32_t g_ntpSyncStartMs = 0;
uint32_t g_lastNtpCheckMs = 0;
const uint32_t kSdInitDelayMs = 3000;
const uint32_t kSdRetryIntervalMs = 1000;
const uint8_t kSdInitMaxAttempts = 3;
uint32_t g_lastSdInitAttemptMs = 0;
}

TaskHandle_t sdInitTaskHandle = NULL;

bool startCameraServer();
bool remountMicroSD();
bool testMicroSD(String *message = nullptr);
size_t clearCaptures(String *message = nullptr);
String listCapturesJson(size_t limit = 20);
void recordingTask(void *parameter);
void loadWifiConfig();
void saveWifiConfig();
void saveTimeHttpConfig();
void clearWifiConfig();
bool connectWifi(uint32_t timeoutMs = 12000);
void serialConsoleTask(void *parameter);
void ethSerialMiniTask(void *parameter);
TaskHandle_t recordingTaskHandle = NULL;

static void warmupCamera(uint8_t frames) {
  SDBGf("[BOOT] +%lums sensor warmup %u JPEG frames (this can take a few seconds)...\n",
        (unsigned long)(millis() - g_bootMillis), (unsigned)frames);
#if CCTV_SERIAL_VERBOSE
  DBG_PORT.flush();
#endif
  const uint32_t w0 = millis();
  for (uint8_t i = 0; i < frames; ++i) {
    cctv_camera_lock();
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      esp_camera_fb_return(fb);
    }
    cctv_camera_unlock();
    vTaskDelay(pdMS_TO_TICKS(30));
    if ((i + 1u) % 5u == 0u || i + 1u == frames) {
      SDBGf("[BOOT]   frame %u/%u\n", (unsigned)(i + 1u), (unsigned)frames);
#if CCTV_SERIAL_VERBOSE
      DBG_PORT.flush();
#endif
    }
  }
  SDBGf("[BOOT] +%lums sensor warmup done (%lu ms)\n",
        (unsigned long)(millis() - g_bootMillis),
        (unsigned long)(millis() - w0));
#if CCTV_SERIAL_VERBOSE
  DBG_PORT.flush();
#endif
}

static const char* wifiStatusStr(wl_status_t s) {
  switch(s) {
    case WL_NO_SSID_AVAIL:   return "SSID not found";
    case WL_CONNECT_FAILED:  return "Wrong password";
    case WL_CONNECTION_LOST: return "Connection lost";
    case WL_DISCONNECTED:    return "Disconnected";
    case WL_IDLE_STATUS:     return "Idle";
    case WL_CONNECTED:       return "Connected";
    default:                 return "Unknown";
  }
}

bool connectWifi(uint32_t timeoutMs) {
    // Always load auto-connect flag from NVS (in case changed by user)
    loadWifiAutoConnect();
  // Do NOT use WiFi.mode(WIFI_OFF) — it destroys the netif and causes
  // "netstack cb reg failed" (ESP_ERR_ESP_NETIF, 12308) on the next call.
  // Simply disconnect and stay in STA mode.
  // false = STA netif stays registered (avoids ESP_ERR_ESP_NETIF / "netstack cb reg failed" 12308).
  // Credentials are still applied by the following WiFi.begin(...).
  WiFi.disconnect(false);
  delay(150);

  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
    delay(100);
  }
  WiFi.setSleep(false);

  if (g_wifiEnterprise) {
    cctv_apply_wifi_throughput_tuning();
    // Always reset enterprise client state before applying new credentials.
    (void)esp_wifi_sta_enterprise_disable();
    esp_eap_client_clear_identity();
    esp_eap_client_clear_username();
    esp_eap_client_clear_password();
    esp_eap_client_clear_ca_cert();
    esp_eap_client_clear_certificate_and_key();

    String eapUser = g_wifiIdentity;
    eapUser.trim();
    // Many enterprise networks accept plain username (no @domain).
    if (eapUser.length() == 0) {
      eapUser = g_wifiSsid;
    }
    // When user doesn't provide realm/domain, many RADIUS setups prefer anonymous outer identity.
    const String outerIdentity = (eapUser.indexOf('@') >= 0) ? eapUser : String("anonymous");
    SDBGf("[WiFi] Enterprise connect → SSID: %s  user: %s  outer-id: %s\n",
          g_wifiSsid.c_str(), eapUser.c_str(), outerIdentity.c_str());
    const esp_err_t e1 = esp_eap_client_set_eap_methods(ESP_EAP_TYPE_ALL);
    const esp_err_t e2 = esp_eap_client_set_disable_time_check(true);
    const esp_err_t e3 = esp_eap_client_set_ca_cert(nullptr, 0);  // no certificate mode
    const esp_err_t e4 = esp_eap_client_set_identity((uint8_t*)outerIdentity.c_str(), outerIdentity.length());
    const esp_err_t e5 = esp_eap_client_set_username((uint8_t*)eapUser.c_str(), eapUser.length());
    const esp_err_t e6 = esp_eap_client_set_password((uint8_t*)g_wifiEapPass.c_str(), g_wifiEapPass.length());
    const esp_err_t e7 = esp_wifi_sta_enterprise_enable();
    SDBGf("[WiFi] EAP cfg: methods=%s time=%s ca=%s id=%s user=%s pass=%s enable=%s\n",
          esp_err_to_name(e1),
          esp_err_to_name(e2),
          esp_err_to_name(e3),
          esp_err_to_name(e4),
          esp_err_to_name(e5),
          esp_err_to_name(e6),
          esp_err_to_name(e7));
    WiFi.begin(g_wifiSsid.c_str());
  } else {
    // Prevent stale enterprise mode from affecting normal WPA/WPA2 joins.
    (void)esp_wifi_sta_enterprise_disable();
    cctv_apply_wifi_throughput_tuning();
    SDBGf("[WiFi] Personal connect → SSID: %s\n", g_wifiSsid.c_str());
    WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
  }

  const uint32_t start = millis();
  wl_status_t lastStatus = WL_IDLE_STATUS;
  uint32_t dotCount = 0;
  while (millis() - start < timeoutMs) {
    wl_status_t st = WiFi.status();
    if (st != lastStatus) {
      SDBGf("\n[WiFi] Status → %s (%d)  [%lu ms]\n",
            wifiStatusStr(st), (int)st, (unsigned long)(millis() - start));
      lastStatus = st;
    }
    if (st == WL_CONNECTED) break;
    // WL_CONNECT_FAILED (4) and WL_NO_SSID_AVAIL (1) are terminal failures — stop waiting
    // WL_DISCONNECTED (6) is a transient state during association — keep waiting
    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
      SDBGf("[WiFi] Terminal failure (%d) — stopping\n", (int)st);
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    if (++dotCount % 10 == 0)
      SDBGf("[WiFi] ... %lu ms elapsed, status=%s\n",
            (unsigned long)(millis() - start), wifiStatusStr(WiFi.status()));
    else
      SDBGf(".");
  }
  SDBGf("\n");

  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) {
#if CONFIG_ESP_WIFI_ENABLED
    (void)WiFi.setTxPower(WIFI_POWER_19_5dBm);
#endif
    // Disable power save AFTER connection — calling before WiFi.begin() has no effect
    // because the driver resets ps mode during association handshake.
    // WIFI_PS_NONE = no modem sleep, no beacon-aligned wakeup, ~0ms latency
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_ps_type_t ps_check = WIFI_PS_MAX_MODEM;
    esp_wifi_get_ps(&ps_check);
    SDBGf("[WiFi] set_ps->%d  verify get_ps=%d (0=none)\n", (int)ps_err, (int)ps_check);
    WiFi.setSleep(false);
    g_wifiStatus = "Connected: " + WiFi.localIP().toString();
    SDBGf("[WiFi] OK  IP: %s  RSSI: %d dBm  Channel: %d  BSSID: %s\n",
          WiFi.localIP().toString().c_str(), WiFi.RSSI(),
          WiFi.channel(), WiFi.BSSIDstr().c_str());
    cctv_net_apply_fallback_dns();
    cctv_time_kick_sync_if_needed(true);
  } else {
    g_wifiStatus = String("Failed: ") + wifiStatusStr(WiFi.status());
    SDBGf("[WiFi] FAIL  final status: %s (%d)  elapsed: %lu ms\n",
          wifiStatusStr(WiFi.status()), (int)WiFi.status(),
          (unsigned long)(millis() - start));
    if (g_wifiEnterprise) {
      SDBGln(F("[WiFi] Enterprise hint: verify SSID really uses WPA2/WPA3-Enterprise and account is enabled for this SSID."));
    }
  }
  return ok;
}

#if CONFIG_ESP_WIFI_ENABLED
// Runs on Core 1 so setup()/HTTP bring-up on the other path is not blocked by multi-slot STA
// attempts (each can take many seconds). LAN users get the web UI as soon as Ethernet DHCP
// completes within CCTV_ETH_BOOT_DHCP_WAIT_MS.
static void wifiStationBackgroundTask(void *) {
  vTaskDelay(pdMS_TO_TICKS(400));
  for (;;) {
    if (!cctv_wifi_any_profile_configured()) {
      vTaskDelay(pdMS_TO_TICKS(15000));
      continue;
    }
    if (WiFi.status() == WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    if (!cctv_wifi_try_lock(200)) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    SDBGln(F("[WiFi] Background STA: trying saved profiles\xe2\x80\xa6"));
    g_wifiStatus = "Connecting (STA)\xe2\x80\xa6";
    const bool ok = cctv_wifi_try_connect_profiles(15000);
    cctv_wifi_unlock();
    if (ok) {
      SDBGf("[WiFi] STA OK  IP: %s\n", WiFi.localIP().toString().c_str());
      cctv_time_kick_sync_if_needed(true);
      vTaskDelay(pdMS_TO_TICKS(3000));
    } else {
      g_wifiStatus = "Not connected \xe2\x80\x94 retry in ~12s";
      vTaskDelay(pdMS_TO_TICKS(12000));
    }
  }
}
#include <Preferences.h>
void saveWifiAutoConnect() {
  Preferences prefs;
  if (prefs.begin("cctv_wifi", false)) {
    prefs.putBool("autoConnect", g_wifiAutoConnect);
    prefs.end();
  }
}
void loadWifiAutoConnect() {
  Preferences prefs;
  if (prefs.begin("cctv_wifi", true)) {
    g_wifiAutoConnect = prefs.getBool("autoConnect", true);
    prefs.end();
  }
}
#endif

void startClockSync() {
  configTzTime(kTimezone, kNtpServer);
  g_ntpSyncPending = true;
  g_ntpSyncDone = false;
  g_ntpSyncStartMs = millis();
  g_lastNtpCheckMs = 0;
}

void tryHttpWorldClockSync() {
  if (g_timeHttpUrl.isEmpty()) {
    return;
  }
  if (!cctv_has_ip_for_internet()) {
    return;
  }
  if (cctvHttpTimeSync(g_timeHttpUrl.c_str())) {
    struct tm ti;
    if (getLocalTime(&ti, 200)) {
      SDBGf(
        "[TIME] World clock (HTTP) OK → %04d-%02d-%02d %02d:%02d:%02d\n",
        ti.tm_year + 1900,
        ti.tm_mon + 1,
        ti.tm_mday,
        ti.tm_hour,
        ti.tm_min,
        ti.tm_sec
      );
    } else {
      SDBGln(F("[TIME] HTTP set clock OK (localtime pending)"));
    }
    // HTTP settimeofday is enough for filenames/OSD — do not wait only on SNTP
    g_ntpSyncDone = true;
    g_ntpSyncPending = false;
    cctv_time_nvs_snapshot_wall();
  } else {
    SDBGln(F("[TIME] World-clock HTTP sync failed — NTP will still run"));
  }
}

void cctv_poll_ntp_housekeeping() {
  const uint32_t now = millis();
  static uint32_t s_lastHttpTryMs = 0;

  // Internet came later (or booted without IP): re-open HTTP/NTP until wall clock is sane.
  cctv_time_kick_sync_if_needed(false);

  // Retry HTTP world-clock while SNTP is still settling (common on Wi‑Fi right after connect).
  if (g_ntpSyncPending && !g_ntpSyncDone && !g_timeHttpUrl.isEmpty() && cctv_has_ip_for_internet()) {
    if (now - s_lastHttpTryMs >= 8000u) {
      s_lastHttpTryMs = now;
      tryHttpWorldClockSync();
    }
  }

  if (!g_ntpSyncPending || g_ntpSyncDone) {
    return;
  }

  if (now - g_lastNtpCheckMs < 250) {
    return;
  }
  g_lastNtpCheckMs = now;

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) {
    SDBGf(
      "Time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
      timeinfo.tm_year + 1900,
      timeinfo.tm_mon + 1,
      timeinfo.tm_mday,
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec
    );
    g_ntpSyncDone = true;
    g_ntpSyncPending = false;
    cctv_time_nvs_snapshot_wall();
    return;
  }

  if (now - g_ntpSyncStartMs > 45000) {
    SDBGln("NTP sync window ended — filenames use millis until next HTTP/NTP success");
    g_ntpSyncPending = false;
  }
}

static bool initMicroSD() {
  SD_MMC.end();
  if (!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN)) {
    SDBGln("SD_MMC setPins failed");
    g_sdStatusMessage = "SD pin mapping failed";
    return false;
  }

  bool mounted = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_HIGHSPEED);
  if (!mounted) {
    SDBGln("SD_MMC high-speed mount failed, retrying default speed");
    mounted = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT);
  }
  if (!mounted) {
    SDBGln("SD_MMC mount failed");
    g_sdStatusMessage = "Mount failed in 1-bit mode on GPIO39/38/40";
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    SDBGln("No microSD card detected");
    g_sdStatusMessage = "No microSD card detected";
    SD_MMC.end();
    return false;
  }

  if (!SD_MMC.exists("/captures") && !SD_MMC.mkdir("/captures")) {
    SDBGln("Failed to create /captures directory");
    g_sdStatusMessage = "Unable to create /captures";
    SD_MMC.end();
    return false;
  }

  uint64_t totalMB = SD_MMC.totalBytes() / (1024 * 1024);
  uint64_t usedMB = SD_MMC.usedBytes() / (1024 * 1024);
  SDBGf("microSD ready: %lluMB total, %lluMB used\n", totalMB, usedMB);
  g_sdStatusMessage = "Mounted and ready";
  return true;
}

static String buildCapturePath() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const unsigned ms = static_cast<unsigned>(tv.tv_usec / 1000);
    char path[72];
    snprintf(
      path,
      sizeof(path),
      "/captures/%04d-%02d-%02d_%02d-%02d-%02d_%03u_%04lu.jpg",
      timeinfo.tm_year + 1900,
      timeinfo.tm_mon + 1,
      timeinfo.tm_mday,
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec,
      ms,
      static_cast<unsigned long>(g_fileCounter++));
    return String(path);
  }

  char path[48];
  snprintf(path, sizeof(path), "/captures/capture_%010lu.jpg", static_cast<unsigned long>(millis()));
  return String(path);
}

bool saveJpegFrameToSd(String *savedPath) {
  if (!g_sdReady) {
    return false;
  }

  camera_fb_t *fb = nullptr;
  for (uint8_t attempt = 0; attempt < 4; ++attempt) {
    cctv_camera_lock();
    fb = esp_camera_fb_get();
    if (fb) {
      break;
    }
    cctv_camera_unlock();
    delay(35);
  }
  if (!fb) {
    SDBGln("Capture failed while saving to SD");
    return false;
  }

  const pixformat_t pixFmt = fb->format;
  uint8_t *jpg_buf = nullptr;
  size_t jpg_buf_len = 0;
  bool jpg_is_heap_copy = false;

  if (pixFmt == PIXFORMAT_JPEG) {
    jpg_buf = (uint8_t *)heap_caps_malloc(
        fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpg_buf) {
      jpg_buf = (uint8_t *)heap_caps_malloc(
          fb->len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!jpg_buf) {
      esp_camera_fb_return(fb);
      cctv_camera_unlock();
      return false;
    }
    memcpy(jpg_buf, fb->buf, fb->len);
    jpg_buf_len = fb->len;
    jpg_is_heap_copy = true;
  } else {
    if (!frame2jpg(fb, CCTV_JPEG_QUALITY, &jpg_buf, &jpg_buf_len)) {
      SDBGln("JPEG re-encoding failed");
      esp_camera_fb_return(fb);
      cctv_camera_unlock();
      return false;
    }
  }

  const uint16_t cap_w = fb->width;
  const uint16_t cap_h = fb->height;

  esp_camera_fb_return(fb);
  cctv_camera_unlock();

#if CCTV_ENABLE_FRAME_OSD && CCTV_OSD_ON_CAPTURE
  if (jpg_is_heap_copy && jpg_buf && jpg_buf_len > 0) {
    (void)cctvStampJpegBottomBar(&jpg_buf, &jpg_buf_len, cap_w, cap_h);
  }
#endif

  const String path = buildCapturePath();
  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    SDBGf("Failed to open %s for writing\n", path.c_str());
    if (jpg_is_heap_copy) {
      heap_caps_free(jpg_buf);
    } else if (jpg_buf) {
      free(jpg_buf);
    }
    return false;
  }

  size_t written = file.write(jpg_buf, jpg_buf_len);
  file.close();

  if (jpg_is_heap_copy) {
    heap_caps_free(jpg_buf);
  } else if (jpg_buf) {
    free(jpg_buf);
  }

  if (written != jpg_buf_len) {
    SDBGf("Short write while saving %s\n", path.c_str());
    SD_MMC.remove(path);
    return false;
  }

  g_lastSavedPath = path;
  if (savedPath != nullptr) {
    *savedPath = path;
  }

  SDBGf("Saved JPEG to %s (%u bytes)\n", path.c_str(), static_cast<unsigned>(written));
  return true;
}

bool saveJpegBufferToSd(const uint8_t *jpegBuf, size_t jpegLen, String *savedPath) {
  if (!g_sdReady || jpegBuf == nullptr || jpegLen == 0) {
    return false;
  }

  const String path = buildCapturePath();
  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    SDBGf("Failed to open %s for writing\n", path.c_str());
    return false;
  }

  size_t written = file.write(jpegBuf, jpegLen);
  file.close();

  if (written != jpegLen) {
    SDBGf("Short write while saving %s\n", path.c_str());
    SD_MMC.remove(path);
    return false;
  }

  g_lastSavedPath = path;
  if (savedPath != nullptr) {
    *savedPath = path;
  }

  SDBGf("Saved JPEG to %s (%u bytes)\n", path.c_str(), static_cast<unsigned>(written));
  return true;
}



static void configureSensorDefaults() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    g_cameraStatusMessage = "Sensor handle not available";
    return;
  }

  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_hmirror(s, 0);
    s->set_brightness(s, 1);
    s->set_saturation(s, -1);
    s->set_whitebal(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_awb_gain(s, 1);
    s->set_aec2(s, 1);
  }

  g_cameraStatusMessage = "Sensor tuned";
}

bool remountMicroSD() {
  SD_MMC.end();
  delay(50);
  g_sdReady = initMicroSD();
  if (g_sdReady) {
    g_sdInitFailures = 0;
    g_sdInitAttempts = 0;
  }
  return g_sdReady;
}

bool testMicroSD(String *message) {
  if (!g_sdReady && !remountMicroSD()) {
    if (message != nullptr) {
      *message = g_sdStatusMessage;
    }
    return false;
  }

  File file = SD_MMC.open("/captures/.probe.txt", FILE_WRITE);
  if (!file) {
    g_sdStatusMessage = "Write test failed";
    if (message != nullptr) {
      *message = g_sdStatusMessage;
    }
    return false;
  }

  file.println("probe");
  file.close();
  SD_MMC.remove("/captures/.probe.txt");
  g_sdStatusMessage = "Read/write test passed";
  if (message != nullptr) {
    *message = g_sdStatusMessage;
  }
  return true;
}

size_t clearCaptures(String *message) {
  if (!g_sdReady && !remountMicroSD()) {
    if (message != nullptr) {
      *message = g_sdStatusMessage;
    }
    return 0;
  }

  File root = SD_MMC.open("/captures");
  if (!root || !root.isDirectory()) {
    g_sdStatusMessage = "Capture folder not available";
    if (message != nullptr) {
      *message = g_sdStatusMessage;
    }
    return 0;
  }

  size_t removed = 0;
  File entry = root.openNextFile();
  while (entry) {
    String path = String("/captures/") + entry.name();
    entry.close();
    if (SD_MMC.remove(path)) {
      removed++;
    }
    entry = root.openNextFile();
  }
  root.close();

  g_sdStatusMessage = "Cleared " + String(removed) + " files";
  g_lastSavedPath = "";
  if (message != nullptr) {
    *message = g_sdStatusMessage;
  }
  return removed;
}

String listCapturesJson(size_t limit) {
  if (!g_sdReady) {
    return "[]";
  }

  File root = SD_MMC.open("/captures");
  if (!root || !root.isDirectory()) {
    return "[]";
  }

  struct FInfo { String name; size_t size; };
  std::vector<FInfo> files;
  files.reserve(64);

  File entry = root.openNextFile();
  while (entry) {
    String n = String(entry.name());
    if (!n.startsWith(".")) {
      files.push_back({n, entry.size()});
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();

  std::sort(files.begin(), files.end(), [](const FInfo &a, const FInfo &b){
    return a.name > b.name;
  });

  String json = "[";
  size_t count = 0;
  for (const auto &f : files) {
    if (count >= limit) break;
    if (count > 0) json += ",";
    json += "{\"name\":\"";
    json += f.name;
    json += "\",\"size\":";
    json += String((unsigned long)f.size);
    json += "}";
    count++;
  }
  json += "]";
  return json;
}

static String buildAviPath() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const unsigned ms = static_cast<unsigned>(tv.tv_usec / 1000);
    char path[72];
    snprintf(
      path,
      sizeof(path),
      "/captures/%04d-%02d-%02d_%02d-%02d-%02d_%03u.avi",
      timeinfo.tm_year + 1900,
      timeinfo.tm_mon + 1,
      timeinfo.tm_mday,
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec,
      ms);
    return String(path);
  }
  char path[48];
  snprintf(path, sizeof(path), "/captures/rec_%010lu.avi", (unsigned long)millis());
  return String(path);
}

// ─── WiFi config: load / save via NVS Preferences ────────────────────────

void loadWifiConfig() {
  cctv_wifi_load_profiles();
  cctv_wifi_apply_preferred_or_first_globals();
  Preferences prefs;
  prefs.begin("cctv", true);
  // thDis=1: user disabled HTTP time sync ("timeurl off"). Otherwise use NVS URL or firmware default.
  if (prefs.getUChar("thDis", 0) == 1) {
    g_timeHttpUrl = "";
  } else {
    g_timeHttpUrl = prefs.getString("timeHttp", "");
    g_timeHttpUrl.trim();
    if (g_timeHttpUrl.isEmpty()) {
      g_timeHttpUrl = String(CCTV_WORLD_TIME_HTTP_URL);
    }
  }
  prefs.end();
}

void saveWifiConfig() {
  cctv_wifi_save_globals_into_best_slot();
}

void saveTimeHttpConfig() {
  Preferences prefs;
  prefs.begin("cctv", false);
  if (g_timeHttpUrl.isEmpty()) {
    prefs.putUChar("thDis", 1);
    prefs.remove("timeHttp");
  } else {
    prefs.putUChar("thDis", 0);
    prefs.putString("timeHttp", g_timeHttpUrl);
  }
  prefs.end();
}

void clearWifiConfig() {
  g_wifiSsid = "";
  g_wifiPass = "";
  g_wifiIdentity = "";
  g_wifiEapPass = "";
  g_wifiEnterprise = false;
  cctv_wifi_clear_all_slots_nvs();
}



// ─── Recording task ────────────────────────────────────────────────────────

// Runs on Core 0 — records AVI segments. Camera is locked only while grabbing
// and copying a frame; SD writes run without the lock so HTTP /stream stays smooth.
void recordingTask(void *parameter) {
  const uint8_t  kFps          = CCTV_RECORD_FPS;
  const uint32_t kFrameMs      = 1000u / kFps;
  const uint32_t kSegmentMs    = 30u * 60u * 1000u;     // 30 minutes per AVI segment
  const uint64_t kMinFreeBytes = 50ULL * 1024 * 1024;   // stop at < 50 MB free

  // Unsubscribe this task from WDT so heavy OSD+SD writes can't trigger it.
  // IDLE tasks on both cores remain subscribed (if enabled).
  (void)esp_task_wdt_delete(xTaskGetCurrentTaskHandle());

  AviRecorder recorder;

  while (true) {
    if (!g_sdReady) {
      g_recordingStatus = "Waiting for SD";
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }

    if ((SD_MMC.totalBytes() - SD_MMC.usedBytes()) < kMinFreeBytes) {
      g_recordingStatus = "SD full — recording paused";
      vTaskDelay(10000 / portTICK_PERIOD_MS);
      continue;
    }

    String path = buildAviPath();
    if (!recorder.begin(path, 640, 480, kFps)) {
      g_recordingStatus = "Cannot create AVI file";
      vTaskDelay(2000 / portTICK_PERIOD_MS);
      continue;
    }

    g_recordingFile = path;
    const uint32_t segStart = millis();
    // Wall-clock grid: frame i is due at segStart + i * kFrameMs. If we fall behind by
    // a full slot, skip indices (drop) instead of bursting — keeps OSD time vs playback aligned.
    uint32_t frameIdx = 0;

    while (g_sdReady && (millis() - segStart < kSegmentMs)) {
      // Catch up dropped frames in O(1) — the old inner while incremented frameIdx in a
      // tight loop and could starve IDLE0 → task_wdt / Guru Meditation on busy builds.
      const uint32_t wall = millis() - segStart;
      if (kFrameMs > 0) {
        const uint32_t expected = wall / kFrameMs;
        if (expected > frameIdx) {
          frameIdx = expected;
        }
      }
      const uint32_t deadline = segStart + frameIdx * kFrameMs;
      const int32_t wait = (int32_t)(deadline - millis());
      if (wait > 1) {
        vTaskDelay((uint32_t)wait / portTICK_PERIOD_MS);
      } else {
        // Behind schedule — still must yield so IDLE0 can reset the task WDT.
        vTaskDelay(1);
      }

      cctv_camera_lock();
      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) {
        cctv_camera_unlock();
        frameIdx++;
        vTaskDelay(2 / portTICK_PERIOD_MS);
        continue;
      }

      const pixformat_t pixFmt = fb->format;
      uint8_t *slot = nullptr;
      size_t slotLen = 0;

      uint16_t stamp_w = fb->width;
      uint16_t stamp_h = fb->height;

      if (pixFmt == PIXFORMAT_JPEG) {
        slot = (uint8_t *)heap_caps_malloc(
            fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!slot) {
          slot = (uint8_t *)heap_caps_malloc(
              fb->len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (slot) {
          memcpy(slot, fb->buf, fb->len);
          slotLen = fb->len;
        }
      } else {
        frame2jpg(fb, CCTV_JPEG_QUALITY, &slot, &slotLen);
      }

      esp_camera_fb_return(fb);
      cctv_camera_unlock();

#if CCTV_ENABLE_FRAME_OSD && CCTV_OSD_ON_RECORDING
      // Stamp OSD only every Nth frame (~1/sec) to cut CPU by ~90%.
      // Non-stamped frames pass through as raw camera JPEG (no decode+encode).
      if (slot && slotLen > 0 && pixFmt == PIXFORMAT_JPEG &&
          (frameIdx % CCTV_OSD_STAMP_EVERY_N) == 0) {
        (void)cctvStampJpegBottomBar(
            &slot, &slotLen, stamp_w, stamp_h, (int)CCTV_OSD_JPEG_QUALITY_RECORD);
        vTaskDelay(1);  // yield after heavy JPEG decode+encode
      }
#endif

      if (slot && slotLen > 0) {
        recorder.writeFrame(slot, slotLen);
      } else {
        vTaskDelay(2 / portTICK_PERIOD_MS);
      }

      if (slot) {
        if (pixFmt == PIXFORMAT_JPEG) {
          heap_caps_free(slot);
        } else {
          free(slot);
        }
      }

      frameIdx++;

      // Update status string every ~30 frames (~2 s)
      if (recorder.frameCount() % 30 == 0) {
        String fname = path.substring(path.lastIndexOf('/') + 1);
        g_recordingStatus = "REC " + fname + " [" + String(recorder.frameCount()) + "]";
      }
    }

    uint32_t savedFrames = recorder.frameCount();
    const uint32_t wallSpan = millis() - segStart;
    recorder.end(wallSpan);
    g_recordingFile = "";
    String fname = path.substring(path.lastIndexOf('/') + 1);
    g_recordingStatus = "Saved: " + fname + " (" + String(savedFrames) + " fr)";
    SDBGf("[REC] Segment: %s  %u frames\n", path.c_str(), savedFrames);
  }
}

void sdInitBackgroundTask(void *parameter) {
  vTaskDelay(3000 / portTICK_PERIOD_MS);
  
  uint32_t attempts = 0;
  while (attempts < kSdInitMaxAttempts) {
    SDBGf("SD init background: attempt %u/%u\n", attempts + 1, kSdInitMaxAttempts);
    g_sdReady = initMicroSD();
    if (g_sdReady) {
      g_sdInitFailures = 0;
      g_sdInitPending = false;
      SDBGln("SD init background: success");
      break;
    }
    g_sdInitFailures++;
    attempts++;
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  
  if (!g_sdReady) {
    g_sdStatusMessage = "Auto SD init paused (3 failures). Use Re-mount SD.";
    SDBGln(g_sdStatusMessage);
  }
  
  vTaskDelete(NULL);
}

void setup() {
#if CCTV_SERIAL_VERBOSE || CCTV_ENABLE_SERIAL_CONSOLE
  DBG_PORT.begin(115200);
  delay(200);
#else
  // USB CDC may still enumerate; no chatty serial boot.
  Serial.begin(115200);
  delay(50);
#endif
  cctv_platform_init();
#if CONFIG_SPIRAM_USE_MALLOC
  heap_caps_malloc_extmem_enable(64);
#endif
  g_bootMillis = millis();
  SDBGf("\n");
  SDBGln("Booting ESP32-S3 Camera Web Server");
  SDBGln(F("[BOOT] Serial OK — next: camera init (no logs until done or warmup progress)"));
#if CCTV_SERIAL_VERBOSE
  DBG_PORT.flush();
#endif
  g_sdStatusMessage = "Waiting for background SD init";
  g_sdInitAttempts = 0;
  g_sdInitFailures = 0;
  g_lastSdInitAttemptMs = 0;

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = CCTV_JPEG_QUALITY;
  // Many JPEG framebuffers in internal DRAM will exhaust heap (~30–50KB left) and break SD / httpd.
  // Do not rely on psramFound() alone — on IDF 5.x, SPIRAM heap may work while Arduino reports 0.
  if (cctv_psram_app_ready()) {
    config.fb_count = 8;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    Serial.printf(
        "[BOOT] PSRAM OK  heap_caps total=%u free=%u  Arduino getPsram=%u\n",
        (unsigned) cctv_psram_total_bytes(),
        (unsigned) cctv_psram_free_bytes(),
        (unsigned) ESP.getPsramSize());
  } else {
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    Serial.println(
        F("[BOOT] WARNING: no SPIRAM heap — using 1 DRAM framebuffer. "
          "Tools: board with PSRAM enabled (e.g. OPI / 8MB)."));
  }
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[BOOT] Camera init FAILED 0x%x — setup stops here (reflash / check camera bus)\n", err);
    Serial.flush();
    g_cameraStatusMessage = "Init failed";
    return;
  }
  SDBGf("[BOOT] +%lums camera driver OK\n", (unsigned long)(millis() - g_bootMillis));
#if CCTV_SERIAL_VERBOSE
  Serial.flush();
#endif

  // Direct sensor setup — no recoverCamera() wrapper for clean fast init
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, FRAMESIZE_VGA);
    s->set_quality(s, CCTV_JPEG_QUALITY);
    // OV3660 orientation
    s->set_vflip(s, 1);
    s->set_hmirror(s, 0);
    // Image tuning
    s->set_brightness(s, 1);
    s->set_contrast(s, 1);
    s->set_saturation(s, -1);
    // Auto exposure + white balance
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);  // auto
    s->set_gain_ctrl(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_aec_value(s, 300);
  }

  // Warm up sensor: allow AWB and AEC to settle (fewer frames = faster time-to-web)
  warmupCamera(4);
  g_cameraStatusMessage = "Sensor ready";

  // ── v2.0 Addon modules: OLED, DHT, PIR, MQTT ─────────────────────────
  // cctv_oled_init();           // DISABLED — GPIO 1 (SDA) temporarily used as ETH SCK
  // cctv_oled_boot_animation(); // DISABLED
  cctv_dht_init();              // DHT11 sensor + background read task
  cctv_pir_init();              // PIR ISR on GPIO 41
  cctv_mqtt_init();             // Load MQTT config from NVS

  loadWifiConfig();
  SDBGf("[BOOT] +%lums net: STA prep → Ethernet → short DHCP → HTTP → Wi‑Fi on core 1\n",
        (unsigned long)(millis() - g_bootMillis));
#if CCTV_SERIAL_VERBOSE
  Serial.flush();
#endif
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  cctv_apply_wifi_throughput_tuning();
  if (cctv_wifi_any_profile_configured()) {
    g_wifiStatus = "STA deferred (connecting in background)";
    SDBGln(F("[WiFi] Saved profiles present — STA join runs in background (LAN web stays fast)."));
  } else {
    g_wifiStatus = "No WiFi profiles — use web WiFi panel or Serial wifiscan";
    SDBGln(F("[WiFi] No saved SSIDs. Configure up to 3 networks via LAN web UI."));
  }
#if CCTV_SERIAL_VERBOSE
  Serial.flush();
#endif

  SDBGf("[BOOT] +%lums W5500 Ethernet init...\n", (unsigned long)(millis() - g_bootMillis));
#if CCTV_SERIAL_VERBOSE
  Serial.flush();
#endif
  cctv_net_init_ethernet();
  SDBGf("[ETH] Boot DHCP wait up to %u ms (slow LAN: continues in EthDhcpPoll task)...\n",
        (unsigned)CCTV_ETH_BOOT_DHCP_WAIT_MS);
#if CCTV_SERIAL_VERBOSE
  Serial.flush();
#endif
  bool ethDhcpOk = cctv_net_wait_eth_dhcp(CCTV_ETH_BOOT_DHCP_WAIT_MS);
  cctv_net_apply_fallback_dns();
  if (ethDhcpOk) {
    SDBGf("[ETH] IPv4 OK  IP: %s\n", cctv_primary_local_ip().toString().c_str());
  } else {
    SDBGln(F("[ETH] No DHCP in wait window — web UI still starts (use WiFi IP or fix LAN DHCP)."));
#if CCTV_SERIAL_VERBOSE
    cctv_net_print_eth_diagnostics(Serial);
#endif
    SDBGln(F("[ETH] Hint: link LED only proves L1/L2. DHCP = router/VLAN."));
    SDBGln(F("[ETH] Direct cable: Serial/Device control → ethstatic  (laptop manual 10.0.0.x)"));
    SDBGln(F("[ETH] Organisation LAN: ethdhcp then devstatus in web console."));
  }
#if CCTV_SERIAL_VERBOSE
  Serial.flush();
#endif

#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  // Do not auto-retry mini-DHCP at boot: some ETH netif builds reject DHCPS and spam logs.
#endif

  // HTTP server: control on Core 0, stream on Core 1 (set inside startCameraServer)
  if (!startCameraServer()) {
    Serial.println(F("[HTTP] FATAL: httpd_start failed — web UI unavailable (check heap / port 80)"));
    Serial.flush();
  }

  // ── v2.0: start background tasks after HTTP is up ────────────────────
  cctv_oled_start_task();       // OLED page-rotation on Core 1
  cctv_mqtt_start_task();       // MQTT reconnect + telemetry push on Core 0

  {
    esp_timer_handle_t ntp_timer = nullptr;
    const esp_timer_create_args_t ntp_args = {
      .callback = &ntp_housekeeping_timer_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "ntp_poll",
      .skip_unhandled_events = true,
    };
    if (esp_timer_create(&ntp_args, &ntp_timer) == ESP_OK) {
      (void) esp_timer_start_periodic(ntp_timer, 250000);
    }
  }
  startClockSync();
  // HTTP world-clock sync deferred — ntp_housekeeping_timer_cb will retry every 8s.
  // Blocking at boot wastes time on local-only LANs (no internet → HTTP timeout).

  // SD init on Core 0 — PSRAM stack frees ~8 KB internal (OPI PSRAM builds)
  (void) cctv_task_create_caps(
    sdInitBackgroundTask,
    "SDInit",
    8 * 1024,
    nullptr,
    2,
    0,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    &sdInitTaskHandle);

  // Continuous AVI recording on Core 1 — IDLE1 is not WDT-monitored by default
  // on ESP32-S3 Arduino, so heavy OSD+SD writes won't trigger task_wdt.
  // HTTPD (Core 1, prio idle+5=6) still preempts Recorder (prio 3) for web requests.
  (void) cctv_task_create_caps(
    recordingTask,
    "Recorder",
    20 * 1024,
    nullptr,
    3,
    1,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
    &recordingTaskHandle);

  g_cameraStatusMessage = "Ready";
  SDBGln(F("[BOOT] Camera + HTTP pipeline ready"));
  SDBGln(F("[HTTP] Browser: use http://  only  (not https). Port 80."));
  {
    const String ethIp = cctv_eth_ip_string();
    const String wfIp =
      (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String();
    if (ethIp.length()) {
      SDBGf("[HTTP] Try Ethernet: http://%s/\n", ethIp.c_str());
    } else {
      SDBGln(F("[HTTP] Ethernet: (no IPv4 yet)"));
    }
    if (wfIp.length()) {
      SDBGf("[HTTP] Try WiFi:    http://%s/\n", wfIp.c_str());
    } else {
      SDBGln(F("[HTTP] WiFi STA: (not connected)"));
    }
  }
  if (ethDhcpOk || WiFi.status() == WL_CONNECTED) {
    SDBGf("[HTTP] Primary URL: http://%s/\n", cctv_primary_local_ip().toString().c_str());
  } else {
    SDBGln(F("[Net] No IP yet — open web after cable/router DHCP or when Wi‑Fi connects."));
  }
#if CCTV_SERIAL_VERBOSE
  Serial.flush();
#endif

#if CONFIG_ESP_WIFI_ENABLED
  // MUST use internal RAM stack: try_connect_profiles calls commit_all_slots_to_nvs
  // which does SPI flash (Preferences). PSRAM stacks crash during cache-disable.
  (void) cctv_task_create_caps(
      wifiStationBackgroundTask,
      "WiFiSTA",
      8192,
      nullptr,
      2,
      1,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
      nullptr);
#endif

  if (!ethDhcpOk) {
    // MUST use internal RAM stack: cctv_time_kick_sync_if_needed may call
    // cctv_time_nvs_snapshot_wall (Preferences / SPI flash). PSRAM stack crashes.
    (void) cctv_task_create_caps(
      [](void *) {
        for (int n = 0; n < 90; ++n) {
          vTaskDelay(2000 / portTICK_PERIOD_MS);
          if (cctv_eth_has_ip()) {
            cctv_time_kick_sync_if_needed(true);
            SDBGf("[ETH] DHCP now OK  IP: %s\n", cctv_primary_local_ip().toString().c_str());
            break;
          }
        }
        vTaskDelete(NULL);
      },
      "EthDhcpPoll",
      4096,
      nullptr,
      1,
      0,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
      nullptr);
  }

#if CCTV_ENABLE_SERIAL_CONSOLE
  // Internal RAM stack: NVS / Preferences (WiFi save, ethstatic, etc.) require a
  // cache-safe stack — PSRAM stacks trip spi_flash_disable_interrupts… assert.
  (void) cctv_task_create_caps(
    serialConsoleTask,
    "SerCon",
    8 * 1024,
    nullptr,
    1,
    0,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
    nullptr);
#elif CCTV_ENABLE_ETH_SERIAL_MINI
  (void) cctv_task_create_caps(
    ethSerialMiniTask,
    "EthSer",
    4 * 1024,
    nullptr,
    1,
    0,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
    nullptr);
#endif

  (void) cctv_task_create_caps(
    [](void *) {
      vTaskDelay(500 / portTICK_PERIOD_MS);
      esp_wifi_set_ps(WIFI_PS_NONE);
      vTaskDelete(NULL);
    },
    "WifiPS",
    2048,
    nullptr,
    5,
    0,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    nullptr);
}

void loop() {
  // Nothing runs here — all work is in FreeRTOS tasks and httpd callbacks
  // Suspend permanently so loop task never wastes scheduler time
  vTaskSuspend(NULL);
}

// ─────────────────────────────────────────────────────────────────────────
// Optional serial console (CCTV_ENABLE_SERIAL_CONSOLE) — same parser as web
// /api/console. NTP housekeeping runs on esp_timer when serial is off.
// ─────────────────────────────────────────────────────────────────────────
void serialConsoleTask(void *parameter) {
  (void)parameter;
#if CCTV_SERIAL_VERBOSE
  DBG_PORT.println(F("\n[CON] Serial console (verbose). Primary control: web Device control."));
#endif
  String line;
  for (;;) {
    if (DBG_PORT.available()) {
      const char c = (char)DBG_PORT.read();
      if (c == '\n' || c == '\r') {
        line.trim();
        if (line.length() > 0) {
          cctv_web_control_dispatch(line, DBG_PORT);
          line = "";
        }
      } else if (line.length() < 240) {
        line += c;
      }
    } else {
      vTaskDelay(10 / portTICK_PERIOD_MS);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Ethernet-only serial (CCTV_ENABLE_ETH_SERIAL_MINI) — same logic as web
// /api/console for devstatus / ethdhcp / ethstatic only. No WiFi/time/etc.
// ─────────────────────────────────────────────────────────────────────────
static bool eth_serial_line_is_ethernet_cmd(const String &line) {
  if (line.equalsIgnoreCase("devstatus")) {
    return true;
  }
  if (line.equalsIgnoreCase("ethdhcp")) {
    return true;
  }
  if (line.equalsIgnoreCase("ethstatic")) {
    return true;
  }
  if (line.length() > 10 && line.substring(0, 10).equalsIgnoreCase("ethstatic ")) {
    return true;
  }
  return false;
}

void ethSerialMiniTask(void *parameter) {
  (void)parameter;
  // One boot line so Serial Monitor users see Ethernet-only mode (rest of boot may be quiet).
  Serial.println(F("[ETH-SER] devstatus | ethdhcp | ethstatic (defaults) | ethstatic <ip> <mask> <gw> [dns] | help"));
  String line;
  for (;;) {
    if (DBG_PORT.available()) {
      const char c = (char)DBG_PORT.read();
      if (c == '\n' || c == '\r') {
        line.trim();
        if (line.length() > 0) {
          if (line.equalsIgnoreCase("help") || line.equalsIgnoreCase("?") ||
              line.equalsIgnoreCase("eth")) {
            DBG_PORT.println(
              F("[ETH-SER] devstatus | ethdhcp | ethstatic | ethstatic <ip> <mask> <gw> [dns]"));
          } else if (eth_serial_line_is_ethernet_cmd(line)) {
            cctv_web_control_dispatch(line, DBG_PORT);
          }
          line = "";
        }
      } else if (line.length() < 200) {
        line += c;
      }
    } else {
      vTaskDelay(20 / portTICK_PERIOD_MS);
    }
  }
}
