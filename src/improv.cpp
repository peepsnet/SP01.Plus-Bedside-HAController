#include "improv.h"
#include <WiFi.h>
#include <vector>
#include "config.h"
#include "version.h"

// Untested on hardware. Written from the Improv serial spec v1.

namespace {
enum : uint8_t { T_STATE = 1, T_ERROR = 2, T_RPC = 3, T_RESULT = 4 };
enum : uint8_t { S_READY = 2, S_PROVISIONING = 3, S_PROVISIONED = 4 };
enum : uint8_t { E_NONE = 0, E_INVALID_RPC = 1, E_UNKNOWN_RPC = 2, E_UNABLE_TO_CONNECT = 3 };
enum : uint8_t { C_WIFI = 1, C_STATE = 2, C_INFO = 3, C_SCAN = 4 };

std::vector<uint8_t> rx;

void sendPacket(uint8_t type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> p = {'I', 'M', 'P', 'R', 'O', 'V', 1, type, (uint8_t)data.size()};
  p.insert(p.end(), data.begin(), data.end());
  uint8_t sum = 0;
  for (uint8_t b : p) sum += b;
  p.push_back(sum);
  p.push_back('\n');
  Serial.write(p.data(), p.size());
}

uint8_t currentState() { return WiFi.status() == WL_CONNECTED ? S_PROVISIONED : S_READY; }
void sendState(uint8_t s) { sendPacket(T_STATE, {s}); }
void sendError(uint8_t e) { sendPacket(T_ERROR, {e}); }

void addStr(std::vector<uint8_t>& d, const String& s) {
  d.push_back((uint8_t)s.length());
  for (size_t i = 0; i < s.length(); i++) d.push_back((uint8_t)s[i]);
}

void sendResult(uint8_t cmd, const std::vector<String>& strs) {
  std::vector<uint8_t> body;
  for (auto& s : strs) addStr(body, s);
  std::vector<uint8_t> d = {cmd, (uint8_t)body.size()};
  d.insert(d.end(), body.begin(), body.end());
  sendPacket(T_RESULT, d);
}

void handleWifi(const uint8_t* d, size_t len) {
  if (len < 2) return sendError(E_INVALID_RPC);
  size_t sl = d[0];
  if (len < 1 + sl + 1) return sendError(E_INVALID_RPC);
  String ssid, pass;
  for (size_t i = 0; i < sl; i++) ssid += (char)d[1 + i];
  size_t pl = d[1 + sl];
  if (len < 2 + sl + pl) return sendError(E_INVALID_RPC);
  for (size_t i = 0; i < pl; i++) pass += (char)d[2 + sl + i];

  sendState(S_PROVISIONING);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    sendError(E_UNABLE_TO_CONNECT);
    sendState(S_READY);
    return;
  }
  cfg.wifiSsid = ssid;
  cfg.wifiPass = pass;
  configSave();
  sendState(S_PROVISIONED);
  sendResult(C_WIFI, {"http://" + WiFi.localIP().toString() + "/"});
  delay(1500);
  ESP.restart();  // simplest way to start normal STA operation, mDNS and the HA client
}

void handleScan() {
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  else if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n && i < 20; i++)
    sendResult(C_SCAN, {WiFi.SSID(i), String(WiFi.RSSI(i)),
                        WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"});
  sendResult(C_SCAN, {});
  WiFi.scanDelete();
}

void handlePacket(uint8_t type, const uint8_t* d, size_t len) {
  if (type != T_RPC || len < 2) return;
  uint8_t cmd = d[0];
  size_t dl = d[1];
  if (len < 2 + dl) return sendError(E_INVALID_RPC);
  switch (cmd) {
    case C_WIFI: handleWifi(d + 2, dl); break;
    case C_STATE:
      sendState(currentState());
      if (currentState() == S_PROVISIONED) sendResult(C_STATE, {"http://" + WiFi.localIP().toString() + "/"});
      break;
    case C_INFO: sendResult(C_INFO, {"SC01Plus-Bedside-HAController", FW_VERSION, "ESP32-S3", cfg.hostname}); break;
    case C_SCAN: handleScan(); break;
    default: sendError(E_UNKNOWN_RPC);
  }
}
}  // namespace

void improvBegin() { sendState(currentState()); }

void improvLoop() {
  while (Serial.available()) {
    rx.push_back((uint8_t)Serial.read());
    // Resync: keep only buffers that can still be an Improv packet.
    const char* hdr = "IMPROV";
    size_t n = rx.size();
    if (n <= 6) {
      if (rx[n - 1] != (uint8_t)hdr[n - 1]) { rx.clear(); continue; }
      continue;
    }
    if (n < 9) continue;
    size_t total = 9 + rx[8] + 1;  // header(6)+ver+type+len + data + checksum
    if (n < total) continue;
    uint8_t sum = 0;
    for (size_t i = 0; i < total - 1; i++) sum += rx[i];
    if (sum == rx[total - 1] && rx[6] == 1) handlePacket(rx[7], rx.data() + 9, rx[8]);
    rx.clear();
  }
}
