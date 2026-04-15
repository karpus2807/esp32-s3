// cctv_pir.cpp — PIR HC-SR501 motion detection
#include "cctv_pir.h"
#include "board_config.h"

static volatile bool     s_alert       = false;
static volatile uint32_t s_lastTrigMs  = 0;
static bool              s_inited      = false;

static void IRAM_ATTR pirISR() {
  const uint32_t now = millis();
  // Software debounce: ignore re-triggers within cooldown window.
  if (now - s_lastTrigMs > CCTV_PIR_DEBOUNCE_MS) {
    s_alert      = true;
    s_lastTrigMs = now;
  }
}

void cctv_pir_init() {
  pinMode(CCTV_PIR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(CCTV_PIR_PIN), pirISR, RISING);
  s_inited = true;
}

bool cctv_pir_alert() { return s_alert; }

void cctv_pir_clear_alert() { s_alert = false; }

uint32_t cctv_pir_last_trigger_age_s() {
  if (s_lastTrigMs == 0) return 0;
  return (millis() - s_lastTrigMs) / 1000u;
}

const char* cctv_pir_status_str() {
  if (!s_inited) return "Not initialised";
  return s_alert ? "MOTION" : "OK";
}
