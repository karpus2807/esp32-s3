#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once before esp_camera_init() / any esp_camera_fb_get().
void cctv_platform_init(void);

// Serialize all camera DMA buffer access (HTTP stream/capture + SD recording + warmup).
void cctv_camera_lock(void);
void cctv_camera_unlock(void);

// Serialize WiFi connect / disconnect / config-save across all tasks.
void cctv_wifi_lock(void);
void cctv_wifi_unlock(void);
bool cctv_wifi_try_lock(uint32_t timeout_ms);

// Human-readable WiFi status (shared — avoids duplicating in every file).
const char *cctv_wifi_status_str(int wl_status);

#ifdef __cplusplus
}
#endif
