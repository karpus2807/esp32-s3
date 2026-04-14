#include "cctv_osd.h"

#if CCTV_ENABLE_FRAME_OSD

#include <Arduino.h>

#include "esp_heap_caps.h"
#include "fb_gfx.h"
#include "img_converters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void format_osd_line(char *buf, size_t cap) {
  struct tm ti;
  if (getLocalTime(&ti, 5)) {
    // CCTV-style: date + time only (no uptime / debug text)
    strftime(buf, cap, "%d-%m-%Y  %H:%M:%S", &ti);
  } else {
    snprintf(buf, cap, "%s", "--/--/---- --:--:--");
  }
}

bool cctvStampJpegBottomBar(uint8_t **jpeg_io, size_t *len_io, uint16_t width, uint16_t height, int jpg_quality) {
  if (jpeg_io == nullptr || len_io == nullptr || *jpeg_io == nullptr || *len_io == 0) {
    return false;
  }
  const int q_use = (jpg_quality >= 1 && jpg_quality <= 63) ? jpg_quality : (int)CCTV_OSD_JPEG_QUALITY;
  if (width < 32 || height < 32) {
    return false;
  }

  const size_t rgb_sz = (size_t)width * (size_t)height * 3u;
  uint8_t *rgb = (uint8_t *)heap_caps_malloc(rgb_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rgb) {
    rgb = (uint8_t *)heap_caps_malloc(rgb_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!rgb) {
    return false;
  }

  if (!fmt2rgb888(*jpeg_io, *len_io, PIXFORMAT_JPEG, rgb)) {
    heap_caps_free(rgb);
    return false;
  }

  const int bar_h = 22;
  const int y0 = (int)height - bar_h;
  fb_data_t fb = {};
  fb.width = width;
  fb.height = height;
  fb.bytes_per_pixel = 3;
  fb.format = FB_RGB888;
  fb.data = rgb;

  fb_gfx_fillRect(&fb, 0, y0, (int32_t)width, bar_h, 0x000000);
  fb_gfx_drawFastHLine(&fb, 0, y0, (int32_t)width, 0x606060);

  char line[48];
  format_osd_line(line, sizeof(line));
  fb_gfx_print(&fb, 4, y0 + 4, 0xFFFFFF, line);

  uint8_t *mj = nullptr;
  size_t mj_len = 0;
  const bool enc = fmt2jpg(
      rgb,
      rgb_sz,
      width,
      height,
      PIXFORMAT_RGB888,
      (uint8_t)q_use,
      &mj,
      &mj_len);
  heap_caps_free(rgb);

  if (!enc || mj == nullptr || mj_len == 0) {
    if (mj) {
      free(mj);
    }
    return false;
  }

  uint8_t *out = (uint8_t *)heap_caps_malloc(mj_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!out) {
    out = (uint8_t *)heap_caps_malloc(mj_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!out) {
    free(mj);
    return false;
  }
  memcpy(out, mj, mj_len);
  free(mj);

  heap_caps_free(*jpeg_io);
  *jpeg_io = out;
  *len_io = mj_len;
  return true;
}

#endif  // CCTV_ENABLE_FRAME_OSD
