#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include "config.h"
#include "ha_client.h"
#include "ui.h"
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
    uiSetStatus("Setup AP: Bedside-Setup  192.168.4.1/settings");
  } else {
    MDNS.begin(cfg.hostname.c_str());
    MDNS.addService("http", "tcp", 80);
    uiSetStatus("http://" + cfg.hostname + ".local  " + WiFi.localIP().toString());
  }
}

void setup() {
  Serial.begin(115200);
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  configLoad();

  uiInit([](const String& entity, const String& action) { haCallAction(entity, action); });
  if (!uiBuild(layoutRead())) uiBuild(DEFAULT_LAYOUT);

  startWifi();
  webBegin(applyLayout);
  if (!apMode) {
    haBegin([](const String& e, const String& s) { uiSetState(e, s); },
            [](const String& st) { Serial.println(st); uiSetStatus(st); });
    haSetEntities(uiEntities());
  }
}

void loop() {
  uiTick();
  webLoop();
  if (!apMode) haLoop();
  delay(5);
}
