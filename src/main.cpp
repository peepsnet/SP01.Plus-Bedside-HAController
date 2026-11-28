#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include "config.h"
#include "ha_client.h"
#include "improv.h"
#include "menu.h"
#include "ui.h"
#include "version.h"
#include "web.h"

static bool apMode = false;

static bool applyLayout(const String& json) {
  if (!uiBuild(json)) return false;
  haSetEntities(uiEntities());
  return true;
}

// Tries the saved networks in priority order, visible ones first (a hidden network is tried last).
static bool joinBestSaved() {
  if (wifiNets.empty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.hostname.c_str());
  int n = WiFi.scanNetworks();  // blocking, a few seconds
  std::vector<int> visible, hidden;
  for (size_t i = 0; i < wifiNets.size(); i++) {
    bool seen = false;
    for (int k = 0; k < n; k++) if (WiFi.SSID(k) == wifiNets[i].ssid) seen = true;
    (seen ? visible : hidden).push_back(i);
  }
  WiFi.scanDelete();
  for (int pass = 0; pass < 2; pass++) {
    for (int i : (pass == 0 ? visible : hidden)) {
      WiFi.begin(wifiNets[i].ssid.c_str(), wifiNets[i].pass.c_str());
      uint32_t t = millis();
      uint32_t limit = pass == 0 ? 12000 : 6000;
      while (WiFi.status() != WL_CONNECTED && millis() - t < limit) {
        uiTick();
        delay(50);
      }
      if (WiFi.status() == WL_CONNECTED) return true;
      WiFi.disconnect();
    }
  }
  return false;
}

static void startWifi() {
  apMode = !joinBestSaved();
  if (apMode) {
    WiFi.mode(WIFI_AP_STA);  // AP for setup; the station side stays available for scanning
    WiFi.softAP("Bedside-Setup");  // open AP; browse to http://192.168.4.1/settings
    if (wifiNets.empty())
      uiSetBanner("Wi-Fi is not set up yet\n1. On your phone, join Wi-Fi \"Bedside-Setup\"\n2. Open http://192.168.4.1/settings\nTap the gear for the web login.");
    else
      uiSetBanner("Could not join any saved Wi-Fi\nJoin \"Bedside-Setup\" and open\nhttp://192.168.4.1/settings to fix it.");
  } else {
    MDNS.begin(cfg.hostname.c_str());
    MDNS.addService("http", "tcp", 80);
  }
}

// Reconnect logic: a lost connection is retried by the Wi-Fi stack; if it stays down for 2 minutes, or in setup mode
// a saved network becomes visible (and nobody is using the setup AP), restart to run the best-network join again.
static void wifiWatchdog() {
  static uint32_t downSince = 0, lastScan = 0;
  uint32_t now = millis();
  if (!apMode) {
    if (WiFi.status() == WL_CONNECTED) downSince = 0;
    else if (downSince == 0) downSince = now;
    else if (now - downSince > 120000) ESP.restart();
  } else if (!wifiNets.empty() && now - lastScan > 60000 && WiFi.softAPgetStationNum() == 0) {
    lastScan = now;
    int n = WiFi.scanNetworks();
    for (int k = 0; k < n; k++)
      if (wifiIndexOf(WiFi.SSID(k)) >= 0) ESP.restart();
    WiFi.scanDelete();
  }
}

void setup() {
  Serial.begin(115200);
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  configLoad();
  if (cfg.adminPass.isEmpty()) {  // first boot: generate a web UI password, shown in the touchscreen settings menu
    char buf[10];
    snprintf(buf, sizeof buf, "%08x", (unsigned)esp_random());
    cfg.adminPass = buf;
    configSave();
  }
  Serial.printf("Bedside controller v%s\n", FW_VERSION);

  uiInit([](const String& entity, const String& action) { haCallAction(entity, action); },
         [](const String& entity, const String& attr, float v) { haCallSlider(entity, attr, v); });
  menuInit();
  if (!uiBuild(layoutRead())) uiBuild(DEFAULT_LAYOUT);

  startWifi();
  improvBegin();
  webBegin(applyLayout);
  if (!apMode) {
    haBegin([](const String& e, const String& s, const HaLevels& lv) { uiSetState(e, s, lv); },
            [](const String& st) { Serial.println(st); uiSetStatus(st); });
    haSetEntities(uiEntities());
  }
}

static void updateStatusIcons() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  if (apMode) uiSetWifiState(1, 0);
  else if (WiFi.status() == WL_CONNECTED) uiSetWifiState(3, WiFi.RSSI());
  else uiSetWifiState(2, 0);
  uiSetHaState(haState());
  wifiWatchdog();
}

void loop() {
  uiTick();
  updateStatusIcons();
  webLoop();
  improvLoop();
  if (!apMode) haLoop();
  delay(5);
}
