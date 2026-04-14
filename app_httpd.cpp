// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "esp32-hal-ledc.h"
#include <Arduino.h>
#include <WiFi.h>

#include "cctv_net.h"
#include "cctv_wifi_profiles.h"
#include "cctv_psram.h"
#include "cctv_time_sync.h"
#include <SD_MMC.h>
#include "sdkconfig.h"
#include "board_config.h"
#include "cctv_osd.h"
#include "cctv_platform.h"
#include "cctv_web_control.h"
#include "cctv_ui_log.h"
#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#endif

// LED FLASH setup
#if defined(LED_GPIO_NUM)
#define CONFIG_LED_MAX_INTENSITY 255

int led_duty = 0;

#endif

typedef struct {
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;

extern bool g_sdReady;
extern String g_lastSavedPath;
extern String g_sdStatusMessage;
extern String g_cameraStatusMessage;
extern String g_recordingStatus;
extern String g_recordingFile;
extern String g_wifiSsid;
extern String g_wifiPass;
extern bool g_wifiEnterprise;
extern String g_wifiIdentity;
extern String g_wifiEapPass;
extern String g_wifiStatus;
bool saveJpegFrameToSd(String *savedPath = nullptr);
bool saveJpegBufferToSd(const uint8_t *jpegBuf, size_t jpegLen, String *savedPath = nullptr);
bool remountMicroSD();
bool testMicroSD(String *message = nullptr);
size_t clearCaptures(String *message = nullptr);
String listCapturesJson(size_t limit = 20);
void saveWifiConfig();
void clearWifiConfig();

// URL-decode a query-parameter value (%XX → char, + → space)
// ESP-IDF httpd_query_key_value() does NOT url-decode automatically.
static String urlDecode(const char *src) {
  String out;
  char hex[3] = {0};
  for (size_t i = 0; src[i]; i++) {
    if (src[i] == '%' && isxdigit((unsigned char)src[i+1]) && isxdigit((unsigned char)src[i+2])) {
      hex[0] = src[i+1]; hex[1] = src[i+2];
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else if (src[i] == '+') {
      out += ' ';
    } else {
      out += src[i];
    }
  }
  return out;
}



httpd_handle_t camera_httpd = NULL;



#if defined(LED_GPIO_NUM)
void enable_led(bool en) {
  int duty = en ? led_duty : 0;
  if (duty > CONFIG_LED_MAX_INTENSITY) duty = CONFIG_LED_MAX_INTENSITY;
  ledcWrite(LED_GPIO_NUM, duty);
  log_i("Set LED intensity to %d", duty);
}
#endif

static esp_err_t bmp_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  uint64_t fr_start = esp_timer_get_time();
#endif
  cctv_camera_lock();
  fb = esp_camera_fb_get();
  if (!fb) {
    cctv_camera_unlock();
    log_e("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/x-windows-bmp");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.bmp");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char ts[32];
  snprintf(ts, 32, "%lld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
  httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

  uint8_t *buf = NULL;
  size_t buf_len = 0;
  bool converted = frame2bmp(fb, &buf, &buf_len);
  esp_camera_fb_return(fb);
  cctv_camera_unlock();
  if (!converted) {
    log_e("BMP Conversion failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  res = httpd_resp_send(req, (const char *)buf, buf_len);
  free(buf);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  uint64_t fr_end = esp_timer_get_time();
#endif
  log_i("BMP: %llums, %uB", (uint64_t)((fr_end - fr_start) / 1000), buf_len);
  return res;
}

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len) {
  jpg_chunking_t *j = (jpg_chunking_t *)arg;
  if (!index) {
    j->len = 0;
  }
  if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) {
    return 0;
  }
  j->len += len;
  return len;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  int64_t fr_start = esp_timer_get_time();
#endif

  cctv_camera_lock();
#if defined(LED_GPIO_NUM)
  enable_led(true);
  vTaskDelay(150 / portTICK_PERIOD_MS);  // The LED needs to be turned on ~150ms before the call to esp_camera_fb_get()
  fb = esp_camera_fb_get();              // or it won't be visible in the frame. A better way to do this is needed.
  enable_led(false);
#else
  fb = esp_camera_fb_get();
#endif

  if (!fb) {
    cctv_camera_unlock();
    log_e("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char ts[32];
  snprintf(ts, 32, "%lld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
  httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

  size_t jpg_len = 0;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  size_t fb_len = 0;
#endif
  if (fb->format == PIXFORMAT_JPEG) {
    jpg_len = fb->len;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    fb_len = jpg_len;
#endif
    uint8_t *dup = (uint8_t *)heap_caps_malloc(
        jpg_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dup) {
      dup = (uint8_t *)heap_caps_malloc(jpg_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!dup) {
      esp_camera_fb_return(fb);
      cctv_camera_unlock();
      return httpd_resp_send_500(req);
    }
    memcpy(dup, fb->buf, jpg_len);
    esp_camera_fb_return(fb);
    fb = nullptr;
    cctv_camera_unlock();
    res = httpd_resp_send(req, (const char *)dup, jpg_len);
    heap_caps_free(dup);
  } else {
    jpg_chunking_t jchunk = {req, 0};
    res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
    httpd_resp_send_chunk(req, NULL, 0);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    fb_len = jchunk.len;
#endif
    esp_camera_fb_return(fb);
    cctv_camera_unlock();
  }
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  int64_t fr_end = esp_timer_get_time();
#endif
  log_i("JPG: %uB %ums", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start) / 1000));
  return res;
}

// MJPEG over one TCP connection — much lower latency and higher FPS than polling /capture.
// Same lwIP send-buffer tuning as file download (see download_handler); avoid TCP_NODELAY here
// because each frame is several small chunks (boundary + headers + body).
#define STREAM_PART_BOUNDARY "1234567890000000000009876543210"
// Space after ';' helps strict clients (some VLC builds, embedded players).
static const char STREAM_CONTENT_TYPE[] =
    "multipart/x-mixed-replace; boundary=" STREAM_PART_BOUNDARY;

static void tune_stream_socket(httpd_req_t *req) {
  int sockfd = httpd_req_to_sockfd(req);
  if (sockfd < 0) {
    return;
  }
  int sndbuf = 65535;
  (void)setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  // One large chunk per frame — safe to disable Nagle for lower latency to VLC/browser.
  int one = 1;
  setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// Rolling MJPEG stream FPS (updated from stream_handler, read by /sysinfo).
static portMUX_TYPE s_stream_fps_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_stream_fps_window_ms = 0;
static uint32_t s_stream_fps_frame_count = 0;
static float s_stream_fps_value = 0.f;

static void stream_fps_on_frame_sent() {
  const uint32_t m = millis();
  portENTER_CRITICAL(&s_stream_fps_mux);
  if (s_stream_fps_window_ms == 0) {
    s_stream_fps_window_ms = m;
  }
  s_stream_fps_frame_count++;
  const uint32_t elapsed = m - s_stream_fps_window_ms;
  if (elapsed >= 1000u && s_stream_fps_frame_count > 0) {
    s_stream_fps_value = (s_stream_fps_frame_count * 1000.f) / (float)elapsed;
    s_stream_fps_window_ms = m;
    s_stream_fps_frame_count = 0;
  }
  portEXIT_CRITICAL(&s_stream_fps_mux);
}

static float stream_fps_get() {
  portENTER_CRITICAL(&s_stream_fps_mux);
  const float v = s_stream_fps_value;
  portEXIT_CRITICAL(&s_stream_fps_mux);
  return v;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  tune_stream_socket(req);

  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");

  static const char boundary[] = "\r\n--" STREAM_PART_BOUNDARY "\r\n";
  static const char part_tmpl[] = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
  char part_hdr[96];
  // One contiguous buffer per frame (boundary + part headers + JPEG) → one send_chunk;
  // defer free until next frame so lwIP is done reading (fixes black/corrupt MJPEG).
  uint8_t *pending_pkt = nullptr;

  log_i("MJPEG session start (VLC: http://<ip>/mjpeg or /live.mjpg)");

  while (true) {
    if (pending_pkt) {
      heap_caps_free(pending_pkt);
      pending_pkt = nullptr;
    }

    cctv_camera_lock();
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      cctv_camera_unlock();
      log_e("Stream: camera capture failed");
      res = ESP_FAIL;
      break;
    }

    if (fb->format != PIXFORMAT_JPEG) {
      log_e("Stream: need JPEG pixformat");
      esp_camera_fb_return(fb);
      cctv_camera_unlock();
      res = ESP_FAIL;
      break;
    }

    if (fb->len == 0) {
      esp_camera_fb_return(fb);
      cctv_camera_unlock();
      continue;
    }

    const uint16_t fb_w = fb->width;
    const uint16_t fb_h = fb->height;
    size_t jpeg_len = fb->len;

    uint8_t *jpeg_copy = (uint8_t *)heap_caps_malloc(
        jpeg_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_copy) {
      jpeg_copy = (uint8_t *)heap_caps_malloc(jpeg_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!jpeg_copy) {
      esp_camera_fb_return(fb);
      cctv_camera_unlock();
      res = ESP_ERR_NO_MEM;
      break;
    }
    memcpy(jpeg_copy, fb->buf, jpeg_len);
    esp_camera_fb_return(fb);
    cctv_camera_unlock();

#if CCTV_ENABLE_FRAME_OSD && CCTV_OSD_ON_STREAM
    (void)cctvStampJpegBottomBar(&jpeg_copy, &jpeg_len, fb_w, fb_h);
#endif

    int hlen = snprintf(part_hdr, sizeof(part_hdr), part_tmpl, (unsigned)jpeg_len);
    if (hlen <= 0 || (size_t)hlen >= sizeof(part_hdr)) {
      heap_caps_free(jpeg_copy);
      res = ESP_FAIL;
      break;
    }

    const size_t bl = strlen(boundary);
    const size_t total = bl + (size_t)hlen + jpeg_len;
    uint8_t *pkt = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pkt) {
      pkt = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!pkt) {
      heap_caps_free(jpeg_copy);
      res = ESP_ERR_NO_MEM;
      break;
    }
    memcpy(pkt, boundary, bl);
    memcpy(pkt + bl, part_hdr, (size_t)hlen);
    memcpy(pkt + bl + (size_t)hlen, jpeg_copy, jpeg_len);
    heap_caps_free(jpeg_copy);

    res = httpd_resp_send_chunk(req, (const char *)pkt, total);
    if (res == ESP_OK) {
      pending_pkt = pkt;
      stream_fps_on_frame_sent();
      vTaskDelay(1);
    } else {
      heap_caps_free(pkt);
    }
    if (res != ESP_OK) {
      break;
    }
  }

  if (pending_pkt) {
    heap_caps_free(pending_pkt);
    pending_pkt = nullptr;
  }

  portENTER_CRITICAL(&s_stream_fps_mux);
  s_stream_fps_value = 0.f;
  s_stream_fps_frame_count = 0;
  s_stream_fps_window_ms = 0;
  portEXIT_CRITICAL(&s_stream_fps_mux);

  httpd_resp_send_chunk(req, NULL, 0);
  return res;
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf) {
  char *buf = NULL;
  size_t buf_len = 0;

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char *)malloc(buf_len);
    if (!buf) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      *obuf = buf;
      return ESP_OK;
    }
    free(buf);
  }
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
  char *buf = NULL;
  char variable[32];
  char value[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK || httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int val = atoi(value);
  log_i("%s = %d", variable, val);
  sensor_t *s = esp_camera_sensor_get();
  int res = 0;

  if (!strcmp(variable, "framesize")) {
    if (s->pixformat == PIXFORMAT_JPEG) {
      res = s->set_framesize(s, (framesize_t)val);
    }
  } else if (!strcmp(variable, "quality")) {
    res = s->set_quality(s, val);
  } else if (!strcmp(variable, "contrast")) {
    res = s->set_contrast(s, val);
  } else if (!strcmp(variable, "brightness")) {
    res = s->set_brightness(s, val);
  } else if (!strcmp(variable, "saturation")) {
    res = s->set_saturation(s, val);
  } else if (!strcmp(variable, "gainceiling")) {
    res = s->set_gainceiling(s, (gainceiling_t)val);
  } else if (!strcmp(variable, "colorbar")) {
    res = s->set_colorbar(s, val);
  } else if (!strcmp(variable, "awb")) {
    res = s->set_whitebal(s, val);
  } else if (!strcmp(variable, "agc")) {
    res = s->set_gain_ctrl(s, val);
  } else if (!strcmp(variable, "aec")) {
    res = s->set_exposure_ctrl(s, val);
  } else if (!strcmp(variable, "hmirror")) {
    res = s->set_hmirror(s, val);
  } else if (!strcmp(variable, "vflip")) {
    res = s->set_vflip(s, val);
  } else if (!strcmp(variable, "awb_gain")) {
    res = s->set_awb_gain(s, val);
  } else if (!strcmp(variable, "agc_gain")) {
    res = s->set_agc_gain(s, val);
  } else if (!strcmp(variable, "aec_value")) {
    res = s->set_aec_value(s, val);
  } else if (!strcmp(variable, "aec2")) {
    res = s->set_aec2(s, val);
  } else if (!strcmp(variable, "dcw")) {
    res = s->set_dcw(s, val);
  } else if (!strcmp(variable, "bpc")) {
    res = s->set_bpc(s, val);
  } else if (!strcmp(variable, "wpc")) {
    res = s->set_wpc(s, val);
  } else if (!strcmp(variable, "raw_gma")) {
    res = s->set_raw_gma(s, val);
  } else if (!strcmp(variable, "lenc")) {
    res = s->set_lenc(s, val);
  } else if (!strcmp(variable, "special_effect")) {
    res = s->set_special_effect(s, val);
  } else if (!strcmp(variable, "wb_mode")) {
    res = s->set_wb_mode(s, val);
  } else if (!strcmp(variable, "ae_level")) {
    res = s->set_ae_level(s, val);
  }
#if defined(LED_GPIO_NUM)
  else if (!strcmp(variable, "led_intensity")) {
    led_duty = val;
    enable_led(led_duty > 0);
  }
#endif
  else {
    log_i("Unknown command: %s", variable);
    res = -1;
  }

  if (res < 0) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static int print_reg(char *p, sensor_t *s, uint16_t reg, uint32_t mask) {
  return sprintf(p, "\"0x%x\":%u,", reg, s->get_reg(s, reg, mask));
}

static esp_err_t status_handler(httpd_req_t *req) {
  static char json_response[1280];

  sensor_t *s = esp_camera_sensor_get();
  char *p = json_response;
  *p++ = '{';

  if (s->id.PID == OV5640_PID || s->id.PID == OV3660_PID) {
    for (int reg = 0x3400; reg < 0x3406; reg += 2) {
      p += print_reg(p, s, reg, 0xFFF);  //12 bit
    }
    p += print_reg(p, s, 0x3406, 0xFF);

    p += print_reg(p, s, 0x3500, 0xFFFF0);  //16 bit
    p += print_reg(p, s, 0x3503, 0xFF);
    p += print_reg(p, s, 0x350a, 0x3FF);   //10 bit
    p += print_reg(p, s, 0x350c, 0xFFFF);  //16 bit

    for (int reg = 0x5480; reg <= 0x5490; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }

    for (int reg = 0x5380; reg <= 0x538b; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }

    for (int reg = 0x5580; reg < 0x558a; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }
    p += print_reg(p, s, 0x558a, 0x1FF);  //9 bit
  } else if (s->id.PID == OV2640_PID) {
    p += print_reg(p, s, 0xd3, 0xFF);
    p += print_reg(p, s, 0x111, 0xFF);
    p += print_reg(p, s, 0x132, 0xFF);
  }

  p += sprintf(p, "\"xclk\":%u,", s->xclk_freq_hz / 1000000);
  p += sprintf(p, "\"pixformat\":%u,", s->pixformat);
  p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
  p += sprintf(p, "\"quality\":%u,", s->status.quality);
  p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
  p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
  p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
  p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
  p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
  p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
  p += sprintf(p, "\"awb\":%u,", s->status.awb);
  p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
  p += sprintf(p, "\"aec\":%u,", s->status.aec);
  p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
  p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
  p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
  p += sprintf(p, "\"agc\":%u,", s->status.agc);
  p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
  p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
  p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
  p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
  p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
  p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
  p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
  p += sprintf(p, "\"vflip\":%u,", s->status.vflip);
  p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
  p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);
#if defined(LED_GPIO_NUM)
  p += sprintf(p, ",\"led_intensity\":%u", led_duty);
#else
  p += sprintf(p, ",\"led_intensity\":%d", -1);
#endif
  *p++ = '}';
  *p++ = 0;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json_response, strlen(json_response));
}

static esp_err_t xclk_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _xclk[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "xclk", _xclk, sizeof(_xclk)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int xclk = atoi(_xclk);
  log_i("Set XCLK: %d MHz", xclk);

  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_xclk(s, LEDC_TIMER_0, xclk);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t reg_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _reg[32];
  char _mask[32];
  char _val[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK || httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK
      || httpd_query_key_value(buf, "val", _val, sizeof(_val)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int reg = atoi(_reg);
  int mask = atoi(_mask);
  int val = atoi(_val);
  log_i("Set Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, val);

  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_reg(s, reg, mask, val);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t greg_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _reg[32];
  char _mask[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK || httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int reg = atoi(_reg);
  int mask = atoi(_mask);
  sensor_t *s = esp_camera_sensor_get();
  int res = s->get_reg(s, reg, mask);
  if (res < 0) {
    return httpd_resp_send_500(req);
  }
  log_i("Get Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, res);

  char buffer[20];
  const char *val = itoa(res, buffer, 10);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, val, strlen(val));
}

static int parse_get_var(char *buf, const char *key, int def) {
  char _int[16];
  if (httpd_query_key_value(buf, key, _int, sizeof(_int)) != ESP_OK) {
    return def;
  }
  return atoi(_int);
}

static esp_err_t pll_handler(httpd_req_t *req) {
  char *buf = NULL;

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }

  int bypass = parse_get_var(buf, "bypass", 0);
  int mul = parse_get_var(buf, "mul", 0);
  int sys = parse_get_var(buf, "sys", 0);
  int root = parse_get_var(buf, "root", 0);
  int pre = parse_get_var(buf, "pre", 0);
  int seld5 = parse_get_var(buf, "seld5", 0);
  int pclken = parse_get_var(buf, "pclken", 0);
  int pclk = parse_get_var(buf, "pclk", 0);
  free(buf);

  log_i("Set Pll: bypass: %d, mul: %d, sys: %d, root: %d, pre: %d, seld5: %d, pclken: %d, pclk: %d", bypass, mul, sys, root, pre, seld5, pclken, pclk);
  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_pll(s, bypass, mul, sys, root, pre, seld5, pclken, pclk);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t win_handler(httpd_req_t *req) {
  char *buf = NULL;

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }

  int startX = parse_get_var(buf, "sx", 0);
  int startY = parse_get_var(buf, "sy", 0);
  int endX = parse_get_var(buf, "ex", 0);
  int endY = parse_get_var(buf, "ey", 0);
  int offsetX = parse_get_var(buf, "offx", 0);
  int offsetY = parse_get_var(buf, "offy", 0);
  int totalX = parse_get_var(buf, "tx", 0);
  int totalY = parse_get_var(buf, "ty", 0);  // codespell:ignore totaly
  int outputX = parse_get_var(buf, "ox", 0);
  int outputY = parse_get_var(buf, "oy", 0);
  bool scale = parse_get_var(buf, "scale", 0) == 1;
  bool binning = parse_get_var(buf, "binning", 0) == 1;
  free(buf);

  log_i(
    "Set Window: Start: %d %d, End: %d %d, Offset: %d %d, Total: %d %d, Output: %d %d, Scale: %u, Binning: %u", startX, startY, endX, endY, offsetX, offsetY,
    totalX, totalY, outputX, outputY, scale, binning  // codespell:ignore totaly
  );
  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_res_raw(s, startX, startY, endX, endY, offsetX, offsetY, totalX, totalY, outputX, outputY, scale, binning);  // codespell:ignore totaly
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t save_handler(httpd_req_t *req) {
  String savedPath;
  bool ok = saveJpegFrameToSd(&savedPath);

  String response = "{\"ok\":";
  response += ok ? "true" : "false";
  response += ",\"sdReady\":";
  response += g_sdReady ? "true" : "false";
  response += ",\"path\":\"";
  response += ok ? savedPath : "";
  response += "\"}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, response.c_str(), response.length());
}

static esp_err_t storage_handler(httpd_req_t *req) {
  char *buf = NULL;
  String action;
  String actionMessage = g_sdStatusMessage;
  size_t removed = 0;

  if (httpd_req_get_url_query_len(req) > 0 && parse_get(req, &buf) == ESP_OK) {
    char actionBuf[24] = {0};
    httpd_query_key_value(buf, "action", actionBuf, sizeof(actionBuf) - 1);
    action = actionBuf;
    free(buf);
  }

  if (action == "remount") {
    remountMicroSD();
    actionMessage = g_sdStatusMessage;
  } else if (action == "test") {
    testMicroSD(&actionMessage);
  } else if (action == "wipe") {
    removed = clearCaptures(&actionMessage);
  }

  uint64_t totalMB = g_sdReady ? (SD_MMC.totalBytes() / (1024 * 1024)) : 0;
  uint64_t freeMB  = g_sdReady ? ((SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024)) : 0;

  String response = "{";
  response += "\"ok\":true,";
  response += "\"sdReady\":";
  response += g_sdReady ? "true" : "false";
  response += ",\"totalMB\":";
  response += String((unsigned long)totalMB);
  response += ",\"freeMB\":";
  response += String((unsigned long)freeMB);
  response += ",\"recordingStatus\":\"";
  response += g_recordingStatus;
  response += "\",\"recordingFile\":\"";
  response += g_recordingFile;
  response += "\",\"message\":\"";
  response += actionMessage;
  response += "\",\"removed\":";
  response += String((unsigned long)removed);
  response += "}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, response.c_str(), response.length());
}

static void append_json_escaped(String &json, const String &src) {
  for (size_t i = 0; i < src.length(); ++i) {
    const char c = src[i];
    if (c == '\\') {
      json += "\\\\";
    } else if (c == '"') {
      json += "\\\"";
    } else if (c == '\n') {
      json += "\\n";
    } else if (c == '\r') {
      json += "\\r";
    } else if ((unsigned char)c < 0x20) {
      json += ' ';
    } else {
      json += c;
    }
  }
}

class CctvLinePrint : public Print {
 public:
  explicit CctvLinePrint(String &b) : b_(b) {}
  size_t write(uint8_t c) override {
    b_ += (char)c;
    return 1;
  }

 private:
  String &b_;
};

static esp_err_t api_console_handler(httpd_req_t *req) {
  if (httpd_req_get_url_query_len(req) < 1) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cmd query required");
    return ESP_FAIL;
  }
  const size_t buf_len = httpd_req_get_url_query_len(req) + 1;
  char *buf = (char *)malloc(buf_len);
  if (!buf || httpd_req_get_url_query_str(req, buf, buf_len) != ESP_OK) {
    free(buf);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  char cmd[384];
  if (httpd_query_key_value(buf, "cmd", cmd, sizeof(cmd)) != ESP_OK) {
    free(buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cmd required");
    return ESP_FAIL;
  }
  free(buf);
  String line = String(cmd);
  line.trim();
  if (line.length() == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty cmd");
    return ESP_FAIL;
  }
  String out;
  CctvLinePrint lp(out);
  cctv_web_control_dispatch(line, lp);
  if (cctv_ui_log_get()) {
    String entry = String("> ") + line + "\n" + out;
    if (entry.length() == 0 || entry.charAt(entry.length() - 1) != '\n') {
      entry += "\n";
    }
    cctv_ui_log_append(entry);
  }
  String json;
  json.reserve(out.length() + (out.length() >> 2u) + 48);
  json = "{\"ok\":true,\"out\":\"";
  append_json_escaped(json, out);
  json += "\"}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t api_logs_handler(httpd_req_t *req) {
  if (httpd_req_get_url_query_len(req) > 0) {
    const size_t q_len = httpd_req_get_url_query_len(req) + 1;
    char *qbuf = (char *)malloc(q_len);
    if (qbuf && httpd_req_get_url_query_str(req, qbuf, q_len) == ESP_OK) {
      char v[8];
      if (httpd_query_key_value(qbuf, "enable", v, sizeof(v)) == ESP_OK) {
        cctv_ui_log_set(atoi(v) != 0);
      }
      if (httpd_query_key_value(qbuf, "clear", v, sizeof(v)) == ESP_OK && atoi(v) != 0) {
        cctv_ui_log_clear();
      }
    }
    free(qbuf);
  }
  String snap;
  cctv_ui_log_snapshot(snap);
  String json = "{\"enabled\":";
  json += cctv_ui_log_get() ? "true" : "false";
  json += ",\"text\":\"";
  append_json_escaped(json, snap);
  json += "\"}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  const char *html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 CCTV</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh}
.c{max-width:920px;margin:0 auto;padding:16px}
h1{font-size:1.4rem;font-weight:700;color:#f8fafc;margin-bottom:3px}
.sub{font-size:.82rem;color:#64748b;margin-bottom:16px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}
.grid3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;margin-bottom:12px}
.card{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:14px}
.card h2{font-size:.76rem;font-weight:700;color:#64748b;text-transform:uppercase;letter-spacing:.06em;margin-bottom:10px}
.row{display:flex;justify-content:space-between;align-items:center;padding:5px 0;border-bottom:1px solid #0f172a;font-size:.84rem}
.row:last-child{border-bottom:none}
.lbl{color:#94a3b8}
.badge{display:inline-block;padding:2px 9px;border-radius:20px;font-size:.72rem;font-weight:700}
.g{background:#166534;color:#86efac}.r{background:#7f1d1d;color:#fca5a5}.y{background:#713f12;color:#fde68a}.b{background:#1e3a5f;color:#93c5fd}
.btns{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px;margin-top:0}
button{padding:9px 14px;border:none;border-radius:8px;font-size:.82rem;font-weight:600;cursor:pointer;transition:opacity .15s}
button:hover{opacity:.82}
.bp{background:#3b82f6;color:#fff}.bd{background:#ef4444;color:#fff}.bn{background:#334155;color:#e2e8f0}.bw{background:#0ea5e9;color:#fff}
.sm{padding:5px 10px;font-size:.76rem;border-radius:6px}
#msg{margin-top:8px;font-size:.8rem;color:#94a3b8;min-height:16px}
.ftbl{width:100%;border-collapse:collapse}
.ftbl th{padding:7px 12px;text-align:left;font-size:.7rem;color:#64748b;text-transform:uppercase;letter-spacing:.05em;background:#0f172a;position:sticky;top:0;z-index:1}
.ftbl td{padding:8px 12px;font-size:.8rem;border-top:1px solid #1e293b}
.ftbl tr:hover td{background:#1e293b88}
.mono{font-family:monospace;font-size:.76rem}
.acts{display:flex;gap:5px;flex-wrap:wrap}
.empty{text-align:center;padding:36px;color:#475569;font-size:.85rem}
.fhdr{padding:10px 14px;border-bottom:1px solid #334155;display:flex;justify-content:space-between;align-items:center}
.fscroll{max-height:340px;overflow-y:auto}
.panel-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:10px}
.panel-grid .full{grid-column:1/-1}
.inp-lbl{font-size:.75rem;color:#94a3b8;display:block;margin-bottom:3px}
.inp{width:100%;background:#0f172a;border:1px solid #334155;color:#e2e8f0;border-radius:6px;padding:7px 10px;font-size:.82rem}
.inp:focus{outline:none;border-color:#3b82f6}
.pnl-msg{margin-top:6px;font-size:.78rem;color:#94a3b8;min-height:14px}
.toast{position:fixed;bottom:18px;right:18px;background:#1e293b;border:1px solid #334155;border-radius:8px;padding:10px 16px;font-size:.82rem;opacity:0;transition:opacity .3s;pointer-events:none;z-index:99}
.toast.show{opacity:1}
.preview-wrap{position:relative;width:100%;background:#000;border-radius:10px;overflow:hidden;min-height:180px;display:flex;align-items:center;justify-content:center}
#prevImg{width:100%;display:block;border-radius:10px;max-height:340px;object-fit:contain}
.prev-off{position:absolute;color:#475569;font-size:.85rem;text-align:center}
/* CPU/RAM bar */
.bar-wrap{background:#0f172a;border-radius:4px;height:8px;overflow:hidden;margin-top:4px}
.bar-fill{height:8px;border-radius:4px;transition:width .5s}
.bar-cpu{background:#f59e0b}
.bar-ram{background:#3b82f6}
@media(max-width:580px){.grid{grid-template-columns:1fr}.grid3{grid-template-columns:1fr}.panel-grid{grid-template-columns:1fr}.panel-grid .full{grid-column:1}}
</style>
</head>
<body>
<div class="c">
  <h1>&#128247; ESP32-S3 CCTV Recorder</h1>
  <div id="deviceClockRow" style="margin-bottom:10px;padding:8px 12px;background:#0f172a;border:1px solid #334155;border-radius:8px;display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px">
    <span style="font-size:.72rem;font-weight:700;color:#64748b;text-transform:uppercase;letter-spacing:.06em">Device time</span>
    <span id="deviceClock" class="mono" style="font-size:1.05rem;font-weight:700;color:#7dd3fc">--</span>
  </div>
  <p class="sub" id="ipLbl">Loading...</p>

  <!-- Live Preview -->
  <div class="card" style="margin-bottom:12px">
    <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:10px">
      <h2 style="margin:0">&#128065; Live Preview</h2>
      <div style="display:flex;gap:8px;align-items:center">
        <span id="prevFps" style="font-size:.75rem;color:#64748b"></span>
        <button id="prevToggle" class="bn sm" onclick="togglePreview()">&#9654; Start</button>
      </div>
    </div>
    <div class="preview-wrap">
      <img id="prevImg" alt="" style="display:none">
      <div class="prev-off" id="prevOff">Preview off &mdash; click Start</div>
    </div>
  </div>

  <!-- Status grid -->
  <div class="grid" style="margin-bottom:12px">
    <div class="card">
      <h2>Recording</h2>
      <div class="row"><span class="lbl">Status</span><span id="recBadge" class="badge y">&#8226; Connecting</span></div>
      <div class="row"><span class="lbl">Current file</span><span id="recFile" class="mono" style="color:#94a3b8;max-width:60%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">-</span></div>
    </div>
    <div class="card">
      <h2>Storage</h2>
      <div class="row"><span class="lbl">SD card</span><span id="sdBadge" class="badge y">-</span></div>
      <div class="row"><span class="lbl">Free / Total</span><span id="sdSpace" style="font-weight:600">-</span></div>
      <div class="row"><span class="lbl">WiFi</span><span id="wifiBadge" class="badge y">-</span></div>
    </div>
  </div>

  <!-- CPU / RAM -->
  <div class="grid3" style="margin-bottom:12px">
    <div class="card">
      <h2>CPU Usage</h2>
      <div style="display:flex;justify-content:space-between;font-size:.82rem;margin-bottom:2px">
        <span class="lbl">Core 0</span><span id="cpu0Pct" style="color:#f59e0b;font-weight:700">-</span>
      </div>
      <div class="bar-wrap"><div class="bar-fill bar-cpu" id="cpu0Bar" style="width:0%"></div></div>
      <div style="display:flex;justify-content:space-between;font-size:.82rem;margin-top:6px;margin-bottom:2px">
        <span class="lbl">Core 1</span><span id="cpu1Pct" style="color:#f59e0b;font-weight:700">-</span>
      </div>
      <div class="bar-wrap"><div class="bar-fill bar-cpu" id="cpu1Bar" style="width:0%"></div></div>
    </div>
    <div class="card">
      <h2>RAM (Heap)</h2>
      <div style="display:flex;justify-content:space-between;font-size:.82rem;margin-bottom:2px">
        <span class="lbl">Free</span><span id="ramFree" style="color:#3b82f6;font-weight:700">-</span>
      </div>
      <div class="bar-wrap"><div class="bar-fill bar-ram" id="ramBar" style="width:0%"></div></div>
      <div style="font-size:.75rem;color:#64748b;margin-top:5px">
        Total: <span id="ramTotal">-</span> &nbsp; Min: <span id="ramMin">-</span>
      </div>
    </div>
    <div class="card">
      <h2>PSRAM</h2>
      <div style="display:flex;justify-content:space-between;font-size:.82rem;margin-bottom:2px">
        <span class="lbl">Free</span><span id="psramFree" style="color:#a78bfa;font-weight:700">-</span>
      </div>
      <div class="bar-wrap"><div class="bar-fill" style="background:#7c3aed" id="psramBar" style="width:0%"></div></div>
      <div style="font-size:.75rem;color:#64748b;margin-top:5px">
        Total: <span id="psramTotal">-</span>
      </div>
    </div>
  </div>

  <!-- WiFi Config -->
  <div class="card" style="margin-bottom:12px">
    <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:4px">
      <h2 style="margin:0">&#128246; WiFi</h2>
      <button class="bn sm" onclick="togglePanel('wifiPanel',loadWifiConfig)">&#9881; Configure</button>
    </div>
    <div id="wifiStatusRow" style="font-size:.8rem;color:#94a3b8;margin-top:4px">-</div>
    <div id="wifiPanel" style="display:none;margin-top:10px">
      <div class="panel-grid">
        <div class="full">
          <label class="inp-lbl">Mode</label>
          <div style="display:flex;gap:10px;margin-bottom:6px">
            <label style="font-size:.82rem;cursor:pointer"><input type="radio" name="wMode" value="0" onchange="wModeChange(0)" checked> Normal (WPA/WPA2/WPA3-Personal)</label>
            <label style="font-size:.82rem;cursor:pointer"><input type="radio" name="wMode" value="1" onchange="wModeChange(1)"> Enterprise (WPA-EAP / LDAP)</label>
          </div>
        </div>
        <div class="full" id="scanResultsRow" style="display:none">
          <label class="inp-lbl">Scan Results &mdash; click to select</label>
          <div id="scanResults" style="background:#0f172a;border:1px solid #334155;border-radius:6px;max-height:180px;overflow-y:auto;font-size:.78rem"></div>
        </div>
        <div class="full"><label class="inp-lbl">SSID (Network Name)</label>
          <div style="display:flex;gap:6px">
            <input id="wSsid" class="inp" type="text" placeholder="YourSSID" style="flex:1">
            <button class="bn sm" onclick="scanWifi()" id="scanBtn">&#128225; Scan</button>
          </div>
        </div>
        <div id="wPassRow"><label class="inp-lbl">Password</label><input id="wPass" class="inp" type="password" placeholder="(blank = keep current)"></div>
        <div id="wIdentRow" style="display:none"><label class="inp-lbl">LDAP Username (Identity)</label><input id="wIdent" class="inp" type="text" placeholder="user@domain.com"></div>
        <div id="wEapPassRow" style="display:none"><label class="inp-lbl">LDAP Password</label><input id="wEapPass" class="inp" type="password" placeholder="(blank = keep current)"></div>
        <div class="full">
          <label class="inp-lbl">Save to profile slot (up to 3 networks; device tries preferred first, then others)</label>
          <select id="wSlot" class="inp" style="max-width:220px">
            <option value="0">Slot 1 (primary)</option>
            <option value="1">Slot 2</option>
            <option value="2">Slot 3</option>
          </select>
        </div>
        <div class="full">
          <label class="inp-lbl">Saved profiles</label>
          <div id="wifiProfList" style="font-size:.78rem;background:#0f172a;border:1px solid #334155;border-radius:6px;padding:8px;min-height:2.5rem"></div>
        </div>
      </div>
      <div style="display:flex;gap:8px;flex-wrap:wrap">
        <button class="bw sm" onclick="saveWifi(false)">&#10003; Save Config</button>
        <button class="bp sm" onclick="saveWifi(true)">&#8635; Save &amp; Reconnect</button>
        <button class="bd sm" onclick="clearWifiCfg()">&#128465; Clear WiFi</button>
      </div>
      <div class="pnl-msg" id="wifiMsg"></div>
    </div>
  </div>

  <!-- Controls -->
  <div class="card" style="margin-bottom:12px">
    <h2>Controls</h2>
    <div class="btns">
      <button class="bp" onclick="saveSnap()">&#128247; Snapshot</button>
      <button class="bn" onclick="sdAct('remount')">&#8635; Re-mount SD</button>
      <button class="bn" onclick="sdAct('test')">&#10003; Test SD</button>
      <button class="bd" onclick="if(confirm('Delete ALL recordings?'))sdAct('wipe')">&#128465; Wipe All</button>
      <button class="bd" onclick="rebootDevice()">&#9211; Reboot</button>
    </div>
    <div id="msg"></div>
  </div>

  <!-- Device control (serial commands via HTTP) -->
  <div class="card" style="margin-bottom:12px">
    <h2 style="margin-bottom:6px">&#9881; Device control</h2>
    <p style="font-size:.78rem;color:#94a3b8;margin:0 0 8px 0">One line per command (same as old Serial). Run <b>wifiscan</b> before <b>12 norm pass:...</b> style joins.</p>
    <div style="display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px">
      <button type="button" class="bn sm" onclick="devQuick('wifistatus')">WiFi status</button>
      <button type="button" class="bn sm" onclick="devQuick('devstatus')">Device status</button>
      <button type="button" class="bn sm" onclick="devQuick('wifiscan')">WiFi scan</button>
      <button type="button" class="bn sm" onclick="devQuick('ethdhcp')">ETH DHCP</button>
      <button type="button" class="bn sm" onclick="devQuick('ethstatic')">ETH static (10.0.0.20)</button>
      <button type="button" class="bn sm" onclick="devQuick('timeurl')">Time URL</button>
      <button type="button" class="bn sm" onclick="devQuick('clearwifi')">Clear WiFi</button>
      <button type="button" class="bn sm" onclick="devQuick('wifiprofiles')">WiFi profiles</button>
      <button type="button" class="bd sm" onclick="devQuick('reboot')">Reboot</button>
    </div>
    <label class="inp-lbl">Command</label>
    <input id="devCmdIn" class="inp" type="text" maxlength="360" placeholder="wifiprofiles / wifidel 0 / ethstatic …" autocomplete="off">
    <div style="display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:10px">
      <button type="button" class="bp sm" onclick="devRun()">Run command</button>
      <label style="font-size:.8rem;cursor:pointer;display:flex;align-items:center;gap:6px">
        <input type="checkbox" id="devLogEn" onchange="devLogToggle()"> Session log (~16KB RAM, polled)</label>
      <button type="button" class="bn sm" id="devLogClr" style="display:none" onclick="devLogClear()">Clear log</button>
    </div>
    <pre id="devOut" class="mono" style="display:none;margin-top:10px;background:#0f172a;border:1px solid #334155;border-radius:6px;padding:10px;font-size:.72rem;max-height:180px;overflow:auto;white-space:pre-wrap;color:#cbd5e1"></pre>
    <div id="devLogWrap" style="display:none;margin-top:10px">
      <div style="font-size:.75rem;color:#64748b;margin-bottom:4px">Session log</div>
      <pre id="devLogView" class="mono" style="background:#0f172a;border:1px solid #334155;border-radius:6px;padding:10px;font-size:.72rem;max-height:220px;overflow:auto;white-space:pre-wrap;color:#94a3b8"></pre>
    </div>
  </div>

  <!-- File list -->
  <div class="card" style="padding:0;overflow:hidden">
    <div class="fhdr">
      <h2 style="margin:0">Recorded Files</h2>
      <button class="bn sm" onclick="loadFiles()">&#8635; Refresh</button>
    </div>
    <div class="fscroll">
      <div id="fbody"><div class="empty">Loading...</div></div>
    </div>
  </div>
</div>
<div class="toast" id="toast"></div>
<script>
function toast(m,d=3000){const t=document.getElementById('toast');t.textContent=m;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),d);}
function fmtSz(b){if(b>=1073741824)return(b/1073741824).toFixed(1)+' GB';if(b>=1048576)return(b/1048576).toFixed(1)+' MB';if(b>=1024)return(b/1024).toFixed(1)+' KB';return b+' B';}

async function devQuick(c){document.getElementById('devCmdIn').value=c;await devRun();}
async function devRun(){
  const cmd=document.getElementById('devCmdIn').value.trim();
  if(!cmd){toast('Enter a command');return;}
  const out=document.getElementById('devOut');
  try{
    const r=await fetch('/api/console?cmd='+encodeURIComponent(cmd));
    const j=await r.json();
    out.style.display='block';
    out.textContent=(j&&j.out)?j.out:'(no output)';
  }catch(e){out.style.display='block';out.textContent='Error: '+e.message;}
}
async function devLogRefresh(){
  try{
    const j=await fetch('/api/logs').then(r=>r.json());
    document.getElementById('devLogView').textContent=(j&&j.text)?j.text:'';
  }catch(e){}
}
async function devLogToggle(){
  const on=document.getElementById('devLogEn').checked;
  await fetch('/api/logs?enable='+(on?'1':'0'));
  const wrap=document.getElementById('devLogWrap');
  const clr=document.getElementById('devLogClr');
  wrap.style.display=on?'block':'none';
  clr.style.display=on?'inline-block':'none';
  if(on){
    await devLogRefresh();
    if(!window._devLogIv) window._devLogIv=setInterval(devLogRefresh,2000);
  }else{
    if(window._devLogIv){clearInterval(window._devLogIv); window._devLogIv=null;}
  }
}
async function devLogClear(){
  await fetch('/api/logs?clear=1');
  await devLogRefresh();
  toast('Log cleared');
}

(function(){
  const el=document.getElementById('ipLbl');
  if(location.protocol!=='http:'){
    el.innerHTML='<span style="color:#f87171;font-weight:700">Use <b>http://</b> not https — this board has no HTTPS.</span><br><span style="font-size:.78rem;color:#94a3b8">Then open: http://'+location.hostname+'/</span>';
  }else{
    el.textContent='http://'+location.hostname+'/ \u00B7 Same dashboard on Ethernet or Wi\u2011Fi IP \u00B7 Live: /mjpeg (VLC) \u00B7 /stream';
  }
})();

function togglePanel(id, onOpen){
  const p=document.getElementById(id);
  const show=p.style.display==='none';
  p.style.display=show?'block':'none';
  if(show && onOpen) onOpen();
}

// ── Status refresh ─────────────────────────────────────────────────────────
async function refreshStatus(){
  try{
    const [stor,wifi,sysinfo]=await Promise.all([
      fetch('/storage').then(r=>r.json()).catch(()=>({})),
      fetch('/wifi').then(r=>r.json()).catch(()=>({})),
      fetch('/sysinfo').then(r=>r.json()).catch(()=>({}))
    ]);

    // Recording
    const rs=stor.recordingStatus||'';
    const rb=document.getElementById('recBadge');
    if(rs.startsWith('REC ')){rb.textContent='\u25cf REC';rb.className='badge g';}
    else if(rs.includes('full')||rs.includes('Cannot')){rb.textContent='\u2717 Error';rb.className='badge r';}
    else{rb.textContent=rs.length>28?rs.slice(0,28)+'\u2026':rs||'Idle';rb.className='badge y';}
    const rf=(stor.recordingFile||'').replace('/captures/','');
    document.getElementById('recFile').textContent=rf||'-';

    // SD
    const sb=document.getElementById('sdBadge');
    if(stor.sdReady){sb.textContent='Mounted';sb.className='badge g';}else{sb.textContent='Not mounted';sb.className='badge r';}
    document.getElementById('sdSpace').textContent=stor.sdReady?(stor.freeMB+' / '+stor.totalMB+' MB'):'-';

    // WiFi badge (also shows Ethernet-only: LAN config works without Wi\u2011Fi)
    const ws=wifi.status||'';
    const wb=document.getElementById('wifiBadge');
    const ethOk=wifi.ethIp && wifi.ethIp.length>0 && wifi.ethIp!=='0.0.0.0';
    if(ws.startsWith('Connected')){wb.textContent='\u2714 '+(wifi.wifiIp||wifi.ip);wb.className='badge g';}
    else if(ethOk){wb.textContent='\u2714 ETH '+wifi.ethIp;wb.className='badge g';}
    else if(ws.includes('Retry')||ws.includes('retry')||ws.includes('Connecting')){wb.textContent='\u23F3 '+ws.slice(0,20);wb.className='badge y';}
    else if(ws.includes('failed')||ws.includes('Disconnected')||ws.includes('Failed')){wb.textContent='\u2717 No Wi\u2011Fi';wb.className='badge r';}
    else{wb.textContent=ws.length>22?ws.slice(0,22)+'\u2026':ws||'-';wb.className='badge y';}
    document.getElementById('wifiStatusRow').textContent=ws;

    // Network URLs (HTTP only) — same UI on LAN or Wi\u2011Fi
    let netHtml='';
    if(location.protocol!=='http:'){
      netHtml='<span style="color:#f87171;font-weight:700">Open <b>http://</b>'+location.host+'/ (not https)</span><br>';
    }
    netHtml+='<span style="font-size:.78rem">This browser: <span class="mono">http://'+location.hostname+'/</span></span>';
    if(wifi.ethIp && wifi.ethIp.length>0 && wifi.ethIp!=='0.0.0.0'){
      netHtml+='<br><span style="font-size:.78rem">Ethernet: <a class="mono" style="color:#38bdf8" href="http://'+wifi.ethIp+'/">http://'+wifi.ethIp+'/</a> (ethLink='+(wifi.ethLink?'yes':'no')+')</span>';
    }
    if(wifi.wifiIp && wifi.wifiIp.length>0 && wifi.wifiIp!=='0.0.0.0'){
      netHtml+='<br><span style="font-size:.78rem">Wi\u2011Fi STA: <a class="mono" style="color:#38bdf8" href="http://'+wifi.wifiIp+'/">http://'+wifi.wifiIp+'/</a></span>';
    }
    if(location.protocol==='http:'){
      netHtml+='<br><span style="font-size:.72rem;color:#64748b">Live: /mjpeg or /stream (VLC) \u00B7 Primary IP (API): '+((wifi.ip&&wifi.ip!=='0.0.0.0')?wifi.ip:'-')+'</span>';
    }
    document.getElementById('ipLbl').innerHTML=netHtml;

    const dc=document.getElementById('deviceClock');
    if(dc && sysinfo.deviceTime!==undefined){
      dc.textContent=sysinfo.deviceTime;
      dc.style.color=sysinfo.timeOk?'#7dd3fc':'#f87171';
    }

    // CPU
    if(sysinfo.cpu0!==undefined){
      const c0=Math.min(sysinfo.cpu0,100), c1=Math.min(sysinfo.cpu1,100);
      document.getElementById('cpu0Pct').textContent=c0+'%';
      document.getElementById('cpu1Pct').textContent=c1+'%';
      document.getElementById('cpu0Bar').style.width=c0+'%';
      document.getElementById('cpu1Bar').style.width=c1+'%';
    }
    // RAM
    if(sysinfo.heapFree!==undefined){
      const hf=sysinfo.heapFree, ht=sysinfo.heapTotal, hm=sysinfo.heapMin;
      document.getElementById('ramFree').textContent=fmtSz(hf);
      document.getElementById('ramTotal').textContent=fmtSz(ht);
      document.getElementById('ramMin').textContent=fmtSz(hm);
      const pct=ht>0?Math.round((1-hf/ht)*100):0;
      document.getElementById('ramBar').style.width=pct+'%';
    }
    // PSRAM
    if(sysinfo.psramFree!==undefined){
      const pf=sysinfo.psramFree, pt=sysinfo.psramTotal;
      document.getElementById('psramFree').textContent=fmtSz(pf);
      document.getElementById('psramTotal').textContent=fmtSz(pt);
      const pct=pt>0?Math.round((1-pf/pt)*100):0;
      document.getElementById('psramBar').style.width=pct+'%';
    }
  }catch(e){console.error(e);}
}

// ── WiFi config panel ──────────────────────────────────────────────────────
let wEnterprise=false;
function wModeChange(v){
  wEnterprise=v===1;
  document.getElementById('wPassRow').style.display=wEnterprise?'none':'block';
  document.getElementById('wIdentRow').style.display=wEnterprise?'block':'none';
  document.getElementById('wEapPassRow').style.display=wEnterprise?'block':'none';
}
function renderWifiProfiles(w){
  const el=document.getElementById('wifiProfList');
  if(!el) return;
  const profs=Array.isArray(w.profiles)?w.profiles:[];
  if(profs.length===0){
    el.innerHTML='<span style="color:#64748b">No profile data</span>';
    return;
  }
  let h='';
  for(const p of profs){
    const ssid=(p.ssid!=null&&String(p.ssid).length)?String(p.ssid):'(empty)';
    const pref=p.pref?'<span style="color:#4ade80;margin-left:6px">preferred</span>':'';
    const kind=p.enterprise?'EAP':'WPA';
    const delBtn=(p.ssid!=null&&String(p.ssid).length)
      ? '<button type="button" class="bd sm" onclick="deleteWifiSlot('+p.slot+')">Delete</button>'
      : '';
    h+='<div style="display:flex;justify-content:space-between;align-items:center;gap:8px;padding:6px 0;border-bottom:1px solid #1e293b">'+
      '<span><b>#'+(p.slot+1)+'</b> <span class="mono">'+ssid.replace(/&/g,'&amp;').replace(/</g,'&lt;')+'</span>'+pref+
      ' <span style="color:#64748b">'+kind+'</span></span>'+delBtn+'</div>';
  }
  el.innerHTML=h;
}
async function deleteWifiSlot(slot){
  if(!confirm('Delete saved Wi\u2011Fi profile slot '+(slot+1)+'?')) return;
  try{
    await fetch('/wifi?delslot='+encodeURIComponent(slot)).then(r=>r.json());
    toast('Profile deleted');
    await loadWifiConfig();
    refreshStatus();
  }catch(e){ toast('Error: '+e.message); }
}
async function loadWifiConfig(){
  try{
    const w=await fetch('/wifi').then(r=>r.json());
    document.getElementById('wSsid').value=w.ssid||'';
    document.getElementById('wIdent').value=w.identity||'';
    document.getElementById('wPass').value='';
    document.getElementById('wEapPass').value='';
    wEnterprise=w.enterprise||false;
    const radios=document.getElementsByName('wMode');
    radios[0].checked=!wEnterprise; radios[1].checked=wEnterprise;
    wModeChange(wEnterprise?1:0);
    const ps=(typeof w.prefSlot==='number')?w.prefSlot:parseInt(w.prefSlot,10)||0;
    document.getElementById('wSlot').value=String(Math.max(0,Math.min(2,ps)));
    renderWifiProfiles(w);
    let hint='';
    if(wEnterprise) hint=(w.eapPassSet?'EAP password saved':'No EAP password');
    else hint=(w.passSet?'Password saved':'No password saved');
    document.getElementById('wifiMsg').textContent=hint;
  }catch(e){}
}
async function saveWifi(reconnect){
  const ssid=document.getElementById('wSsid').value.trim();
  if(!ssid){document.getElementById('wifiMsg').textContent='SSID is required';return;}
  let url='/wifi?ssid='+encodeURIComponent(ssid)+'&ent='+(wEnterprise?1:0);
  if(wEnterprise){
    const id=document.getElementById('wIdent').value.trim();
    const ep=document.getElementById('wEapPass').value;
    url+='&id='+encodeURIComponent(id);
    if(ep) url+='&ep='+encodeURIComponent(ep);
  } else {
    const p=document.getElementById('wPass').value;
    if(p) url+='&pass='+encodeURIComponent(p);
  }
  const slotEl=document.getElementById('wSlot');
  if(slotEl) url+='&slot='+encodeURIComponent(slotEl.value);
  if(reconnect) url+='&reconnect=1';
  try{
    const w=await fetch(url).then(r=>r.json());
    renderWifiProfiles(w);
    if(reconnect){
      document.getElementById('wifiMsg').textContent='Reconnecting (saved profiles / failover). Watch status badge; if IP changes, open the new URL.';
      toast('Reconnecting… if IP changes, use the new http:// address',8000);
    } else {
      document.getElementById('wifiMsg').textContent='Saved. '+w.status;
      toast('WiFi config saved');
    }
  }catch(e){document.getElementById('wifiMsg').textContent='Error: '+e.message;}
}

async function scanWifi(){
  const btn=document.getElementById('scanBtn');
  const row=document.getElementById('scanResultsRow');
  const res=document.getElementById('scanResults');
  btn.disabled=true; btn.textContent='Scanning...';
  res.innerHTML='<div style="padding:8px;color:#64748b">Scanning... please wait ~5s</div>';
  row.style.display='block';
  try{
    const nets=await fetch('/wifiscan').then(r=>r.json());
    if(!nets||nets.length===0){
      res.innerHTML='<div style="padding:8px;color:#64748b">No networks found. Check antenna.</div>';
    } else {
      let h='';
      for(const n of nets){
        const bars=n.rssi>=-55?'▪▪▪▪':n.rssi>=-70?'▪▪▪○':n.rssi>=-80?'▪▪○○':'▪○○○';
        const ent=n.auth.includes('ENT');
        const safeSsid=n.ssid.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
        h+=`<div style="padding:7px 10px;border-bottom:1px solid #1e293b;cursor:pointer;display:flex;justify-content:space-between;align-items:center"
              onmouseover="this.style.background='#1e293b'" onmouseout="this.style.background=''"
              data-ssid="${safeSsid}" data-ent="${ent?1:0}" onclick="selectSsidFromEl(this)">
            <span>${safeSsid}</span>
            <span style="color:#64748b;white-space:nowrap;margin-left:8px">${bars} ${n.rssi}dBm &nbsp;<span style="color:#93c5fd">${n.auth}</span></span>
          </div>`;
      }
      res.innerHTML=h;
    }
  }catch(e){res.innerHTML='<div style="padding:8px;color:#ef4444">Scan error: '+e.message+'</div>';}
  btn.disabled=false; btn.textContent='\uD83D\uDCE1 Scan';
}

function selectSsidFromEl(el){selectSsid(el.dataset.ssid,parseInt(el.dataset.ent)||0);}
function selectSsidFromEl(el){selectSsid(el.dataset.ssid,parseInt(el.dataset.ent)||0);}
function selectSsid(ssid, isEnt){
  document.getElementById('wSsid').value=ssid;
  document.getElementById('scanResultsRow').style.display='none';
  if(isEnt){ const r=document.getElementsByName('wMode'); r[1].checked=true; wModeChange(1); }
  else      { const r=document.getElementsByName('wMode'); r[0].checked=true; wModeChange(0); }
  toast('Selected: '+ssid);
}

async function clearWifiCfg(){
  if(!confirm('Clear saved WiFi config?')) return;
  try{
    await fetch('/wifi?clear=1');
    document.getElementById('wSsid').value='';
    document.getElementById('wPass').value='';
    document.getElementById('wIdent').value='';
    document.getElementById('wEapPass').value='';
    document.getElementById('wifiMsg').textContent='WiFi config cleared.';
    toast('WiFi config cleared');
    await loadWifiConfig();
  }catch(e){document.getElementById('wifiMsg').textContent='Error: '+e.message;}
}

async function rebootDevice(){
  if(!confirm('Reboot device?')) return;
  try{
    await fetch('/reboot');
    toast('Rebooting... reopen page in ~15s',10000);
    document.getElementById('msg').textContent='Rebooting — please wait ~15s then reopen dashboard.';
  }catch(e){ toast('Reboot sent (connection lost is normal)'); }
}

// ── File list ──────────────────────────────────────────────────────────────
async function loadFiles(){
  const b=document.getElementById('fbody');
  try{
    const files=await fetch('/files').then(r=>r.json());
    if(!files||files.length===0){b.innerHTML='<div class="empty">No recordings yet</div>';return;}
    let h='<table class="ftbl"><thead><tr><th>Filename</th><th>Size</th><th>Actions</th></tr></thead><tbody>';
    for(const f of files){
      const n=encodeURIComponent(f.name);
      const safen=f.name.replace(/'/g,"\\'").replace(/"/g,'&quot;');
      h+=`<tr data-name="${f.name.replace(/"/g,'&quot;')}">
        <td class="mono">${f.name}</td>
        <td>${fmtSz(f.size)}</td>
        <td class="acts">
          <a href="/download?f=${n}" download="${f.name}"><button class="bp sm" title="Download">&#8595; Download</button></a>
          <button class="bd sm" title="Delete" onclick="delFile('${safen}',this.closest('tr'))">&#10007; Delete</button>
        </td>
      </tr>`;
    }
    h+='</tbody></table>';
    b.innerHTML=h;
  }catch(e){b.innerHTML='<div class="empty">Error loading files</div>';}
}

async function delFile(name,row){
  if(!confirm('Delete '+name+'?'))return;
  try{
    const d=await fetch('/delete?f='+encodeURIComponent(name)).then(r=>r.json());
    if(d.ok){row.remove();toast('Deleted: '+name);}
    else toast('Delete failed: '+(d.error||'unknown'));
  }catch(e){toast('Error: '+e.message);}
}

// ── Snapshot / SD ──────────────────────────────────────────────────────────
async function saveSnap(){
  document.getElementById('msg').textContent='Saving snapshot...';
  try{
    const d=await fetch('/save').then(r=>r.json());
    document.getElementById('msg').textContent=d.ok?'Saved: '+(d.path||''):'Snapshot failed';
    if(d.ok)loadFiles();
  }catch(e){document.getElementById('msg').textContent='Error: '+e.message;}
}
async function sdAct(a){
  document.getElementById('msg').textContent='Running '+a+'...';
  try{
    const d=await fetch('/storage?action='+a).then(r=>r.json());
    document.getElementById('msg').textContent=d.message||'Done';
    refreshStatus();if(a==='wipe')loadFiles();
  }catch(e){document.getElementById('msg').textContent='Error: '+e.message;}
}

// ── Live preview (native MJPEG + /sysinfo streamFps) ──
let prevActive=false,prevFpsTimer=null;
async function updatePrevStreamFps(){
  if(!prevActive)return;
  try{
    const s=await fetch('/sysinfo').then(r=>r.json());
    if(s.streamFps!==undefined){
      const v=Number(s.streamFps);
      const el=document.getElementById('prevFps');
      if(v>0.05)el.textContent=v.toFixed(1)+' fps';
      else el.textContent='live\u2026';
    }
  }catch(e){}
}
function togglePreview(){
  prevActive=!prevActive;
  document.getElementById('prevToggle').textContent=prevActive?'\u23F8 Stop':'\u25B6 Start';
  const img=document.getElementById('prevImg');
  const off=document.getElementById('prevOff');
  const fps=document.getElementById('prevFps');
  if(prevActive){
    img.onerror=function(){
      fps.textContent='no video \u2014 open /stream in new tab or check Serial log';
    };
    img.onload=function(){fps.textContent='live\u2026';};
    img.src=window.location.origin+'/mjpeg?'+Date.now();
    img.style.display='block';
    off.style.display='none';
    fps.textContent='\u2026';
    updatePrevStreamFps();
    prevFpsTimer=setInterval(updatePrevStreamFps,1500);
  }else{
    if(prevFpsTimer){clearInterval(prevFpsTimer);prevFpsTimer=null;}
    img.onerror=null;
    img.onload=null;
    img.src='';
    img.style.display='none';
    off.style.display='';
    fps.textContent='';
  }
}

// ── Boot ───────────────────────────────────────────────────────────────────
refreshStatus();
loadFiles();
window._statusIv=setInterval(refreshStatus,4000);
window._filesIv=setInterval(loadFiles,30000);
// Pause polling when tab is hidden to save bandwidth + device CPU
document.addEventListener('visibilitychange',function(){
  if(document.hidden){
    clearInterval(window._statusIv);clearInterval(window._filesIv);
    if(window._devLogIv){clearInterval(window._devLogIv);window._devLogIv=null;}
    if(prevActive){togglePreview();}
  }else{
    refreshStatus();loadFiles();
    window._statusIv=setInterval(refreshStatus,4000);
    window._filesIv=setInterval(loadFiles,30000);
  }
});
</script>
</body>
</html>
)rawliteral";
  return httpd_resp_sendstr(req, html);
}

// Security: reject filenames that could escape /captures/
static bool isValidFilename(const char *name) {
  if (!name || name[0] == '\0' || name[0] == '.') return false;
  size_t len = strlen(name);
  if (len > 60) return false;
  for (size_t i = 0; i < len; i++) {
    char c = name[i];
    if (c == '/' || c == '\\' || c == ':') return false;
    if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') return false;
  }
  return true;
}

static esp_err_t files_handler(httpd_req_t *req) {
  String json = listCapturesJson(50);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t download_handler(httpd_req_t *req) {
  char *buf = NULL;
  char fname[64] = {0};
  if (parse_get(req, &buf) != ESP_OK) return ESP_FAIL;
  httpd_query_key_value(buf, "f", fname, sizeof(fname) - 1);
  free(buf);

  if (!isValidFilename(fname)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
    return ESP_FAIL;
  }
  if (!g_sdReady) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD not ready");
    return ESP_FAIL;
  }

  String path = String("/captures/") + fname;
  File file = SD_MMC.open(path, FILE_READ);
  if (!file) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  size_t fileSize = file.size();

  // TCP socket tuning — larger send buffer, Nagle ON (don't set TCP_NODELAY:
  // chunked encoding does 3 separate send() calls per chunk; NODELAY turns each
  // into a tiny packet and causes ACK-ping-pong, making throughput 10x worse)
  int sockfd = httpd_req_to_sockfd(req);
  if (sockfd >= 0) {
    // NOTE: SO_SNDBUF is stored as u16_t in lwIP (max 65535).
    // Setting 65536 would wrap to 0 and kill all sends!
    // lwIP caps at CONFIG_LWIP_TCP_SND_BUF_DEFAULT=5744 unless we match that cap.
    // Set to 32768 which fits u16_t and is above the cap — lwIP will use the cap.
    int sndbuf = 65535;
    (void)setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  }

  // Content headers
  const char *ct = "application/octet-stream";
  size_t fnLen = strlen(fname);
  if (fnLen > 4 && strcasecmp(fname + fnLen - 4, ".jpg") == 0) ct = "image/jpeg";
  else if (fnLen > 4 && strcasecmp(fname + fnLen - 4, ".avi") == 0) ct = "video/x-msvideo";
  httpd_resp_set_type(req, ct);
  String disp = String("attachment; filename=\"") + fname + "\"";
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
  char cl[20]; snprintf(cl, sizeof(cl), "%u", (unsigned)fileSize);
  httpd_resp_set_hdr(req, "Content-Length", cl);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  // 256 KB PSRAM read buffer — fewer SD mutex acquisitions per download,
  // recording task gets SD back between each 256KB read cycle
  const size_t kChunk = 256 * 1024;
  size_t readCap = kChunk;
  uint8_t *chunk = (uint8_t *)heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!chunk) {
    readCap = 32768;
    chunk = (uint8_t *)heap_caps_malloc(readCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!chunk) { file.close(); return httpd_resp_send_500(req); }

  // Diagnostic counters
  uint32_t tReadMs = 0, tSendMs = 0, nChunks = 0;
  uint32_t tTotal = millis();

  size_t n;
  esp_err_t res = ESP_OK;
  while (true) {
    uint32_t tR0 = millis();
    n = file.read(chunk, readCap);
    tReadMs += millis() - tR0;
    if (n == 0) break;
    uint32_t tS0 = millis();
    if (httpd_resp_send_chunk(req, (const char *)chunk, n) != ESP_OK) {
      res = ESP_FAIL; break;
    }
    tSendMs += millis() - tS0;
    nChunks++;
  }
  heap_caps_free(chunk);
  file.close();

  uint32_t elapsed = millis() - tTotal;
  float speedKBs = elapsed ? (fileSize / 1024.0f) / (elapsed / 1000.0f) : 0;
  log_i("[DL] %s  %u bytes  %.0f KB/s  | SD: %u ms  WiFi: %u ms  chunks: %u",
        fname, (unsigned)fileSize, speedKBs, tReadMs, tSendMs, nChunks);

  if (res == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
  return res;
}

static esp_err_t delete_handler(httpd_req_t *req) {
  char *buf = NULL;
  char fname[64] = {0};
  if (parse_get(req, &buf) != ESP_OK) return ESP_FAIL;
  httpd_query_key_value(buf, "f", fname, sizeof(fname) - 1);
  free(buf);

  if (!isValidFilename(fname)) {
    const char *err = "{\"ok\":false,\"error\":\"invalid filename\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, err, strlen(err));
  }

  String path = String("/captures/") + fname;
  bool ok = g_sdReady && SD_MMC.remove(path);
  String resp = ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"remove failed\"}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp.c_str(), resp.length());
}

// GET /wifiscan  → scan networks async (non-blocking), return JSON array
// Uses WIFI_SCAN_RUNNING / WIFI_SCAN_DONE pattern to avoid blocking httpd task
static esp_err_t wifiscan_handler(httpd_req_t *req) {
  // Acquire WiFi lock so scan doesn't collide with background connect task
  if (!cctv_wifi_try_lock(500)) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "[]");
  }
  WiFi.scanDelete();
  vTaskDelay(50 / portTICK_PERIOD_MS);

  // Start async scan (works while associated; may pause traffic briefly)
  int n = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
  // Poll for up to 5s without yielding control for multi-second blocks
  const uint32_t scanStart = millis();
  while (n == WIFI_SCAN_RUNNING && (millis() - scanStart) < 5000) {
    vTaskDelay(200 / portTICK_PERIOD_MS);  // yield for 200ms slices
    n = WiFi.scanComplete();
  }
  if (n < 0) n = 0;
  cctv_wifi_unlock();

  String resp = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) resp += ",";
    wifi_auth_mode_t auth = WiFi.encryptionType(i);
    const char *authStr = "OPEN";
    if      (auth == WIFI_AUTH_WPA2_ENTERPRISE) authStr = "WPA2-ENT";
    else if (auth == WIFI_AUTH_WPA3_ENTERPRISE) authStr = "WPA3-ENT";
    else if (auth == WIFI_AUTH_WPA3_PSK)        authStr = "WPA3";
    else if (auth == WIFI_AUTH_WPA2_PSK)        authStr = "WPA2";
    else if (auth == WIFI_AUTH_WPA_WPA2_PSK)    authStr = "WPA/WPA2";
    else if (auth == WIFI_AUTH_WPA_PSK)         authStr = "WPA";
    else if (auth != WIFI_AUTH_OPEN)            authStr = "OTHER";
    // Escape SSID for JSON
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\"); ssid.replace("\"", "\\\"");
    resp += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"auth\":\"" + authStr + "\",\"ch\":" + String(WiFi.channel(i)) + "}";
  }
  resp += "]";
  WiFi.scanDelete();

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp.c_str(), resp.length());
}

// GET /sysinfo → heap, PSRAM, CPU usage (idle task watermarks as proxy)
static esp_err_t sysinfo_handler(httpd_req_t *req) {
  // Heap
  size_t heapFree  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t heapTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  size_t heapMin   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  // PSRAM: prefer heap_caps + cctv_psram_* (Arduino getPsram* can be 0 on IDF 5.x).
  const size_t psramFree  = cctv_psram_free_bytes();
  const size_t psramTotal = cctv_psram_total_bytes();
  // CPU load via FreeRTOS runtime stats (stack watermarks of idle tasks as proxy)
  // Use configRUN_TIME_COUNTER_TYPE approach is ESP-IDF specific.
  // Simple approach: read last idle tick counts delta. Use esp_cpu_cycle_count or
  // just approximate via idle task stack high-watermarks as a rough indicator.
  // More reliable: use esp_get_idf_version, but for CPU% we use the following trick:
  // measure how many times idle loop runs per status period — instead we report uptime.
  uint32_t upSec = millis() / 1000;

  String resp = "{";
  resp += "\"heapFree\":"  + String(heapFree)  + ",";
  resp += "\"heapTotal\":" + String(heapTotal) + ",";
  resp += "\"heapMin\":"   + String(heapMin)   + ",";
  resp += "\"psramFree\":" + String(psramFree) + ",";
  resp += "\"psramTotal\":"+ String(psramTotal)+ ",";
  // CPU% approximation: check idle task stack usage
  // FreeRTOS idle tasks are named "IDLE0" and "IDLE1"
  // We read their high watermarks as an inverse proxy for load
  // (low watermark = task used heavily = high load)
  // Use a simple heuristic: report 0 unless we get real data
  UBaseType_t idle0Wm = 0, idle1Wm = 0;
  TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCPU(0);
  TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCPU(1);
  if (idle0) idle0Wm = uxTaskGetStackHighWaterMark(idle0);
  if (idle1) idle1Wm = uxTaskGetStackHighWaterMark(idle1);
  // idle stack is typically 768 words; lower watermark → less idle time → higher load
  // normalize: assume initial watermark ~700, current watermark → linear inverse
  uint32_t idleStack = 700;
  uint8_t cpu0 = (idle0Wm < idleStack) ? (uint8_t)((idleStack - idle0Wm) * 100 / idleStack) : 0;
  uint8_t cpu1 = (idle1Wm < idleStack) ? (uint8_t)((idleStack - idle1Wm) * 100 / idleStack) : 0;
  resp += "\"cpu0\":" + String(cpu0) + ",";
  resp += "\"cpu1\":" + String(cpu1) + ",";
  resp += "\"uptimeSec\":" + String(upSec) + ",";
  {
    char fpsBuf[16];
    snprintf(fpsBuf, sizeof(fpsBuf), "%.2f", stream_fps_get());
    resp += "\"streamFps\":" + String(fpsBuf) + ",";
  }
  resp += "\"deviceTime\":\"";
  struct tm ti;
  bool t_ok = false;
  if (getLocalTime(&ti, 50)) {
    char tbuf[40];
    strftime(tbuf, sizeof(tbuf), "%d-%m-%Y  %H:%M:%S", &ti);
    resp += String(tbuf);
    t_ok = (ti.tm_year + 1900 >= 2020);
  } else {
    resp += "--/--/---- --:--:--";
  }
  resp += "\",\"timeOk\":";
  resp += t_ok ? "true" : "false";
  resp += "}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp.c_str(), resp.length());
}

// GET /reboot  → immediate reboot (no WiFi check)
static esp_err_t reboot_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Rebooting...\"}");
  xTaskCreate([](void*){
    vTaskDelay(800 / portTICK_PERIOD_MS);
    esp_restart();
    vTaskDelete(NULL);
  }, "RebootTask", 4096, NULL, 2, NULL);
  return ESP_OK;
}

// GET /wifi                              → return config (pass masked) + profiles[]
// GET /wifi?ssid=..&pass=..&ent=0      → save normal WiFi (optional &slot=0..2)
// GET /wifi?ssid=..&ent=1&id=..&ep=..  → save enterprise WiFi
// GET /wifi?reconnect=1                → reconnect / failover (no reboot)
// GET /wifi?clear=1                    → clear all saved WiFi profiles
// GET /wifi?delslot=0..2               → delete one profile slot
static esp_err_t wifi_handler(httpd_req_t *req) {
  char *buf = NULL;
  if (httpd_req_get_url_query_len(req) > 0 && parse_get(req, &buf) == ESP_OK) {
    char tsmall[24] = {0};
    char qssid[160] = {0};
    char qpass[384] = {0};
    char qid[320] = {0};
    char qep[384] = {0};
    bool doReconnect = false;
    bool only_delslot = false;
    bool skip_save = false;

    // Clear WiFi config
    if (httpd_query_key_value(buf, "clear", tsmall, sizeof(tsmall) - 1) == ESP_OK && atoi(tsmall)) {
      free(buf);
      cctv_wifi_lock();
      clearWifiConfig();
      g_wifiStatus = "Not connected";
      cctv_wifi_unlock();
      httpd_resp_set_type(req, "application/json");
      httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
      return httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"WiFi config cleared\"}");
    }

    // Lock WiFi for all state-modifying operations (delslot, globals, save).
    // Prevents race with wifiStationBackgroundTask reading globals/slots.
    cctv_wifi_lock();

    if (httpd_query_key_value(buf, "delslot", tsmall, sizeof(tsmall) - 1) == ESP_OK &&
        strlen(tsmall) > 0) {
      const int ds = atoi(tsmall);
      if (ds >= 0 && ds <= 2) {
        cctv_wifi_delete_slot((uint8_t)ds);
        cctv_wifi_apply_preferred_or_first_globals();
        only_delslot = true;
        skip_save = true;
      }
    }

    if (httpd_query_key_value(buf, "reconnect", tsmall, sizeof(tsmall) - 1) == ESP_OK && atoi(tsmall))
      doReconnect = true;

    int saveSlot = -1;
    if (!only_delslot && httpd_query_key_value(buf, "slot", tsmall, sizeof(tsmall) - 1) == ESP_OK) {
      saveSlot = atoi(tsmall);
      if (saveSlot < 0 || saveSlot > 2) {
        saveSlot = -1;
      }
    }

    if (!only_delslot) {
      if (httpd_query_key_value(buf, "ssid", qssid, sizeof(qssid) - 1) == ESP_OK)
        g_wifiSsid = urlDecode(qssid);
      if (httpd_query_key_value(buf, "pass", qpass, sizeof(qpass) - 1) == ESP_OK && strlen(qpass) > 0)
        g_wifiPass = urlDecode(qpass);
      if (httpd_query_key_value(buf, "ent", tsmall, sizeof(tsmall) - 1) == ESP_OK)
        g_wifiEnterprise = (atoi(tsmall) != 0);
      if (httpd_query_key_value(buf, "id", qid, sizeof(qid) - 1) == ESP_OK)
        g_wifiIdentity = urlDecode(qid);
      if (httpd_query_key_value(buf, "ep", qep, sizeof(qep) - 1) == ESP_OK && strlen(qep) > 0)
        g_wifiEapPass = urlDecode(qep);
      if (g_wifiEnterprise) {
        g_wifiPass = "";
      } else {
        g_wifiIdentity = "";
        g_wifiEapPass = "";
      }
    }
    free(buf);

    if (!skip_save) {
      if (saveSlot >= 0 && g_wifiSsid.length() > 0) {
        cctv_wifi_save_globals_into_slot((uint8_t)saveSlot);
        cctv_wifi_set_preferred_slot((uint8_t)saveSlot);
        log_i("[WiFi] NVS profile saved slot=%d ssid_len=%u", saveSlot, (unsigned)g_wifiSsid.length());
      } else {
        saveWifiConfig();
      }
    }
    cctv_wifi_unlock();

    if (doReconnect) {
      g_wifiStatus = "Reconnecting...";
      xTaskCreate([](void*){
        vTaskDelay(600 / portTICK_PERIOD_MS);
        cctv_wifi_lock();
        const bool ok = cctv_wifi_try_connect_profiles(18000);
        cctv_wifi_unlock();
        if (ok) {
          g_wifiStatus = "Connected: " + WiFi.localIP().toString();
          log_i("[WiFi] Connected %s (no reboot)", WiFi.localIP().toString().c_str());
          cctv_time_kick_sync_if_needed(true);
        } else {
          g_wifiStatus = "Failed: " + String(WiFi.status());
        }
        vTaskDelete(NULL);
      }, "WifiRC", 6144, NULL, 2, NULL);
    }
  } else {
    // no query string — buf was never allocated
  }

  const String ethIp = cctv_eth_ip_string();
  const String wfIp = cctv_wifi_ip_string();
  const wl_status_t liveWifiState = WiFi.status();
  String liveWifiStatus = g_wifiStatus;
  if (wfIp.length() > 0) {
    liveWifiStatus = "Connected: " + wfIp;
  } else if (liveWifiState == WL_CONNECTED) {
    liveWifiStatus = "Connected";
  } else if (liveWifiStatus.startsWith("Connected")) {
    // Avoid stale "Connected" when WiFi is down and no STA IPv4 is present.
    liveWifiStatus = String("Not connected (") + String((int)liveWifiState) + ")";
  }

  String resp = "{";
  resp += "\"ssid\":\"";
  append_json_escaped(resp, g_wifiSsid);
  resp += "\",";
  resp += "\"enterprise\":" + String(g_wifiEnterprise ? "true" : "false") + ",";
  resp += "\"identity\":\"";
  append_json_escaped(resp, g_wifiIdentity);
  resp += "\",";
  resp += "\"passSet\":" + String(g_wifiPass.length() > 0 ? "true" : "false") + ",";
  resp += "\"eapPassSet\":" + String(g_wifiEapPass.length() > 0 ? "true" : "false") + ",";
  resp += "\"status\":\"";
  append_json_escaped(resp, liveWifiStatus);
  resp += "\",";
  resp += "\"ip\":\"" + cctv_primary_local_ip().toString() + "\",";
  resp += "\"ethIp\":\"" + ethIp + "\",";
  resp += "\"wifiIp\":\"" + wfIp + "\",";
  resp += "\"ethLink\":" + String(cctv_eth_link_up() ? "true" : "false");
  resp += ",\"prefSlot\":" + String((unsigned)cctv_wifi_preferred_slot());
  resp += ",\"profiles\":" + cctv_wifi_profiles_json_array();
  resp += "}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp.c_str(), resp.length());
}



bool startCameraServer() {
  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t cmd_uri = {
    .uri = "/control",
    .method = HTTP_GET,
    .handler = cmd_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t capture_uri = {
    .uri = "/capture",
    .method = HTTP_GET,
    .handler = capture_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t save_uri = {
    .uri = "/save",
    .method = HTTP_GET,
    .handler = save_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t storage_uri = {
    .uri = "/storage",
    .method = HTTP_GET,
    .handler = storage_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t bmp_uri = {
    .uri = "/bmp",
    .method = HTTP_GET,
    .handler = bmp_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };
  httpd_uri_t xclk_uri = {
    .uri = "/xclk",
    .method = HTTP_GET,
    .handler = xclk_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };
  httpd_uri_t reg_uri = {
    .uri = "/reg",
    .method = HTTP_GET,
    .handler = reg_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };
  httpd_uri_t greg_uri = {
    .uri = "/greg",
    .method = HTTP_GET,
    .handler = greg_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };
  httpd_uri_t pll_uri = {
    .uri = "/pll",
    .method = HTTP_GET,
    .handler = pll_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };
  httpd_uri_t win_uri = {
    .uri = "/resolution",
    .method = HTTP_GET,
    .handler = win_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };
  // VLC / players often expect a path ending in .mjpg — same MJPEG handler.
  httpd_uri_t mjpeg_uri = {
    .uri = "/mjpeg",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };
  httpd_uri_t live_mjpg_uri = {
    .uri = "/live.mjpg",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t files_uri = {
    .uri = "/files",
    .method = HTTP_GET,
    .handler = files_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t download_uri = {
    .uri = "/download",
    .method = HTTP_GET,
    .handler = download_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t delete_uri = {
    .uri = "/delete",
    .method = HTTP_GET,
    .handler = delete_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t sysinfo_uri = {
    .uri = "/sysinfo",
    .method = HTTP_GET,
    .handler = sysinfo_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t wifi_uri = {
    .uri = "/wifi",
    .method = HTTP_GET,
    .handler = wifi_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t wifiscan_uri = {
    .uri = "/wifiscan",
    .method = HTTP_GET,
    .handler = wifiscan_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t reboot_uri = {
    .uri = "/reboot",
    .method = HTTP_GET,
    .handler = reboot_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t api_console_uri = {
    .uri = "/api/console",
    .method = HTTP_GET,
    .handler = api_console_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t api_logs_uri = {
    .uri = "/api/logs",
    .method = HTTP_GET,
    .handler = api_logs_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  // Single HTTP server on port 80, Core 1
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  // Default CONFIG_HTTPD_MAX_URI_LEN is 512 — WiFi save uses GET with long URL-encoded
  // passwords; truncation caused silent save failures and broken JSON from the client.
  config.max_uri_len = 2048;
  config.max_uri_handlers = 28;
  config.max_open_sockets = 9;
  config.task_priority = tskIDLE_PRIORITY + 5;
  config.stack_size = 16384;     // download_handler + listCapturesJson need headroom
  config.send_wait_timeout = 30; // 30s timeout for large file downloads
  config.core_id = 1;
  // DO NOT use PSRAM stack for httpd — wifi_handler / cmd_handler do NVS flash writes
  // (Preferences::putString). On ESP32-S3 OPI-PSRAM the MSPI bus is shared; flash writes
  // disable cache → PSRAM stack inaccessible → silent write failure (putString returns 0).
  // 16 KB from internal DRAM is fine (~256 KB free at boot).

  log_i("Starting CCTV web server on port %d", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    cctv_ui_log_init();
    Serial.printf("[HTTP] httpd_start OK  port=%d  core=%d\n", config.server_port, (int)config.core_id);
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &cmd_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &save_uri);
    httpd_register_uri_handler(camera_httpd, &storage_uri);
    httpd_register_uri_handler(camera_httpd, &bmp_uri);
    httpd_register_uri_handler(camera_httpd, &xclk_uri);
    httpd_register_uri_handler(camera_httpd, &reg_uri);
    httpd_register_uri_handler(camera_httpd, &greg_uri);
    httpd_register_uri_handler(camera_httpd, &pll_uri);
    httpd_register_uri_handler(camera_httpd, &win_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
    httpd_register_uri_handler(camera_httpd, &mjpeg_uri);
    httpd_register_uri_handler(camera_httpd, &live_mjpg_uri);
    httpd_register_uri_handler(camera_httpd, &files_uri);
    httpd_register_uri_handler(camera_httpd, &download_uri);
    httpd_register_uri_handler(camera_httpd, &delete_uri);
    httpd_register_uri_handler(camera_httpd, &sysinfo_uri);
    httpd_register_uri_handler(camera_httpd, &wifi_uri);
    httpd_register_uri_handler(camera_httpd, &wifiscan_uri);
    httpd_register_uri_handler(camera_httpd, &reboot_uri);
    httpd_register_uri_handler(camera_httpd, &api_console_uri);
    httpd_register_uri_handler(camera_httpd, &api_logs_uri);
    return true;
  }
  Serial.printf("[HTTP] httpd_start FAILED (check port 80 in use / lwIP)\n");
  return false;
}
void setupLedFlash() {
#if defined(LED_GPIO_NUM)
  ledcAttach(LED_GPIO_NUM, 5000, 8);
#else
  log_i("LED flash is disabled -> LED_GPIO_NUM undefined");
#endif
}
