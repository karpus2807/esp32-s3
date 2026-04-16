#include "cctv_rec_writer.h"

#include "avi_recorder.h"

#include <cstring>
#include <esp_heap_caps.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum { RECW_Q_DEPTH = 4, RECW_PATH_MAX = 96 };

typedef enum {
  RECW_OP_BEGIN = 0,
  RECW_OP_FRAME,
  RECW_OP_END,
} recw_op_t;

typedef struct {
  recw_op_t op;
  char path[RECW_PATH_MAX];
  uint16_t w, h;
  uint8_t fps;
  uint8_t *jpeg;
  size_t len;
  uint8_t caps_alloc;
  uint32_t wall_ms;
} recw_msg_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_ctl_done;
static volatile bool s_begin_ok;
static TaskHandle_t s_task;

static void rec_writer_task(void *) {
  AviRecorder rec;
  bool open = false;
  recw_msg_t m;

  for (;;) {
    if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (m.op == RECW_OP_BEGIN) {
      if (open) {
        rec.end(0);
        open = false;
      }
      s_begin_ok = rec.begin(String(m.path), m.w, m.h, m.fps);
      open = (bool)s_begin_ok;
      xSemaphoreGive(s_ctl_done);
    } else if (m.op == RECW_OP_FRAME) {
      if (open && m.jpeg && m.len) {
        (void)rec.writeFrame(m.jpeg, m.len);
      }
      if (m.jpeg) {
        if (m.caps_alloc) {
          heap_caps_free(m.jpeg);
        } else {
          free(m.jpeg);
        }
      }
    } else if (m.op == RECW_OP_END) {
      if (open) {
        rec.end(m.wall_ms);
        open = false;
      }
      xSemaphoreGive(s_ctl_done);
    }
  }
}

void cctv_rec_writer_init(void) {
  if (s_q) {
    return;
  }
  s_q = xQueueCreate(RECW_Q_DEPTH, sizeof(recw_msg_t));
  s_ctl_done = xSemaphoreCreateBinary();
  if (!s_q || !s_ctl_done) {
    return;
  }
  if (xTaskCreatePinnedToCore(
          rec_writer_task,
          "RecWr",
          12 * 1024,
          nullptr,
          3,
          &s_task,
          0) != pdPASS) {
    s_task = nullptr;
  }
}

bool cctv_rec_writer_begin(const char *path, uint16_t w, uint16_t h, uint8_t fps) {
  if (!s_q || !s_ctl_done || !s_task || !path) {
    return false;
  }
  recw_msg_t m = {};
  m.op = RECW_OP_BEGIN;
  m.w = w;
  m.h = h;
  m.fps = fps;
  strncpy(m.path, path, RECW_PATH_MAX - 1);
  m.path[RECW_PATH_MAX - 1] = '\0';

  if (xQueueSend(s_q, &m, pdMS_TO_TICKS(8000)) != pdTRUE) {
    return false;
  }
  if (xSemaphoreTake(s_ctl_done, pdMS_TO_TICKS(8000)) != pdTRUE) {
    return false;
  }
  return (bool)s_begin_ok;
}

void cctv_rec_writer_submit(uint8_t *jpeg, size_t len, bool heap_caps_alloc) {
  if (!s_q || !jpeg || !len) {
    if (jpeg) {
      if (heap_caps_alloc) {
        heap_caps_free(jpeg);
      } else {
        free(jpeg);
      }
    }
    return;
  }
  recw_msg_t m = {};
  m.op = RECW_OP_FRAME;
  m.jpeg = jpeg;
  m.len = len;
  m.caps_alloc = heap_caps_alloc ? (uint8_t)1 : (uint8_t)0;
  (void)xQueueSend(s_q, &m, portMAX_DELAY);
}

void cctv_rec_writer_end(uint32_t wall_clock_ms) {
  if (!s_q || !s_ctl_done) {
    return;
  }
  recw_msg_t m = {};
  m.op = RECW_OP_END;
  m.wall_ms = wall_clock_ms;
  (void)xQueueSend(s_q, &m, portMAX_DELAY);
  (void)xSemaphoreTake(s_ctl_done, portMAX_DELAY);
}
