#include "cctv_time_sync.h"

#include "cctv_net.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

bool cctvHttpTimeSync(const char *url) {
  if (url == nullptr || url[0] == '\0') {
    return false;
  }
  if (!cctv_has_ip_for_internet()) {
    return false;
  }
  // Ensure DNS is set even if DHCP left it empty (common on some LANs/VLANs).
  cctv_net_apply_fallback_dns();

  HTTPClient http;
  http.setTimeout(4000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(url)) {
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  const char *s = body.c_str();
  const char *key = strstr(s, "\"unixtime\"");
  if (!key) {
    key = strstr(s, "\"unixTime\"");
  }
  if (!key) {
    key = strstr(s, "\"UnixTime\"");
  }
  if (!key) {
    key = strstr(s, "\"epoch\"");
  }
  if (!key) {
    key = strstr(s, "\"Epoch\"");
  }
  if (!key) {
    return false;
  }
  key = strchr(key, ':');
  if (!key) {
    return false;
  }
  while (*key == ':' || *key == ' ' || *key == '\t') {
    key++;
  }
  const unsigned long long u = strtoull(key, nullptr, 10);
  if (u < 1700000000ULL || u > 4000000000ULL) {
    return false;
  }

  struct timeval tv = {};
  tv.tv_sec = (time_t)u;
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) {
    return false;
  }

  return true;
}

extern void startClockSync(void);
extern void tryHttpWorldClockSync(void);

bool cctv_wall_clock_sane(void) {
  struct tm t;
  if (!getLocalTime(&t, 10)) {
    return false;
  }
  return (t.tm_year + 1900) >= 2020;
}

void cctv_time_nvs_snapshot_wall(void) {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0) {
    return;
  }
  Preferences p;
  if (!p.begin("cctv", false)) {
    return;
  }
  p.putULong64("wallEp", (uint64_t)tv.tv_sec);
  p.end();
}

void cctv_time_kick_sync_if_needed(bool force) {
  static uint32_t s_lastKickMs = 0;
  if (!cctv_has_ip_for_internet()) {
    return;
  }
  if (cctv_wall_clock_sane()) {
    return;
  }
  const uint32_t m = millis();
  if (!force && (m - s_lastKickMs < 8000u)) {
    return;
  }
  s_lastKickMs = m;
  cctv_net_apply_fallback_dns();
  startClockSync();
  tryHttpWorldClockSync();
}
