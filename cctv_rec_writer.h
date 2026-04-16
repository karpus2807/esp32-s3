#pragma once

#include <Arduino.h>
#include <stdint.h>

// Background AVI writer: recording task only grabs + encodes JPEGs; this task
// performs SD writes so long fs latency does not extend camera mutex hold time.

void cctv_rec_writer_init(void);

bool cctv_rec_writer_begin(const char *path, uint16_t w, uint16_t h, uint8_t fps);

void cctv_rec_writer_submit(uint8_t *jpeg, size_t len, bool heap_caps_alloc);

void cctv_rec_writer_end(uint32_t wall_clock_ms);
