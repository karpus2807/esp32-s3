#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// Start W5500 (SPI pins from board_config.h). Safe to call if ETH disabled in sdkconfig.
void cctv_net_init_ethernet();

// Block until Ethernet has DHCP (or timeout). Returns true if IP obtained.
bool cctv_net_wait_eth_dhcp(uint32_t timeout_ms);

// Prefer Ethernet IP when link+DHCP OK, else WiFi STA IP.
IPAddress cctv_primary_local_ip();

// True if Ethernet cable linked (PHY link), independent of DHCP.
bool cctv_eth_link_up();

// True if Ethernet has a non-zero IPv4 (DHCP done).
bool cctv_eth_has_ip();

// For web UI: separate addresses (empty string if none).
String cctv_eth_ip_string();
String cctv_wifi_ip_string();

// True if we have a routable STA address on Ethernet or WiFi (for HTTP / NTP).
bool cctv_has_ip_for_internet();

// If DHCP left DNS empty, set public DNS on STA/ETH netifs (fixes NTP / HTTP to hostnames).
void cctv_net_apply_fallback_dns(void);

// Serial diagnostics: mode, MAC, link, speed, IPv4 / mask / gateway (no-op if ETH off).
void cctv_net_print_eth_diagnostics(Print &out);

// NVS "cctv": ethSt=1 + ethIP/ethMk/ethGw[/ethDns] — applied right after ETH.begin (before DHCP wait).
bool cctv_eth_nvs_static_enabled();
bool cctv_eth_save_static_nvs(const IPAddress &ip, const IPAddress &mask, const IPAddress &gw, const IPAddress &dns);
void cctv_eth_clear_static_nvs();
// Apply static IPv4 now (ETH must be started). dns may be 0.0.0.0 → uses gateway.
bool cctv_eth_apply_static_ipv4(const IPAddress &ip, const IPAddress &mask, const IPAddress &gw, const IPAddress &dns);
// Stop static + mini-DHCP; clear runtime IPv4 and start DHCP client (after ethdhcp).
void cctv_eth_restart_dhcp_client();

// With ethstatic: mini DHCP on the same /24 so a direct-connected laptop gets automatic IPv4.
bool cctv_eth_mini_dhcp_active();
bool cctv_eth_start_mini_dhcp_for_static_subnet(Print &out);
