#include "config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

Config cfg;
std::vector<WifiNet> wifiNets;

int wifiIndexOf(const String& ssid) {
  for (size_t i = 0; i < wifiNets.size(); i++)
    if (wifiNets[i].ssid == ssid) return (int)i;
  return -1;
}

void wifiSave(const String& ssid, const String& pass, bool first, bool keepOldPass) {
  if (ssid.isEmpty()) return;
  int i = wifiIndexOf(ssid);
  if (i >= 0) {
    if (!(keepOldPass && pass.isEmpty())) wifiNets[i].pass = pass;
  } else {
    wifiNets.push_back({ssid, pass});
    i = wifiNets.size() - 1;
  }
  if (first && i > 0) {
    WifiNet n = wifiNets[i];
    wifiNets.erase(wifiNets.begin() + i);
    wifiNets.insert(wifiNets.begin(), n);
  }
  configSave();
}

bool wifiRemove(const String& ssid) {
  int i = wifiIndexOf(ssid);
  if (i < 0) return false;
  wifiNets.erase(wifiNets.begin() + i);
  configSave();
  return true;
}

bool wifiMove(const String& ssid, int delta) {
  int i = wifiIndexOf(ssid), j = i + delta;
  if (i < 0 || j < 0 || j >= (int)wifiNets.size()) return false;
  std::swap(wifiNets[i], wifiNets[j]);
  configSave();
  return true;
}

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

void configClampBrightness() {
  cfg.brightMinPct = constrain(cfg.brightMinPct, 5, 90);
  cfg.brightMaxPct = constrain(cfg.brightMaxPct, cfg.brightMinPct + 10, 100);
  cfg.brightnessPct = constrain(cfg.brightnessPct, cfg.brightMinPct, cfg.brightMaxPct);
}

bool configLoad() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) return false;
  JsonDocument d;
  if (deserializeJson(d, f)) return false;
  wifiNets.clear();
  for (JsonObject n : d["wifi"].as<JsonArray>()) {
    String ssid = n["ssid"] | "";
    if (!ssid.isEmpty()) wifiNets.push_back({ssid, String(n["pass"] | "")});
  }
  // Older single-network files (and data/config.example.json) use wifi_ssid / wifi_pass.
  String legacy = d["wifi_ssid"] | "";
  if (!legacy.isEmpty() && wifiIndexOf(legacy) < 0) wifiNets.push_back({legacy, String(d["wifi_pass"] | "")});
  cfg.haHost = d["ha_host"] | "";
  cfg.haPort = d["ha_port"] | 8123;
  cfg.haTls = d["ha_tls"] | false;
  cfg.haExtHost = d["ha_ext_host"] | "";
  cfg.haExtPort = d["ha_ext_port"] | 443;
  cfg.haExtTls = d["ha_ext_tls"] | true;
  cfg.haToken = d["ha_token"] | "";
  cfg.hostname = d["hostname"] | "bedside";
  cfg.adminPass = d["admin_pass"] | "";
  cfg.rotation = constrain((int)(d["rotation"] | 0), 0, 3);
  cfg.brightMinPct = d["bright_min"] | 10;
  cfg.brightMaxPct = d["bright_max"] | 100;
  cfg.brightnessPct = d["brightness"] | 80;
  configClampBrightness();
  return true;
}

bool configSave() {
  JsonDocument d;
  JsonArray w = d["wifi"].to<JsonArray>();
  for (auto& n : wifiNets) {
    JsonObject o = w.add<JsonObject>();
    o["ssid"] = n.ssid;
    o["pass"] = n.pass;
  }
  d["ha_host"] = cfg.haHost;
  d["ha_port"] = cfg.haPort;
  d["ha_tls"] = cfg.haTls;
  d["ha_ext_host"] = cfg.haExtHost;
  d["ha_ext_port"] = cfg.haExtPort;
  d["ha_ext_tls"] = cfg.haExtTls;
  d["ha_token"] = cfg.haToken;
  d["hostname"] = cfg.hostname;
  d["admin_pass"] = cfg.adminPass;
  d["rotation"] = cfg.rotation;
  d["bright_min"] = cfg.brightMinPct;
  d["bright_max"] = cfg.brightMaxPct;
  d["brightness"] = cfg.brightnessPct;
  File f = LittleFS.open("/config.json", "w");
  if (!f) return false;
  serializeJson(d, f);
  return true;
}

static String landscapeRead() {
  File f = LittleFS.open("/layout.json", "r");
  if (!f) return String(DEFAULT_LAYOUT);
  String s = f.readString();
  return s.length() ? s : String(DEFAULT_LAYOUT);
}

String layoutRead(bool portrait) {
  if (!portrait) return landscapeRead();
  File f = LittleFS.open("/layout_portrait.json", "r");
  if (f) {
    String s = f.readString();
    if (s.length()) return s;
  }
  // No portrait layout yet: squeeze the landscape one (480 wide) into the 320 px portrait width.
  JsonDocument d;
  if (deserializeJson(d, landscapeRead())) return landscapeRead();
  for (JsonObject p : d["pages"].as<JsonArray>())
    for (JsonObject b : p["buttons"].as<JsonArray>()) {
      b["x"] = (int)(b["x"] | 0) * 2 / 3;
      b["w"] = max(40, (int)(b["w"] | 100) * 2 / 3);
    }
  String out;
  serializeJson(d, out);
  return out;
}

bool layoutWrite(const String& json, bool portrait) {
  File f = LittleFS.open(portrait ? "/layout_portrait.json" : "/layout.json", "w");
  if (!f) return false;
  f.print(json);
  return true;
}
