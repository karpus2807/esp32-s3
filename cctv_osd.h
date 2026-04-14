#pragma once

#include "board_config.h"

#include <stddef.h>
#include <stdint.h>

#if CCTV_ENABLE_FRAME_OSD

// Decode JPEG → draw date/time bar → re-encode. Frees *jpeg_io (must be heap_caps-allocated)
// and replaces it with a new heap_caps buffer. On failure, input is unchanged.
// jpg_quality: 1–63 style encoder step (same as fmt2jpg); pass -1 to use CCTV_OSD_JPEG_QUALITY.
bool cctvStampJpegBottomBar(uint8_t **jpeg_io, size_t *len_io, uint16_t width, uint16_t height, int jpg_quality = -1);

#else

#include <stdint.h>

inline bool cctvStampJpegBottomBar(uint8_t **jpeg_io, size_t *len_io, uint16_t width, uint16_t height, int = -1) {
  (void)jpeg_io;
  (void)len_io;
  (void)width;
  (void)height;
  return false;
}

#endif
