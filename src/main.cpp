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

static void startWifi() {
  if (cfg.wifiSsid.isEmpty()) {
    apMode = true;
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(cfg.hostname.c_str());
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
      uiTick();
      delay(50);
    }
    apMode = WiFi.status() != WL_CONNECTED;
  }
  if (apMode) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Bedside-Setup");  // open AP; browse to http://192.168.4.1/settings
    if (cfg.wifiSsid.isEmpty())
      uiSetBanner("Wi-Fi is not set up yet\n1. On your phone, join Wi-Fi \"Bedside-Setup\"\n2. Open http://192.168.4.1/settings\nTap the gear for the web login.");
    else
      uiSetBanner("Could not join Wi-Fi \"" + cfg.wifiSsid + "\"\nJoin \"Bedside-Setup\" and open\nhttp://192.168.4.1/settings to fix it.");
  } else {
    MDNS.begin(cfg.hostname.c_str());
    MDNS.addService("http", "tcp", 80);
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
}

void loop() {
  uiTick();
  updateStatusIcons();
  webLoop();
  improvLoop();
  if (!apMode) haLoop();
  delay(5);
}
