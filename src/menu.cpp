#include "menu.h"
#include <WiFi.h>
#include <lvgl.h>
#include "config.h"
#include "ota.h"
#include "version.h"

static lv_obj_t* panel;
static lv_obj_t* info;
static lv_obj_t* result;
static lv_obj_t* installBtn;

static void refreshInfo() {
  String ip = WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String t = String("Firmware: v") + FW_VERSION + "\nIP: " + ip + "\nWeb UI: http://" + cfg.hostname +
             ".local\nLogin: admin / " + cfg.adminPass;
  lv_label_set_text(info, t.c_str());
}

static void say(const String& s) {
  lv_label_set_text(result, s.c_str());
  lv_refr_now(NULL);  // paint before a blocking network call
}

static lv_obj_t* mkBtn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, LV_SIZE_CONTENT, 40);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

void menuInit() {
  lv_obj_t* scr = lv_screen_active();

  // Gear button, bottom right, above the page bar.
  lv_obj_t* gear = lv_button_create(scr);
  lv_obj_set_size(gear, 40, 32);
  lv_obj_set_pos(gear, 436, 284);
  lv_obj_set_style_bg_color(gear, lv_color_hex(0x2B2F33), 0);
  lv_obj_t* gl = lv_label_create(gear);
  lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
  lv_obj_center(gl);

  panel = lv_obj_create(scr);
  lv_obj_set_size(panel, 480, 320);
  lv_obj_set_pos(panel, 0, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x181B1F), 0);
  lv_obj_set_style_radius(panel, 0, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* title = lv_label_create(panel);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_label_set_text(title, "Settings");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  info = lv_label_create(panel);
  lv_obj_set_style_text_font(info, &lv_font_montserrat_18, 0);
  lv_obj_align(info, LV_ALIGN_TOP_LEFT, 0, 40);

  result = lv_label_create(panel);
  lv_obj_set_width(result, 440);
  lv_label_set_text(result, "");
  lv_obj_align(result, LV_ALIGN_TOP_LEFT, 0, 160);

  lv_obj_t* row = lv_obj_create(panel);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 440, 48);
  lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 8, 0);

  mkBtn(row, "Check update", [](lv_event_t*) {
    say("Checking GitHub...");
    String tag = otaLatestTag();
    if (tag.isEmpty()) say("Could not reach GitHub (needs Wi-Fi and a public repo).");
    else if (otaIsNewer(tag)) {
      say("New version " + tag + " available.");
      lv_obj_remove_flag(installBtn, LV_OBJ_FLAG_HIDDEN);
    } else say("Up to date (" + tag + ").");
  });
  installBtn = mkBtn(row, "Install", [](lv_event_t*) {
    say("Downloading and flashing... do not power off.");
    String err = otaInstallLatest();  // reboots on success
    say("Update failed: " + err);
  });
  lv_obj_add_flag(installBtn, LV_OBJ_FLAG_HIDDEN);
  mkBtn(row, "Close", [](lv_event_t*) { lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN); });

  lv_obj_add_event_cb(gear, [](lv_event_t*) {
    refreshInfo();
    lv_label_set_text(result, "");
    lv_obj_add_flag(installBtn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(panel);
  }, LV_EVENT_CLICKED, nullptr);
}
