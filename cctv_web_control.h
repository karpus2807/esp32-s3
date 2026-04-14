#pragma once

#include <Arduino.h>
#include <Print.h>

// Run one serial-style command line; output goes to `out` (Serial or String buffer).
void cctv_web_control_dispatch(const String &line, Print &out);
