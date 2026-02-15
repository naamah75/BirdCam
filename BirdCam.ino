#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include <Preferences.h>

#include "esp_camera.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include "utilities.h"
#include "birdcam_settings.h"
#include "secrets.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <PubSubClient.h>
#include "birdcam_ha.h"

// app_httpd.cpp
void startCameraServer();

// ----------------- Compat PIR macro -----------------
#if defined(PIR_INPUT_PIN)
  #define PIR_PIN PIR_INPUT_PIN
#endif
#ifndef PIR_PIN
  #error "Define PIR_PIN or PIR_INPUT_PIN in utilities.h"
#endif

// ----------------- Globals shared with app_httpd.cpp -----------------
XPowersPMU PMU;
SemaphoreHandle_t g_cam_mutex = nullptr;
volatile uint32_t pir_count = 0;
bool stream_active = false;

// Boot time (epoch) per status web + HA
time_t g_boot_time = 0;

// ----------------- OLED display -----------------
static Adafruit_SSD1306 display(128, 64, &Wire, -1);
static bool g_display_ok = false;
static uint32_t g_display_last_ms = 0;
static uint32_t g_batt_msg_until_ms = 0;

// ----------------- Settings (persisted) -----------------
static Preferences prefs;
static int g_framesize = (int)FRAMESIZE_QVGA; // default safe
static int g_jpeg_quality = 12;
static int g_img_mode = 0;
static int g_archive_keep = 6;

// Camera image controls (persisted)
static int g_brightness    = 0;   // -2..2
static int g_contrast      = 0;   // -2..2
static int g_saturation    = 0;   // -2..2
static int g_sharpness     = 0;   // -2..2 (if supported)
static int g_gain_ctrl     = 1;   // auto gain
static int g_exposure_ctrl = 1;   // auto exposure
static int g_awb           = 1;   // auto white balance
static int g_agc_gain      = 0;   // 0..30 (manual gain)
static int g_aec_value     = 300; // 0..1200 (manual exposure)

// ----------------- Archive ring (PSRAM) -----------------
struct Snap {
  uint8_t* data = nullptr;
  size_t   len  = 0;
  time_t   ts   = 0;
};

static portMUX_TYPE snap_mux = portMUX_INITIALIZER_UNLOCKED;
static Snap* snaps = nullptr;
static int snap_head = -1;
static int snap_count = 0;

static const uint32_t MAX_SNAPSHOT_BYTES = 220 * 1024;

// ----------------- MQTT / Home Assistant -----------------
static WiFiClient g_wifi_client;
static PubSubClient mqtt(g_wifi_client);

static char g_dev_id[13] = {0};          // 12 hex + null
static char g_base_topic[96] = {0};      // birdcam/<id>
static char g_status_topic[140] = {0};   // birdcam/<id>/status

static uint32_t g_mqtt_last_try_ms = 0;
static uint32_t g_pir_off_at_ms = 0;

// Stream MQTT: 1 fps
static uint32_t g_mqtt_stream_last_ms = 0;
static const uint32_t MQTT_STREAM_PERIOD_MS = 1000;

// “Firmware version” per HA (metti quello che vuoi)
static const char* FW_VERSION = "2026-01-25";

// --- helpers ---
static void free_snap(Snap &s) {
  if (s.data) { free(s.data); s.data = nullptr; }
  s.len = 0; s.ts = 0;
}

static void realloc_archive(int keep) {
  if (keep < 1) keep = 1;
  if (keep > 20) keep = 20;

  if (snaps) {
    for (int i = 0; i < g_archive_keep; i++) free_snap(snaps[i]);
    free(snaps);
    snaps = nullptr;
  }

  snaps = (Snap*)calloc((size_t)keep, sizeof(Snap));
  g_archive_keep = keep;
  snap_head = -1;
  snap_count = 0;
}

static void store_snapshot_from_fb(const camera_fb_t* fb) {
  if (!fb || !snaps) return;
  if (fb->len == 0 || fb->len > MAX_SNAPSHOT_BYTES) return;

  uint8_t* copy = (uint8_t*)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!copy) return;
  memcpy(copy, fb->buf, fb->len);

  time_t now = time(nullptr);

  portENTER_CRITICAL(&snap_mux);
  int next = (snap_head + 1) % g_archive_keep;
  free_snap(snaps[next]);
  snaps[next].data = copy;
  snaps[next].len  = fb->len;
  snaps[next].ts   = now;
  snap_head = next;
  if (snap_count < g_archive_keep) snap_count++;
  portEXIT_CRITICAL(&snap_mux);
}

static void apply_sensor_settings() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;

  s->set_framesize(s, (framesize_t)g_framesize);
  s->set_quality(s, g_jpeg_quality);

  int mirror = (g_img_mode == 1 || g_img_mode == 3) ? 1 : 0;
  int flip   = (g_img_mode == 2 || g_img_mode == 3) ? 1 : 0;
  s->set_hmirror(s, mirror);
  s->set_vflip(s, flip);

  // Image controls (if supported)
  if (s->set_brightness)    s->set_brightness(s, g_brightness);
  if (s->set_contrast)      s->set_contrast(s, g_contrast);
  if (s->set_saturation)    s->set_saturation(s, g_saturation);
  if (s->set_sharpness)     s->set_sharpness(s, g_sharpness);
  if (s->set_gain_ctrl)     s->set_gain_ctrl(s, g_gain_ctrl ? 1 : 0);
  if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, g_exposure_ctrl ? 1 : 0);
  // Auto white balance: alcune versioni non hanno set_awb, ma set_whitebal
  if (s->set_whitebal)      s->set_whitebal(s, g_awb ? 1 : 0);
  // opzionale: alcune cam supportano anche awb_gain
  // if (s->set_awb_gain)   s->set_awb_gain(s, g_awb ? 1 : 0);
  // Manual values (used when corresponding auto is OFF)
  if (s->set_agc_gain)      s->set_agc_gain(s, g_agc_gain);
  if (s->set_aec_value)     s->set_aec_value(s, g_aec_value);
}

// ----------------- bc_* API (declared in birdcam_settings.h) -----------------
extern "C" {
int bc_get_framesize() { return g_framesize; }
int bc_get_jpeg_quality() { return g_jpeg_quality; }
int bc_get_img_mode() { return g_img_mode; }

int bc_get_brightness() { return g_brightness; }
int bc_get_contrast() { return g_contrast; }
int bc_get_saturation() { return g_saturation; }
int bc_get_sharpness() { return g_sharpness; }
int bc_get_gain_ctrl() { return g_gain_ctrl; }
int bc_get_exposure_ctrl() { return g_exposure_ctrl; }
int bc_get_awb() { return g_awb; }
int bc_get_agc_gain() { return g_agc_gain; }
int bc_get_aec_value() { return g_aec_value; }

void bc_apply_cam_controls(int brightness, int contrast, int saturation, int sharpness,
                           int gain_ctrl, int exposure_ctrl, int awb,
                           int agc_gain, int aec_value)
{
  if (brightness < -2) brightness = -2; if (brightness > 2) brightness = 2;
  if (contrast < -2) contrast = -2; if (contrast > 2) contrast = 2;
  if (saturation < -2) saturation = -2; if (saturation > 2) saturation = 2;
  if (sharpness < -2) sharpness = -2; if (sharpness > 2) sharpness = 2;
  if (agc_gain < 0) agc_gain = 0; if (agc_gain > 30) agc_gain = 30;
  if (aec_value < 0) aec_value = 0; if (aec_value > 1200) aec_value = 1200;
  g_brightness = brightness;
  g_contrast = contrast;
  g_saturation = saturation;
  g_sharpness = sharpness;
  g_gain_ctrl = gain_ctrl ? 1 : 0;
  g_exposure_ctrl = exposure_ctrl ? 1 : 0;
  g_awb = awb ? 1 : 0;
  g_agc_gain = agc_gain;
  g_aec_value = aec_value;
  if (g_cam_mutex) xSemaphoreTake(g_cam_mutex, portMAX_DELAY);
  apply_sensor_settings();
  if (g_cam_mutex) xSemaphoreGive(g_cam_mutex);
}

void bc_apply_settings(int framesize, int jpeg_quality, int img_mode) {
  if (jpeg_quality < 10) jpeg_quality = 10;
  if (jpeg_quality > 63) jpeg_quality = 63;

  g_framesize = framesize;
  g_jpeg_quality = jpeg_quality;
  g_img_mode = img_mode;

  if (g_cam_mutex) xSemaphoreTake(g_cam_mutex, portMAX_DELAY);
  apply_sensor_settings();
  if (g_cam_mutex) xSemaphoreGive(g_cam_mutex);
}

void bc_save_settings() {
  prefs.begin("birdcam", false);
  prefs.putInt("fs", g_framesize);
  prefs.putInt("jq", g_jpeg_quality);
  prefs.putInt("im", g_img_mode);
  prefs.putInt("ak", g_archive_keep);
  prefs.putInt("br", g_brightness);
  prefs.putInt("ct", g_contrast);
  prefs.putInt("sa", g_saturation);
  prefs.putInt("sh", g_sharpness);
  prefs.putInt("gc", g_gain_ctrl);
  prefs.putInt("ec", g_exposure_ctrl);
  prefs.putInt("wb", g_awb);
  prefs.putInt("gg", g_agc_gain);
  prefs.putInt("ev", g_aec_value);
  prefs.end();
}

int bc_get_archive_keep() { return g_archive_keep; }
int bc_set_archive_keep(int keep) { realloc_archive(keep); return g_archive_keep; }
int bc_get_snapshot_count() { return snap_count; }

uint32_t bc_get_snapshot_bytes_used() {
  uint32_t sum = 0;
  portENTER_CRITICAL(&snap_mux);
  for (int i = 0; i < g_archive_keep; i++) sum += (uint32_t)snaps[i].len;
  portEXIT_CRITICAL(&snap_mux);
  return sum;
}
uint32_t bc_get_snapshot_bytes_limit() { return MAX_SNAPSHOT_BYTES; }
} // extern "C"

// usata da app_httpd.cpp
bool bc_get_snapshot(int n, const uint8_t** data, size_t* len, time_t* ts) {
  if (!snaps || snap_count <= 0) return false;
  if (!data || !len || !ts) return false;
  if (n < 0) n = 0;
  if (n >= snap_count) return false;

  portENTER_CRITICAL(&snap_mux);
  int idx = snap_head - n;
  while (idx < 0) idx += g_archive_keep;

  if (!snaps[idx].data || snaps[idx].len == 0) {
    portEXIT_CRITICAL(&snap_mux);
    return false;
  }
  *data = snaps[idx].data;
  *len  = snaps[idx].len;
  *ts   = snaps[idx].ts;
  portEXIT_CRITICAL(&snap_mux);
  return true;
}

// ----------------- Device id / topics -----------------
static void make_device_id() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(g_dev_id, sizeof(g_dev_id), "%012llx", (unsigned long long)(mac & 0xFFFFFFFFFFFFULL));
  snprintf(g_base_topic, sizeof(g_base_topic), "birdcam/%s", g_dev_id);
  snprintf(g_status_topic, sizeof(g_status_topic), "%s/status", g_base_topic);
}

// ----------------- PMU init -----------------
static void initPMU_forCamera() {
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
    while (1) delay(1000);
  }

  PMU.setALDO1Voltage(1800); PMU.enableALDO1();
  PMU.setALDO2Voltage(2800); PMU.enableALDO2();
  PMU.setALDO4Voltage(3000); PMU.enableALDO4();

  PMU.setALDO3Voltage(3300); PMU.enableALDO3();
  PMU.setBLDO1Voltage(3300); PMU.enableBLDO1();

  PMU.disableTSPinMeasure();
  PMU.setChargingLedMode(XPOWERS_CHG_LED_OFF);

  PMU.enableVbusVoltageMeasure();
  PMU.enableSystemVoltageMeasure();
  PMU.enableBattVoltageMeasure();
  PMU.enableBattDetection();
}

static bool on_external_power() {
  return PMU.getVbusVoltage() > 1000;
}

// ----------------- WiFi -----------------
static void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(250);
}

// ----------------- Time (NTP) -----------------
static void initTimeNTP() {
  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  time_t now = 0;
  for (int i = 0; i < 20; i++) {
    now = time(nullptr);
    if (now > 1700000000) { g_boot_time = now; return; }
    delay(250);
  }
}

// ----------------- OLED -----------------
static void display_off() {
  if (!g_display_ok) return;
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
}
static void display_on() {
  if (!g_display_ok) return;
  display.ssd1306_command(SSD1306_DISPLAYON);
}
static void fmt_datetime(time_t ts, char* out, size_t outlen) {
  if (ts <= 0) { snprintf(out, outlen, "--/-- --:--"); return; }
  struct tm t;
  localtime_r(&ts, &t);
  strftime(out, outlen, "%d/%m %H:%M", &t);
}
static void display_show_capture() {
  if (!g_display_ok) return;
  display_on();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 18);
  display.print("CAPTURE");
  display.setTextSize(1);
  display.setCursor(0, 46);
  display.printf("PIR:%lu ARCH:%d/%d", (unsigned long)pir_count, snap_count, g_archive_keep);
  display.display();
}
static void display_show_info() {
  if (!g_display_ok) return;
  char dt[24]; fmt_datetime(time(nullptr), dt, sizeof(dt));

  display_on();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print(dt);

  display.setCursor(0, 16);
  display.print("IP ");
  display.print(WiFi.isConnected() ? WiFi.localIP().toString() : String("--"));

  display.setCursor(0, 32);
  if (WiFi.isConnected()) display.printf("RSSI %d  CH %d", WiFi.RSSI(), WiFi.channel());
  else display.print("RSSI --  CH --");

  display.setCursor(0, 48);
  display.printf("PIR %lu  ARCH %d/%d", (unsigned long)pir_count, snap_count, g_archive_keep);

  display.display();
}
static void initDisplay() {
  g_display_ok = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!g_display_ok) return;
  display.setRotation(2);
  display_off();
}

// ----------------- Camera init -----------------
static void initCameraStable() {
  camera_config_t config{};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.fb_count = psramFound() ? 2 : 1;
  config.frame_size = (framesize_t)g_framesize;
  config.jpeg_quality = g_jpeg_quality;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) while (1) delay(1000);

  apply_sensor_settings();
}

// ----------------- PIR ISR -----------------
static void IRAM_ATTR pirISR() { pir_count++; }

// ----------------- Load settings -----------------
static void load_settings() {
  prefs.begin("birdcam", true);
  g_framesize = prefs.getInt("fs", (int)FRAMESIZE_QVGA);
  g_jpeg_quality = prefs.getInt("jq", 12);
  g_img_mode = prefs.getInt("im", 0);
  g_archive_keep = prefs.getInt("ak", 6);
  g_brightness    = prefs.getInt("br", 0);
  g_contrast      = prefs.getInt("ct", 0);
  g_saturation    = prefs.getInt("sa", 0);
  g_sharpness     = prefs.getInt("sh", 0);
  g_gain_ctrl     = prefs.getInt("gc", 1);
  g_exposure_ctrl = prefs.getInt("ec", 1);
  g_awb           = prefs.getInt("wb", 1);
  g_agc_gain      = prefs.getInt("gg", 0);
  g_aec_value     = prefs.getInt("ev", 300);
  prefs.end();

  if (g_jpeg_quality < 10) g_jpeg_quality = 10;
  if (g_jpeg_quality > 63) g_jpeg_quality = 63;
  if (g_archive_keep < 1) g_archive_keep = 1;
  if (g_archive_keep > 20) g_archive_keep = 20;
  if (g_brightness < -2) g_brightness = -2; if (g_brightness > 2) g_brightness = 2;
  if (g_contrast < -2) g_contrast = -2; if (g_contrast > 2) g_contrast = 2;
  if (g_saturation < -2) g_saturation = -2; if (g_saturation > 2) g_saturation = 2;
  if (g_sharpness < -2) g_sharpness = -2; if (g_sharpness > 2) g_sharpness = 2;
  if (g_agc_gain < 0) g_agc_gain = 0; if (g_agc_gain > 30) g_agc_gain = 30;
  if (g_aec_value < 0) g_aec_value = 0; if (g_aec_value > 1200) g_aec_value = 1200;
  g_gain_ctrl = g_gain_ctrl ? 1 : 0;
  g_exposure_ctrl = g_exposure_ctrl ? 1 : 0;
  g_awb = g_awb ? 1 : 0;
}


// ----------------- Camera controls via MQTT (and HA) -----------------
static void mqtt_publish_cam_ctrl_states() {
  if (!mqtt.connected()) return;
  char topic[200], val[32];

  auto pubi = [&](const char* key, int v){
    snprintf(topic, sizeof(topic), "%s/ctrl/%s", g_base_topic, key);
    snprintf(val, sizeof(val), "%d", v);
    mqtt.publish(topic, val, true);
  };
  auto pubb = [&](const char* key, bool on){
    snprintf(topic, sizeof(topic), "%s/ctrl/%s", g_base_topic, key);
    mqtt.publish(topic, on ? "ON" : "OFF", true);
  };

  pubi("brightness", g_brightness);
  pubi("contrast",   g_contrast);
  pubi("saturation", g_saturation);
  pubi("sharpness",  g_sharpness);

  pubb("gain_ctrl",     g_gain_ctrl);
  pubb("exposure_ctrl", g_exposure_ctrl);
  pubb("awb",           g_awb);

  pubi("agc_gain",  g_agc_gain);
  pubi("aec_value", g_aec_value);
}

static void mqtt_cam_ctrl_apply_and_publish() {
  bc_apply_cam_controls(g_brightness, g_contrast, g_saturation, g_sharpness,
                        g_gain_ctrl, g_exposure_ctrl, g_awb,
                        g_agc_gain, g_aec_value);
  bc_save_settings();
  mqtt_publish_cam_ctrl_states();
}

static bool topic_is_ctrl_set(const char* topic, const char* key) {
  char t[220];
  snprintf(t, sizeof(t), "%s/ctrl/%s/set", g_base_topic, key);
  return strcmp(topic, t) == 0;
}

static void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  if (!topic || !payload) return;

  char msg[64];
  unsigned int n = (length < sizeof(msg)-1) ? length : (sizeof(msg)-1);
  memcpy(msg, payload, n);
  msg[n] = 0;

  auto toInt = [&](int def)->int{
    char* end=nullptr;
    long v=strtol(msg,&end,10);
    if (end==msg) return def;
    return (int)v;
  };
  auto toBool = [&](bool def)->bool{
    if (!strcasecmp(msg,"ON") || !strcasecmp(msg,"1") || !strcasecmp(msg,"true")) return true;
    if (!strcasecmp(msg,"OFF")|| !strcasecmp(msg,"0") || !strcasecmp(msg,"false")) return false;
    return def;
  };

  bool touched = false;

  if (topic_is_ctrl_set(topic, "brightness")) { g_brightness = toInt(g_brightness); touched = true; }
  else if (topic_is_ctrl_set(topic, "contrast")) { g_contrast = toInt(g_contrast); touched = true; }
  else if (topic_is_ctrl_set(topic, "saturation")) { g_saturation = toInt(g_saturation); touched = true; }
  else if (topic_is_ctrl_set(topic, "sharpness")) { g_sharpness = toInt(g_sharpness); touched = true; }
  else if (topic_is_ctrl_set(topic, "gain_ctrl")) { g_gain_ctrl = toBool(g_gain_ctrl) ? 1 : 0; touched = true; }
  else if (topic_is_ctrl_set(topic, "exposure_ctrl")) { g_exposure_ctrl = toBool(g_exposure_ctrl) ? 1 : 0; touched = true; }
  else if (topic_is_ctrl_set(topic, "awb")) { g_awb = toBool(g_awb) ? 1 : 0; touched = true; }
  else if (topic_is_ctrl_set(topic, "agc_gain")) { g_agc_gain = toInt(g_agc_gain); touched = true; }
  else if (topic_is_ctrl_set(topic, "aec_value")) { g_aec_value = toInt(g_aec_value); touched = true; }

  if (touched) mqtt_cam_ctrl_apply_and_publish();
}

// ----------------- MQTT connect (Last Will OK) -----------------
static void mqtt_connect_if_needed() {
  if (mqtt.connected()) return;

  uint32_t now = millis();
  if (now - g_mqtt_last_try_ms < 5000) return;
  g_mqtt_last_try_ms = now;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqtt_callback);

  char client_id[48];
  snprintf(client_id, sizeof(client_id), "birdcam-%s", g_dev_id);

  // Last Will: offline retained
  bool ok = mqtt.connect(client_id,
                         MQTT_USER, MQTT_PASS,
                         g_status_topic, 1, true,
                         "offline");

  if (!ok) return;

  // Online retained
  mqtt.publish(g_status_topic, "online", true);

  // Init HA module (safe to call multiple times)
  ha_init(mqtt, g_dev_id, g_base_topic, g_status_topic, FW_VERSION);
  ha_publish_discovery();
  // Subscribe to camera control topics (HA number/switch set)
  char sub[220];
  const char* keys[] = {"brightness","contrast","saturation","sharpness","gain_ctrl","exposure_ctrl","awb","agc_gain","aec_value"};
  for (auto k : keys) {
    snprintf(sub, sizeof(sub), "%s/ctrl/%s/set", g_base_topic, k);
    mqtt.subscribe(sub);
  }
  // Publish retained current states so HA UI matches device state
  mqtt_publish_cam_ctrl_states();


  // pubblica stati iniziali
  ha_set_boot_time(g_boot_time);
  ha_set_wifi(WiFi.isConnected() ? WiFi.RSSI() : 0, WiFi.isConnected() ? WiFi.channel() : 0);

  uint16_t vbus = PMU.getVbusVoltage();
  uint16_t sysv = PMU.getSystemVoltage();
  uint16_t batt = PMU.getBattVoltage();
  bool vbus_present = vbus > 1000;
  bool batt_present = PMU.isBatteryConnect(); // se la tua lib non ha questo, dimmelo e lo adattiamo
  ha_set_pmu(vbus, sysv, batt, vbus_present, batt_present);

  String ipS = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
  ha_publish_periodic(millis(), pir_count, snap_count, ipS.c_str());
}

// ----------------- Publish JPEG frame to MQTT camera (small) -----------------
static bool publish_small_jpeg_to_topic(const char* topic, bool retained) {
  if (!mqtt.connected() || !topic) return false;

  // PubSubClient: per payload grandi serve buffer grande.
  // Noi mandiamo QQVGA/quality 40 per restare piccoli.
  const framesize_t FS_MQTT = FRAMESIZE_QQVGA;
  const int Q_MQTT = 40;

  bool ok = false;

  if (g_cam_mutex) xSemaphoreTake(g_cam_mutex, portMAX_DELAY);

  sensor_t* s = esp_camera_sensor_get();
  framesize_t old_fs = (framesize_t)g_framesize;
  int old_q = g_jpeg_quality;

  if (s) {
    s->set_framesize(s, FS_MQTT);
    s->set_quality(s, Q_MQTT);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb && fb->format == PIXFORMAT_JPEG && fb->len > 0) {
    ok = mqtt.publish(topic, fb->buf, fb->len, retained);
    esp_camera_fb_return(fb);
  } else if (fb) {
    esp_camera_fb_return(fb);
  }

  // restore
  if (s) {
    s->set_framesize(s, old_fs);
    s->set_quality(s, old_q);
  }

  if (g_cam_mutex) xSemaphoreGive(g_cam_mutex);

  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(800);

  make_device_id();

  pinMode(PIR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), pirISR, RISING);

  g_cam_mutex = xSemaphoreCreateMutex();

  load_settings();
  realloc_archive(g_archive_keep);

  initPMU_forCamera();
  initDisplay();
  initWiFi();
  initTimeNTP();
  initCameraStable();

  // MQTT: alza buffer (ma non esagerare)
  mqtt.setBufferSize(32768);
  mqtt.setKeepAlive(30);

  startCameraServer();
}

void loop() {
  // Fix boot time se NTP arriva dopo
  if (g_boot_time == 0) {
    time_t now = time(nullptr);
    if (now > 1700000000) g_boot_time = now;
  }

  // MQTT
  mqtt_connect_if_needed();
  mqtt.loop();

  // Update HA cached values + periodic publish
  ha_set_boot_time(g_boot_time);
  ha_set_wifi(WiFi.isConnected() ? WiFi.RSSI() : 0, WiFi.isConnected() ? WiFi.channel() : 0);

  uint16_t vbus = PMU.getVbusVoltage();
  uint16_t sysv = PMU.getSystemVoltage();
  uint16_t batt = PMU.getBattVoltage();
  bool vbus_present = vbus > 1000;
  bool batt_present = PMU.isBatteryConnect();
  ha_set_pmu(vbus, sysv, batt, vbus_present, batt_present);

  String ipS = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
  ha_publish_periodic(millis(), pir_count, snap_count, ipS.c_str());

  // PIR handling
  static uint32_t last_pir_seen = 0;
  uint32_t cur = pir_count;
  if (cur != last_pir_seen) {
    last_pir_seen = cur;

    stream_active = false; // anti-OVF

    if (g_cam_mutex) xSemaphoreTake(g_cam_mutex, portMAX_DELAY);
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      store_snapshot_from_fb(fb);
      esp_camera_fb_return(fb);

      if (!on_external_power()) {
        g_batt_msg_until_ms = millis() + 2500;
        display_show_capture();
      }
    }
    if (g_cam_mutex) xSemaphoreGive(g_cam_mutex);

    // HA event
    if (mqtt.connected()) {
      long ts = (long)time(nullptr);
      if (ts <= 0) ts = (long)(millis() / 1000);

      ha_on_pir(pir_count, snap_count, ipS.c_str(), ts);
      g_pir_off_at_ms = millis() + 800;

      // Snapshot MQTT retained (frame piccolo)
      publish_small_jpeg_to_topic(ha_topic_cam_snapshot(), true);
    }
  }

  // PIR OFF
  if (g_pir_off_at_ms && (int32_t)(g_pir_off_at_ms - millis()) <= 0) {
    g_pir_off_at_ms = 0;
    if (mqtt.connected()) ha_pir_off();
  }

  // MQTT “Stream”: solo se alimentato + non stai facendo MJPEG web
  if (mqtt.connected() && on_external_power() && !stream_active) {
    uint32_t now = millis();
    if (now - g_mqtt_stream_last_ms >= MQTT_STREAM_PERIOD_MS) {
      g_mqtt_stream_last_ms = now;
      publish_small_jpeg_to_topic(ha_topic_cam_stream(), false);
    }
  }

  // OLED tick
  uint32_t now_ms = millis();
  if (g_display_ok) {
    if (on_external_power()) {
      if (now_ms - g_display_last_ms > 1000) {
        g_display_last_ms = now_ms;
        display_show_info();
      }
    } else {
      if (g_batt_msg_until_ms && (int32_t)(g_batt_msg_until_ms - now_ms) <= 0) {
        g_batt_msg_until_ms = 0;
        display_off();
      }
    }
  }

  delay(50);
}
