#include "cctv_devstatus.h"

#include "board_config.h"
#include "cctv_net.h"
#include "cctv_psram.h"

#include <SD_MMC.h>
#include <WiFi.h>
#include <esp_chip_info.h>
#include <stdint.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern String g_wifiStatus;
extern bool g_sdReady;

static const char *wl_str(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:
      return "SSID not found";
    case WL_CONNECT_FAILED:
      return "Wrong password";
    case WL_CONNECTION_LOST:
      return "Connection lost";
    case WL_DISCONNECTED:
      return "Disconnected";
    case WL_IDLE_STATUS:
      return "Idle";
    case WL_CONNECTED:
      return "Connected";
    default:
      return "Unknown";
  }
}

void cctv_print_devstatus(Print &out) {
  out.println(F("========== DEV STATUS =========="));

  out.printf("[UP]  %lu s since boot\n", (unsigned long)(millis() / 1000UL));
  out.printf("[SDK] %s\n", ESP.getSdkVersion());
  out.printf("[CPU] chip rev %u  CPU %u MHz\n", (unsigned)ESP.getChipRevision(), (unsigned)(ESP.getCpuFreqMHz()));

  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  out.printf("[CPU] cores=%u  chip_features=0x%08lx\n",
             (unsigned)chip_info.cores,
             (unsigned long)chip_info.features);

  UBaseType_t idle0Wm = 0, idle1Wm = 0;
  TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCPU(0);
  TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCPU(1);
  if (idle0) {
    idle0Wm = uxTaskGetStackHighWaterMark(idle0);
  }
  if (idle1) {
    idle1Wm = uxTaskGetStackHighWaterMark(idle1);
  }
  const uint32_t idleStack = 700;
  const uint8_t cpu0 =
    (idle0Wm < idleStack) ? (uint8_t)((idleStack - idle0Wm) * 100 / idleStack) : 0;
  const uint8_t cpu1 =
    (idle1Wm < idleStack) ? (uint8_t)((idleStack - idle1Wm) * 100 / idleStack) : 0;
  out.printf("[CPU] est. load (idle HWM proxy): core0 ~%u%%  core1 ~%u%%  (idle0 wm=%u  idle1 wm=%u)\n",
             (unsigned)cpu0,
             (unsigned)cpu1,
             (unsigned)idle0Wm,
             (unsigned)idle1Wm);

  const size_t heapFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t heapTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  const size_t heapMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  out.printf("[RAM] internal: free %u / total %u bytes  (ever min free %u)\n",
             (unsigned)heapFree,
             (unsigned)heapTotal,
             (unsigned)heapMin);

  {
    const size_t capsTot = cctv_psram_heap_total_caps();
    const size_t capsFree = cctv_psram_heap_free_caps();
    const size_t arduinoTot = ESP.getPsramSize();
    const size_t arduinoFree = ESP.getFreePsram();
    const size_t appTot = cctv_psram_total_bytes();
    const size_t appFree = cctv_psram_free_bytes();
    if (appTot > 0u) {
      out.printf("[PSRAM] free %u / total %u bytes (app view; heap_caps tot=%u free=%u)\n",
                 (unsigned)appFree,
                 (unsigned)appTot,
                 (unsigned)capsTot,
                 (unsigned)capsFree);
      if (arduinoTot == 0u && capsTot > 0u) {
        out.println(F("[PSRAM] note: Arduino getPsramSize() is 0 — SPIRAM still in heap (IDF 5.x quirk)."));
      }
    } else {
      out.printf(
          "[PSRAM] not in heap  heap_caps SPIRAM tot=%u  Arduino getPsramSize=%u\n",
          (unsigned)capsTot,
          (unsigned)arduinoTot);
      out.println(F("[PSRAM] Build: Tools → PSRAM → **OPI PSRAM** for WROOM N16R8 (not QSPI / PSRAM=enabled)."));
    }
  }

  const uint32_t flashChip = ESP.getFlashChipSize();
  const uint32_t sketch = ESP.getSketchSize();
  const uint32_t sketchFree = ESP.getFreeSketchSpace();
  out.printf("[FLASH] chip size %u bytes (~%u MB)\n", (unsigned)flashChip, (unsigned)(flashChip / (1024 * 1024)));
  out.printf("[FLASH] app firmware: used %u bytes  remaining in app slot ~%u bytes (for growth / OTA layout)\n",
             (unsigned)sketch,
             (unsigned)sketchFree);

  out.println(F("---------- Network ----------"));
  out.printf("[WiFi] status string: %s\n", g_wifiStatus.c_str());
  out.printf("[WiFi] wl_status_t: %s (%d)\n", wl_str(WiFi.status()), (int)WiFi.status());
  {
    uint8_t wm[6] = {0};
    WiFi.macAddress(wm);
    out.printf("[WiFi] STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
               wm[0], wm[1], wm[2], wm[3], wm[4], wm[5]);
  }
  if (WiFi.status() == WL_CONNECTED) {
    out.printf("[WiFi] IP: %s  GW: %s  mask: %s\n",
               WiFi.localIP().toString().c_str(),
               WiFi.gatewayIP().toString().c_str(),
               WiFi.subnetMask().toString().c_str());
    out.printf("[WiFi] SSID: %s  RSSI: %d dBm  ch: %d\n",
               WiFi.SSID().c_str(),
               WiFi.RSSI(),
               (int)WiFi.channel());
  } else {
    out.println(F("[WiFi] not associated (no IP)"));
  }

  out.println(F("---------- Ethernet (W5500) ----------"));
  out.println(F("[ETH] W5500 = 10/100 PHY only (not Gigabit). Switch showing 100M is expected."));
  cctv_net_print_eth_diagnostics(out);

  out.printf("[NET] primary IP (routing hint): %s\n", cctv_primary_local_ip().toString().c_str());

  out.println(F("---------- microSD ----------"));
  if (g_sdReady) {
    const uint64_t total = SD_MMC.totalBytes();
    const uint64_t used = SD_MMC.usedBytes();
    const uint64_t free = total - used;
    out.printf("[SD] mounted  total %llu MB  used %llu MB  free %llu MB\n",
               (unsigned long long)(total / (1024ULL * 1024ULL)),
               (unsigned long long)(used / (1024ULL * 1024ULL)),
               (unsigned long long)(free / (1024ULL * 1024ULL)));
  } else {
    out.println(F("[SD] not mounted"));
  }

  out.println(F("=================================="));
}
