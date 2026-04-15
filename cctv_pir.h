#pragma once
// cctv_pir.h — PIR HC-SR501 motion detection (GPIO 41)

#include <Arduino.h>

// Initialise PIR on CCTV_PIR_PIN. Call once from setup().
void cctv_pir_init();

// True if motion detected (stays true for debounce window).
bool cctv_pir_alert();

// Clear the alert flag (e.g. after MQTT push).
void cctv_pir_clear_alert();

// Seconds since last motion trigger (0 if never).
uint32_t cctv_pir_last_trigger_age_s();

const char* cctv_pir_status_str();
