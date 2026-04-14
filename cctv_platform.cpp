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
