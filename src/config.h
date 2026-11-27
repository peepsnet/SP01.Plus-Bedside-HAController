#pragma once
#include <Arduino.h>

// Device settings (Wi-Fi + Home Assistant). Stored in /config.json on LittleFS.
// That file holds secrets: it is gitignored and written by the on-device /settings page.
struct Config {
  String wifiSsid;
  String wifiPass;
  String haHost;      // e.g. "homeassistant.local" or "192.168.1.10"
  uint16_t haPort = 8123;
  bool haTls = false;  // use wss:// instead of ws://
  String haToken;      // long-lived access token
  String hostname = "bedside";
};

extern Config cfg;

bool configLoad();
bool configSave();
extern const char* DEFAULT_LAYOUT;  // used when /layout.json is missing
String layoutRead();
bool layoutWrite(const String& json);
