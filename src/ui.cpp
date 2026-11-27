#include "ui.h"
#include "display.h"
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

static void touchCb(lv_indev_t*, lv_indev_data_t* data) {
  int32_t x, y;
  if (tft.getTouch(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void uiInit(ButtonHandler onPress, SliderHandler onSlide) {
  pressHandler = onPress;
  slideHandler = onSlide;
  tft.init();
  tft.setRotation(1);  // GUESS: landscape with USB port on the correct side; flip to 3 if upside down
  tft.setBrightness(200);
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
  lv_obj_set_size(barHolder, 430, 40);
  lv_obj_set_pos(barHolder, 0, UI_CANVAS_H);
  lv_obj_set_flex_flow(barHolder, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(barHolder, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(barHolder, 4, 0);
  lv_obj_set_style_pad_column(barHolder, 6, 0);
  statusLabel = nullptr;
}

static void showPage(size_t idx) {
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
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* cap = lv_label_create(card);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_18, 0);
  lv_label_set_text(cap, w.label.c_str());
  lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);
  w.text = cap;
  lv_obj_t* sl = lv_slider_create(card);
  w.obj = sl;
  bool horizontal = bw >= bh;
  if (horizontal) {
    lv_obj_set_size(sl, bw - 28, 18);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -8);
  } else {
    lv_obj_set_size(sl, 18, bh - 60);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -8);
  }
  lv_slider_set_range(sl, mn * w.mul, mx * w.mul);
  lv_obj_set_style_bg_color(sl, c, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, c, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 8, LV_PART_KNOB);
  if (w.entity.length()) {
    auto send = [](lv_event_t* e) {
      auto* cx = (Widget*)lv_event_get_user_data(e);
      float v = lv_slider_get_value(cx->obj) / cx->mul;
      if (slideHandler) slideHandler(cx->entity, cx->attr, v);
    };
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
  if (multi) {
    statusLabel = lv_label_create(barHolder);
  } else {
    statusLabel = lv_label_create(barHolder);
  }
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(0xE8ECEF), 0);
  lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_18, 0);
  lv_label_set_text(statusLabel, statusText.c_str());
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

void uiSetStatus(const String& text) {
  statusText = text;
  if (statusLabel) lv_label_set_text(statusLabel, text.c_str());
}

std::vector<String> uiEntities() { return entityList; }

void uiTick() { lv_timer_handler(); }
