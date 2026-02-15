#include "esp_http_server.h"
#include "esp_camera.h"
#include "freertos/semphr.h"
#include <WiFi.h>
#include <Arduino.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "camera_index.h"
#include "birdcam_settings.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

// ---- extern from BirdCam.ino ----
extern XPowersPMU PMU;
extern SemaphoreHandle_t g_cam_mutex;
extern volatile uint32_t pir_count;
extern bool stream_active;
extern time_t g_boot_time;

// archive getter implemented in BirdCam.ino
bool bc_get_snapshot(int n, const uint8_t** data, size_t* len, time_t* ts);

static httpd_handle_t camera_httpd = NULL;

// ---------------- helpers ----------------
static inline void set_common_headers(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static bool uri_is(const char* uri, const char* target) {
  if (!uri || !target) return false;
  return strcmp(uri, target) == 0;
}

static void format_ts(time_t ts, char *out, size_t outlen) {
  if (ts <= 0) { snprintf(out, outlen, "--/--/---- --:--:--"); return; }
  struct tm t;
  localtime_r(&ts, &t);
  strftime(out, outlen, "%d/%m/%Y %H:%M:%S", &t);
}

// ---------------- CRT amber theme (menu semplice: underline) ----------------
static const char *CRT_CSS =
"<style>"
":root{--amber:#ffb000;--bg:#060606;--panel:#0b0b0b;}"
"body{margin:0;background:var(--bg);color:var(--amber);font-family:ui-monospace,Consolas,monospace;}"
"a{color:inherit;text-decoration:none;}"
".nav{background:var(--amber);color:#000;padding:10px 14px;font-weight:bold;letter-spacing:.5px;}"
".nav a{margin-right:14px;color:#000;font-variant:small-caps;text-transform:uppercase;letter-spacing:.6px;padding-bottom:2px;}"
".nav a.active{border-bottom:3px solid rgba(0,0,0,.75);}"
".wrap{max-width:1100px;margin:0 auto;padding:14px;}"
".card{background:rgba(10,10,10,.75);border-radius:14px;padding:14px;box-shadow:0 0 24px rgba(0,0,0,.5);}"
".media{position:relative;display:inline-block;border-radius:16px;overflow:hidden;}"
".media::after{content:'';position:absolute;inset:0;"
"background:linear-gradient(to bottom,rgba(0,0,0,.12) 50%,rgba(0,0,0,.32) 50%);"
"background-size:100% 3px;mix-blend-mode:multiply;pointer-events:none;opacity:.55;}"
".media::before{content:'';position:absolute;inset:-40px;"
"background:radial-gradient(circle at 50% 50%, rgba(255,176,0,.22), transparent 60%);"
"pointer-events:none;filter:blur(10px);opacity:.7;}"
".frame{border:2px solid rgba(255,176,0,.75);border-radius:16px;box-shadow:0 0 36px rgba(255,176,0,.2);}"
".pill{display:inline-block;padding:2px 8px;border:1px solid rgba(255,176,0,.5);border-radius:999px;opacity:.9;margin-right:8px;margin-bottom:8px;}"
"hr{border:0;border-top:1px solid rgba(255,176,0,.25);margin:14px 0;}"
"input,select{background:#111;color:var(--amber);border:1px solid rgba(255,176,0,.35);border-radius:10px;padding:8px 10px;}"
"button{padding:8px 14px;background:var(--amber);color:#000;border:2px solid #000;border-radius:10px;font-weight:bold;}"
"</style>";

static void send_nav(httpd_req_t *req) {
  const char* uri = req->uri ? req->uri : "";

  const char* a_view     = uri_is(uri, "/view")     ? "active" : "";
  const char* a_status   = uri_is(uri, "/status")   ? "active" : "";
  const char* a_archive  = uri_is(uri, "/archive")  ? "active" : "";
  const char* a_settings = (uri_is(uri, "/settings") || uri_is(uri, "/settings/")) ? "active" : "";
  const char* a_photo    = uri_is(uri, "/photo")    ? "active" : "";

  // se siamo in /photo, evidenziamo GALLERY
  if (a_photo[0]) a_archive = "active";

  char nav[512];
  snprintf(nav, sizeof(nav),
    "<div class='nav'>"
    " <a class='%s' href='/view'>LIVE</a>"
    " <a class='%s' href='/status'>STATUS</a>"
    " <a class='%s' href='/archive'>GALLERY</a>"
    " <a class='%s' href='/settings'>SETTINGS</a>"
    "</div>",
    a_view, a_status, a_archive, a_settings
  );
  httpd_resp_sendstr_chunk(req, nav);
}

// ---------------- handlers ----------------
static esp_err_t root_redirect_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/view");
  set_common_headers(req);
  return httpd_resp_send(req, "", 0);
}

static esp_err_t settings_slash_redirect_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "301 Moved Permanently");
  httpd_resp_set_hdr(req, "Location", "/settings");
  set_common_headers(req);
  return httpd_resp_send(req, "", 0);
}

static esp_err_t view_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  set_common_headers(req);
  return httpd_resp_send(req, index_html, strlen(index_html));
}

static esp_err_t api_mode_handler(httpd_req_t *req) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", bc_get_img_mode());
  httpd_resp_set_type(req, "text/plain");
  set_common_headers(req);
  return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t snapshot_handler(httpd_req_t *req) {
  if (g_cam_mutex) xSemaphoreTake(g_cam_mutex, portMAX_DELAY);
// Snapshot "live": se la risoluzione è molto alta, scendi a VGA per aumentare l'affidabilità
sensor_t* s = esp_camera_sensor_get();
framesize_t old_fs = FRAMESIZE_QVGA;
int old_q = 30;
if (s) {
  old_fs = s->status.framesize;
  old_q  = s->status.quality;
  if (old_fs > FRAMESIZE_VGA) {
    s->set_framesize(s, FRAMESIZE_VGA);
  }
  // tieni la qualità corrente (o più compressa se troppo "bella")
  int q = old_q;
  if (q < 30) q = 30;
  s->set_quality(s, q);
}

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    if (g_cam_mutex) xSemaphoreGive(g_cam_mutex);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snapshot.jpg");
  set_common_headers(req);

  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  if (g_cam_mutex) xSemaphoreGive(g_cam_mutex);
  return res;
}

// ---- MJPEG stream (5 FPS + stop controllato) ----
#define PART_BOUNDARY "frame"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t mjpeg_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  set_common_headers(req);

  char part_buf[64];
  uint32_t last_frame_ms = 0;

  stream_active = true;


// Stream leggero: forza QVGA + qualità più compressa per evitare OOM quando l'utente imposta risoluzioni alte
sensor_t* s = esp_camera_sensor_get();
framesize_t old_fs = FRAMESIZE_QVGA;
int old_q = 30;
if (s) {
  old_fs = s->status.framesize;
  old_q  = s->status.quality;
  s->set_framesize(s, FRAMESIZE_QVGA);
  // qualità: numero più alto = più compressione (file più piccoli)
  int q = old_q;
  if (q < 30) q = 30;
  s->set_quality(s, q);
}

  while (stream_active) {
    uint32_t now = millis();
    if (now - last_frame_ms < 200) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    last_frame_ms = now;

    if (g_cam_mutex) xSemaphoreTake(g_cam_mutex, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      if (g_cam_mutex) xSemaphoreGive(g_cam_mutex);
      break;
    }

    esp_err_t res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      int hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, (unsigned)fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    }

    esp_camera_fb_return(fb);
    if (g_cam_mutex) xSemaphoreGive(g_cam_mutex);

    if (res != ESP_OK) break;
  }

// ripristina impostazioni sensore
{
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, old_fs);
    s->set_quality(s, old_q);
  }
}

  stream_active = false;
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// snapshot dal ring (n=0 ultimo, 1 precedente, etc.)
static esp_err_t snap_n_handler(httpd_req_t *req) {
  char qs[32];
  int n = 0;
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
    char param[8];
    if (httpd_query_key_value(qs, "n", param, sizeof(param)) == ESP_OK) n = atoi(param);
  }

  const uint8_t *data = nullptr;
  size_t len = 0;
  time_t ts = 0;
  if (!bc_get_snapshot(n, &data, &len, &ts)) {
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No snapshot");
  }

  httpd_resp_set_type(req, "image/jpeg");
  set_common_headers(req);
  return httpd_resp_send(req, (const char*)data, len);
}

// Pagina HTML “foto grande” (per evitare la sensazione di pagina nera)
static esp_err_t photo_handler(httpd_req_t *req) {
  char qs[32];
  int n = 0;
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
    char param[8];
    if (httpd_query_key_value(qs, "n", param, sizeof(param)) == ESP_OK) n = atoi(param);
  }

  const uint8_t *data = nullptr;
  size_t len = 0;
  time_t ts = 0;
  if (!bc_get_snapshot(n, &data, &len, &ts)) {
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No snapshot");
  }

  char tsbuf[32];
  format_ts(ts, tsbuf, sizeof(tsbuf));

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  set_common_headers(req);

  httpd_resp_sendstr_chunk(req, "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  httpd_resp_sendstr_chunk(req, CRT_CSS);
  httpd_resp_sendstr_chunk(req, "</head><body>");
  send_nav(req);

  char hdr[320];
  snprintf(hdr, sizeof(hdr),
    "<div class='wrap'><div class='card'>"
    "<div style='display:flex;justify-content:space-between;align-items:baseline;gap:10px;flex-wrap:wrap'>"
    "<div style='font-weight:bold;letter-spacing:.3px'>SNAPSHOT #%d</div>"
    "<div style='opacity:.9;font-size:.95em'>%s</div>"
    "</div><hr>", n, tsbuf
  );
  httpd_resp_sendstr_chunk(req, hdr);

  char body[640];
  snprintf(body, sizeof(body),
    "<div class='media frame' style='width:100%%;max-width:1024px;background:#000'>"
      "<img src='/snap?n=%d' style='width:100%%;height:auto;display:block'>"
    "</div>"
    "<div style='margin-top:12px;opacity:.9'>"
      "<a href='/archive' style='text-decoration:underline'>← Back to gallery</a>"
    "</div>"
    "</div></div></body></html>", n
  );
  httpd_resp_sendstr_chunk(req, body);
  return httpd_resp_sendstr_chunk(req, NULL);
}

// =================== ARCHIVE (gallery) ===================
static esp_err_t archive_handler(httpd_req_t *req) {
  set_common_headers(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");

  httpd_resp_sendstr_chunk(req, "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  httpd_resp_sendstr_chunk(req, CRT_CSS);
  httpd_resp_sendstr_chunk(req, "</head><body>");
  send_nav(req);
  httpd_resp_sendstr_chunk(req, "<div class='wrap'><div class='card'>");

  int keep = bc_get_archive_keep();
  int count = bc_get_snapshot_count();
  if (count <= 0) {
    httpd_resp_sendstr_chunk(req, "<div style='opacity:.8'>Nessuno snapshot ancora.</div></div></div></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
  }

  httpd_resp_sendstr_chunk(req, "<div style='display:flex;flex-wrap:wrap;gap:12px'>");

  for (int i = 0; i < count && i < keep; i++) {
    const uint8_t *data = nullptr;
    size_t len = 0;
    time_t ts = 0;
    if (!bc_get_snapshot(i, &data, &len, &ts)) continue;

    char tsbuf[32];
    format_ts(ts, tsbuf, sizeof(tsbuf));

    char card[700];
    snprintf(card, sizeof(card),
      "<div class='card' style='width:210px;padding:10px'>"
        "<a href='/photo?n=%d'>"
          "<div class='media frame' style='width:190px;height:140px;background:#000'>"
            "<img src='/snap?n=%d' style='width:190px;height:140px;object-fit:cover;display:block'>"
          "</div>"
        "</a>"
        "<div style='margin-top:8px;font-size:12px;opacity:.9'>%s</div>"
      "</div>",
      i, i, tsbuf
    );
    httpd_resp_sendstr_chunk(req, card);
  }

  httpd_resp_sendstr_chunk(req, "</div></div></div></body></html>");
  return httpd_resp_sendstr_chunk(req, NULL);
}

// =================== STATUS ===================
static esp_err_t status_handler(httpd_req_t *req) {
  set_common_headers(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");

  String ssid = WiFi.SSID();
  String mac  = WiFi.macAddress();
  int ch = WiFi.channel();
  int rssi = WiFi.RSSI();

  uint16_t vbus = PMU.getVbusVoltage();
  uint16_t sysv = PMU.getSystemVoltage();
  uint16_t batt = PMU.getBattVoltage();

  size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
  size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

  int heap_pct = heap_total ? (int)((heap_free * 100ULL) / heap_total) : 0;
  int ps_pct   = ps_total   ? (int)((ps_free   * 100ULL) / ps_total)   : 0;

  uint32_t used = bc_get_snapshot_bytes_used();
  uint32_t per_snap_limit = bc_get_snapshot_bytes_limit();
  int keep = bc_get_archive_keep();
  int count = bc_get_snapshot_count();

  // “spazio logico” massimo stimato (keep * max snapshot bytes)
  uint64_t total_limit = (uint64_t)keep * (uint64_t)per_snap_limit;
  uint64_t free_b = (used >= total_limit) ? 0 : (total_limit - used);
  int free_pct = total_limit ? (int)((free_b * 100ULL) / total_limit) : 0;

  char bootbuf[32];
  format_ts(g_boot_time, bootbuf, sizeof(bootbuf));

  httpd_resp_sendstr_chunk(req, "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  httpd_resp_sendstr_chunk(req, CRT_CSS);
  httpd_resp_sendstr_chunk(req, "</head><body>");
  send_nav(req);
  httpd_resp_sendstr_chunk(req, "<div class='wrap'><div class='card'>");

  char line[560];
  snprintf(line, sizeof(line),
    "<div class='pill'>WiFi: %s</div>"
    "<div class='pill'>MAC: %s</div>"
    "<div class='pill'>CH: %d</div>"
    "<div class='pill'>RSSI: %d dBm</div>"
    "<div class='pill'>BOOT: %s</div>"
    "<div class='pill'>PIR: %lu</div>"
    "<div class='pill'>STREAM: %s</div>",
    ssid.c_str(), mac.c_str(), ch, rssi, bootbuf,
    (unsigned long)pir_count, stream_active ? "ON" : "OFF"
  );
  httpd_resp_sendstr_chunk(req, line);

  httpd_resp_sendstr_chunk(req, "<hr>");
  snprintf(line, sizeof(line),
    "<div style='opacity:.9'><b>PMU</b> · VBUS %u mV · SYS %u mV · BAT %u mV</div>",
    (unsigned)vbus, (unsigned)sysv, (unsigned)batt
  );
  httpd_resp_sendstr_chunk(req, line);

  httpd_resp_sendstr_chunk(req, "<hr>");
  snprintf(line, sizeof(line),
    "<div style='opacity:.9'><b>MEM</b> · HEAP %d%% free · PSRAM %d%% free</div>"
    "<div style='opacity:.9'>ARCHIVE · keep %d · stored %d · bytes %u · free %d%%</div>",
    heap_pct, ps_pct, keep, count, (unsigned)used, free_pct
  );
  httpd_resp_sendstr_chunk(req, line);

  httpd_resp_sendstr_chunk(req, "</div></div></body></html>");
  return httpd_resp_sendstr_chunk(req, NULL);
}

// =================== SETTINGS ===================
static esp_err_t settings_get_handler(httpd_req_t *req) {
  set_common_headers(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");

  int fs = bc_get_framesize();
  int jq = bc_get_jpeg_quality();
  int im = bc_get_img_mode();
  int ak = bc_get_archive_keep();

  int br = bc_get_brightness();
  int ct = bc_get_contrast();
  int sa = bc_get_saturation();
  int sh = bc_get_sharpness();
  int gc = bc_get_gain_ctrl();
  int ec = bc_get_exposure_ctrl();
  int wb = bc_get_awb();
  int gg = bc_get_agc_gain();
  int ev = bc_get_aec_value();

  auto sel = [](int a, int b){ return a==b ? " selected" : ""; };

  httpd_resp_sendstr_chunk(req, "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  httpd_resp_sendstr_chunk(req, CRT_CSS);
  httpd_resp_sendstr_chunk(req, "</head><body>");
  send_nav(req);
  httpd_resp_sendstr_chunk(req, "<div class='wrap'><div class='card'>");

  httpd_resp_sendstr_chunk(req, "<form method='POST' action='/settings'>");

  httpd_resp_sendstr_chunk(req, "<label>Resolution</label><br><select name='fs'>");
  char line[260];
  snprintf(line, sizeof(line), "<option value='%d'%s>QQVGA 160×120</option>", (int)FRAMESIZE_QQVGA, sel(fs,(int)FRAMESIZE_QQVGA)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='%d'%s>QVGA 320×240 (safe)</option>", (int)FRAMESIZE_QVGA, sel(fs,(int)FRAMESIZE_QVGA)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='%d'%s>VGA 640×480</option>", (int)FRAMESIZE_VGA, sel(fs,(int)FRAMESIZE_VGA)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='%d'%s>SVGA 800×600</option>", (int)FRAMESIZE_SVGA, sel(fs,(int)FRAMESIZE_SVGA)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='%d'%s>XGA 1024×768</option>", (int)FRAMESIZE_XGA, sel(fs,(int)FRAMESIZE_XGA)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='%d'%s>UXGA 1600×1200</option>", (int)FRAMESIZE_UXGA, sel(fs,(int)FRAMESIZE_UXGA)); httpd_resp_sendstr_chunk(req, line);
  httpd_resp_sendstr_chunk(req, "</select><br><br>");

  httpd_resp_sendstr_chunk(req, "<label>JPEG quality (10 best → 63 more compression)</label><br>");
  snprintf(line, sizeof(line), "<input name='jq' type='number' min='10' max='63' value='%d'><br><br>", jq); httpd_resp_sendstr_chunk(req, line);

  httpd_resp_sendstr_chunk(req, "<label>Rotation / mirror</label><br><select name='im'>");
  snprintf(line, sizeof(line), "<option value='0'%s>Normal</option>", sel(im,0)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='1'%s>Mirror</option>", sel(im,1)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='2'%s>Flip</option>", sel(im,2)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='3'%s>Rotate 180</option>", sel(im,3)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='4'%s>Rotate +90 (web)</option>", sel(im,4)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='5'%s>Rotate -90 (web)</option>", sel(im,5)); httpd_resp_sendstr_chunk(req, line);
  httpd_resp_sendstr_chunk(req, "</select><br><br>");

  httpd_resp_sendstr_chunk(req, "<label>Archive keep (1..20)</label><br>");
  snprintf(line, sizeof(line), "<input name='ak' type='number' min='1' max='20' value='%d'><br><br>", ak); httpd_resp_sendstr_chunk(req, line);

  httpd_resp_sendstr_chunk(req, "<hr><h3>Image controls</h3>");
  httpd_resp_sendstr_chunk(req, "<label>Brightness (-2..2)</label><br>");
  snprintf(line, sizeof(line), "<input name='br' type='number' min='-2' max='2' step='1' value='%d'><br><br>", br); httpd_resp_sendstr_chunk(req, line);
  httpd_resp_sendstr_chunk(req, "<label>Contrast (-2..2)</label><br>");
  snprintf(line, sizeof(line), "<input name='ct' type='number' min='-2' max='2' step='1' value='%d'><br><br>", ct); httpd_resp_sendstr_chunk(req, line);
  httpd_resp_sendstr_chunk(req, "<label>Saturation (-2..2)</label><br>");
  snprintf(line, sizeof(line), "<input name='sa' type='number' min='-2' max='2' step='1' value='%d'><br><br>", sa); httpd_resp_sendstr_chunk(req, line);
  httpd_resp_sendstr_chunk(req, "<label>Sharpness (-2..2)</label><br>");
  snprintf(line, sizeof(line), "<input name='sh' type='number' min='-2' max='2' step='1' value='%d'><br><br>", sh); httpd_resp_sendstr_chunk(req, line);

  httpd_resp_sendstr_chunk(req, "<label>Auto gain</label><br><select name='gc'>");
  snprintf(line, sizeof(line), "<option value='1'%s>On</option>", sel(gc,1)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='0'%s>Off</option>", sel(gc,0)); httpd_resp_sendstr_chunk(req, line);
  httpd_resp_sendstr_chunk(req, "</select><br><br>");

  httpd_resp_sendstr_chunk(req, "<label>Auto exposure</label><br><select name='ec'>");
  snprintf(line, sizeof(line), "<option value='1'%s>On</option>", sel(ec,1)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='0'%s>Off</option>", sel(ec,0)); httpd_resp_sendstr_chunk(req, line);
  httpd_resp_sendstr_chunk(req, "</select><br><br>");

  httpd_resp_sendstr_chunk(req, "<label>Auto white balance</label><br><select name='wb'>");
  snprintf(line, sizeof(line), "<option value='1'%s>On</option>", sel(wb,1)); httpd_resp_sendstr_chunk(req, line);
  snprintf(line, sizeof(line), "<option value='0'%s>Off</option>", sel(wb,0)); httpd_resp_sendstr_chunk(req, line);
  httpd_resp_sendstr_chunk(req, "</select><br><br>");

  httpd_resp_sendstr_chunk(req, "<label>Manual gain (0..30, used when Auto gain=Off)</label><br>");
  snprintf(line, sizeof(line), "<input name='gg' type='number' min='0' max='30' step='1' value='%d'><br><br>", gg); httpd_resp_sendstr_chunk(req, line);

  httpd_resp_sendstr_chunk(req, "<label>Manual exposure (0..1200, used when Auto exposure=Off)</label><br>");
  snprintf(line, sizeof(line), "<input name='ev' type='number' min='0' max='1200' step='1' value='%d'><br><br>", ev); httpd_resp_sendstr_chunk(req, line);

  httpd_resp_sendstr_chunk(req, "<button type='submit'>Save</button>");
  httpd_resp_sendstr_chunk(req, "</form></div></div></body></html>");
  return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t settings_post_handler(httpd_req_t *req) {
  char buf[256];
  int len = httpd_req_recv(req, buf, sizeof(buf)-1);
  if (len <= 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
  buf[len] = 0;

  auto geti = [&](const char* key, int def){
    char v[16];
    if (httpd_query_key_value(buf, key, v, sizeof(v)) == ESP_OK) return atoi(v);
    return def;
  };

  int fs = geti("fs", bc_get_framesize());
  int jq = geti("jq", bc_get_jpeg_quality());
  int im = geti("im", bc_get_img_mode());
  int ak = geti("ak", bc_get_archive_keep());
  int br = geti("br", bc_get_brightness());
  int ct = geti("ct", bc_get_contrast());
  int sa = geti("sa", bc_get_saturation());
  int sh = geti("sh", bc_get_sharpness());
  int gc = geti("gc", bc_get_gain_ctrl());
  int ec = geti("ec", bc_get_exposure_ctrl());
  int wb = geti("wb", bc_get_awb());
  int gg = geti("gg", bc_get_agc_gain());
  int ev = geti("ev", bc_get_aec_value());

  bc_apply_settings(fs, jq, im);
  bc_apply_cam_controls(br, ct, sa, sh, gc, ec, wb, gg, ev);
  bc_set_archive_keep(ak);
  bc_save_settings();

  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/settings");
  set_common_headers(req);
  return httpd_resp_send(req, "", 0);
}

// ---------------- server ----------------
void startCameraServer() {
  Serial.println("### BirdCam WEB BUILD: 2026-02-03 11:55");

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 8192;

  // noi registriamo ~12 handler
  config.max_uri_handlers = 16;

  if (httpd_start(&camera_httpd, &config) != ESP_OK) {
    Serial.println("httpd_start FAILED");
    return;
  }

  httpd_uri_t uri_root   = { .uri="/",          .method=HTTP_GET,  .handler=root_redirect_handler,           .user_ctx=NULL };
  httpd_uri_t uri_view   = { .uri="/view",      .method=HTTP_GET,  .handler=view_handler,                    .user_ctx=NULL };
  httpd_uri_t uri_status = { .uri="/status",    .method=HTTP_GET,  .handler=status_handler,                  .user_ctx=NULL };
  httpd_uri_t uri_snap   = { .uri="/snapshot",  .method=HTTP_GET,  .handler=snapshot_handler,                .user_ctx=NULL };
  httpd_uri_t uri_mjpeg  = { .uri="/mjpeg",     .method=HTTP_GET,  .handler=mjpeg_handler,                   .user_ctx=NULL };
  httpd_uri_t uri_mode   = { .uri="/api/mode",  .method=HTTP_GET,  .handler=api_mode_handler,                .user_ctx=NULL };
  httpd_uri_t uri_arch   = { .uri="/archive",   .method=HTTP_GET,  .handler=archive_handler,                 .user_ctx=NULL };
  httpd_uri_t uri_snapn  = { .uri="/snap",      .method=HTTP_GET,  .handler=snap_n_handler,                  .user_ctx=NULL };
  httpd_uri_t uri_photo  = { .uri="/photo",     .method=HTTP_GET,  .handler=photo_handler,                   .user_ctx=NULL };
  httpd_uri_t uri_set_g  = { .uri="/settings",  .method=HTTP_GET,  .handler=settings_get_handler,            .user_ctx=NULL };
  httpd_uri_t uri_set_p  = { .uri="/settings",  .method=HTTP_POST, .handler=settings_post_handler,           .user_ctx=NULL };
  httpd_uri_t uri_set_s  = { .uri="/settings/", .method=HTTP_GET,  .handler=settings_slash_redirect_handler, .user_ctx=NULL };

  httpd_register_uri_handler(camera_httpd, &uri_root);
  httpd_register_uri_handler(camera_httpd, &uri_view);
  httpd_register_uri_handler(camera_httpd, &uri_status);
  httpd_register_uri_handler(camera_httpd, &uri_snap);
  httpd_register_uri_handler(camera_httpd, &uri_mjpeg);
  httpd_register_uri_handler(camera_httpd, &uri_mode);
  httpd_register_uri_handler(camera_httpd, &uri_arch);
  httpd_register_uri_handler(camera_httpd, &uri_snapn);
  httpd_register_uri_handler(camera_httpd, &uri_photo);
  httpd_register_uri_handler(camera_httpd, &uri_set_g);
  httpd_register_uri_handler(camera_httpd, &uri_set_p);
  httpd_register_uri_handler(camera_httpd, &uri_set_s);
}
