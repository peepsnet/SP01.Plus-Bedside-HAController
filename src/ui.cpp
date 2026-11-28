#include "ui.h"
#include "display.h"
#include "config.h"
#include "menu.h"
#include <ArduinoJson.h>
#include <lvgl.h>
#include <map>
#include <algorithm>
#include <cmath>
#include <memory>

static LGFX tft;
static lv_display_t* disp;
static uint8_t* drawBuf;
static ButtonHandler pressHandler;

// One on-screen control (button or slider). Owned by `widgets`; LVGL events hold raw pointers into it.
struct Widget {
  bool slider = false;
  String entity, action, attr, label;
  float mul = 1;       // slider units per displayed unit (climate uses half degrees)
  const char* unit = "";
  lv_obj_t* obj = nullptr;
  lv_obj_t* text = nullptr;
  uint32_t lastSend = 0;
};
static std::vector<std::unique_ptr<Widget>> widgets;
static std::map<String, std::vector<Widget*>> byEntity;
struct EntityInfo {
  bool on = false;
  bool unavailable = false;
  HaLevels lv;
};
static std::map<String, EntityInfo> known;  // latest state per entity, so a rebuild restores it
static SliderHandler slideHandler;
static std::vector<lv_obj_t*> pages;
static std::vector<lv_obj_t*> tabs;
static lv_obj_t* statusLabel;
static lv_obj_t* canvasHolder;
static lv_obj_t* barHolder;
static std::vector<String> entityList;
static String statusText;

static void flushCb(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  const int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  tft.startWrite();
  tft.pushImage(a->x1, a->y1, w, h, (lgfx::rgb565_t*)px);
  tft.endWrite();
  lv_display_flush_ready(d);
}

// --- gestures ---------------------------------------------------------------------------------------------
// Detected from raw touch points: swipe down from the top edge opens the brightness panel, swipe left/right
// changes page. Gestures that start on a slider are ignored so dragging a slider never flips the page.
static bool touching = false;
static int32_t tx0, ty0, tx, ty;
static bool sliderHeld = false;
static uint32_t lastSwipeMs = 0;  // buttons ignore the click that a swipe would otherwise end with
enum PendingSwipe { SW_NONE, SW_OPEN_BRIGHT, SW_CLOSE_BRIGHT, SW_NEXT, SW_PREV };
static PendingSwipe pendingSwipe = SW_NONE;
static bool brightOpen = false;
static constexpr int TOP_ZONE = 44;      // swipe must start within this many pixels of the top edge
static constexpr int SWIPE_MIN = 70;     // horizontal distance
static constexpr int PULL_MIN = 50;      // vertical distance
static inline bool recentlySwiped() { return millis() - lastSwipeMs < 500; }

static void evalSwipe() {
  if (sliderHeld || menuIsOpen()) return;
  int dx = tx - tx0, dy = ty - ty0;
  if (brightOpen) {
    if (dy < -PULL_MIN && -dy > abs(dx)) pendingSwipe = SW_CLOSE_BRIGHT;
  } else if (ty0 < TOP_ZONE && dy > PULL_MIN && dy > abs(dx)) {
    pendingSwipe = SW_OPEN_BRIGHT;
  } else if (abs(dx) > SWIPE_MIN && abs(dx) > abs(dy) * 2 && ty0 < UI_CANVAS_H) {
    pendingSwipe = dx < 0 ? SW_NEXT : SW_PREV;
  }
  if (pendingSwipe != SW_NONE) lastSwipeMs = millis();
}

static void touchCb(lv_indev_t*, lv_indev_data_t* data) {
  int32_t x, y;
  if (tft.getTouch(&x, &y)) {
    if (!touching) {
      touching = true;
      tx0 = x;
      ty0 = y;
    }
    tx = x;
    ty = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    if (touching) {
      touching = false;
      evalSwipe();  // runs before LVGL delivers the release/click events for this touch
    }
    data->state = LV_INDEV_STATE_RELEASED;
  }
  data->point.x = tx;
  data->point.y = ty;
}

// --- status icons -----------------------------------------------------------------------------------------
static const uint32_t C_OK = 0x2ED39A, C_WARN = 0xFFB300, C_BAD = 0xFF5252, C_OFF = 0x6B7480;
static lv_obj_t* wifiIcon;
static lv_obj_t* haPill;
static lv_obj_t* haPillLabel;

static void buildStatusIcons() {
  lv_obj_t* scr = lv_screen_active();
  wifiIcon = lv_label_create(scr);
  lv_obj_set_style_text_font(wifiIcon, &lv_font_montserrat_24, 0);
  lv_label_set_text(wifiIcon, LV_SYMBOL_WIFI);
  lv_obj_set_pos(wifiIcon, 338, 288);
  lv_obj_add_flag(wifiIcon, LV_OBJ_FLAG_CLICKABLE);  // tap the Wi-Fi icon to open the Wi-Fi settings
  lv_obj_set_ext_click_area(wifiIcon, 10);
  lv_obj_add_event_cb(wifiIcon, [](lv_event_t*) { menuOpen(true); }, LV_EVENT_CLICKED, nullptr);

  // Outlined pill like a connection badge: colored border and text, transparent inside.
  haPill = lv_obj_create(scr);
  lv_obj_remove_style_all(haPill);
  lv_obj_set_size(haPill, LV_SIZE_CONTENT, 28);
  lv_obj_set_pos(haPill, 374, 286);
  lv_obj_set_style_border_width(haPill, 2, 0);
  lv_obj_set_style_radius(haPill, 8, 0);
  lv_obj_set_style_pad_hor(haPill, 8, 0);
  lv_obj_set_style_pad_ver(haPill, 2, 0);
  haPillLabel = lv_label_create(haPill);
  lv_obj_set_style_text_font(haPillLabel, &lv_font_montserrat_18, 0);
  lv_label_set_text(haPillLabel, "HA");
  uiSetWifiState(0, 0);
  uiSetHaState(HA_NOT_CONFIGURED);
}

static void setColor(lv_obj_t* o, uint32_t c, bool border) {
  if (border) lv_obj_set_style_border_color(o, lv_color_hex(c), 0);
  else lv_obj_set_style_text_color(o, lv_color_hex(c), 0);
}

void uiSetWifiState(int state, int rssi) {
  if (!wifiIcon) return;
  uint32_t c = C_OFF;
  if (state == 1 || state == 2) c = C_WARN;
  else if (state == 3) c = rssi > -75 ? C_OK : C_WARN;  // weak signal shows amber
  setColor(wifiIcon, c, false);
}

void uiSetHaState(HaState st) {
  if (!haPill) return;
  uint32_t c = C_OFF;
  const char* txt = "HA";
  switch (st) {
    case HA_LOCAL: c = C_OK; break;
    case HA_EXTERNAL: c = C_OK; txt = "HA ext"; break;  // connected through the external address
    case HA_CONNECTING: c = C_WARN; break;
    case HA_AUTH_ERROR: c = C_BAD; break;
    default: break;
  }
  lv_label_set_text(haPillLabel, txt);
  setColor(haPill, c, true);
  setColor(haPillLabel, c, false);
}

static lv_obj_t* brightScrim;
static lv_obj_t* brightPanel;
static lv_obj_t* brightSlider;
static lv_obj_t* brightLabel;
static lv_timer_t* brightTimer;

void uiSetBrightnessPct(int pct, bool save) {
  pct = constrain(pct, cfg.brightMinPct, cfg.brightMaxPct);
  tft.setBrightness((uint8_t)(pct * 255 / 100));
  cfg.brightnessPct = pct;
  if (save) configSave();
}

static void showBrightness(bool open) {
  brightOpen = open;
  if (open) {
    lv_slider_set_value(brightSlider, cfg.brightnessPct, LV_ANIM_OFF);
    String t = String("Brightness  ") + cfg.brightnessPct + "%";
    lv_label_set_text(brightLabel, t.c_str());
    lv_obj_remove_flag(brightScrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(brightPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(brightScrim);
    lv_obj_move_foreground(brightPanel);
    lv_timer_reset(brightTimer);
    lv_timer_resume(brightTimer);
  } else {
    lv_obj_add_flag(brightScrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(brightPanel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(brightTimer);
  }
}

static void buildBrightnessPanel() {
  lv_obj_t* scr = lv_screen_active();
  // Tapping outside the panel closes it.
  brightScrim = lv_obj_create(scr);
  lv_obj_remove_style_all(brightScrim);
  lv_obj_set_size(brightScrim, 480, 320);
  lv_obj_add_flag(brightScrim, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(brightScrim, [](lv_event_t*) { showBrightness(false); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(brightScrim, LV_OBJ_FLAG_HIDDEN);

  brightPanel = lv_obj_create(scr);
  lv_obj_set_size(brightPanel, 480, 110);
  lv_obj_set_pos(brightPanel, 0, 0);
  lv_obj_set_style_bg_color(brightPanel, lv_color_hex(0x0B0E11), 0);
  lv_obj_set_style_bg_opa(brightPanel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(brightPanel, 0, 0);
  lv_obj_set_style_radius(brightPanel, 0, 0);
  lv_obj_set_style_pad_all(brightPanel, 14, 0);
  lv_obj_remove_flag(brightPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(brightPanel, LV_OBJ_FLAG_HIDDEN);

  brightLabel = lv_label_create(brightPanel);
  lv_obj_set_style_text_font(brightLabel, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(brightLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(brightLabel, LV_ALIGN_TOP_LEFT, 0, 0);

  brightSlider = lv_slider_create(brightPanel);
  lv_obj_set_size(brightSlider, 420, 22);
  lv_obj_align(brightSlider, LV_ALIGN_TOP_MID, 0, 52);
  lv_slider_set_range(brightSlider, cfg.brightMinPct, cfg.brightMaxPct);
  lv_obj_set_style_pad_all(brightSlider, 10, LV_PART_KNOB);
  lv_obj_add_event_cb(brightSlider, [](lv_event_t*) { sliderHeld = true; }, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(brightSlider, [](lv_event_t*) { sliderHeld = false; uiSetBrightnessPct(lv_slider_get_value(brightSlider), true); },
                      LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(brightSlider, [](lv_event_t*) {
    int v = lv_slider_get_value(brightSlider);
    uiSetBrightnessPct(v, false);  // live preview; saved on release
    String t = String("Brightness  ") + v + "%";
    lv_label_set_text(brightLabel, t.c_str());
    lv_timer_reset(brightTimer);
  }, LV_EVENT_VALUE_CHANGED, nullptr);

  // Close by itself after 6 s without interaction.
  brightTimer = lv_timer_create([](lv_timer_t*) { if (!sliderHeld) showBrightness(false); }, 6000, nullptr);
  lv_timer_pause(brightTimer);
}

void uiInit(ButtonHandler onPress, SliderHandler onSlide) {
  pressHandler = onPress;
  slideHandler = onSlide;
  tft.init();
  tft.setRotation(1);  // GUESS: landscape with USB port on the correct side; flip to 3 if upside down
  tft.fillScreen(TFT_BLACK);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  const size_t bufPx = 480 * 40;  // 40 lines, partial render
  drawBuf = (uint8_t*)heap_caps_malloc(bufPx * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  disp = lv_display_create(480, 320);
  lv_display_set_flush_cb(disp, flushCb);
  lv_display_set_buffers(disp, drawBuf, nullptr, bufPx * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_indev_t* in = lv_indev_create();
  lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(in, touchCb);

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101214), 0);
  // LVGL's default text color is dark; set white on the screen so every label inherits readable text.
  lv_obj_set_style_text_color(scr, lv_color_hex(0xFFFFFF), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  canvasHolder = lv_obj_create(scr);
  lv_obj_remove_style_all(canvasHolder);
  lv_obj_set_size(canvasHolder, UI_CANVAS_W, UI_CANVAS_H);
  lv_obj_set_pos(canvasHolder, 0, 0);
  barHolder = lv_obj_create(scr);
  lv_obj_remove_style_all(barHolder);
  lv_obj_set_size(barHolder, 320, 40);
  lv_obj_set_pos(barHolder, 0, UI_CANVAS_H);
  lv_obj_set_flex_flow(barHolder, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(barHolder, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(barHolder, 4, 0);
  lv_obj_set_style_pad_column(barHolder, 6, 0);
  statusLabel = nullptr;
  buildStatusIcons();
  buildBrightnessPanel();
  uiSetBrightnessPct(cfg.brightnessPct, false);
}

static size_t curPage = 0;

static void showPage(size_t idx) {
  curPage = idx;
  for (size_t i = 0; i < pages.size(); i++) {
    if (i == idx) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    if (i < tabs.size()) {
      if (i == idx) lv_obj_add_state(tabs[i], LV_STATE_CHECKED);
      else lv_obj_remove_state(tabs[i], LV_STATE_CHECKED);
    }
  }
}

static const char* iconSymbol(const String& n) {
  if (n == "power") return LV_SYMBOL_POWER;
  if (n == "eye") return LV_SYMBOL_EYE_OPEN;
  if (n == "home") return LV_SYMBOL_HOME;
  if (n == "play") return LV_SYMBOL_PLAY;
  if (n == "pause") return LV_SYMBOL_PAUSE;
  if (n == "bell") return LV_SYMBOL_BELL;
  if (n == "settings") return LV_SYMBOL_SETTINGS;
  if (n == "ok") return LV_SYMBOL_OK;
  if (n == "wifi") return LV_SYMBOL_WIFI;
  if (n == "audio") return LV_SYMBOL_AUDIO;
  if (n == "charge") return LV_SYMBOL_CHARGE;
  if (n == "image") return LV_SYMBOL_IMAGE;
  return nullptr;
}

static void applyState(lv_obj_t* b, bool on) {
  if (on) lv_obj_add_state(b, LV_STATE_CHECKED);
  else lv_obj_remove_state(b, LV_STATE_CHECKED);
}

// Slider range and unit by domain/attribute. Defaults can be overridden with "min"/"max" in the layout.
static void sliderSpec(Widget& w, JsonObject b, int& mn, int& mx) {
  int dot = w.entity.indexOf('.');
  String domain = dot > 0 ? w.entity.substring(0, dot) : "";
  mn = 0; mx = 100; w.mul = 1; w.unit = "%";
  if (domain == "light" && w.attr == "color_temp") { mn = 2000; mx = 6500; w.unit = "K"; }
  else if (domain == "climate") { mn = 10; mx = 30; w.unit = "\xC2\xB0"; w.mul = 2; }
  else if (domain == "number" || domain == "input_number") { w.unit = ""; }
  mn = b["min"] | mn;
  mx = b["max"] | mx;
}

static float levelFor(const Widget& w, const EntityInfo& e) {
  int dot = w.entity.indexOf('.');
  String d = dot > 0 ? w.entity.substring(0, dot) : "";
  if (d == "light") return w.attr == "color_temp" ? e.lv.kelvin : e.lv.brightness;
  if (d == "cover") return e.lv.position;
  if (d == "fan") return e.lv.percentage;
  if (d == "media_player") return e.lv.volume;
  if (d == "climate") return e.lv.temperature;
  if (d == "number" || d == "input_number") return e.lv.value;
  return NAN;
}

static void refreshWidget(Widget& w, const EntityInfo& e) {
  if (e.unavailable) lv_obj_add_state(w.obj, LV_STATE_DISABLED);
  else lv_obj_remove_state(w.obj, LV_STATE_DISABLED);
  if (!w.slider) {
    applyState(w.obj, e.on);
    return;
  }
  float v = levelFor(w, e);
  if (std::isnan(v) || lv_slider_is_dragged(w.obj)) return;  // never fight the user's finger
  lv_slider_set_value(w.obj, (int)lroundf(v * w.mul), LV_ANIM_OFF);
  String t = w.label + "  " + String(v, w.mul > 1 ? 1 : 0) + w.unit;
  lv_label_set_text(w.text, t.c_str());
}

static void buildButton(Widget& w, lv_obj_t* page, lv_color_t c, JsonObject b, int x, int y, int bw, int bh) {
  lv_obj_t* btn = lv_button_create(page);
  w.obj = btn;
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, bw, bh);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_set_style_bg_color(btn, lv_color_darken(c, 180), 0);  // off
  lv_obj_set_style_bg_color(btn, c, LV_STATE_CHECKED);         // on
  lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  const char* sym = iconSymbol(String(b["icon"] | ""));
  if (sym) {
    lv_obj_t* ic = lv_label_create(btn);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
    lv_label_set_text(ic, sym);
  }
  lv_obj_t* lb = lv_label_create(btn);
  lv_obj_set_style_text_font(lb, &lv_font_montserrat_18, 0);
  lv_label_set_text(lb, w.label.c_str());
  w.text = lb;
  if (w.entity.length()) {
    lv_obj_add_event_cb(btn, [](lv_event_t* e) {
      if (recentlySwiped()) return;  // this click only ended a swipe
      auto* cx = (Widget*)lv_event_get_user_data(e);
      if (pressHandler) pressHandler(cx->entity, cx->action);
    }, LV_EVENT_CLICKED, &w);
  }
}

static void buildSlider(Widget& w, lv_obj_t* page, lv_color_t c, JsonObject b, int x, int y, int bw, int bh) {
  w.attr = String(b["attr"] | "");
  int mn, mx;
  sliderSpec(w, b, mn, mx);
  // Card with a caption on top and the slider below it (horizontal when the card is wider than tall).
  lv_obj_t* card = lv_obj_create(page);
  lv_obj_remove_style_all(card);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, bw, bh);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x23272B), 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* cap = lv_label_create(card);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
  lv_label_set_text(cap, w.label.c_str());
  lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);
  w.text = cap;
  lv_obj_t* sl = lv_slider_create(card);
  w.obj = sl;
  bool horizontal = bw >= bh;
  if (horizontal) {
    lv_obj_set_size(sl, bw - 28, 12);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -6);
  } else {
    lv_obj_set_size(sl, 12, bh - 50);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -6);
  }
  lv_slider_set_range(sl, mn * w.mul, mx * w.mul);
  lv_obj_set_style_bg_color(sl, c, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, c, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 6, LV_PART_KNOB);
  if (w.entity.length()) {
    auto send = [](lv_event_t* e) {
      auto* cx = (Widget*)lv_event_get_user_data(e);
      float v = lv_slider_get_value(cx->obj) / cx->mul;
      if (slideHandler) slideHandler(cx->entity, cx->attr, v);
    };
    lv_obj_add_event_cb(sl, [](lv_event_t*) { sliderHeld = true; }, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(sl, [](lv_event_t* e) { sliderHeld = false; }, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(sl, send, LV_EVENT_RELEASED, &w);
    // While dragging: update the caption and send at most every 300 ms so lights follow the finger.
    lv_obj_add_event_cb(sl, [](lv_event_t* e) {
      auto* cx = (Widget*)lv_event_get_user_data(e);
      float v = lv_slider_get_value(cx->obj) / cx->mul;
      String t = cx->label + "  " + String(v, cx->mul > 1 ? 1 : 0) + cx->unit;
      lv_label_set_text(cx->text, t.c_str());
      if (lv_slider_is_dragged(cx->obj) && millis() - cx->lastSend > 300) {
        cx->lastSend = millis();
        if (slideHandler) slideHandler(cx->entity, cx->attr, v);
      }
    }, LV_EVENT_VALUE_CHANGED, &w);
  }
}

bool uiBuild(const String& layoutJson) {
  JsonDocument doc;
  if (deserializeJson(doc, layoutJson) || !doc["pages"].is<JsonArray>()) return false;

  byEntity.clear();
  widgets.clear();
  pages.clear();
  tabs.clear();
  entityList.clear();
  lv_obj_clean(canvasHolder);
  lv_obj_clean(barHolder);

  JsonArray pg = doc["pages"].as<JsonArray>();
  const bool multi = pg.size() > 1;
  for (JsonObject p : pg) {
    lv_obj_t* page = lv_obj_create(canvasHolder);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, UI_CANVAS_W, UI_CANVAS_H);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    pages.push_back(page);
    size_t pageIdx = pages.size() - 1;

    if (multi) {
      lv_obj_t* t = lv_button_create(barHolder);
      lv_obj_set_size(t, LV_SIZE_CONTENT, 30);
      lv_obj_set_style_bg_color(t, lv_color_hex(0x2B2F33), 0);
      lv_obj_set_style_bg_color(t, lv_color_hex(0x3D7BD9), LV_STATE_CHECKED);
      lv_obj_t* l = lv_label_create(t);
      lv_label_set_text(l, p["name"] | "Page");
      lv_obj_center(l);
      lv_obj_add_event_cb(t, [](lv_event_t* e) {
        if (recentlySwiped()) return;
        showPage((size_t)(uintptr_t)lv_event_get_user_data(e));
      }, LV_EVENT_CLICKED, (void*)(uintptr_t)pageIdx);
      tabs.push_back(t);
    }

    for (JsonObject b : p["buttons"].as<JsonArray>()) {
      auto w = std::unique_ptr<Widget>(new Widget());
      w->entity = String(b["entity_id"] | "");
      w->action = String(b["action"] | "toggle");
      w->label = String(b["label"] | "");
      String type = b["type"] | "button";
      w->slider = type == "slider";
      const char* hex = b["color"] | "#4A90E2";
      lv_color_t c = lv_color_hex(strtoul(hex[0] == '#' ? hex + 1 : hex, nullptr, 16));
      int bx = b["x"] | 0, by = b["y"] | 0, bw = b["w"] | 100, bh = b["h"] | 70;

      if (w->slider) buildSlider(*w, page, c, b, bx, by, bw, bh);
      else buildButton(*w, page, c, b, bx, by, bw, bh);

      if (w->entity.length()) {
        if (std::find(entityList.begin(), entityList.end(), w->entity) == entityList.end())
          entityList.push_back(w->entity);
        byEntity[w->entity].push_back(w.get());
        auto it = known.find(w->entity);
        if (it != known.end()) refreshWidget(*w, it->second);
      }
      widgets.push_back(std::move(w));
    }
  }
  if (!pages.empty()) showPage(0);
  return true;
}

void uiSetState(const String& entityId, const String& state, const HaLevels& lv) {
  static const char* ON_STATES[] = {"on", "open", "opening", "playing", "unlocked", "home", "heat", "cool",
                                    "heat_cool", "auto", "dry", "fan_only"};
  EntityInfo& e = known[entityId];
  e.on = false;
  for (auto* o : ON_STATES) if (state == o) e.on = true;
  e.unavailable = (state == "unavailable");
  // Merge: updates may carry only the attributes that changed.
  auto merge = [](float& dst, float src) { if (!std::isnan(src)) dst = src; };
  merge(e.lv.brightness, lv.brightness);
  merge(e.lv.kelvin, lv.kelvin);
  merge(e.lv.position, lv.position);
  merge(e.lv.percentage, lv.percentage);
  merge(e.lv.volume, lv.volume);
  merge(e.lv.temperature, lv.temperature);
  merge(e.lv.value, lv.value);
  auto it = byEntity.find(entityId);
  if (it == byEntity.end()) return;
  for (Widget* w : it->second) refreshWidget(*w, e);
}

static lv_obj_t* banner;
static lv_obj_t* bannerLabel;

void uiSetBanner(const String& text) {
  if (!banner) {
    banner = lv_obj_create(lv_screen_active());
    lv_obj_set_size(banner, 440, LV_SIZE_CONTENT);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(banner, lv_color_hex(0x0B0E11), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(banner, lv_color_hex(0xFFC107), 0);
    lv_obj_set_style_border_width(banner, 3, 0);
    lv_obj_set_style_radius(banner, 12, 0);
    lv_obj_set_style_pad_all(banner, 14, 0);
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    bannerLabel = lv_label_create(banner);
    lv_obj_set_width(bannerLabel, 410);
    lv_obj_set_style_text_font(bannerLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(bannerLabel, lv_color_hex(0xFFFFFF), 0);
  }
  if (text.isEmpty()) {
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(bannerLabel, text.c_str());
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(banner);
  }
}

void uiSetStatus(const String& text) { statusText = text; }
String uiStatusText() { return statusText; }

std::vector<String> uiEntities() { return entityList; }

void uiTick() {
  PendingSwipe sw = pendingSwipe;
  pendingSwipe = SW_NONE;
  if (sw == SW_OPEN_BRIGHT) showBrightness(true);
  else if (sw == SW_CLOSE_BRIGHT) showBrightness(false);
  else if (sw == SW_NEXT && curPage + 1 < pages.size()) showPage(curPage + 1);
  else if (sw == SW_PREV && curPage > 0) showPage(curPage - 1);
  lv_timer_handler();
}
