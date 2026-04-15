#include "cctv_web_control.h"

#include "board_config.h"
#include "cctv_devstatus.h"
#include "cctv_net.h"
#include "cctv_platform.h"
#include "cctv_time_sync.h"
#include "cctv_wifi_profiles.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern String g_wifiStatus;
extern String g_wifiSsid;
extern String g_wifiPass;
extern bool g_wifiEnterprise;
extern String g_wifiIdentity;
extern String g_wifiEapPass;
extern String g_timeHttpUrl;
extern volatile bool g_wifiAutoConnect;

extern void saveWifiConfig();
extern void clearWifiConfig();
extern void saveTimeHttpConfig();
extern void saveWifiAutoConnect();

static void apply_eth_static_addrs(Print &out,
                                   const IPAddress &ip,
                                   const IPAddress &mask,
                                   const IPAddress &gw,
                                   const IPAddress &dns) {
  IPAddress dns_use = dns;
  if ((uint32_t)dns_use == 0u) {
    dns_use = gw;
  }
  if (!cctv_eth_save_static_nvs(ip, mask, gw, dns_use)) {
    out.println(F("[CON] NVS save failed"));
    return;
  }
  if (!cctv_eth_apply_static_ipv4(ip, mask, gw, dns_use)) {
    out.println(F("[CON] ETH.config failed"));
    return;
  }
  out.printf("[CON] Static saved. Open http://%s/\n", ip.toString().c_str());
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  (void) cctv_eth_start_mini_dhcp_for_static_subnet(out);
#endif
}

static const char *wifi_status_str(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:
      return "SSID not found";
    case WL_CONNECT_FAILED:
      return "Wrong password";
    case WL_CONNECTION_LOST:
      return "Connection lost";
    case WL_DISCONNECTED:
      return "Disconnected";
    case WL_IDLE_STATUS:
      return "Idle";
    case WL_CONNECTED:
      return "Connected";
    default:
      return "Unknown";
  }
}

static String s_scan_ssid[20];
static int s_scan_count = 0;

void cctv_web_control_dispatch(const String &line, Print &out) {
  if (line.length() == 0) {
    return;
  }

  // --- WiFi auto-connect control ---
  if (line.equalsIgnoreCase("autowifi on")) {
    g_wifiAutoConnect = true;
    saveWifiAutoConnect();
    out.println(F("[CON] WiFi auto-connect ENABLED (device will auto-connect if not connected)"));
    return;
  }
  if (line.equalsIgnoreCase("autowifi off")) {
    g_wifiAutoConnect = false;
    saveWifiAutoConnect();
    out.println(F("[CON] WiFi auto-connect DISABLED (device will stay idle until manual scan/connect)"));
    return;
  }
  if (line.equalsIgnoreCase("wifiscan")) {
    cctv_wifi_lock();
    int maxTries = 3;
    int tryCount = 0;
    bool found = false;
    while (tryCount < maxTries && !found) {
      out.printf("[CON] WiFi scan try %d...\n", tryCount + 1);
      WiFi.disconnect(false);
      vTaskDelay(300 / portTICK_PERIOD_MS);
      out.println(F("[CON] Please wait ~3s..."));
      s_scan_count = WiFi.scanNetworks(false, true);
      if (s_scan_count > 0) {
        found = true;
        break;
      }
      tryCount++;
      if (s_scan_count == WIFI_SCAN_FAILED || s_scan_count < 0) {
        out.println(F("[CON] Scan failed — retrying..."));
        vTaskDelay(1000 / portTICK_PERIOD_MS);
      }
    }
    if (!found) {
      out.println(F("[CON] No networks found after 3 tries. Waiting 5 minutes before next scan..."));
      cctv_wifi_unlock();
      vTaskDelay(300000 / portTICK_PERIOD_MS); // 5 minutes
      // After 5 min, allow scan again (user can re-issue command)
      return;
    }
    if (s_scan_count > 20) {
      s_scan_count = 20;
    }
    out.printf("[CON] Found %d network(s):\n", s_scan_count);
    for (int i = 0; i < s_scan_count; i++) {
      s_scan_ssid[i] = WiFi.SSID(i);
      wifi_auth_mode_t auth = WiFi.encryptionType(i);
      const char *authStr = "OPEN";
      if (auth == WIFI_AUTH_WPA2_ENTERPRISE) {
        authStr = "WPA2-ENT";
      } else if (auth == WIFI_AUTH_WPA3_ENTERPRISE) {
        authStr = "WPA3-ENT";
      } else if (auth == WIFI_AUTH_WPA3_PSK) {
        authStr = "WPA3";
      } else if (auth == WIFI_AUTH_WPA2_PSK) {
        authStr = "WPA2";
      } else if (auth == WIFI_AUTH_WPA_WPA2_PSK) {
        authStr = "WPA/WPA2";
      } else if (auth == WIFI_AUTH_WPA_PSK) {
        authStr = "WPA";
      } else if (auth != WIFI_AUTH_OPEN) {
        authStr = "OTHER";
      }
      out.printf("  [%2d] %-32s  %4d dBm  %-10s  ch%d\n",
                 i + 1,
                 WiFi.SSID(i).c_str(),
                 WiFi.RSSI(i),
                 authStr,
                 WiFi.channel(i));
    }
    out.println(F("[CON] Web UI: pick SSID from scan, or use: <n> norm pass:<pw> / <n> ent user:<id> pass:<pw>"));
    WiFi.scanDelete();
    cctv_wifi_unlock();
    return;
  }

  if (line.equalsIgnoreCase("wifistatus")) {
    out.printf("[WiFi] Status: %s\n", g_wifiStatus.c_str());
    out.printf("[WiFi] State:  %s (%d)\n", wifi_status_str(WiFi.status()), (int)WiFi.status());
    if (WiFi.status() == WL_CONNECTED) {
      out.printf("[WiFi] IP: %s  RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    out.printf("[ETH] link=%s dhcp=%s  primary IP: %s\n",
               cctv_eth_link_up() ? "up" : "down",
               cctv_eth_has_ip() ? "yes" : "no",
               cctv_primary_local_ip().toString().c_str());
    return;
  }

  if (line.equalsIgnoreCase("devstatus")) {
    cctv_print_devstatus(out);
    return;
  }

  if (line.equalsIgnoreCase("ethdhcp")) {
    cctv_eth_clear_static_nvs();
    cctv_eth_restart_dhcp_client();
    out.println(F("[CON] Static cleared; Ethernet DHCP client restarted (router/org LAN)."));
    return;
  }

  if (line.equalsIgnoreCase("ethstatic")) {
    IPAddress ip;
    IPAddress mask;
    IPAddress gw;
    IPAddress dns;
    if (!ip.fromString(CCTV_ETH_STATIC_DEFAULT_IP) || !mask.fromString(CCTV_ETH_STATIC_DEFAULT_MASK) ||
        !gw.fromString(CCTV_ETH_STATIC_DEFAULT_GW) || !dns.fromString(CCTV_ETH_STATIC_DEFAULT_DNS)) {
      out.println(F("[CON] Default static parse error (check board_config.h)."));
      return;
    }
    out.printf(
      "[CON] Default static: %s  %s  gw %s  dns %s\n",
      CCTV_ETH_STATIC_DEFAULT_IP,
      CCTV_ETH_STATIC_DEFAULT_MASK,
      CCTV_ETH_STATIC_DEFAULT_GW,
      CCTV_ETH_STATIC_DEFAULT_DNS);
    apply_eth_static_addrs(out, ip, mask, gw, dns);
    return;
  }

  if (line.length() > 10 && line.substring(0, 10).equalsIgnoreCase("ethstatic ")) {
    String r = line.substring(10);
    r.trim();
    const int p1 = r.indexOf(' ');
    const int p2 = r.indexOf(' ', p1 + 1);
    if (p1 < 1 || p2 < p1 + 2) {
      out.println(F("[CON] Usage: ethstatic   OR   ethstatic ip mask gw [dns]"));
    } else {
      const int p3 = r.indexOf(' ', p2 + 1);
      String sIp = r.substring(0, p1);
      String sMk = r.substring(p1 + 1, p2);
      String sGw = (p3 > p2) ? r.substring(p2 + 1, p3) : r.substring(p2 + 1);
      String sDns = (p3 > p2) ? r.substring(p3 + 1) : String();
      sIp.trim();
      sMk.trim();
      sGw.trim();
      sDns.trim();
      IPAddress ip;
      IPAddress mask;
      IPAddress gw;
      IPAddress dns(0, 0, 0, 0);
      if (!ip.fromString(sIp) || !mask.fromString(sMk) || !gw.fromString(sGw)) {
        out.println(F("[CON] Invalid ip/mask/gw"));
      } else {
        if (sDns.length() > 0 && !dns.fromString(sDns)) {
          out.println(F("[CON] Invalid DNS — using gateway"));
          dns = IPAddress(0, 0, 0, 0);
        }
        apply_eth_static_addrs(out, ip, mask, gw, dns);
      }
    }
    return;
  }

  if (line.equalsIgnoreCase("timeurl")) {
    if (g_timeHttpUrl.isEmpty()) {
      out.println(F("[CON] timeurl: (disabled)"));
    } else {
      out.printf("[CON] timeurl: %s\n", g_timeHttpUrl.c_str());
    }
    out.println(F("[CON] Set: timeurl http://... | timeurl off | timeurl default"));
    return;
  }

  if (line.length() > 8 && line.substring(0, 8).equalsIgnoreCase("timeurl ")) {
    String arg = line.substring(8);
    arg.trim();
    if (arg.equalsIgnoreCase("off")) {
      g_timeHttpUrl = "";
      saveTimeHttpConfig();
      out.println(F("[CON] timeurl off (saved)."));
    } else if (arg.equalsIgnoreCase("default")) {
      g_timeHttpUrl = String(CCTV_WORLD_TIME_HTTP_URL);
      saveTimeHttpConfig();
      out.printf("[CON] timeurl default: %s\n", g_timeHttpUrl.c_str());
      cctv_time_kick_sync_if_needed(true);
    } else if (arg.length() < 8) {
      out.println(F("[CON] URL too short."));
    } else {
      g_timeHttpUrl = arg;
      saveTimeHttpConfig();
      out.printf("[CON] timeurl saved: %s\n", g_timeHttpUrl.c_str());
      cctv_time_kick_sync_if_needed(true);
    }
    return;
  }

  if (line.equalsIgnoreCase("clearwifi")) {
    cctv_wifi_lock();
    clearWifiConfig();
    g_wifiStatus = "Not connected";
    cctv_wifi_unlock();
    out.println(F("[CON] WiFi cleared."));
    return;
  }

  if (line.equalsIgnoreCase("wifiprofiles")) {
    cctv_wifi_print_profiles(out);
    return;
  }

  if (line.length() >= 7 && line.substring(0, 7).equalsIgnoreCase("wifidel")) {
    String arg = line.substring(7);
    arg.trim();
    if (arg.length() != 1 || !isdigit((unsigned char)arg[0])) {
      out.println(F("[CON] Usage: wifidel 0  (or 1 / 2)"));
      return;
    }
    const int n = arg[0] - '0';
    if (n < 0 || n > 2) {
      out.println(F("[CON] Slot must be 0, 1, or 2."));
      return;
    }
    cctv_wifi_lock();
    cctv_wifi_delete_slot((uint8_t)n);
    cctv_wifi_apply_preferred_or_first_globals();
    cctv_wifi_unlock();
    out.printf("[CON] WiFi profile slot %d cleared.\n", n);
    return;
  }

  if (line.equalsIgnoreCase("reboot")) {
    out.println(F("[CON] Reboot scheduled..."));
    xTaskCreate(
      [](void *) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        esp_restart();
      },
      "RebootCmd",
      3072,
      nullptr,
      5,
      nullptr);
    return;
  }

  int numEnd = 0;
  while (numEnd < (int)line.length() && isDigit((unsigned char)line[numEnd])) {
    numEnd++;
  }
  if (numEnd > 0 && numEnd < (int)line.length() && line[numEnd] == ' ') {
    const int spaceIdx = numEnd;
    const int ssidNum = line.substring(0, spaceIdx).toInt();
    if (ssidNum < 1 || ssidNum > s_scan_count || s_scan_count == 0) {
      out.printf("[CON] Invalid SSID number %d (run wifiscan first)\n", ssidNum);
      return;
    }
    String rest = line.substring(spaceIdx + 1);
    rest.trim();
    g_wifiSsid = s_scan_ssid[ssidNum - 1];

    if (rest.startsWith("norm ")) {
      int pi = rest.indexOf("pass:");
      if (pi < 0) {
        out.println(F("[CON] Syntax: <n> norm pass:<password>"));
        return;
      }
      g_wifiPass = rest.substring(pi + 5);
      g_wifiEnterprise = false;
      g_wifiIdentity = "";
      g_wifiEapPass = "";
      g_wifiPass.trim();
    } else if (rest.startsWith("ent ")) {
      int ui = rest.indexOf("user:");
      int pi = rest.indexOf("pass:");
      if (ui < 0 || pi < 0 || pi <= ui + 5) {
        out.println(F("[CON] Syntax: <n> ent user:<id> pass:<password>"));
        return;
      }
      String identity = rest.substring(ui + 5, pi);
      identity.trim();
      String eapPass = rest.substring(pi + 5);
      eapPass.trim();
      if (identity.length() == 0 || eapPass.length() == 0) {
        out.println(F("[CON] Enterprise needs user + password."));
        return;
      }
      g_wifiPass = "";
      g_wifiIdentity = identity;
      g_wifiEapPass = eapPass;
      g_wifiEnterprise = true;
    } else {
      out.println(F("[CON] Use norm or ent"));
      return;
    }
    cctv_wifi_lock();
    saveWifiConfig();
    out.println(F("[CON] Connecting (saved profiles / failover)..."));
    const bool ok = cctv_wifi_try_connect_profiles(20000);
    cctv_wifi_unlock();
    if (ok) {
      out.printf("[CON] Connected IP: %s\n", WiFi.localIP().toString().c_str());
      cctv_time_kick_sync_if_needed(true);
    } else {
      out.printf("[CON] Failed: %s\n", g_wifiStatus.c_str());
    }
    return;
  }

  out.printf("[CON] Unknown: %s\n", line.c_str());
}
