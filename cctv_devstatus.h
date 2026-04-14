#pragma once

#include <Arduino.h>

// Full device snapshot (network, flash, RAM/PSRAM, CPU, SD) — text for Serial / web console.
void cctv_print_devstatus(Print &out);
