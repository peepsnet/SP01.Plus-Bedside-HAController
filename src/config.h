#pragma once
#include <Arduino.h>
#include <vector>

// Device settings (Wi-Fi + Home Assistant). Stored in /config.json on LittleFS.
// That file holds secrets: it is gitignored and written by the on-device /settings page.
struct Config {
  String haHost;      // e.g. "homeassistant.local" or "192.168.1.10"
  uint16_t haPort = 8123;
  bool haTls = false;  // use wss:// instead of ws://
  // Optional external URL, used when the local one is unreachable (e.g. Nabu Casa or a reverse proxy).
  String haExtHost;
  uint16_t haExtPort = 443;
  bool haExtTls = true;
  String haToken;      // long-lived access token
  String hostname = "bedside";
  // Screen brightness in percent. The quick-panel slider only offers min..max, so the screen can never go fully dark.
  int brightMinPct = 10;
  int brightMaxPct = 100;
  int brightnessPct = 80;  // last chosen value
  String adminPass;    // HTTP basic-auth password for the web UI (user: admin). Generated on first boot.
};

extern Config cfg;

// Saved Wi-Fi networks, highest priority first (index 0). Passwords stay on the device and are never sent to a browser.
struct WifiNet {
  String ssid;
  String pass;
};
extern std::vector<WifiNet> wifiNets;
int wifiIndexOf(const String& ssid);
// Add a network, or update the password of an existing one (an empty pass keeps the old password when keepOldPass).
// first=true also moves it to the top priority. Saves to flash.
void wifiSave(const String& ssid, const String& pass, bool first, bool keepOldPass = false);
bool wifiRemove(const String& ssid);
bool wifiMove(const String& ssid, int delta);  // -1 = higher priority, +1 = lower


// Keeps brightness limits sane: min 5..90 %, max at least min+10 and at most 100 %, current value inside them.
void configClampBrightness();
bool configLoad();
bool configSave();
extern const char* DEFAULT_LAYOUT;  // used when /layout.json is missing
String layoutRead();
bool layoutWrite(const String& json);
