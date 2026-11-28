#include "menu.h"
#include <WiFi.h>
#include <lvgl.h>
#include "config.h"
#include "ota.h"
#include "ui.h"
#include "version.h"

// Untested on hardware. Screens inside one full-screen overlay: main, wifi, scan, password, about.
static lv_obj_t* panel;
static lv_obj_t* scrMain;
static lv_obj_t* scrWifi;
static lv_obj_t* scrScan;
static lv_obj_t* scrPass;
static lv_obj_t* scrAbout;
static lv_obj_t* wifiInfo;
static lv_obj_t* scanList;
static lv_obj_t* scanNote;
static lv_obj_t* passTitle;
static lv_obj_t* passArea;
static lv_obj_t* passMsg;
static lv_obj_t* kb;
static lv_obj_t* aboutInfo;
static lv_obj_t* aboutMsg;
static lv_obj_t* installBtn;

static String chosenSsid;
struct Net { String ssid; bool open; int rssi; };
static Net nets[20];
static int netCount = 0;

static const lv_color_t WHITE = lv_color_hex(0xFFFFFF);

static void showScreen(lv_obj_t* s) {
  lv_obj_t* all[] = {scrMain, scrWifi, scrScan, scrPass, scrAbout};
  for (auto* o : all) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s, LV_OBJ_FLAG_HIDDEN);
}

static void say(lv_obj_t* label, const String& s) {
  lv_label_set_text(label, s.c_str());
  lv_refr_now(NULL);  // paint before a blocking call
}

static lv_obj_t* mkScreen() {
  lv_obj_t* s = lv_obj_create(panel);
  lv_obj_remove_style_all(s);
  lv_obj_set_size(s, 480, 320);
  lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
  return s;
}

static lv_obj_t* mkTitle(lv_obj_t* parent, const char* text) {
  lv_obj_t* t = lv_label_create(parent);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(t, WHITE, 0);
  lv_label_set_text(t, text);
  lv_obj_set_pos(t, 0, 0);
  return t;
}

static lv_obj_t* mkLabel(lv_obj_t* parent, int x, int y, int w, const lv_font_t* font, lv_color_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_width(l, w);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, "");
  return l;
}

static lv_obj_t* mkBtn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, int x, int y, int w, int h = 44,
                       uint32_t color = 0x1976D2) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
  lv_obj_t* l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(l, WHITE, 0);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

static String ipText() {
  return WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

// --- Wi-Fi screen -----------------------------------------------------------------------------
static void refreshWifi() {
  String t;
  bool sta = WiFi.status() == WL_CONNECTED;
  if (sta) {
    t = String("Network: ") + WiFi.SSID() + "\nStatus: connected, " + WiFi.RSSI() + " dBm\nIP: " + WiFi.localIP().toString();
  } else if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    t = String("Not connected.\nSetup network: Bedside-Setup\nIP: ") + WiFi.softAPIP().toString();
  } else {
    t = "Not connected.";
  }
  t += String("\nSaved network: ") + (cfg.wifiSsid.isEmpty() ? "(none)" : cfg.wifiSsid);
  lv_label_set_text(wifiInfo, t.c_str());
}

static void doScan() {
  say(scanNote, "Scanning...");
  lv_obj_clean(scanList);
  if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);  // scanning needs the station interface
  else if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  netCount = 0;
  for (int i = 0; i < n && netCount < 20; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    nets[netCount++] = {s, WiFi.encryptionType(i) == WIFI_AUTH_OPEN, WiFi.RSSI(i)};
  }
  WiFi.scanDelete();
  for (int i = 0; i < netCount; i++) {  // strongest first
    for (int j = i + 1; j < netCount; j++)
      if (nets[j].rssi > nets[i].rssi) std::swap(nets[i], nets[j]);
  }
  for (int i = 0; i < netCount; i++) {
    String label = nets[i].ssid + (nets[i].open ? "" : "  " LV_SYMBOL_EYE_CLOSE) + "   " + nets[i].rssi + " dBm";
    lv_obj_t* b = lv_list_add_button(scanList, LV_SYMBOL_WIFI, label.c_str());
    lv_obj_set_style_text_color(b, WHITE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1F2933), 0);
    lv_obj_add_event_cb(b, [](lv_event_t* e) {
      int idx = (int)(intptr_t)lv_event_get_user_data(e);
      chosenSsid = nets[idx].ssid;
      if (nets[idx].open) {
        // Open network: connect straight away.
        extern void menuConnect(const String&, const String&, lv_obj_t*);
        menuConnect(chosenSsid, "", scanNote);
      } else {
        String t = String("Password for ") + chosenSsid;
        lv_label_set_text(passTitle, t.c_str());
        lv_textarea_set_text(passArea, "");
        lv_label_set_text(passMsg, "");
        showScreen(scrPass);
      }
    }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
  lv_label_set_text(scanNote, n <= 0 ? "No networks found." : "Tap a network to join it.");
}

// Joins a network; on success saves it and restarts. Blocks for up to 15 s.
void menuConnect(const String& ssid, const String& pass, lv_obj_t* msg) {
  say(msg, String("Connecting to ") + ssid + "...");
  bool wasAp = WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA;
  WiFi.mode(wasAp ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    cfg.wifiSsid = ssid;
    cfg.wifiPass = pass;
    configSave();
    say(msg, "Connected. Restarting...");
    delay(1200);
    ESP.restart();
  }
  WiFi.disconnect();
  if (!cfg.wifiSsid.isEmpty()) WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());  // back to the old network
  say(msg, "Could not connect. Check the password and try again.");
}

// --- About screen -----------------------------------------------------------------------------
static void refreshAbout() {
  String t = String("Firmware: v") + FW_VERSION + "\nIP: " + ipText() + "\nWeb UI: http://" + cfg.hostname +
             ".local\nLogin: admin / " + cfg.adminPass + "\nHome Assistant: " + uiStatusText() + "\nUptime: " +
             (millis() / 60000) + " min";
  lv_label_set_text(aboutInfo, t.c_str());
}

void menuOpen(bool wifiScreen) {
  lv_label_set_text(aboutMsg, "");
  lv_obj_add_flag(installBtn, LV_OBJ_FLAG_HIDDEN);
  if (wifiScreen) {
    refreshWifi();
    showScreen(scrWifi);
  } else {
    showScreen(scrMain);
  }
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(panel);
}

bool menuIsOpen() { return panel && !lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN); }

void menuInit() {
  lv_obj_t* scr = lv_screen_active();

  // Gear button, bottom right.
  lv_obj_t* gear = lv_button_create(scr);
  lv_obj_set_size(gear, 40, 32);
  lv_obj_set_pos(gear, 436, 284);
  lv_obj_set_style_bg_color(gear, lv_color_hex(0x2B2F33), 0);
  lv_obj_t* gl = lv_label_create(gear);
  lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
  lv_obj_center(gl);
  lv_obj_add_event_cb(gear, [](lv_event_t*) { menuOpen(false); }, LV_EVENT_CLICKED, nullptr);

  panel = lv_obj_create(scr);
  lv_obj_set_size(panel, 480, 320);
  lv_obj_set_pos(panel, 0, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B0E11), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(panel, WHITE, 0);  // high contrast: white on near-black
  lv_obj_set_style_radius(panel, 0, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

  scrMain = mkScreen();
  scrWifi = mkScreen();
  scrScan = mkScreen();
  scrPass = mkScreen();
  scrAbout = mkScreen();

  // Content is inset 14 px from the screen edge.
  for (auto* s : {scrMain, scrWifi, scrScan, scrAbout}) {
    lv_obj_set_style_pad_all(s, 14, 0);
  }

  // ---- main ----
  mkTitle(scrMain, "Settings");
  mkBtn(scrMain, LV_SYMBOL_WIFI "  Wi-Fi", [](lv_event_t*) { refreshWifi(); showScreen(scrWifi); }, 0, 60, 210, 90);
  mkBtn(scrMain, LV_SYMBOL_LIST "  About", [](lv_event_t*) {
    refreshAbout();
    lv_label_set_text(aboutMsg, "");
    lv_obj_add_flag(installBtn, LV_OBJ_FLAG_HIDDEN);
    showScreen(scrAbout);
  }, 226, 60, 210, 90);
  mkBtn(scrMain, "Close", [](lv_event_t*) { lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN); }, 0, 244, 140, 44, 0x455A64);

  // ---- wifi ----
  mkTitle(scrWifi, "Wi-Fi");
  wifiInfo = mkLabel(scrWifi, 0, 40, 440, &lv_font_montserrat_18, WHITE);
  lv_obj_set_style_text_line_space(wifiInfo, 6, 0);
  mkBtn(scrWifi, "Join network", [](lv_event_t*) { showScreen(scrScan); doScan(); }, 0, 244, 150);
  mkBtn(scrWifi, "Forget", [](lv_event_t*) {
    cfg.wifiSsid = "";
    cfg.wifiPass = "";
    configSave();
    ESP.restart();  // comes back up in setup mode
  }, 158, 244, 110, 44, 0xC62828);
  mkBtn(scrWifi, "Back", [](lv_event_t*) { showScreen(scrMain); }, 276, 244, 100, 44, 0x455A64);

  // ---- scan ----
  mkTitle(scrScan, "Choose network");
  scanNote = mkLabel(scrScan, 0, 34, 300, &lv_font_montserrat_14, lv_color_hex(0xFFD54F));
  scanList = lv_list_create(scrScan);
  lv_obj_set_size(scanList, 452, 170);
  lv_obj_set_pos(scanList, 0, 58);
  lv_obj_set_style_bg_color(scanList, lv_color_hex(0x11161B), 0);
  lv_obj_set_style_border_width(scanList, 0, 0);
  mkBtn(scrScan, "Rescan", [](lv_event_t*) { doScan(); }, 0, 244, 120);
  mkBtn(scrScan, "Back", [](lv_event_t*) { refreshWifi(); showScreen(scrWifi); }, 128, 244, 100, 44, 0x455A64);

  // ---- password ----
  passTitle = mkTitle(scrPass, "Password");
  lv_obj_set_pos(passTitle, 14, 4);
  passArea = lv_textarea_create(scrPass);
  lv_obj_set_size(passArea, 452, 44);
  lv_obj_set_pos(passArea, 14, 38);
  lv_textarea_set_one_line(passArea, true);
  lv_textarea_set_password_mode(passArea, true);
  lv_textarea_set_placeholder_text(passArea, "Wi-Fi password");
  lv_obj_set_style_text_font(passArea, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(passArea, lv_color_hex(0x101214), 0);  // dark text on the light input box
  passMsg = mkLabel(scrPass, 226, 92, 250, &lv_font_montserrat_14, lv_color_hex(0xFFD54F));
  mkBtn(scrPass, "Cancel", [](lv_event_t*) { showScreen(scrScan); }, 14, 90, 96, 36, 0x455A64);
  mkBtn(scrPass, "Show", [](lv_event_t*) {
    lv_textarea_set_password_mode(passArea, !lv_textarea_get_password_mode(passArea));
  }, 118, 90, 80, 36, 0x455A64);
  kb = lv_keyboard_create(scrPass);
  lv_obj_set_size(kb, 480, 188);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(kb, passArea);
  lv_obj_add_event_cb(kb, [](lv_event_t*) {  // the keyboard's own OK key
    menuConnect(chosenSsid, String(lv_textarea_get_text(passArea)), passMsg);
  }, LV_EVENT_READY, nullptr);

  // ---- about ----
  mkTitle(scrAbout, "About");
  aboutInfo = mkLabel(scrAbout, 0, 38, 440, &lv_font_montserrat_18, WHITE);
  lv_obj_set_style_text_line_space(aboutInfo, 4, 0);
  aboutMsg = mkLabel(scrAbout, 0, 214, 440, &lv_font_montserrat_14, lv_color_hex(0xFFD54F));
  mkBtn(scrAbout, "Check update", [](lv_event_t*) {
    say(aboutMsg, "Checking GitHub...");
    String tag = otaLatestTag();
    if (tag.isEmpty()) say(aboutMsg, "Could not reach GitHub (needs Wi-Fi and a public repo).");
    else if (otaIsNewer(tag)) {
      say(aboutMsg, "New version " + tag + " available.");
      lv_obj_remove_flag(installBtn, LV_OBJ_FLAG_HIDDEN);
    } else say(aboutMsg, "Up to date (" + tag + ").");
  }, 0, 244, 160);
  installBtn = mkBtn(scrAbout, "Install", [](lv_event_t*) {
    say(aboutMsg, "Downloading and flashing... do not power off.");
    String err = otaInstallLatest();  // reboots on success
    say(aboutMsg, "Update failed: " + err);
  }, 168, 244, 100, 44, 0x2E7D32);
  lv_obj_add_flag(installBtn, LV_OBJ_FLAG_HIDDEN);
  mkBtn(scrAbout, "Back", [](lv_event_t*) { showScreen(scrMain); }, 276, 244, 100, 44, 0x455A64);
}
