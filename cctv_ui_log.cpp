#include "cctv_ui_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static bool s_enabled = false;
static String s_text;
static SemaphoreHandle_t s_mtx = nullptr;

static void ensure_mtx() {
  if (!s_mtx) {
    s_mtx = xSemaphoreCreateMutex();
  }
}

void cctv_ui_log_init() {
  ensure_mtx();
}

void cctv_ui_log_set(bool enabled) {
  ensure_mtx();
  if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
    return;
  }
  s_enabled = enabled;
  if (!enabled) {
    s_text = "";
  }
  xSemaphoreGive(s_mtx);
}

bool cctv_ui_log_get() {
  return s_enabled;
}

void cctv_ui_log_clear() {
  ensure_mtx();
  if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
    return;
  }
  s_text = "";
  xSemaphoreGive(s_mtx);
}

void cctv_ui_log_append(const String &chunk) {
  if (!s_enabled || chunk.length() == 0) {
    return;
  }
  ensure_mtx();
  if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
    return;
  }
  constexpr size_t kMax = 16384;
  while (s_text.length() + chunk.length() > kMax) {
    const size_t drop = (s_text.length() / 3) + 1;
    s_text = s_text.substring(drop);
  }
  s_text += chunk;
  xSemaphoreGive(s_mtx);
}

void cctv_ui_log_snapshot(String &out) {
  ensure_mtx();
  out = "";
  if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
    return;
  }
  out = s_text;
  xSemaphoreGive(s_mtx);
}
