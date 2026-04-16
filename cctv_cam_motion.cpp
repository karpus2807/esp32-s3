#include "cctv_cam_motion.h"

#include "cctv_platform.h"
#include "esp_camera.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr uint32_t kPollMs = 300;
constexpr uint8_t kSamples = 64;
// Byte delta threshold for sampled JPEG payload bytes. Too low causes false motion
// due to auto-exposure / compression jitter; too high reduces sensitivity.
constexpr uint8_t kByteDeltaThreshold = 18;
constexpr uint8_t kMotionScoreThresholdDefault = 45;
constexpr uint8_t kMotionScoreThresholdMin = 5;
constexpr uint8_t kMotionScoreThresholdMax = 95;
constexpr uint8_t kConsecutiveOn = 3;
constexpr uint8_t kConsecutiveOff = 4;
// Margin above estimated noise floor. Lower margin = easier to trigger.
constexpr uint8_t kNoiseMargin = 8;
// Skip the first part of JPEG bitstream (headers/tables) when sampling.
// This reduces false positives from small header variations.
constexpr size_t kJpegSkipBytes = 256;
// Cap background noise estimate to avoid "stuck high" effective thresholds.
constexpr uint8_t kNoiseFloorCap = 60;

volatile bool s_motion = false;
volatile uint32_t s_lastTrigMs = 0;
volatile uint8_t s_score = 0;
volatile uint8_t s_score_ema = 0;
volatile uint8_t s_threshold = kMotionScoreThresholdDefault;
volatile uint8_t s_threshold_effective = kMotionScoreThresholdDefault;
volatile uint8_t s_noise_floor = 0;

uint8_t s_prev[kSamples] = {0};
bool s_has_prev = false;
uint8_t s_on_cnt = 0;
uint8_t s_off_cnt = 0;

uint8_t clamp_thr(uint8_t v) {
  if (v < kMotionScoreThresholdMin) return kMotionScoreThresholdMin;
  if (v > kMotionScoreThresholdMax) return kMotionScoreThresholdMax;
  return v;
}

void load_threshold_nvs() {
  Preferences p;
  if (!p.begin("cammd", true)) {
    s_threshold = kMotionScoreThresholdDefault;
    return;
  }
  const uint8_t thr = p.getUChar("thr", kMotionScoreThresholdDefault);
  p.end();
  s_threshold = clamp_thr(thr);
}

void sample_jpeg(const uint8_t *buf, size_t len, uint8_t out[kSamples]) {
  if (!buf || len == 0) {
    memset(out, 0, kSamples);
    return;
  }
  size_t start = 0;
  if (len > (kJpegSkipBytes + 32)) {
    start = kJpegSkipBytes;
  }
  size_t avail = len - start;
  if (avail == 0) avail = 1;
  const size_t step = (avail > kSamples) ? (avail / kSamples) : 1;
  size_t idx = start;
  for (uint8_t i = 0; i < kSamples; ++i) {
    out[i] = buf[idx];
    idx += step;
    if (idx >= len) idx = len - 1;
  }
}

uint8_t compute_score(const uint8_t cur[kSamples], const uint8_t prev[kSamples]) {
  uint8_t changed = 0;
  for (uint8_t i = 0; i < kSamples; ++i) {
    const int d = (int)cur[i] - (int)prev[i];
    if ((d < 0 ? -d : d) >= kByteDeltaThreshold) {
      ++changed;
    }
  }
  return (uint8_t)((changed * 100u) / kSamples);
}

void camMotionTask(void *) {
  uint8_t cur[kSamples] = {0};

  for (;;) {
    cctv_camera_lock();
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb && fb->buf && fb->len > 0) {
      sample_jpeg(fb->buf, fb->len, cur);
      esp_camera_fb_return(fb);
      cctv_camera_unlock();

      if (!s_has_prev) {
        memcpy(s_prev, cur, kSamples);
        s_has_prev = true;
        s_score = 0;
      } else {
        const uint8_t score = compute_score(cur, s_prev);
        s_score = score;
        s_score_ema = (uint8_t)((((uint16_t)s_score_ema * 3u) + score + 2u) / 4u);
        memcpy(s_prev, cur, kSamples);

        if (!s_motion) {
          // Update noise floor only in no-motion state.
          // Use an asymmetric filter so occasional spikes don't "poison" the baseline:
          // - rising: very slow (resists auto-exposure / compression jitter spikes)
          // - falling: faster (adapts back down quickly)
          if (s_score_ema > s_noise_floor) {
            s_noise_floor = (uint8_t)((((uint16_t)s_noise_floor * 31u) + s_score_ema + 16u) / 32u);
          } else {
            s_noise_floor = (uint8_t)((((uint16_t)s_noise_floor * 7u) + s_score_ema + 4u) / 8u);
          }
          if (s_noise_floor > kNoiseFloorCap) s_noise_floor = kNoiseFloorCap;
        }
        uint8_t eff = s_noise_floor + kNoiseMargin;
        if (eff < s_threshold) eff = s_threshold;
        if (eff > kMotionScoreThresholdMax) eff = kMotionScoreThresholdMax;
        s_threshold_effective = eff;

        const bool moving = (s_score_ema >= s_threshold_effective);
        if (moving) {
          if (s_on_cnt < 250) ++s_on_cnt;
          s_off_cnt = 0;
        } else {
          if (s_off_cnt < 250) ++s_off_cnt;
          s_on_cnt = 0;
        }

        if (!s_motion && s_on_cnt >= kConsecutiveOn) {
          s_motion = true;
          s_lastTrigMs = millis();
        } else if (s_motion && s_off_cnt >= kConsecutiveOff) {
          s_motion = false;
        }
      }
    } else {
      if (fb) esp_camera_fb_return(fb);
      cctv_camera_unlock();
    }

    vTaskDelay(pdMS_TO_TICKS(kPollMs));
  }
}
}  // namespace

void cctv_cam_motion_init() {
  load_threshold_nvs();
  xTaskCreatePinnedToCore(camMotionTask, "CamMotion", 6144, nullptr, 1, nullptr, 0);
}

bool cctv_cam_motion_alert() { return s_motion; }

uint32_t cctv_cam_motion_last_trigger_age_s() {
  if (s_lastTrigMs == 0) return 0;
  return (millis() - s_lastTrigMs) / 1000u;
}

uint8_t cctv_cam_motion_score() { return s_score; }
uint8_t cctv_cam_motion_score_smooth() { return s_score_ema; }

uint8_t cctv_cam_motion_threshold() { return s_threshold; }
uint8_t cctv_cam_motion_threshold_effective() { return s_threshold_effective; }
uint8_t cctv_cam_motion_noise_floor() { return s_noise_floor; }
bool cctv_cam_motion_moving_now() { return s_score_ema >= s_threshold_effective; }
uint8_t cctv_cam_motion_on_count() { return s_on_cnt; }
uint8_t cctv_cam_motion_off_count() { return s_off_cnt; }

bool cctv_cam_motion_set_threshold(uint8_t threshold_pct) {
  const uint8_t thr = clamp_thr(threshold_pct);
  Preferences p;
  if (!p.begin("cammd", false)) return false;
  p.putUChar("thr", thr);
  p.end();
  s_threshold = thr;
  return true;
}
