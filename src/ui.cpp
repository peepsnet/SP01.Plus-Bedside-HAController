#include "ui.h"
#include "display.h"
#include "config.h"
#include "camera.h"
#include "menu.h"
#include "fan_icon.h"
#include <ArduinoJson.h>
#include <lvgl.h>
#include <map>
#include <algorithm>
#include <cmath>
#include <memory>

static LGFX tft;
static int scrW = 480, scrH = 320;  // 320x480 when the screen is rotated to portrait
int uiW() { return scrW; }
int uiH() { return scrH; }
int uiCanvasH() { return scrH - 40; }
bool uiPortrait() { return scrW < scrH; }
static lv_display_t* disp;
static uint8_t* drawBuf;
static ButtonHandler pressHandler;

// One on-screen control (button or slider). Owned by `widgets`; LVGL events hold raw pointers into it.
struct Widget {
  bool slider = false;
  bool camera = false;  // button that opens the full-screen camera view
  bool fullBright = false;  // camera: force full brightness while it is open
  bool thermo = false;      // thermostat tile: current temperature, set point and - / + buttons
  bool value = false;       // value tile: title, value (state or attribute), subtitle, optional level bar
  String entity, action, attr, label, url;
  String actionEntity;      // value tile: entity the tap action is sent to (default: the tile's own entity)
  String subEntity, subAttr, subText, uText;  // value tile: subtitle source; thermostat/value: unit override
  bool hasU = false;        // "unit" is set in the layout (may be empty) and replaces the one HA reports
  int decimals = -1;        // value tile: digits after the point, -1 = automatic
  float vmin = 0, vmax = 100, step = 0.5f, tmin = NAN, tmax = NAN;
  int pw = 0, ph = 0, vIdx = 0, sIdx = 0, ctrlH = 0, cbw = 0, topH = 0;  // tile size and chosen font indexes
  float target = NAN;       // thermostat: set point shown while taps are being collected
  bool pending = false;
  uint32_t lastTap = 0, holdUntil = 0;
  lv_obj_t *lA = nullptr, *lB = nullptr, *lC = nullptr, *lBar = nullptr, *lVal = nullptr;
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
  String state;                          // raw HA state text
  std::map<String, String> attrs;        // short scalar attributes (value and thermostat tiles read these)
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
static bool progShown = false;  // firmware update card is up
static constexpr int TOP_ZONE = 44;      // swipe must start within this many pixels of the top edge
static constexpr int SWIPE_MIN = 70;     // horizontal distance
static constexpr int PULL_MIN = 50;      // vertical distance
static inline bool recentlySwiped() { return millis() - lastSwipeMs < 500; }

static void evalSwipe() {
  if (sliderHeld || menuIsOpen() || cameraIsOpen() || progShown) return;
  int dx = tx - tx0, dy = ty - ty0;
  if (brightOpen) {
    if (dy < -PULL_MIN && -dy > abs(dx)) pendingSwipe = SW_CLOSE_BRIGHT;
  } else if (ty0 < TOP_ZONE && dy > PULL_MIN && dy > abs(dx)) {
    pendingSwipe = SW_OPEN_BRIGHT;
  } else if (abs(dx) > SWIPE_MIN && abs(dx) > abs(dy) * 2 && ty0 < uiCanvasH()) {
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
  // Small icons in the bottom bar, left of the gear button, laid out in a row so they follow the screen width.
  lv_obj_t* box = lv_obj_create(scr);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, LV_SIZE_CONTENT, 24);
  lv_obj_align(box, LV_ALIGN_BOTTOM_RIGHT, -54, -8);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
  // START, not END: with a content-sized box, END packing pushes the first icon outside the box where it is clipped away.
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(box, 8, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  wifiIcon = lv_label_create(box);
  lv_obj_set_style_text_font(wifiIcon, &lv_font_montserrat_18, 0);
  lv_label_set_text(wifiIcon, LV_SYMBOL_WIFI);
  lv_obj_add_flag(wifiIcon, LV_OBJ_FLAG_CLICKABLE);  // tap the Wi-Fi icon to open the Wi-Fi settings
  lv_obj_set_ext_click_area(wifiIcon, 12);
  lv_obj_add_event_cb(wifiIcon, [](lv_event_t*) { menuOpen(true); }, LV_EVENT_CLICKED, nullptr);

  // Outlined pill like a connection badge: colored border and text, transparent inside.
  haPill = lv_obj_create(box);
  lv_obj_remove_style_all(haPill);
  lv_obj_set_size(haPill, LV_SIZE_CONTENT, 20);
  lv_obj_set_style_border_width(haPill, 1, 0);
  lv_obj_set_style_radius(haPill, 6, 0);
  lv_obj_set_style_pad_hor(haPill, 6, 0);
  lv_obj_set_style_pad_ver(haPill, 1, 0);
  haPillLabel = lv_label_create(haPill);
  lv_obj_set_style_text_font(haPillLabel, &lv_font_montserrat_14, 0);
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

void uiSetFullBrightness(bool on) { tft.setBrightness(on ? 255 : (uint8_t)(cfg.brightnessPct * 255 / 100)); }

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
  lv_obj_set_size(brightScrim, scrW, scrH);
  lv_obj_add_flag(brightScrim, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(brightScrim, [](lv_event_t*) { showBrightness(false); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(brightScrim, LV_OBJ_FLAG_HIDDEN);

  brightPanel = lv_obj_create(scr);
  lv_obj_set_size(brightPanel, scrW, 110);
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
  lv_obj_set_size(brightSlider, scrW - 60, 22);
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
  // Base orientation is rotation 1 (landscape). cfg.rotation adds 0..3 clockwise quarter turns. The direction is a guess
  // for this panel: if 90 and 270 come out swapped, change the + to a - below.
  tft.setRotation((1 + cfg.rotation) & 3);
  scrW = tft.width();
  scrH = tft.height();
  Serial.printf("Screen rotation setting %d -> panel rotation %d, %dx%d\n", cfg.rotation, (1 + cfg.rotation) & 3, scrW, scrH);
  tft.fillScreen(TFT_BLACK);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  const size_t bufPx = scrW * 40;  // 40 lines, partial render
  drawBuf = (uint8_t*)heap_caps_malloc(bufPx * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  disp = lv_display_create(scrW, scrH);
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
  lv_obj_set_size(canvasHolder, scrW, uiCanvasH());
  lv_obj_set_pos(canvasHolder, 0, 0);
  barHolder = lv_obj_create(scr);
  lv_obj_remove_style_all(barHolder);
  lv_obj_set_size(barHolder, scrW - 150, 40);  // leaves room for the status icons and the gear
  lv_obj_set_pos(barHolder, 0, uiCanvasH());
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
  if (n == "video") return LV_SYMBOL_VIDEO;
  // "fan" is drawn from a bitmap (fan_icon.h), see buildButton
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

// ---- Value and thermostat tiles ---------------------------------------------------------------------------------------
// Text sizes for these tiles, largest first (the same 24/18/16/14/12 px Montserrat sizes as buttons). The web preview uses the
// same rules (valueFit / thermoFit in web_pages.h); keep them in step.
static const lv_font_t* const VAL_FONT[5] = {&lv_font_montserrat_24, &lv_font_montserrat_18, &lv_font_montserrat_16, &lv_font_montserrat_14, &lv_font_montserrat_12};
static const int VAL_LH[5] = {27, 21, 18, 16, 15};
static const int VAL_PX[5] = {24, 18, 16, 14, 12};

static bool parseNum(const String& s, float& v) {
  char* end;
  v = strtof(s.c_str(), &end);
  return end != s.c_str() && *end == 0;
}
static String fmtNum(float v, int decimals) {
  int d = decimals >= 0 ? decimals : (fabsf(v - roundf(v)) < 0.05f ? 0 : 1);
  return String(v, d);
}
static String prettyText(String s) {  // "charging_complete" -> "Charging complete"
  s.replace('_', ' ');
  if (s.length()) s.setCharAt(0, toupper(s[0]));
  return s;
}
static String joinUnit(const String& num, const String& unit) {
  if (!unit.length()) return num;
  const bool tight = unit == "%" || unit.startsWith("\xC2\xB0");
  return num + (tight ? "" : " ") + unit;
}
// Reads an entity's state (attr empty) or one of its attributes. false when it is not known yet / unavailable.
static bool readSource(const String& ent, const String& attr, String& raw, float& num, bool& isNum, String& unit) {
  auto it = known.find(ent);
  if (it == known.end()) return false;
  const EntityInfo& e = it->second;
  if (attr.length()) {
    auto a = e.attrs.find(attr);
    if (a == e.attrs.end()) return false;
    raw = a->second;
    unit = "";
  } else {
    if (e.state == "unavailable" || e.state == "unknown" || !e.state.length()) return false;
    raw = e.state;
    auto u = e.attrs.find("unit_of_measurement");
    unit = u == e.attrs.end() ? String("") : u->second;
  }
  isNum = parseNum(raw, num);
  return true;
}
static String shownText(const String& ent, const String& attr, int decimals, bool hasU, const String& uOver, float* numOut) {
  String raw, unit;
  float num = NAN;
  bool isNum = false;
  if (numOut) *numOut = NAN;
  if (!readSource(ent, attr, raw, num, isNum, unit)) return "--";
  if (!isNum) return prettyText(raw);
  if (numOut) *numOut = num;
  return joinUnit(fmtNum(num, decimals), hasU ? uOver : unit);
}
static int pickFont(const String& t, int from, int maxW, int maxH) {  // first size that fits, -1 if none
  for (int i = from; i < 5; i++)
    if (VAL_LH[i] <= maxH && lv_text_get_width(t.c_str(), t.length(), VAL_FONT[i], 0) <= maxW) return i;
  return -1;
}

// Tile labels are one line: fixed width and height, cut with "..." when the text is too long.
static void tileFont(lv_obj_t* l, int idx) {
  lv_obj_set_style_text_font(l, VAL_FONT[idx], 0);
  lv_obj_set_height(l, VAL_LH[idx]);
}

static void refreshValue(Widget& w) {
  float num;
  String vt = shownText(w.entity, w.attr, w.decimals, w.hasU, w.uText, &num);
  int idx = w.vIdx;
  while (idx < 4 && lv_text_get_width(vt.c_str(), vt.length(), VAL_FONT[idx], 0) > w.pw - 12) idx++;  // long values shrink
  tileFont(w.lVal, idx);
  lv_label_set_text(w.lVal, vt.c_str());
  if (w.lC) {
    String st = w.subText;
    if (w.subEntity.length()) {
      String sv = shownText(w.subEntity, w.subAttr, -1, false, "", nullptr);
      if (!st.length()) st = sv;
      else if (st.indexOf("{}") >= 0) st.replace("{}", sv);
      else st += " " + sv;
    }
    lv_label_set_text(w.lC, st.c_str());
  }
  if (w.lBar) {
    float f = isnan(num) || w.vmax <= w.vmin ? 0 : (num - w.vmin) / (w.vmax - w.vmin);
    lv_bar_set_value(w.lBar, (int)lroundf(std::max(0.0f, std::min(1.0f, f)) * 1000), LV_ANIM_OFF);
  }
}

static void refreshThermo(Widget& w) {
  auto it = known.find(w.entity);
  const bool ok = it != known.end() && it->second.state != "unavailable" && it->second.state != "unknown";
  const String unit = w.hasU ? w.uText : String("\xC2\xB0");
  String cur = "--", set = "--", status;
  uint32_t accent = 0xFFFFFF;
  float setV = NAN;
  if (ok) {
    const EntityInfo& e = it->second;
    auto a = e.attrs.find("current_temperature");
    float cv;
    if (a != e.attrs.end() && parseNum(a->second, cv)) cur = joinUnit(fmtNum(cv, -1), unit);
    setV = e.lv.temperature;
    auto act = e.attrs.find("hvac_action");
    String action = act == e.attrs.end() ? String("") : act->second;
    if (e.state == "off") status = "Off";
    else if (action.length()) status = prettyText(action);
    else status = prettyText(e.state);
    if (action == "heating") accent = 0xFF8A3D;
    else if (action == "cooling") accent = 0x4FA8FF;
  }
  if (!isnan(w.target) && (w.pending || (int32_t)(w.holdUntil - millis()) > 0)) setV = w.target;
  if (!isnan(setV)) set = joinUnit(fmtNum(setV, -1), unit);
  const bool top = w.topH >= 30;
  // top line: title and status
  String line = w.label;
  if (status.length() && ok) line += (line.length() ? " - " : "") + status;
  const int aIdx = w.topH >= 44 ? 3 : 4;
  const bool hasA = top && line.length();
  if (hasA) { lv_label_set_text(w.lA, line.c_str()); lv_obj_remove_flag(w.lA, LV_OBJ_FLAG_HIDDEN); }
  else lv_obj_add_flag(w.lA, LV_OBJ_FLAG_HIDDEN);
  if (top) {
    const int rt = 4 + (hasA ? VAL_LH[aIdx] + 2 : 0), rb = w.topH;
    int bi = pickFont(cur, 0, w.pw - 12, rb - rt);
    if (bi < 0) bi = 4;
    tileFont(w.lB, bi);
    lv_obj_set_style_text_color(w.lB, lv_color_hex(accent), 0);
    lv_label_set_text(w.lB, cur.c_str());
    lv_obj_align(w.lB, LV_ALIGN_TOP_MID, 0, (rt + rb) / 2 - VAL_LH[bi] / 2);
    lv_obj_remove_flag(w.lB, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(w.lB, LV_OBJ_FLAG_HIDDEN);
  }
  // middle of the control row: "Set 22" (tall tile) or "21.5 > 22" (small tile), else just the set point
  const int cw = w.pw - 2 * w.cbw - 16;
  String o1 = top ? String("Set ") + set : cur + " > " + set;
  int ci = pickFont(o1, 1, cw, w.ctrlH - 2);
  String ct = o1;
  if (ci < 0 || ci > 3) {
    ct = set;
    ci = pickFont(set, 1, cw, w.ctrlH - 2);
    if (ci < 0) ci = 4;
  }
  tileFont(w.lC, ci);
  lv_label_set_text(w.lC, ct.c_str());
}

static void thermoTap(lv_event_t* e) {
  if (recentlySwiped()) return;
  Widget* w = (Widget*)lv_event_get_user_data(e);
  const int dir = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
  auto it = known.find(w->entity);
  if (it == known.end()) return;
  float base = isnan(w->target) || !(w->pending || (int32_t)(w->holdUntil - millis()) > 0) ? it->second.lv.temperature : w->target;
  if (isnan(base)) return;
  float step = w->step;
  float lo = w->tmin, hi = w->tmax;
  float v;
  auto a = it->second.attrs.find("target_temp_step");
  if (step <= 0 && a != it->second.attrs.end() && parseNum(a->second, v)) step = v;
  const bool fahr = w->hasU && (w->uText.indexOf("F") >= 0 || w->uText.indexOf("f") >= 0);
  if (step <= 0) step = fahr ? 1.0f : 0.5f;
  if (isnan(lo) && (a = it->second.attrs.find("min_temp")) != it->second.attrs.end() && parseNum(a->second, v)) lo = v;
  if (isnan(hi) && (a = it->second.attrs.find("max_temp")) != it->second.attrs.end() && parseNum(a->second, v)) hi = v;
  if (isnan(lo)) lo = fahr ? 50.0f : 7.0f;  // typical limits when the thermostat reports none
  if (isnan(hi)) hi = fahr ? 90.0f : 35.0f;
  float t = roundf((base + dir * step) / step) * step;
  if (!isnan(lo) && t < lo) t = lo;
  if (!isnan(hi) && t > hi) t = hi;
  w->target = t;
  w->pending = true;
  w->lastTap = millis();
  refreshThermo(*w);
}

static void refreshWidget(Widget& w, const EntityInfo& e) {
  if (w.thermo) { refreshThermo(w); return; }
  if (w.value) { refreshValue(w); return; }
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
  if (w.text) lv_label_set_text(w.text, t.c_str());  // a slider built into a button has no caption
}

// ---- Fitting a button's contents --------------------------------------------------------------------------------------
// Everything inside a button scales down to fit, whatever its size. Maximums are the original sizes: label 18 px, icon 24 px,
// slider track 10 px. The web preview (web_pages.h, btnLayout) uses the very same rules; keep the two in step.
//  1. The label takes the largest of 18/16/14/12 px that fits the width on one line; at 12 px it wraps to 2 lines (cut with "...").
//  2. If label + icon are too tall, the icon shrinks first (24/18/14/12), then the label font, then the label drops to 1 line.
//  3. Only as a last resort the icon is left out.
//  4. A slider built into the button takes the bottom third of the button (18 to 34 px) and its track/knob shrink with it.
static const lv_font_t* const LBL_FONT[4] = {&lv_font_montserrat_18, &lv_font_montserrat_16, &lv_font_montserrat_14, &lv_font_montserrat_12};
static const lv_font_t* const ICON_FONT[4] = {&lv_font_montserrat_24, &lv_font_montserrat_18, &lv_font_montserrat_14, &lv_font_montserrat_12};
static const lv_image_dsc_t* const FAN_ICON[4] = {&fanIcon24, &fanIcon18, &fanIcon14, &fanIcon12};
static const int ICON_PX[4] = {24, 18, 14, 12};

static int sliderArea(int bh) { return std::max(18, std::min(34, bh / 3)); }
static int trackPx(int area) { return area >= 30 ? 10 : (area >= 24 ? 8 : 6); }

struct BtnFit {
  int lbl = 3;   // index into LBL_FONT
  int lines = 1;
  int icon = -1; // index into ICON_*, -1 = no icon
};

// area: height available for icon + label; hasIcon/label as set on the button.
static BtnFit fitButton(const String& label, int bw, int area, bool hasIcon) {
  BtnFit r;
  const int avail = bw - 12;
  int first = 3;
  bool fitsWidth = false;
  for (int i = 0; i < 4; i++)
    if (lv_text_get_width(label.c_str(), label.length(), LBL_FONT[i], 0) <= avail) { first = i; fitsWidth = true; break; }
  const int room = area - 4;
  if (!label.length()) {  // icon only: the largest that fits
    for (int k = 0; hasIcon && k < 4; k++)
      if (ICON_PX[k] <= room && ICON_PX[k] <= bw - 8) { r.icon = k; break; }
    return r;
  }
  for (int i = first; i < 4; i++) {
    const bool one = fitsWidth || i < 3 ? lv_text_get_width(label.c_str(), label.length(), LBL_FONT[i], 0) <= avail : false;
    const int lines = one ? 1 : 2;
    const int lh = lv_font_get_line_height(LBL_FONT[i]) * lines;
    for (int k = 0; hasIcon && k < 4; k++) {
      if (ICON_PX[k] > bw - 8) continue;
      if (lh + lv_font_get_line_height(ICON_FONT[k]) + 2 <= room) { r.lbl = i; r.lines = lines; r.icon = k; return r; }
    }
    if (!hasIcon && lh <= room) { r.lbl = i; r.lines = lines; return r; }
  }
  r.lbl = 3;  // last resort: smallest text on one line, no icon
  r.lines = 1;
  r.icon = -1;
  return r;
}

// ws: the button has a slider built in at the bottom (JSON "with_slider"): one element, the button is taller to hold it.
static void buildButton(Widget& w, lv_obj_t* page, lv_color_t c, JsonObject b, int x, int y, int bw, int bh, bool ws) {
  lv_obj_t* btn = lv_button_create(page);
  w.obj = btn;
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, bw, bh);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_set_style_bg_color(btn, lv_color_darken(c, 180), 0);  // off
  lv_obj_set_style_bg_color(btn, c, LV_STATE_CHECKED);         // on
  const int sa = ws ? sliderArea(bh) : 0;
  lv_obj_set_style_pad_all(btn, 0, 0);
  lv_obj_t* host = lv_obj_create(btn);  // icon and label sit in the top part, the slider (if any) in the bottom sa px
  lv_obj_remove_style_all(host);
  lv_obj_set_size(host, bw, bh - sa);
  lv_obj_remove_flag(host, LV_OBJ_FLAG_CLICKABLE);  // touches fall through to the button
  lv_obj_remove_flag(host, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_hor(host, 6, 0);
  lv_obj_set_flex_flow(host, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(host, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  const String iconName = String(b["icon"] | "");
  const bool fan = iconName == "fan";
  const char* sym = fan ? nullptr : iconSymbol(iconName);
  const BtnFit f = fitButton(w.label, bw, bh - sa, fan || sym);
  if (f.icon >= 0) {
    if (fan) {
      lv_obj_t* im = lv_image_create(host);
      lv_image_set_src(im, FAN_ICON[f.icon]);
      lv_obj_set_style_image_recolor(im, lv_color_white(), 0);
      lv_obj_set_style_image_recolor_opa(im, LV_OPA_COVER, 0);
    } else {
      lv_obj_t* ic = lv_label_create(host);
      lv_obj_set_style_text_font(ic, ICON_FONT[f.icon], 0);
      lv_label_set_text(ic, sym);
    }
  }
  lv_obj_t* lb = lv_label_create(host);
  lv_obj_set_style_text_font(lb, LBL_FONT[f.lbl], 0);
  if (f.lines > 1 || lv_text_get_width(w.label.c_str(), w.label.length(), LBL_FONT[f.lbl], 0) > bw - 12) {
    lv_obj_set_size(lb, bw - 12, f.lines * lv_font_get_line_height(LBL_FONT[f.lbl]));  // wrap, then cut with "..."
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
  }
  lv_label_set_text(lb, w.label.c_str());
  w.text = lb;
  if (w.camera) {
    lv_obj_add_event_cb(btn, [](lv_event_t* e) {
      if (recentlySwiped()) return;
      auto* cx = (Widget*)lv_event_get_user_data(e);
      cameraOpen(cx->label, cx->entity, cx->url, cx->fullBright);
    }, LV_EVENT_CLICKED, &w);
  } else if (w.entity.length()) {
    lv_obj_add_event_cb(btn, [](lv_event_t* e) {
      if (recentlySwiped()) return;  // this click only ended a swipe
      auto* cx = (Widget*)lv_event_get_user_data(e);
      if (pressHandler) pressHandler(cx->entity, cx->action);
    }, LV_EVENT_CLICKED, &w);
  }
}

// Touch handling shared by stand-alone sliders and sliders built into a button.
static void wireSlider(Widget& w, lv_obj_t* sl) {
  if (!w.entity.length()) return;
  auto send = [](lv_event_t* e) {
    auto* cx = (Widget*)lv_event_get_user_data(e);
    float v = lv_slider_get_value(cx->obj) / cx->mul;
    if (slideHandler) slideHandler(cx->entity, cx->attr, v);
  };
  lv_obj_add_event_cb(sl, [](lv_event_t*) { sliderHeld = true; }, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(sl, [](lv_event_t* e) { sliderHeld = false; }, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(sl, send, LV_EVENT_RELEASED, &w);
  // While dragging: update the caption (if any) and send at most every 300 ms so lights follow the finger.
  lv_obj_add_event_cb(sl, [](lv_event_t* e) {
    auto* cx = (Widget*)lv_event_get_user_data(e);
    float v = lv_slider_get_value(cx->obj) / cx->mul;
    if (cx->text) {
      String t = cx->label + "  " + String(v, cx->mul > 1 ? 1 : 0) + cx->unit;
      lv_label_set_text(cx->text, t.c_str());
    }
    if (lv_slider_is_dragged(cx->obj) && millis() - cx->lastSend > 300) {
      cx->lastSend = millis();
      if (slideHandler) slideHandler(cx->entity, cx->attr, v);
    }
  }, LV_EVENT_VALUE_CHANGED, &w);
}

// The slider inside a button: white knob and fill on a darkened track so it reads on any button color. Sized from the button.
static void buildInlineSlider(Widget& sw, lv_obj_t* btn, JsonObject b, int bw, int bh) {
  sw.attr = String(b["slider_attr"] | "");
  int mn, mx;
  sliderSpec(sw, b, mn, mx);
  const int area = sliderArea(bh), tr = trackPx(area);
  const int margin = std::max(tr + 1, std::min(14, bw / 6));
  lv_obj_t* sl = lv_slider_create(btn);
  sw.obj = sl;
  lv_obj_set_size(sl, bw - 2 * margin, tr);
  lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -std::max(2, (area - tr) / 2 - 2));
  lv_slider_set_range(sl, mn * sw.mul, mx * sw.mul);
  lv_obj_set_style_bg_color(sl, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sl, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, tr / 2, LV_PART_KNOB);
  wireSlider(sw, sl);
}

// Value tile: title (label), value, subtitle and an optional level bar. Height decides the sizes: the value keeps the largest
// font that fits together with the title and subtitle (14 px, else 12 px); if nothing fits the subtitle, then the title, is dropped.
static void fitValue(int area, bool& hasTitle, bool& hasSub, int& vi, int& si) {
  const bool combos[3][2] = {{hasTitle, hasSub}, {hasTitle, false}, {false, false}};
  for (auto& cb : combos) {
    for (int v = 0; v < 5; v++)
      for (int sz = 3; sz <= 4; sz++) {
        if (VAL_PX[v] < VAL_PX[sz]) continue;
        if (VAL_LH[v] + (cb[0] ? VAL_LH[sz] : 0) + (cb[1] ? VAL_LH[sz] : 0) <= area) {
          hasTitle = cb[0]; hasSub = cb[1]; vi = v; si = sz;
          return;
        }
      }
  }
  hasTitle = hasSub = false;
  vi = si = 4;
}

static lv_obj_t* tileLabel(lv_obj_t* parent, int w, int fontIdx) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_width(l, w);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  tileFont(l, fontIdx);
  return l;
}

static void buildValue(Widget& w, lv_obj_t* page, lv_color_t c, JsonObject b, int x, int y, int bw, int bh) {
  lv_obj_t* card = lv_obj_create(page);
  w.obj = card;
  w.pw = bw;
  w.ph = bh;
  lv_obj_remove_style_all(card);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, bw, bh);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(card, lv_color_darken(c, 180), 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  const bool bar = b["bar"] | false;
  const int barH = bh >= 60 ? 8 : 6, reserve = bar ? barH + 14 : 0;
  bool hasTitle = w.label.length(), hasSub = w.subText.length() || w.subEntity.length();
  fitValue(bh - reserve - 6, hasTitle, hasSub, w.vIdx, w.sIdx);
  lv_obj_set_style_pad_hor(card, 6, 0);
  lv_obj_set_style_pad_bottom(card, reserve, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (hasTitle) {
    w.lA = tileLabel(card, bw - 12, w.sIdx);
    lv_obj_set_style_text_opa(w.lA, LV_OPA_80, 0);
    lv_label_set_text(w.lA, w.label.c_str());
  }
  w.lVal = tileLabel(card, bw - 12, w.vIdx);
  if (hasSub) {
    w.lC = tileLabel(card, bw - 12, w.sIdx);
    lv_obj_set_style_text_opa(w.lC, LV_OPA_80, 0);
  }
  if (bar) {
    w.lBar = lv_bar_create(card);
    lv_obj_add_flag(w.lBar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(w.lBar, bw - 16, barH);
    lv_obj_align(w.lBar, LV_ALIGN_BOTTOM_MID, 0, reserve - 6);  // aligned to the padded area: push it down to 6 px above the edge
    lv_bar_set_range(w.lBar, 0, 1000);
    lv_obj_set_style_radius(w.lBar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(w.lBar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(w.lBar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(w.lBar, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(w.lBar, c, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(w.lBar, LV_OPA_COVER, LV_PART_INDICATOR);
  }
  lv_label_set_text(w.lVal, "--");
  if (w.action.length() && (w.actionEntity.length() || w.entity.length())) {  // optional tap action, same options as a button
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(card, lv_color_lighten(lv_color_darken(c, 180), 40), LV_STATE_PRESSED);
    lv_obj_add_event_cb(card, [](lv_event_t* e) {
      if (recentlySwiped()) return;
      auto* cx = (Widget*)lv_event_get_user_data(e);
      const String& target = cx->actionEntity.length() ? cx->actionEntity : cx->entity;
      if (cx->action == "camera") cameraOpen(cx->label, target, "", false);
      else if (pressHandler) pressHandler(target, cx->action);
    }, LV_EVENT_CLICKED, &w);
  }
}

// Thermostat tile: dark card with title and status on top, the current temperature, and a row [ - ] Set 22 [ + ].
static void buildThermostat(Widget& w, lv_obj_t* page, lv_color_t c, JsonObject b, int x, int y, int bw, int bh) {
  lv_obj_t* card = lv_obj_create(page);
  w.obj = card;
  w.pw = bw;
  w.ph = bh;
  lv_obj_remove_style_all(card);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, bw, bh);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x23272B), 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  w.ctrlH = std::max(24, std::min(40, bh * 36 / 100));
  w.cbw = std::max(28, std::min(48, bw * 22 / 100));
  w.topH = bh - w.ctrlH - 8;
  w.lA = tileLabel(card, bw - 12, w.topH >= 44 ? 3 : 4);
  lv_obj_align(w.lA, LV_ALIGN_TOP_MID, 0, 4);
  lv_obj_set_style_text_opa(w.lA, LV_OPA_80, 0);
  w.lB = tileLabel(card, bw - 12, 0);
  for (int dir = -1; dir <= 1; dir += 2) {
    lv_obj_t* bt = lv_button_create(card);
    lv_obj_set_size(bt, w.cbw, w.ctrlH);
    lv_obj_align(bt, dir < 0 ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, dir < 0 ? 4 : -4, -4);
    lv_obj_set_style_radius(bt, 8, 0);
    lv_obj_set_style_pad_all(bt, 0, 0);
    lv_obj_set_style_bg_color(bt, lv_color_hex(0x3A3F44), 0);
    lv_obj_set_style_bg_color(bt, c, LV_STATE_PRESSED);
    lv_obj_set_user_data(bt, (void*)(intptr_t)dir);
    lv_obj_add_event_cb(bt, thermoTap, LV_EVENT_CLICKED, &w);
    lv_obj_t* l = lv_label_create(bt);
    lv_obj_set_style_text_font(l, w.ctrlH >= 30 ? &lv_font_montserrat_18 : &lv_font_montserrat_14, 0);
    lv_label_set_text(l, dir < 0 ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS);
    lv_obj_center(l);
  }
  w.lC = tileLabel(card, bw - 2 * w.cbw - 16, 2);
  lv_obj_align(w.lC, LV_ALIGN_BOTTOM_MID, 0, -4 - (w.ctrlH - 18) / 2);
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
    lv_obj_set_size(sl, bw - 28, 10);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -6);
  } else {
    lv_obj_set_size(sl, 10, bh - 50);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -6);
  }
  lv_slider_set_range(sl, mn * w.mul, mx * w.mul);
  lv_obj_set_style_bg_color(sl, c, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, c, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 5, LV_PART_KNOB);
  wireSlider(w, sl);
}

// Older layouts stored a button's auto-added slider as a separate element ("linked" = the button's id). Fold each one into
// its button ("with_slider") so old layouts keep working: if the slider sat directly below the button, the button grows to
// cover both, otherwise it just grows by 40 px.
static void mergeLinkedSliders(JsonArray bs) {
  for (JsonObject s : bs) {
    String link = s["linked"] | "";
    if (link.isEmpty() || String(s["type"] | "") != "slider") continue;
    for (JsonObject t : bs) {
      if (String(t["id"] | "") != link || String(t["type"] | "button") == "slider") continue;
      int ty = t["y"] | 0, th = t["h"] | 70, sy = s["y"] | 0, sh = s["h"] | 60;
      t["with_slider"] = true;
      String a = s["attr"] | "";
      if (a.length()) t["slider_attr"] = a;
      t["h"] = sy >= ty + th - 5 ? (sy + sh - ty) : th + 40;
      s["merged"] = true;
      break;
    }
  }
  for (size_t i = bs.size(); i-- > 0;)
    if (bs[i]["merged"] | false) bs.remove(i);
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
  for (JsonObject p : pg) mergeLinkedSliders(p["buttons"].as<JsonArray>());
  const bool multi = pg.size() > 1;
  for (JsonObject p : pg) {
    lv_obj_t* page = lv_obj_create(canvasHolder);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, scrW, uiCanvasH());
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
      w->actionEntity = String(b["action_entity"] | "");
      w->label = String(b["label"] | "");
      String type = b["type"] | "button";
      w->slider = type == "slider";
      w->camera = type == "camera";
      w->thermo = type == "thermostat";
      w->value = type == "value";
      if (w->thermo || w->value) w->action = String(b["action"] | "");  // tiles do nothing on a tap unless an action is chosen
      w->attr = String(b["attr"] | "");
      w->subEntity = String(b["sub_entity"] | "");
      w->subAttr = String(b["sub_attr"] | "");
      w->subText = String(b["subtitle"] | "");
      w->hasU = b["unit"].is<const char*>();
      w->uText = String(b["unit"] | "");
      w->decimals = b["decimals"] | -1;
      w->vmin = b["min"] | 0.0f;
      w->vmax = b["max"] | 100.0f;
      w->step = b["step"] | 0.0f;
      if (w->thermo) {
        if (b["min"].is<float>()) w->tmin = b["min"].as<float>();
        if (b["max"].is<float>()) w->tmax = b["max"].as<float>();
      }
      w->url = String(b["url"] | "");
      w->fullBright = b["full_brightness"] | false;
      const char* hex = b["color"] | "#4A90E2";
      lv_color_t c = lv_color_hex(strtoul(hex[0] == '#' ? hex + 1 : hex, nullptr, 16));
      int bx = b["x"] | 0, by = b["y"] | 0, bw = b["w"] | 100, bh = b["h"] | 70;
      const bool ws = !w->slider && !w->camera && (b["with_slider"] | false) && w->entity.length() && bh >= 40 && bw >= 40;

      if (w->slider) buildSlider(*w, page, c, b, bx, by, bw, bh);
      else if (w->thermo) buildThermostat(*w, page, c, b, bx, by, bw, bh);
      else if (w->value) buildValue(*w, page, c, b, bx, by, bw, bh);
      else buildButton(*w, page, c, b, bx, by, bw, bh, ws);

      if (w->entity.length() && !w->camera) {  // a camera tile opens a stream, it has no state to follow
        if (std::find(entityList.begin(), entityList.end(), w->entity) == entityList.end())
          entityList.push_back(w->entity);
        byEntity[w->entity].push_back(w.get());
        auto it = known.find(w->entity);
        if (it != known.end()) refreshWidget(*w, it->second);
      }
      if (w->value && w->subEntity.length()) {  // the subtitle follows a second entity
        if (std::find(entityList.begin(), entityList.end(), w->subEntity) == entityList.end())
          entityList.push_back(w->subEntity);
        byEntity[w->subEntity].push_back(w.get());
      }
      if (w->thermo || w->value) {
        static const EntityInfo none;
        refreshWidget(*w, none);  // fills in the current values (or "--" until Home Assistant has reported)
      }
      if (ws) {  // the slider built into the button follows the same entity
        auto sw = std::unique_ptr<Widget>(new Widget());
        sw->slider = true;
        sw->entity = w->entity;
        buildInlineSlider(*sw, w->obj, b, bw, bh);
        byEntity[sw->entity].push_back(sw.get());
        auto it = known.find(sw->entity);
        if (it != known.end()) refreshWidget(*sw, it->second);
        widgets.push_back(std::move(sw));
      }
      widgets.push_back(std::move(w));
    }
  }
  if (!pages.empty()) showPage(0);
  return true;
}

void uiSetState(const String& entityId, const String& state, const HaLevels& lv, JsonObjectConst attrs) {
  static const char* ON_STATES[] = {"on", "open", "opening", "playing", "unlocked", "home", "heat", "cool",
                                    "heat_cool", "auto", "dry", "fan_only"};
  EntityInfo& e = known[entityId];
  e.on = false;
  for (auto* o : ON_STATES) if (state == o) e.on = true;
  e.unavailable = (state == "unavailable");
  e.state = state;
  // Keep short scalar attributes (value and thermostat tiles read them); an update may carry only the changed ones.
  for (JsonPairConst kv : attrs) {
    if (e.attrs.size() >= 24 && e.attrs.find(kv.key().c_str()) == e.attrs.end()) continue;
    JsonVariantConst v = kv.value();
    if (v.is<JsonObjectConst>() || v.is<JsonArrayConst>()) continue;
    String t = v.is<const char*>() ? String(v.as<const char*>()) : (v.isNull() ? String("") : String(v.as<String>()));
    if (t.length() <= 48) e.attrs[kv.key().c_str()] = t;
  }
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
    lv_obj_set_size(banner, scrW - 40, LV_SIZE_CONTENT);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(banner, lv_color_hex(0x0B0E11), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(banner, lv_color_hex(0xFFC107), 0);
    lv_obj_set_style_border_width(banner, 3, 0);
    lv_obj_set_style_radius(banner, 12, 0);
    lv_obj_set_style_pad_all(banner, 14, 0);
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    bannerLabel = lv_label_create(banner);
    lv_obj_set_width(bannerLabel, scrW - 70);
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
  cameraTick();
  for (auto& w : widgets)  // thermostat taps are collected, then sent as one set_temperature call
    if (w->thermo && w->pending && millis() - w->lastTap > 700) {
      w->pending = false;
      w->holdUntil = millis() + 2500;
      if (slideHandler) slideHandler(w->entity, "", w->target);
    }
  lv_timer_handler();
}

// --- firmware update progress card -------------------------------------------------------------------------
static lv_obj_t* progCard;
static lv_obj_t* progTitle;
static lv_obj_t* progPhase;
static lv_obj_t* progBar;
static lv_obj_t* progPct;
static lv_obj_t* progClose;

static void buildProgress() {
  if (progCard) return;
  progCard = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(progCard);
  lv_obj_set_size(progCard, scrW, scrH);
  lv_obj_set_style_bg_color(progCard, lv_color_hex(0x0B0E11), 0);
  lv_obj_set_style_bg_opa(progCard, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(progCard, lv_color_hex(0xFFFFFF), 0);
  lv_obj_add_flag(progCard, LV_OBJ_FLAG_CLICKABLE);  // swallows touches
  lv_obj_remove_flag(progCard, LV_OBJ_FLAG_SCROLLABLE);
  progTitle = lv_label_create(progCard);
  lv_obj_set_style_text_font(progTitle, &lv_font_montserrat_24, 0);
  lv_obj_align(progTitle, LV_ALIGN_TOP_MID, 0, scrH / 2 - 100);
  progPhase = lv_label_create(progCard);
  lv_obj_set_style_text_font(progPhase, &lv_font_montserrat_18, 0);
  lv_obj_set_width(progPhase, scrW - 40);
  lv_obj_set_style_text_align(progPhase, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(progPhase, LV_ALIGN_TOP_MID, 0, scrH / 2 - 56);
  progBar = lv_bar_create(progCard);
  lv_obj_set_size(progBar, scrW - 80, 18);
  lv_obj_align(progBar, LV_ALIGN_TOP_MID, 0, scrH / 2 - 8);
  lv_bar_set_range(progBar, 0, 100);
  progPct = lv_label_create(progCard);
  lv_obj_set_style_text_font(progPct, &lv_font_montserrat_18, 0);
  lv_obj_align(progPct, LV_ALIGN_TOP_MID, 0, scrH / 2 + 22);
  lv_obj_t* warn = lv_label_create(progCard);
  lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(warn, lv_color_hex(0xFFD54F), 0);
  lv_label_set_text(warn, "Do not power off or unplug");
  lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, scrH / 2 + 60);
  progClose = lv_button_create(progCard);
  lv_obj_set_size(progClose, 120, 44);
  lv_obj_align(progClose, LV_ALIGN_TOP_MID, 0, scrH / 2 + 90);
  lv_obj_t* cl = lv_label_create(progClose);
  lv_label_set_text(cl, "Close");
  lv_obj_center(cl);
  lv_obj_add_event_cb(progClose, [](lv_event_t*) { lv_obj_add_flag(progCard, LV_OBJ_FLAG_HIDDEN); progShown = false; },
                      LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(progCard, LV_OBJ_FLAG_HIDDEN);
}

void uiProgress(const char* title, const char* phase, int pct) {
  static String lastPhase;
  static int lastPct = -2;
  static uint32_t lastPaint = 0;
  buildProgress();
  bool changed = lastPhase != phase || lastPct != pct;
  if (!changed && progShown) return;
  lastPhase = phase;
  lastPct = pct;
  if (!progShown) {
    progShown = true;
    cameraClose();
    lv_obj_remove_flag(progCard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(progCard);
  }
  lv_obj_add_flag(progClose, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_color(progPhase, lv_color_hex(0xFFFFFF), 0);
  lv_label_set_text(progTitle, title);
  lv_obj_align(progTitle, LV_ALIGN_TOP_MID, 0, scrH / 2 - 100);
  lv_label_set_text(progPhase, phase);
  if (pct >= 0) {
    lv_obj_remove_flag(progBar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(progPct, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(progBar, pct, LV_ANIM_OFF);
    String t = String(pct) + " %";
    lv_label_set_text(progPct, t.c_str());
    lv_obj_align(progPct, LV_ALIGN_TOP_MID, 0, scrH / 2 + 22);
  } else {
    lv_obj_add_flag(progBar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(progPct, LV_OBJ_FLAG_HIDDEN);
  }
  if (millis() - lastPaint > 80 || pct >= 100 || pct < 0) {  // paint right away, this runs inside blocking loops
    lastPaint = millis();
    lv_refr_now(NULL);
  }
}

void uiProgressFail(const char* title, const String& why) {
  buildProgress();
  progShown = true;
  lv_obj_remove_flag(progCard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(progCard);
  lv_label_set_text(progTitle, title);
  lv_obj_align(progTitle, LV_ALIGN_TOP_MID, 0, scrH / 2 - 100);
  lv_label_set_text(progPhase, why.c_str());
  lv_obj_set_style_text_color(progPhase, lv_color_hex(0xFF5252), 0);
  lv_obj_add_flag(progBar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(progPct, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(progClose, LV_OBJ_FLAG_HIDDEN);
  lv_refr_now(NULL);
}
