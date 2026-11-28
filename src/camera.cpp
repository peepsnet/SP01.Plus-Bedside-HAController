#include "camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lvgl.h>
#include "config.h"
#include "display.h"
#include "ha_client.h"
#include "mbedtls/base64.h"
#include "ui.h"

// Untested on hardware. A background task (core 0) downloads and JPEG-decodes frames into one of two PSRAM sprites; the
// main loop swaps the newest finished one into an LVGL image. LVGL itself is only touched from the main loop.
//
// Frames are decoded at 1, 1/2, 1/4 or 1/8 size (the JPEG decoder's fast paths) so the picture fits the screen without
// resampling. A 640x480 camera therefore shows as 320x240, centered. Progressive JPEGs are not supported.

static constexpr size_t JPG_MAX = 262144;

// --- shared state between the UI and the task ---
static String srcLabel, srcEntity, srcUrl;
static volatile bool stopFlag = true;
static volatile bool taskDone = true;
static volatile int readyIdx = -1;   // finished frame waiting to be shown
static volatile int frontIdx = 0;    // frame LVGL is showing; the task only writes into the other one
static lgfx::LGFX_Sprite* spr[2] = {nullptr, nullptr};
static int frameW = 0, frameH = 0;
static char statusBuf[80] = "Connecting...";
static volatile uint32_t frameCount = 0;
static volatile float fps = 0;
static bool pendingOpen = false;
static String pLabel, pEntity, pUrl;

// --- LVGL objects (main loop only) ---
static lv_obj_t* camScr;
static lv_obj_t* camImg;
static lv_obj_t* camLabel;
static lv_image_dsc_t dsc[2];
static bool isOpen = false;
static bool shown = false;  // at least one frame displayed in this session
static uint32_t lastLabel = 0;

static void setStatus(const char* s) { strlcpy(statusBuf, s, sizeof statusBuf); }

// ---------- URL handling ----------
struct Url {
  bool tls = false;
  String host;
  uint16_t port = 80;
  String path = "/";
  String basic;  // base64 of user:pass, or empty
};

static String b64(const String& s) {
  size_t olen = 0;
  unsigned char out[200];
  mbedtls_base64_encode(out, sizeof out, &olen, (const unsigned char*)s.c_str(), s.length());
  return String((const char*)out).substring(0, olen);
}

static bool parseUrl(String u, Url& o) {
  u.trim();
  if (u.startsWith("https://")) { o.tls = true; u = u.substring(8); }
  else if (u.startsWith("http://")) u = u.substring(7);
  else return false;
  int slash = u.indexOf('/');
  String hostpart = slash < 0 ? u : u.substring(0, slash);
  o.path = slash < 0 ? "/" : u.substring(slash);
  int at = hostpart.lastIndexOf('@');
  if (at >= 0) {
    o.basic = b64(hostpart.substring(0, at));
    hostpart = hostpart.substring(at + 1);
  }
  int colon = hostpart.lastIndexOf(':');
  o.port = o.tls ? 443 : 80;
  if (colon >= 0) {
    o.port = hostpart.substring(colon + 1).toInt();
    hostpart = hostpart.substring(0, colon);
  }
  o.host = hostpart;
  return !o.host.isEmpty();
}

// ---------- buffered reader ----------
struct Rdr {
  WiFiClient* c = nullptr;
  uint8_t b[2048];
  int pos = 0, len = 0;
  bool fill() {
    uint32_t t = millis();
    while (!stopFlag) {
      int a = c->available();
      if (a > 0) {
        int r = c->read(b, a < (int)sizeof b ? a : (int)sizeof b);
        if (r > 0) { pos = 0; len = r; return true; }
      } else if (!c->connected()) {
        return false;
      }
      if (millis() - t > 6000) return false;
      delay(1);
    }
    return false;
  }
  int get() {
    if (pos >= len && !fill()) return -1;
    return b[pos++];
  }
  bool readN(uint8_t* d, size_t n) {
    while (n) {
      if (pos >= len && !fill()) return false;
      size_t k = (size_t)(len - pos) < n ? (size_t)(len - pos) : n;
      memcpy(d, b + pos, k);
      pos += k; d += k; n -= k;
    }
    return true;
  }
  bool line(String& out) {
    out = "";
    for (int i = 0; i < 300; i++) {
      int ch = get();
      if (ch < 0) return false;
      if (ch == '\n') { if (out.endsWith("\r")) out.remove(out.length() - 1); return true; }
      out += (char)ch;
    }
    return true;
  }
};

// ---------- JPEG helpers ----------
// Reads the pixel size from the SOF marker. Returns 0 if not found, -1 for progressive (unsupported).
static int jpegSize(const uint8_t* p, size_t n, int& w, int& h) {
  size_t i = 2;
  while (i + 9 < n) {
    if (p[i] != 0xFF) { i++; continue; }
    uint8_t m = p[i + 1];
    if (m == 0xFF) { i++; continue; }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
    if (m == 0xC2) return -1;
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      h = (p[i + 5] << 8) | p[i + 6];
      w = (p[i + 7] << 8) | p[i + 8];
      return 1;
    }
    i += 2 + ((p[i + 2] << 8) | p[i + 3]);
  }
  return 0;
}

static bool decodeFrame(const uint8_t* jpg, size_t n) {
  int w = 0, h = 0;
  int r = jpegSize(jpg, n, w, h);
  if (r < 0) { setStatus("Progressive JPEG is not supported"); return false; }
  if (r == 0 || w <= 0 || h <= 0) return false;
  int div = 1;
  while ((w / div > uiW() || h / div > uiH()) && div < 8) div *= 2;
  int dw = w / div, dh = h / div;
  if (!spr[0]) {
    for (int i = 0; i < 2; i++) {
      spr[i] = new lgfx::LGFX_Sprite();
      spr[i]->setPsram(true);
      spr[i]->setColorDepth(16);
      if (!spr[i]->createSprite(dw, dh)) { setStatus("Out of memory for video"); return false; }
    }
    frameW = dw;
    frameH = dh;
  }
  if (dw > frameW || dh > frameH) { setStatus("Camera changed resolution; reopen"); return false; }
  int back = 1 - frontIdx;
  spr[back]->fillScreen(0);
  if (!spr[back]->drawJpg(jpg, n, 0, 0, frameW, frameH, 0, 0, 1.0f / div, 1.0f / div)) return false;
  // The sprite stores big-endian RGB565, LVGL wants little-endian: swap the bytes of every pixel.
  uint32_t* q = (uint32_t*)spr[back]->getBuffer();
  size_t cnt = ((size_t)frameW * frameH) / 2;
  for (size_t i = 0; i < cnt; i++) {
    uint32_t v = q[i];
    q[i] = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
  }
  readyIdx = back;
  return true;
}

// ---------- the streaming task ----------
static void countFrame() {
  static uint32_t t0 = 0, n = 0;
  uint32_t now = millis();
  if (t0 == 0 || now - t0 > 5000) { t0 = now; n = 0; }
  n++;
  frameCount++;
  if (now - t0 >= 1000) fps = n * 1000.0f / (now - t0);
}

static void camTask(void*) {
  uint8_t* jpg = (uint8_t*)ps_malloc(JPG_MAX);
  Rdr* rd = new Rdr();
  bool haMode = srcEntity.length() > 0;
  int mode = 0;  // 0 = MJPEG stream, 1 = snapshot polling
  frameW = frameH = 0;
  frameCount = 0;
  fps = 0;
  if (!jpg || !rd) {
    setStatus("Out of memory");
    stopFlag = true;
  }
  while (!stopFlag) {
    Url u;
    String auth;
    if (haMode) {
      bool ext = haEndpointName() == "external" || cfg.haHost.isEmpty();
      u.host = ext ? cfg.haExtHost : cfg.haHost;
      u.port = ext ? cfg.haExtPort : cfg.haPort;
      u.tls = ext ? cfg.haExtTls : cfg.haTls;
      if (u.host.isEmpty() || cfg.haToken.isEmpty()) { setStatus("Home Assistant is not set up"); delay(2000); continue; }
      u.path = String(mode == 0 ? "/api/camera_proxy_stream/" : "/api/camera_proxy/") + srcEntity;
      auth = "Authorization: Bearer " + cfg.haToken + "\r\n";
    } else {
      if (!parseUrl(srcUrl, u)) { setStatus("Camera URL must start with http:// or https://"); delay(2000); continue; }
      if (u.basic.length()) auth = "Authorization: Basic " + u.basic + "\r\n";
    }
    if (WiFi.status() != WL_CONNECTED) { setStatus("No Wi-Fi"); delay(1000); continue; }

    WiFiClient plain;
    WiFiClientSecure sec;
    sec.setInsecure();  // certificates are not verified (same as the OTA download)
    WiFiClient* c = u.tls ? (WiFiClient*)&sec : &plain;
    c->setTimeout(4);
    setStatus(mode == 0 ? "Connecting..." : "Loading snapshot...");
    if (!c->connect(u.host.c_str(), u.port, 4000)) { setStatus("Cannot reach the camera"); delay(2000); continue; }
    // HTTP/1.0 keeps servers from using chunked encoding on the stream.
    c->print("GET " + u.path + " HTTP/1.0\r\nHost: " + u.host + "\r\n" + auth + "Accept: */*\r\nConnection: close\r\n\r\n");
    rd->c = c;
    rd->pos = rd->len = 0;

    String l;
    if (!rd->line(l) || !l.startsWith("HTTP/")) { setStatus("No response from the camera"); delay(2000); continue; }
    int code = l.substring(l.indexOf(' ') + 1).toInt();
    String ctype;
    int clen = -1;
    bool chunked = false;
    while (rd->line(l) && l.length()) {
      String lo = l;
      lo.toLowerCase();
      if (lo.startsWith("content-type:")) ctype = lo.substring(13);
      else if (lo.startsWith("content-length:")) clen = lo.substring(15).toInt();
      else if (lo.startsWith("transfer-encoding:") && lo.indexOf("chunked") > 0) chunked = true;
    }
    if (code == 401 || code == 403) { setStatus(haMode ? "Home Assistant rejected the token" : "Camera login rejected (basic auth only)"); delay(3000); continue; }
    if (code != 200) {
      if (haMode && mode == 0) { mode = 1; continue; }  // no live proxy for this camera: poll snapshots instead
      char b[48];
      snprintf(b, sizeof b, "Camera answered HTTP %d", code);
      setStatus(b);
      delay(3000);
      continue;
    }
    if (chunked) { setStatus("Chunked stream not supported"); delay(3000); continue; }

    if (ctype.indexOf("multipart") >= 0) {
      setStatus("Waiting for video...");
      bool inPart = false;
      int plen = -1;
      while (!stopFlag) {
        if (!rd->line(l)) break;
        String lo = l;
        lo.toLowerCase();
        if (l.startsWith("--")) { inPart = true; plen = -1; continue; }
        if (!inPart) continue;
        if (lo.startsWith("content-length:")) plen = lo.substring(15).toInt();
        if (l.length() != 0) continue;
        // headers done: the JPEG follows
        size_t n = 0;
        if (plen > 0 && (size_t)plen <= JPG_MAX) {
          if (!rd->readN(jpg, plen)) break;
          n = plen;
        } else {  // no length: read from the JPEG start (FFD8) to its end (FFD9)
          int prev = 0;
          bool started = false;
          while (n < JPG_MAX) {
            int ch = rd->get();
            if (ch < 0) { n = 0; break; }
            if (!started) {
              if (prev == 0xFF && ch == 0xD8) { jpg[0] = 0xFF; jpg[1] = 0xD8; n = 2; started = true; }
            } else {
              jpg[n++] = ch;
              if (prev == 0xFF && ch == 0xD9) break;
            }
            prev = ch;
          }
          if (n == 0 || n >= JPG_MAX) break;
        }
        inPart = false;
        if (decodeFrame(jpg, n)) countFrame();
        else if (stopFlag) break;
      }
      if (haMode && frameCount == 0) mode = 1;  // stream gave nothing: try snapshots
    } else if (ctype.indexOf("jpeg") >= 0 || ctype.indexOf("jpg") >= 0) {
      size_t n = 0;
      if (clen > 0 && (size_t)clen <= JPG_MAX) {
        if (rd->readN(jpg, clen)) n = clen;
      } else {
        int ch;
        while (n < JPG_MAX && (ch = rd->get()) >= 0) jpg[n++] = ch;
      }
      if (n && decodeFrame(jpg, n)) countFrame();
      c->stop();
      delay(80);
      continue;
    } else {
      setStatus("Unsupported stream type (needs MJPEG or JPEG)");
      delay(3000);
    }
    c->stop();
    if (!stopFlag && statusBuf[0] != 'P') setStatus("Reconnecting...");
    delay(500);
  }
  free(jpg);
  delete rd;
  taskDone = true;
  vTaskDelete(nullptr);
}

// ---------- UI side ----------
static void buildOverlay() {
  if (camScr) return;
  camScr = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(camScr);
  lv_obj_set_size(camScr, uiW(), uiH());
  lv_obj_set_style_bg_color(camScr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(camScr, LV_OPA_COVER, 0);
  lv_obj_add_flag(camScr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(camScr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(camScr, [](lv_event_t*) { cameraClose(); }, LV_EVENT_CLICKED, nullptr);  // a tap or a swipe closes
  camImg = lv_image_create(camScr);
  lv_obj_add_flag(camImg, LV_OBJ_FLAG_HIDDEN);
  camLabel = lv_label_create(camScr);
  lv_obj_set_style_text_font(camLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(camLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(camLabel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(camLabel, LV_OPA_70, 0);
  lv_obj_set_style_pad_all(camLabel, 4, 0);
  lv_obj_set_width(camLabel, uiW() - 20);
  lv_obj_align(camLabel, LV_ALIGN_TOP_LEFT, 6, 6);
  lv_obj_add_flag(camScr, LV_OBJ_FLAG_HIDDEN);
}

static void startTask(const String& label, const String& entity, const String& url) {
  srcLabel = label;
  srcEntity = entity;
  srcUrl = url;
  stopFlag = false;
  taskDone = false;
  readyIdx = -1;
  frontIdx = 0;
  shown = false;
  setStatus("Connecting...");
  xTaskCreatePinnedToCore(camTask, "camera", 12288, nullptr, 1, nullptr, 0);
}

void cameraOpen(const String& label, const String& entityId, const String& url, bool fullBrightness) {
  buildOverlay();
  isOpen = true;
  uiSetFullBrightness(fullBrightness);
  lv_obj_add_flag(camImg, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(camScr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(camScr);
  if (!taskDone) {  // an older stream is still shutting down: start the new one when it has ended
    stopFlag = true;
    pendingOpen = true;
    pLabel = label; pEntity = entityId; pUrl = url;
    setStatus("Connecting...");
  } else {
    startTask(label, entityId, url);
  }
}

void cameraClose() {
  if (!isOpen) return;
  isOpen = false;
  uiSetFullBrightness(false);  // back to the saved brightness
  pendingOpen = false;
  stopFlag = true;
  if (camScr) lv_obj_add_flag(camScr, LV_OBJ_FLAG_HIDDEN);
}

bool cameraIsOpen() { return isOpen; }

void cameraTick() {
  if (taskDone) {
    if (pendingOpen && isOpen) {
      pendingOpen = false;
      startTask(pLabel, pEntity, pUrl);
    } else if (!isOpen && spr[0]) {  // closed and the task is gone: give the video memory back
      for (int i = 0; i < 2; i++) { delete spr[i]; spr[i] = nullptr; }
    }
  }
  if (!isOpen) return;
  int idx = readyIdx;
  if (idx >= 0 && spr[idx]) {
    readyIdx = -1;
    frontIdx = idx;
    lv_image_dsc_t& d = dsc[idx];
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_RGB565;
    d.header.w = frameW;
    d.header.h = frameH;
    d.header.stride = frameW * 2;
    d.data_size = frameW * frameH * 2;
    d.data = (const uint8_t*)spr[idx]->getBuffer();
    lv_image_set_src(camImg, &d);
    lv_obj_invalidate(camImg);  // same pixels pointer may be reused, so force a redraw
    if (!shown) {
      shown = true;
      lv_obj_remove_flag(camImg, LV_OBJ_FLAG_HIDDEN);
      lv_obj_center(camImg);
    }
  }
  if (millis() - lastLabel > 400) {
    lastLabel = millis();
    String t = srcLabel.length() ? srcLabel : String("Camera");
    if (shown && taskDone == false && frameCount > 0) t += String("   ") + String(fps, 1) + " fps   (tap to close)";
    else t += String("   ") + statusBuf + "   (tap to close)";
    lv_label_set_text(camLabel, t.c_str());
  }
}
