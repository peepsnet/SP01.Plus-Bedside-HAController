#include "config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

Config cfg;

const char* DEFAULT_LAYOUT = R"JSON({
  "pages": [
    {
      "name": "Bedroom",
      "buttons": [
        {"id":"b1","label":"Lamp","icon":"power","x":10,"y":10,"w":150,"h":90,
         "entity_id":"light.bedroom_lamp","action":"toggle","color":"#F5A623"},
        {"id":"b2","label":"Ceiling","icon":"eye","x":170,"y":10,"w":150,"h":90,
         "entity_id":"light.bedroom_ceiling","action":"toggle","color":"#4A90E2"}
      ]
    }
  ]
})JSON";

bool configLoad() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) return false;
  JsonDocument d;
  if (deserializeJson(d, f)) return false;
  cfg.wifiSsid = d["wifi_ssid"] | "";
  cfg.wifiPass = d["wifi_pass"] | "";
  cfg.haHost = d["ha_host"] | "";
  cfg.haPort = d["ha_port"] | 8123;
  cfg.haTls = d["ha_tls"] | false;
  cfg.haToken = d["ha_token"] | "";
  cfg.hostname = d["hostname"] | "bedside";
  return true;
}

bool configSave() {
  JsonDocument d;
  d["wifi_ssid"] = cfg.wifiSsid;
  d["wifi_pass"] = cfg.wifiPass;
  d["ha_host"] = cfg.haHost;
  d["ha_port"] = cfg.haPort;
  d["ha_tls"] = cfg.haTls;
  d["ha_token"] = cfg.haToken;
  d["hostname"] = cfg.hostname;
  File f = LittleFS.open("/config.json", "w");
  if (!f) return false;
  serializeJson(d, f);
  return true;
}

String layoutRead() {
  File f = LittleFS.open("/layout.json", "r");
  if (!f) return String(DEFAULT_LAYOUT);
  String s = f.readString();
  return s.length() ? s : String(DEFAULT_LAYOUT);
}

bool layoutWrite(const String& json) {
  File f = LittleFS.open("/layout.json", "w");
  if (!f) return false;
  f.print(json);
  return true;
}
