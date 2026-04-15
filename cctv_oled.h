#pragma once
// cctv_oled.h — SH1106 128×64 OLED display (I2C GPIO 1/2)

#include <Arduino.h>

// Initialise I2C + SH1106. Call from setup() early (shows boot animation).
void cctv_oled_init();

// Run 7-second boot animation (blocking — call during setup warmup).
void cctv_oled_boot_animation();

// Start the background page-rotation task (call after setup is done).
void cctv_oled_start_task();

// Force a specific message on display (overrides auto-rotate for ~4s).
void cctv_oled_show_message(const char* line1, const char* line2 = nullptr,
                            const char* line3 = nullptr, const char* line4 = nullptr);

// True if OLED was detected on I2C bus.
bool cctv_oled_ok();

const char* cctv_oled_status_str();
