#pragma once

#include <Arduino.h>

// Initialise camera-based motion detector task.
void cctv_cam_motion_init();

// 1 when camera scene is moving, else 0.
bool cctv_cam_motion_alert();

// Seconds since last motion start edge (0 if never).
uint32_t cctv_cam_motion_last_trigger_age_s();

// Last computed change score (0..100), for web diagnostics.
uint8_t cctv_cam_motion_score();
uint8_t cctv_cam_motion_score_smooth();

// Runtime trigger threshold (% changed score), persisted in NVS.
uint8_t cctv_cam_motion_threshold();
uint8_t cctv_cam_motion_threshold_effective();
uint8_t cctv_cam_motion_noise_floor();
bool cctv_cam_motion_set_threshold(uint8_t threshold_pct);

// Debug/diagnostics: instantaneous decision inputs.
bool cctv_cam_motion_moving_now();
uint8_t cctv_cam_motion_on_count();
uint8_t cctv_cam_motion_off_count();
