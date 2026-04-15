#include "cctv_platform.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_cam_mtx;
static SemaphoreHandle_t s_wifi_mtx;

void cctv_platform_init(void) {
  if (!s_cam_mtx) {
    s_cam_mtx = xSemaphoreCreateMutex();
  }
  if (!s_wifi_mtx) {
    s_wifi_mtx = xSemaphoreCreateMutex();
  }
}

void cctv_camera_lock(void) {
  if (s_cam_mtx) {
    xSemaphoreTake(s_cam_mtx, portMAX_DELAY);
  }
}

void cctv_camera_unlock(void) {
  if (s_cam_mtx) {
    xSemaphoreGive(s_cam_mtx);
  }
}

void cctv_wifi_lock(void) {
  if (s_wifi_mtx) {
    xSemaphoreTake(s_wifi_mtx, portMAX_DELAY);
  }
}

void cctv_wifi_unlock(void) {
  if (s_wifi_mtx) {
    xSemaphoreGive(s_wifi_mtx);
  }
}

bool cctv_wifi_try_lock(uint32_t timeout_ms) {
  if (!s_wifi_mtx) return true;
  return xSemaphoreTake(s_wifi_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

const char *cctv_wifi_status_str(int wl_status) {
  switch (wl_status) {
    case 1:  return "SSID not found";   // WL_NO_SSID_AVAIL
    case 4:  return "Wrong password";   // WL_CONNECT_FAILED
    case 5:  return "Connection lost";  // WL_CONNECTION_LOST
    case 6:  return "Disconnected";     // WL_DISCONNECTED
    case 0:  return "Idle";             // WL_IDLE_STATUS
    case 3:  return "Connected";        // WL_CONNECTED
    default: return "Unknown";
  }
}
