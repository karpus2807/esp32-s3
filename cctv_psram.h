#pragma once

// ESP32-S3 + IDF 5.x: ESP.getPsramSize() / psramFound() may stay 0 even when SPIRAM is
// linked into the heap (OPI PSRAM). Use heap_caps + esp_psram_* for a reliable picture.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
#include <esp_psram.h>  // esp_psram_get_size / esp_psram_is_initialized
#endif

static inline size_t cctv_psram_heap_total_caps(void) {
  return heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline size_t cctv_psram_heap_free_caps(void) {
  return heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/** True if application can allocate from SPIRAM (camera FB, buffers, etc.). */
static inline bool cctv_psram_app_ready(void) {
  if (cctv_psram_heap_total_caps() > 0u) {
    return true;
  }
  if (ESP.getPsramSize() > 0u) {
    return true;
  }
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
  if (esp_psram_is_initialized() && esp_psram_get_size() > 0) {
    return true;
  }
#endif
  return false;
}

/** Bytes visible to heap allocator in SPIRAM (preferred for UI / devstatus). */
static inline size_t cctv_psram_total_bytes(void) {
  const size_t caps = cctv_psram_heap_total_caps();
  if (caps > 0u) {
    return caps;
  }
  const size_t ar = ESP.getPsramSize();
  if (ar > 0u) {
    return ar;
  }
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
  if (esp_psram_is_initialized()) {
    const size_t sz = esp_psram_get_size();
    if (sz > 0u) {
      return sz;
    }
  }
#endif
  return 0u;
}

static inline size_t cctv_psram_free_bytes(void) {
  if (cctv_psram_heap_total_caps() > 0u) {
    return cctv_psram_heap_free_caps();
  }
  return ESP.getFreePsram();
}
