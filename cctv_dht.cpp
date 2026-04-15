// cctv_dht.cpp — DHT11 temperature & humidity sensor
#include "cctv_dht.h"
#include "board_config.h"
#include <DHTesp.h>

static DHTesp s_dht;
static float  s_temp = NAN;
static float  s_hum  = NAN;
static bool   s_ok   = false;
static const char* s_status = "Not initialised";

static void dhtReadTask(void *) {
  for (;;) {
    TempAndHumidity r = s_dht.getTempAndHumidity();
    if (s_dht.getStatus() == DHTesp::ERROR_NONE) {
      s_temp   = r.temperature;
      s_hum    = r.humidity;
      s_ok     = true;
      s_status = "OK";
    } else {
      s_ok     = false;
      s_status = s_dht.getStatusString();
    }
    vTaskDelay(pdMS_TO_TICKS(CCTV_DHT_READ_INTERVAL_MS));
  }
}

void cctv_dht_init() {
  s_dht.setup(CCTV_DHT_PIN, DHTesp::DHT11);
  s_status = "Initialised";
  xTaskCreatePinnedToCore(dhtReadTask, "DHTRead", 3072, nullptr, 1, nullptr, 0);
}

float cctv_dht_temperature() { return s_temp; }
float cctv_dht_humidity()    { return s_hum; }
bool  cctv_dht_ok()          { return s_ok; }
const char* cctv_dht_status_str() { return s_status; }
