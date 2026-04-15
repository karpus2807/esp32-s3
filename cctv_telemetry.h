#pragma once
// cctv_telemetry.h — Collect all telemetry fields into JSON

#include <Arduino.h>

// Build full JSON telemetry string (all 25+ keys).
String cctv_telemetry_build_json();

// Compute alert level: 1=normal, 2=warn, 3=critical.
int cctv_telemetry_alert_level();

// Ethernet MAC accessor (for OLED / web).
String cctv_eth_mac_string();
