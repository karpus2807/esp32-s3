#include "cctv_net.h"

#include "board_config.h"
#include <Arduino.h>

#if CCTV_SERIAL_VERBOSE
#define CETH_LOG(...) Serial.printf(__VA_ARGS__)
#define CETH_LOG_LN(x) Serial.println(x)
#else
#define CETH_LOG(...) ((void)0)
#define CETH_LOG_LN(x) ((void)0)
#endif

#if !CCTV_SERIAL_VERBOSE
struct EthNullPrint : public Print {
  size_t write(uint8_t) override { return 1; }
};
static EthNullPrint s_eth_null_print;
#endif
#include <Preferences.h>
#include <sdkconfig.h>
#include <WiFi.h>
#include <driver/spi_master.h>
#include <esp_netif.h>

#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
#include <ETH.h>
#include <apps/dhcpserver/dhcpserver.h>
#endif

// SPI2 is common; SPI3 avoids rare clashes with other SPI2 users on some boards.
#ifndef CCTV_ETH_SPI_HOST
#define CCTV_ETH_SPI_HOST SPI3_HOST
#endif

#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
static bool s_eth_started = false;
static bool s_eth_mini_dhcp = false;
static bool s_eth_mini_dhcp_unsupported = false;
static bool s_eth_mini_dhcp_warned = false;

static void eth_hw_reset_pulse() {
#if CCTV_ETH_PIN_RST >= 0
  pinMode(CCTV_ETH_PIN_RST, OUTPUT);
  digitalWrite(CCTV_ETH_PIN_RST, LOW);
  delay(5);
  digitalWrite(CCTV_ETH_PIN_RST, HIGH);
  delay(20);
#endif
}

static bool eth_begin_once(int32_t phy_addr, spi_host_device_t host) {
  eth_hw_reset_pulse();
  return ETH.begin(
    ETH_PHY_W5500,
    phy_addr,
    CCTV_ETH_PIN_CS,
    -1,
#if CCTV_ETH_PIN_RST >= 0
    CCTV_ETH_PIN_RST,
#else
    -1,
#endif
    host,
    CCTV_ETH_PIN_SCK,
    CCTV_ETH_PIN_MISO,
    CCTV_ETH_PIN_MOSI,
    CCTV_ETH_SPI_MHZ
  );
}
#endif

#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
static void cctv_eth_try_apply_static_from_nvs();
bool cctv_eth_start_mini_dhcp_for_static_subnet(Print &out);
#endif

void cctv_net_init_ethernet() {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  if (s_eth_started) {
    return;
  }

  CETH_LOG(
    "[ETH] W5500  CS=%d SCK=%d MOSI=%d MISO=%d  host=%d  %uMHz  phy=%d  RSTpin=%d\n",
    CCTV_ETH_PIN_CS,
    CCTV_ETH_PIN_SCK,
    CCTV_ETH_PIN_MOSI,
    CCTV_ETH_PIN_MISO,
    (int)CCTV_ETH_SPI_HOST,
    (unsigned)CCTV_ETH_SPI_MHZ,
    CCTV_ETH_PHY_ADDR,
    CCTV_ETH_PIN_RST);

  const struct {
    int32_t phy;
    spi_host_device_t host;
    const char *note;
  } kTries[] = {
    {CCTV_ETH_PHY_ADDR, CCTV_ETH_SPI_HOST, "default"},
    {1, CCTV_ETH_SPI_HOST, "phy=1"},
#if (CCTV_ETH_SPI_HOST != SPI2_HOST)
    {CCTV_ETH_PHY_ADDR, SPI2_HOST, "SPI2_HOST"},
    {1, SPI2_HOST, "phy=1 SPI2_HOST"},
#endif
  };

  for (size_t i = 0; i < sizeof(kTries) / sizeof(kTries[0]); ++i) {
    if (s_eth_started) {
      break;
    }
    if (i > 0) {
      CETH_LOG("[ETH] retry: %s\n", kTries[i].note);
      ETH.end();
      delay(150);
    }
    s_eth_started = eth_begin_once(kTries[i].phy, kTries[i].host);
  }

  if (s_eth_started) {
    CETH_LOG_LN(F("[ETH] Driver OK — plug RJ45 to router; link LED should blink on jack"));
    cctv_eth_try_apply_static_from_nvs();
  } else {
    CETH_LOG_LN(F("[ETH] FAILED all retries."));
    CETH_LOG_LN(F("[ETH] Hardware: 3V3+GND common; CS/SCK/MOSI/MISO correct; RST must be HIGH (10k to 3V3 or wire GPIO41->RST)"));
  }
#endif
}

#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
static bool apply_eth_static_ipv4_impl(const IPAddress &ip, const IPAddress &mask, const IPAddress &gw, const IPAddress &dns) {
  if (!s_eth_started) {
    return false;
  }
  IPAddress dns1 = dns;
  if ((uint32_t)dns1 == 0u) {
    dns1 = gw;
  }
  return ETH.config(ip, gw, mask, dns1, IPAddress(0, 0, 0, 0));
}

static void cctv_eth_try_apply_static_from_nvs() {
  Preferences p;
  if (!p.begin("cctv", true)) {
    return;
  }
  const uint8_t en = p.getUChar("ethSt", 0);
  const String sIp = p.getString("ethIP", "");
  const String sMk = p.getString("ethMk", "");
  const String sGw = p.getString("ethGw", "");
  const String sDns = p.getString("ethDns", "");
  p.end();
  if (en != 1) {
    return;
  }
  IPAddress ip;
  IPAddress mask;
  IPAddress gw;
  IPAddress dns(0, 0, 0, 0);
  if (!ip.fromString(sIp) || !mask.fromString(sMk) || !gw.fromString(sGw)) {
    CETH_LOG_LN(F("[ETH] NVS static invalid (ethIP/ethMk/ethGw) — fix with ethstatic command"));
    return;
  }
  if (sDns.length() > 0 && !dns.fromString(sDns)) {
    CETH_LOG_LN(F("[ETH] NVS ethDns invalid — using gateway as DNS"));
    dns = gw;
  }
  const IPAddress dns_use = (sDns.length() > 0) ? dns : gw;
  if (!apply_eth_static_ipv4_impl(ip, mask, gw, dns_use)) {
    CETH_LOG_LN(F("[ETH] NVS static ETH.config() failed"));
    return;
  }
  CETH_LOG("[ETH] Using static IPv4 from NVS: %s\n", ETH.localIP().toString().c_str());
#if CCTV_SERIAL_VERBOSE
  (void) cctv_eth_start_mini_dhcp_for_static_subnet(Serial);
#else
  (void) cctv_eth_start_mini_dhcp_for_static_subnet(s_eth_null_print);
#endif
}
#endif

bool cctv_eth_nvs_static_enabled() {
  Preferences p;
  if (!p.begin("cctv", true)) {
    return false;
  }
  const uint8_t en = p.getUChar("ethSt", 0);
  p.end();
  return en == 1;
}

bool cctv_eth_save_static_nvs(const IPAddress &ip, const IPAddress &mask, const IPAddress &gw, const IPAddress &dns) {
  Preferences p;
  if (!p.begin("cctv", false)) {
    return false;
  }
  p.putUChar("ethSt", 1);
  p.putString("ethIP", ip.toString());
  p.putString("ethMk", mask.toString());
  p.putString("ethGw", gw.toString());
  if ((uint32_t)dns != 0u) {
    p.putString("ethDns", dns.toString());
  } else {
    p.remove("ethDns");
  }
  p.end();
  return true;
}

void cctv_eth_clear_static_nvs() {
  Preferences p;
  if (!p.begin("cctv", false)) {
    return;
  }
  p.remove("ethSt");
  p.remove("ethIP");
  p.remove("ethMk");
  p.remove("ethGw");
  p.remove("ethDns");
  p.remove("ethDhcpS");
  p.remove("ethPcLnk");
  p.end();
}

bool cctv_eth_apply_static_ipv4(const IPAddress &ip, const IPAddress &mask, const IPAddress &gw, const IPAddress &dns) {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  return apply_eth_static_ipv4_impl(ip, mask, gw, dns);
#else
  (void)ip;
  (void)mask;
  (void)gw;
  (void)dns;
  return false;
#endif
}

void cctv_eth_restart_dhcp_client() {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  if (!s_eth_started) {
    return;
  }
  esp_netif_t *n = ETH.netif();
  if (n) {
    (void)esp_netif_dhcps_stop(n);
    (void)esp_netif_dhcpc_stop(n);
  }
  s_eth_mini_dhcp = false;
  (void)ETH.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
  if (n) {
    (void)esp_netif_dhcpc_start(n);
  }
  s_eth_mini_dhcp_unsupported = false;
  s_eth_mini_dhcp_warned = false;
#endif
}

bool cctv_eth_mini_dhcp_active() {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  return s_eth_mini_dhcp;
#else
  return false;
#endif
}

bool cctv_eth_start_mini_dhcp_for_static_subnet(Print &out) {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  out.println(F("[ETH] mini-DHCP: disabled for this firmware build (ETH netif DHCPS not supported)."));
  out.println(F("[ETH] Set laptop manual IPv4 in same /24 as board, then open http://<board-ip>/"));
  s_eth_mini_dhcp_unsupported = true;
  s_eth_mini_dhcp_warned = true;
  s_eth_mini_dhcp = false;
  return false;
#else
  (void)out;
  return false;
#endif
}

bool cctv_net_wait_eth_dhcp(uint32_t timeout_ms) {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  if (!s_eth_started) {
    return false;
  }
  if (cctv_eth_nvs_static_enabled()) {
    CETH_LOG_LN(F("[ETH] Waiting for cable + static IPv4 from NVS (ethstatic mode)..."));
  } else {
    CETH_LOG_LN(F("[ETH] Waiting for cable + DHCP from router (ethdhcp / no static in NVS)..."));
  }
  const uint32_t t0 = millis();
  uint32_t lastLogMs = t0;
  while (millis() - t0 < timeout_ms) {
    if (s_eth_started && ETH.linkUp() && (uint32_t)ETH.localIP() != 0u) {
      return true;
    }
    const uint32_t now = millis();
    if ((now - lastLogMs) >= 2000u) {
      lastLogMs = now;
      CETH_LOG(
        "[ETH]   %4u s  link=%s  IPv4=%s\n",
        (unsigned)((now - t0) / 1000u),
        ETH.linkUp() ? "UP" : "DOWN",
        ETH.localIP().toString().c_str());
    }
    delay(200);
  }
  return s_eth_started && ETH.linkUp() && (uint32_t)ETH.localIP() != 0u;
#else
  (void)timeout_ms;
  return false;
#endif
}

bool cctv_eth_link_up() {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  if (s_eth_started) {
    return ETH.linkUp();
  }
#endif
  return false;
}

bool cctv_eth_has_ip() {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  if (s_eth_started && ETH.linkUp()) {
    return (uint32_t)ETH.localIP() != 0u;
  }
#endif
  return false;
}

IPAddress cctv_primary_local_ip() {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  if (s_eth_started && ETH.linkUp()) {
    const IPAddress ip = ETH.localIP();
    if (ip != IPAddress(0, 0, 0, 0)) {
      return ip;
    }
  }
#endif
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP();
  }
  return IPAddress(0, 0, 0, 0);
}

String cctv_eth_ip_string() {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  if (s_eth_started && ETH.linkUp() && (uint32_t)ETH.localIP() != 0u) {
    return ETH.localIP().toString();
  }
#endif
  return String();
}

String cctv_wifi_ip_string() {
  if ((uint32_t)WiFi.localIP() != 0u) {
    return WiFi.localIP().toString();
  }
  return String();
}

bool cctv_has_ip_for_internet() {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  if (s_eth_started && ETH.linkUp()) {
    const IPAddress ip = ETH.localIP();
    // 169.254.x.x = APIPA / link-local — no guaranteed default route; use WiFi for NTP if up.
    if ((uint32_t)ip != 0u && !(ip[0] == 169 && ip[1] == 254)) {
      return true;
    }
  }
#endif
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  const IPAddress wip = WiFi.localIP();
  if ((uint32_t)wip == 0u) {
    return false;
  }
  if (wip[0] == 169 && wip[1] == 254) {
    return false;
  }
  return true;
}

static void cctv_net_dns_fallback_one(esp_netif_t *n) {
  if (!n) {
    return;
  }
  esp_netif_dns_info_t cur{};
  if (esp_netif_get_dns_info(n, ESP_NETIF_DNS_MAIN, &cur) != ESP_OK) {
    return;
  }
  if (cur.ip.type == ESP_IPADDR_TYPE_V4 && cur.ip.u_addr.ip4.addr != 0) {
    return;
  }
  esp_netif_dns_info_t d{};
  d.ip.type = ESP_IPADDR_TYPE_V4;
  if (esp_netif_str_to_ip4("8.8.8.8", &d.ip.u_addr.ip4) == ESP_OK) {
    (void)esp_netif_set_dns_info(n, ESP_NETIF_DNS_MAIN, &d);
  }
  if (esp_netif_str_to_ip4("1.1.1.1", &d.ip.u_addr.ip4) == ESP_OK) {
    (void)esp_netif_set_dns_info(n, ESP_NETIF_DNS_BACKUP, &d);
  }
}

void cctv_net_apply_fallback_dns(void) {
#if CONFIG_ESP_WIFI_ENABLED
  if (WiFi.status() == WL_CONNECTED) {
    cctv_net_dns_fallback_one(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
  }
#endif
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  if (s_eth_started && ETH.linkUp() && (uint32_t)ETH.localIP() != 0u) {
    cctv_net_dns_fallback_one(esp_netif_get_handle_from_ifkey("ETH_DEF"));
  }
#endif
}

void cctv_net_print_eth_diagnostics(Print &out) {
#if CCTV_USE_ETH_W5500 && CONFIG_ETH_ENABLED
  if (!s_eth_started) {
    out.println(F("[ETH] Driver not started."));
    return;
  }
  const bool staticNv = cctv_eth_nvs_static_enabled();
  const bool mini = cctv_eth_mini_dhcp_active();
  if (staticNv && mini) {
    out.println(F("[ETH] Mode: direct cable — saved static IPv4 + mini-DHCP for laptop"));
  } else if (staticNv) {
    out.println(F("[ETH] Mode: static IPv4 saved (mini-DHCP off or cable unplugged)"));
  } else {
    out.println(F("[ETH] Mode: DHCP client (router / organisation LAN)"));
  }
  uint8_t mac[6] = {0};
  ETH.macAddress(mac);
  out.printf("[ETH] MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  out.printf("[ETH] link=%s  %u Mbps  %s\n",
             ETH.linkUp() ? "UP" : "DOWN",
             (unsigned)ETH.linkSpeed(),
             ETH.fullDuplex() ? "full" : "half");
  out.printf("[ETH] IPv4 %s  mask %s  GW %s\n",
             ETH.localIP().toString().c_str(),
             ETH.subnetMask().toString().c_str(),
             ETH.gatewayIP().toString().c_str());
  out.printf("[ETH] NVS ethstatic: %s   mini-DHCP running: %s\n",
             staticNv ? "yes" : "no",
             mini ? "yes" : "no");
  const IPAddress ip = ETH.localIP();
  if (ETH.linkUp() && ip == IPAddress(0, 0, 0, 0)) {
    out.println(F("[ETH] Link up but no IPv4 — DHCP not answering (check router/VLAN/cable)."));
  } else if (ip[0] == 169 && ip[1] == 254) {
    out.println(F("[ETH] 169.254.x.x = link-local (no DHCP). Same-L2 host or set static IP on router."));
  } else if (ETH.linkUp() && ETH.gatewayIP() == IPAddress(0, 0, 0, 0) && (uint32_t)ip != 0u) {
    out.println(F("[ETH] Gateway 0.0.0.0 — may still reach same-subnet devices; no default route for internet."));
  }
#else
  out.println(F("[ETH] Disabled in build (CONFIG_ETH_ENABLED / CCTV_USE_ETH_W5500)."));
#endif
}
