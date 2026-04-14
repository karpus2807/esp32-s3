#pragma once

#include <Arduino.h>

void cctv_ui_log_init();
void cctv_ui_log_set(bool enabled);
bool cctv_ui_log_get();
void cctv_ui_log_clear();
void cctv_ui_log_append(const String &chunk);
void cctv_ui_log_snapshot(String &out);
