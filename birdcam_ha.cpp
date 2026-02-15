#include "birdcam_ha.h"

static PubSubClient* g_mqtt = nullptr;

static char g_dev_id[13] = {0};
static char g_base_topic[96] = {0};
static char g_status_topic[128] = {0};
static char g_fw_version[32] = {0};

static time_t   g_boot_time = 0;
static int      g_wifi_rssi = 0;
static int      g_wifi_ch   = 0;

static uint16_t g_vbus = 0, g_sys = 0, g_batt = 0;
static bool     g_vbus_present = false;
static bool     g_batt_present = false;

static uint32_t g_last_periodic_ms = 0;

static char g_topic_cam_snapshot[160] = {0}; // birdcam/<id>/cam/snapshot
static char g_topic_cam_stream[160]   = {0}; // birdcam/<id>/cam/stream

static inline bool mqtt_ok() { return g_mqtt && g_mqtt->connected(); }

static void pub_retained(const char* topic, const char* payload) {
  if (mqtt_ok()) g_mqtt->publish(topic, payload, true);
}
static void pub(const char* topic, const char* payload) {
  if (mqtt_ok()) g_mqtt->publish(topic, payload, false);
}

static void disco_one(const char* component, const char* object_id, const String& payload) {
  char topic[220];
  snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", component, g_dev_id, object_id);
  pub_retained(topic, payload.c_str());
}

static String make_devblk() {
  String s;
  s.reserve(260);
  s += "\"device\":{";
  s += "\"identifiers\":[\"birdcam_"; s += g_dev_id; s += "\"],";
  s += "\"name\":\"BirdCam\",";
  s += "\"model\":\"LilyGO T-Cam S3\",";
  s += "\"manufacturer\":\"LilyGO\",";
  s += "\"sw_version\":\""; s += g_fw_version; s += "\"";
  s += "}";
  return s;
}

void ha_init(PubSubClient& client,
             const char* dev_id,
             const char* base_topic,
             const char* status_topic,
             const char* fw_version)
{
  g_mqtt = &client;
  strncpy(g_dev_id, dev_id ? dev_id : "", sizeof(g_dev_id) - 1);
  strncpy(g_base_topic, base_topic ? base_topic : "", sizeof(g_base_topic) - 1);
  strncpy(g_status_topic, status_topic ? status_topic : "", sizeof(g_status_topic) - 1);
  strncpy(g_fw_version, fw_version ? fw_version : "dev", sizeof(g_fw_version) - 1);

  snprintf(g_topic_cam_snapshot, sizeof(g_topic_cam_snapshot), "%s/cam/snapshot", g_base_topic);
  snprintf(g_topic_cam_stream,   sizeof(g_topic_cam_stream),   "%s/cam/stream",   g_base_topic);
}

void ha_set_boot_time(time_t boot_time_epoch) { g_boot_time = boot_time_epoch; }
void ha_set_wifi(int rssi, int channel) { g_wifi_rssi = rssi; g_wifi_ch = channel; }
void ha_set_pmu(uint16_t vbus_mv, uint16_t sys_mv, uint16_t batt_mv,
                bool vbus_present, bool batt_present)
{
  g_vbus = vbus_mv; g_sys = sys_mv; g_batt = batt_mv;
  g_vbus_present = vbus_present;
  g_batt_present = batt_present;
}

const char* ha_topic_cam_snapshot() { return g_topic_cam_snapshot; }
const char* ha_topic_cam_stream()   { return g_topic_cam_stream; }

void ha_publish_discovery() {
  if (!mqtt_ok()) return;

  const String dev = make_devblk();
  const String avail =
    String("\"availability_topic\":\"") + g_status_topic + "\"," +
    "\"payload_available\":\"online\",\"payload_not_available\":\"offline\"";

  // ---------- Binary sensor PIR (motion) ----------
  disco_one("binary_sensor", "batt_present",
  "{"
    "\"name\":\"BirdCam Battery Present\","
    "\"unique_id\":\"birdcam_" + String(g_dev_id) + "_batt_present\","
    "\"state_topic\":\"" + String(g_base_topic) + "/batt_present\","
    + avail + ","
    "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
    "\"icon\":\"mdi:battery-check\","
    "\"entity_category\":\"diagnostic\","
    + dev +
  "}"
);

// ---------- Diagnostics: URLs (HTTP endpoints) ----------
disco_one("sensor", "stream_url",
  "{"
    "\"name\":\"BirdCam Stream URL\","
    "\"unique_id\":\"birdcam_" + String(g_dev_id) + "_stream_url\","
    "\"state_topic\":\"" + String(g_base_topic) + "/stream_url\","
    + avail + ","
    "\"icon\":\"mdi:cctv\","
    "\"entity_category\":\"diagnostic\","
    + dev +
  "}"
);

disco_one("sensor", "snapshot_url",
  "{"
    "\"name\":\"BirdCam Snapshot URL\","
    "\"unique_id\":\"birdcam_" + String(g_dev_id) + "_snapshot_url\","
    "\"state_topic\":\"" + String(g_base_topic) + "/snapshot_url\","
    + avail + ","
    "\"icon\":\"mdi:camera\","
    "\"entity_category\":\"diagnostic\","
    + dev +
  "}"
);

disco_one("sensor", "last_snap_0_url",
  "{"
    "\"name\":\"BirdCam Last Snapshot #0 URL\","
    "\"unique_id\":\"birdcam_" + String(g_dev_id) + "_last_snap_0_url\","
    "\"state_topic\":\"" + String(g_base_topic) + "/last_snap_0_url\","
    + avail + ","
    "\"icon\":\"mdi:image\","
    "\"entity_category\":\"diagnostic\","
    + dev +
  "}"
);

disco_one("sensor", "last_snap_1_url",
  "{"
    "\"name\":\"BirdCam Last Snapshot #1 URL\","
    "\"unique_id\":\"birdcam_" + String(g_dev_id) + "_last_snap_1_url\","
    "\"state_topic\":\"" + String(g_base_topic) + "/last_snap_1_url\","
    + avail + ","
    "\"icon\":\"mdi:image\","
    "\"entity_category\":\"diagnostic\","
    + dev +
  "}"
);

disco_one("sensor", "last_snap_2_url",
  "{"
    "\"name\":\"BirdCam Last Snapshot #2 URL\","
    "\"unique_id\":\"birdcam_" + String(g_dev_id) + "_last_snap_2_url\","
    "\"state_topic\":\"" + String(g_base_topic) + "/last_snap_2_url\","
    + avail + ","
    "\"icon\":\"mdi:image\","
    "\"entity_category\":\"diagnostic\","
    + dev +
  "}"
);

// Charging inferred: VBUS present AND battery present
disco_one("binary_sensor", "batt_charging",
  "{"
    "\"name\":\"BirdCam Battery Charging\","
    "\"unique_id\":\"birdcam_" + String(g_dev_id) + "_batt_charging\","
    "\"state_topic\":\"" + String(g_base_topic) + "/batt_charging\","
    + avail + ","
    "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
    "\"device_class\":\"battery_charging\","
    "\"entity_category\":\"diagnostic\","
    + dev +
  "}"
);

  // ---------- Camera MQTT: Snapshot (retained) ----------
  disco_one("camera", "snapshot",
    "{"
      "\"name\":\"BirdCam Snapshot\","
      "\"unique_id\":\"birdcam_" + String(g_dev_id) + "_cam_snapshot\","
      "\"topic\":\"" + String(g_topic_cam_snapshot) + "\","
      + avail + ","
      + dev +
    "}"
  );

  // ---------- Camera MQTT: Stream (frames non-retained) ----------
  disco_one("camera", "stream",
    "{"
      "\"name\":\"BirdCam Stream\","
      "\"unique_id\":\"birdcam_" + String(g_dev_id) + "_cam_stream\","
      "\"topic\":\"" + String(g_topic_cam_stream) + "\","
      + avail + ","
      + dev +
    "}"
  );
}

void ha_publish_periodic(uint32_t now_ms,
                         uint32_t pir_count,
                         int archive_count,
                         const char* ip)
{
  if (!mqtt_ok()) return;
  if (now_ms - g_last_periodic_ms < 60000) return;
  g_last_periodic_ms = now_ms;

  char t[160], v[48];

  snprintf(t, sizeof(t), "%s/pir_count", g_base_topic);
  snprintf(v, sizeof(v), "%lu", (unsigned long)pir_count);
  pub_retained(t, v);

  snprintf(t, sizeof(t), "%s/archive_count", g_base_topic);
  snprintf(v, sizeof(v), "%d", archive_count);
  pub_retained(t, v);

  snprintf(t, sizeof(t), "%s/rssi", g_base_topic);
  snprintf(v, sizeof(v), "%d", g_wifi_rssi);
  pub_retained(t, v);

  snprintf(t, sizeof(t), "%s/channel", g_base_topic);
  snprintf(v, sizeof(v), "%d", g_wifi_ch);
  pub_retained(t, v);

  snprintf(t, sizeof(t), "%s/boot_time", g_base_topic);
  snprintf(v, sizeof(v), "%ld", (long)g_boot_time);
  pub_retained(t, v);

  snprintf(t, sizeof(t), "%s/vbus_mv", g_base_topic);
  snprintf(v, sizeof(v), "%u", (unsigned)g_vbus);
  pub_retained(t, v);

  snprintf(t, sizeof(t), "%s/sys_mv", g_base_topic);
  snprintf(v, sizeof(v), "%u", (unsigned)g_sys);
  pub_retained(t, v);

  snprintf(t, sizeof(t), "%s/batt_mv", g_base_topic);
  snprintf(v, sizeof(v), "%u", (unsigned)g_batt);
  pub_retained(t, v);

  snprintf(t, sizeof(t), "%s/vbus_present", g_base_topic);
  pub_retained(t, g_vbus_present ? "ON" : "OFF");

snprintf(t, sizeof(t), "%s/batt_present", g_base_topic);
pub_retained(t, g_batt_present ? "ON" : "OFF");

snprintf(t, sizeof(t), "%s/batt_charging", g_base_topic);
pub_retained(t, (g_vbus_present && g_batt_present) ? "ON" : "OFF");

// Publish HTTP URLs (retained). Useful in HA as clickable text / for automations.
if (ip && ip[0]) {
  char url[220];

  // Stream URL (mjpeg)
  snprintf(t, sizeof(t), "%s/stream_url", g_base_topic);
  snprintf(url, sizeof(url), "http://%s/mjpeg", ip);
  pub_retained(t, url);

  // Live snapshot URL
  snprintf(t, sizeof(t), "%s/snapshot_url", g_base_topic);
  snprintf(url, sizeof(url), "http://%s/snapshot", ip);
  pub_retained(t, url);

  // Last snapshots in ring buffer: /snap?n=0 is latest, 1 is previous...
  for (int i = 0; i < 3; i++) {
    snprintf(t, sizeof(t), "%s/last_snap_%d_url", g_base_topic, i);
    if (archive_count > i) snprintf(url, sizeof(url), "http://%s/snap?n=%d", ip, i);
    else url[0] = 0;
    pub_retained(t, url);
  }
}

(void)ip; // (se vuoi in futuro possiamo pubblicare anche l'IP come sensor)
}

void ha_on_pir(uint32_t pir_count, int archive_count, const char* ip, long ts_epoch) {
  if (!mqtt_ok()) return;

  char t[160], v[48];

  snprintf(t, sizeof(t), "%s/pir", g_base_topic);
  pub(t, "ON");

  snprintf(t, sizeof(t), "%s/pir_count", g_base_topic);
  snprintf(v, sizeof(v), "%lu", (unsigned long)pir_count);
  pub_retained(t, v);

  snprintf(t, sizeof(t), "%s/archive_count", g_base_topic);
  snprintf(v, sizeof(v), "%d", archive_count);
  pub_retained(t, v);

  // Se vuoi, qui potremmo anche pubblicare URL http (ma tu vuoi snapshot in MQTT: già lo fai via camera topic)
  (void)ip;
  (void)ts_epoch;
}

void ha_pir_off() {
  if (!mqtt_ok()) return;
  char t[160];
  snprintf(t, sizeof(t), "%s/pir", g_base_topic);
  pub(t, "OFF");
}
