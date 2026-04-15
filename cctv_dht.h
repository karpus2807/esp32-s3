#pragma once
// cctv_dht.h — DHT11 temperature & humidity sensor (GPIO 42)

#include <Arduino.h>

// Initialise DHT11 on CCTV_DHT_PIN. Call once from setup().
void cctv_dht_init();

// Latest readings (NaN if not yet read or sensor error).
float cctv_dht_temperature();   // °C
float cctv_dht_humidity();      // %

// True if last read succeeded.
bool  cctv_dht_ok();

// Human-readable status string for web UI.
const char* cctv_dht_status_str();
