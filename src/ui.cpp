#include "ui.h"
#include "display.h"
#include <ArduinoJson.h>
#include <lvgl.h>
#include <map>
#include <memory>

static LGFX tft;
static lv_display_t* disp;
static uint8_t* drawBuf;
static ButtonHandler pressHandler;

struct BtnCtx {
  String entity;
  String action;
};
static std::vector<std::unique_ptr<BtnCtx>> ctxs;
static std::map<String, std::vector<lv_obj_t*>> byEntity;
static std::map<String, bool> lastOn;  // remembered so a rebuild restores button state
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

void uiInit(ButtonHandler onPress) {
  pressHandler = onPress;
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

bool uiBuild(const String& layoutJson) {
  JsonDocument doc;
  if (deserializeJson(doc, layoutJson) || !doc["pages"].is<JsonArray>()) return false;

  byEntity.clear();
  ctxs.clear();
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
      lv_obj_t* btn = lv_button_create(page);
      lv_obj_set_pos(btn, b["x"] | 0, b["y"] | 0);
      lv_obj_set_size(btn, b["w"] | 100, b["h"] | 70);
      lv_obj_set_style_radius(btn, 12, 0);
      const char* hex = b["color"] | "#4A90E2";
      lv_color_t c = lv_color_hex(strtoul(hex[0] == '#' ? hex + 1 : hex, nullptr, 16));
      lv_obj_set_style_bg_color(btn, lv_color_darken(c, 180), 0);       // off
      lv_obj_set_style_bg_color(btn, c, LV_STATE_CHECKED);              // on
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
      lv_label_set_text(lb, b["label"] | "");

      String ent = b["entity_id"] | "";
      String act = b["action"] | "toggle";
      if (ent.length()) {
        ctxs.emplace_back(new BtnCtx{ent, act});
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
          auto* cx = (BtnCtx*)lv_event_get_user_data(e);
          if (pressHandler) pressHandler(cx->entity, cx->action);
        }, LV_EVENT_CLICKED, ctxs.back().get());
        byEntity[ent].push_back(btn);
        if (std::find(entityList.begin(), entityList.end(), ent) == entityList.end())
          entityList.push_back(ent);
        auto it = lastOn.find(ent);
        if (it != lastOn.end()) applyState(btn, it->second);
      }
    }
  }
  if (multi) {
    statusLabel = lv_label_create(barHolder);
  } else {
    statusLabel = lv_label_create(barHolder);
  }
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(0x8A9199), 0);
  lv_label_set_text(statusLabel, statusText.c_str());
  if (!pages.empty()) showPage(0);
  return true;
}

void uiSetState(const String& entityId, const String& state) {
  bool on = (state == "on" || state == "open" || state == "playing" || state == "unlocked");
  lastOn[entityId] = on;
  auto it = byEntity.find(entityId);
  if (it == byEntity.end()) return;
  for (auto* b : it->second) applyState(b, on);
}

void uiSetStatus(const String& text) {
  statusText = text;
  if (statusLabel) lv_label_set_text(statusLabel, text.c_str());
}

std::vector<String> uiEntities() { return entityList; }

void uiTick() { lv_timer_handler(); }
