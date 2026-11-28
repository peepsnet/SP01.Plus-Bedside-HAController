#include "web.h"
#include "config.h"
#include "web_pages.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <WiFi.h>
#include <WebServer.h>
#include "version.h"
#include "ha_client.h"

static WebServer server(80);
static std::function<bool(const String&)> layoutCb;
static bool updateOk = false;

// HTTP basic auth (user "admin"). Credentials are sent unencrypted over plain HTTP: LAN use only.
static bool authed() {
  if (server.authenticate("admin", cfg.adminPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Bedside");
  return false;
}
#define AUTH() if (!authed()) return

void webBegin(std::function<bool(const String&)> onLayout) {
  layoutCb = onLayout;

  server.on("/", HTTP_GET, [] { AUTH(); server.send_P(200, "text/html", EDITOR_HTML); });
  server.on("/settings", HTTP_GET, [] { AUTH(); server.send_P(200, "text/html", SETTINGS_HTML); });

  server.on("/api/layout", HTTP_GET, [] { AUTH(); server.send(200, "application/json", layoutRead()); });
  server.on("/api/layout", HTTP_POST, [] {
    AUTH();
    String body = server.arg("plain");
    if (!layoutCb || !layoutCb(body)) {  // callback validates + rebuilds the UI
      server.send(400, "text/plain", "invalid layout");
      return;
    }
    layoutWrite(body);
    server.send(200, "text/plain", "ok");
  });

  // Secrets are never sent back to the browser: only whether they are set.
  server.on("/api/config", HTTP_GET, [] {
    AUTH();
    JsonDocument d;
    d["ha_host"] = cfg.haHost;
    d["ha_port"] = cfg.haPort;
    d["ha_tls"] = cfg.haTls;
    d["ha_ext_host"] = cfg.haExtHost;
    d["ha_ext_port"] = cfg.haExtPort;
    d["ha_ext_tls"] = cfg.haExtTls;
    d["bright_min"] = cfg.brightMinPct;
    d["bright_max"] = cfg.brightMaxPct;
    d["version"] = FW_VERSION;
    String out;
    serializeJson(d, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/config", HTTP_POST, [] {
    AUTH();
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) {
      server.send(400, "text/plain", "bad json");
      return;
    }
    String legacySsid = d["wifi_ssid"] | "";  // older clients: still accepted, saved as the top network
    if (!legacySsid.isEmpty()) wifiSave(legacySsid, String(d["wifi_pass"] | ""), true, true);
    cfg.haHost = d["ha_host"] | cfg.haHost;
    cfg.haPort = d["ha_port"] | cfg.haPort;
    cfg.haTls = d["ha_tls"] | cfg.haTls;
    cfg.haExtHost = d["ha_ext_host"] | cfg.haExtHost;
    cfg.haExtPort = d["ha_ext_port"] | cfg.haExtPort;
    cfg.haExtTls = d["ha_ext_tls"] | cfg.haExtTls;
    cfg.brightMinPct = d["bright_min"] | cfg.brightMinPct;
    cfg.brightMaxPct = d["bright_max"] | cfg.brightMaxPct;
    configClampBrightness();  // enforce sane limits server-side as well
    String tok = d["ha_token"] | "";
    if (tok.length()) cfg.haToken = tok;
    if (!configSave()) {
      server.send(500, "text/plain", "write failed");
      return;
    }
    server.send(200, "text/plain", "ok");
    delay(500);
    ESP.restart();
  });
  // ---- Wi-Fi networks. Saved passwords are write-only: they are never returned to the browser. ----
  server.on("/api/wifi", HTTP_GET, [] {
    AUTH();
    JsonDocument d;
    bool sta = WiFi.status() == WL_CONNECTED;
    d["connected"] = sta ? WiFi.SSID() : String("");
    d["ip"] = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    JsonArray a = d["networks"].to<JsonArray>();
    for (auto& n : wifiNets) {
      JsonObject o = a.add<JsonObject>();
      o["ssid"] = n.ssid;
      o["has_pass"] = !n.pass.isEmpty();
    }
    String out;
    serializeJson(d, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/wifi/scan", HTTP_GET, [] {
    AUTH();
    if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);
    int n = WiFi.scanNetworks();
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    for (int i = 0; i < n && a.size() < 25; i++) {  // results come strongest first
      String ssid = WiFi.SSID(i);
      bool dup = false;
      for (JsonObject o : a) if (o["ssid"] == ssid) dup = true;
      if (ssid.isEmpty() || dup) continue;
      JsonObject o = a.add<JsonObject>();
      o["ssid"] = ssid;
      o["rssi"] = WiFi.RSSI(i);
      o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
      o["saved"] = wifiIndexOf(ssid) >= 0;
    }
    WiFi.scanDelete();
    String out;
    serializeJson(d, out);
    server.send(200, "application/json", out);
  });
  // body: {ssid, pass?, first?}. Adds a network (even one that is out of range right now) or changes its password.
  server.on("/api/wifi/save", HTTP_POST, [] {
    AUTH();
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) return server.send(400, "text/plain", "bad json");
    String ssid = d["ssid"] | "", pass = d["pass"] | "";
    if (ssid.isEmpty() || ssid.length() > 32) return server.send(400, "text/plain", "SSID must be 1 to 32 characters");
    if (!pass.isEmpty() && (pass.length() < 8 || pass.length() > 63))
      return server.send(400, "text/plain", "Password must be 8 to 63 characters (or empty for an open network)");
    wifiSave(ssid, pass, d["first"] | false, true);
    server.send(200, "text/plain", "ok");
  });
  server.on("/api/wifi/delete", HTTP_POST, [] {
    AUTH();
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) return server.send(400, "text/plain", "bad json");
    server.send(wifiRemove(String(d["ssid"] | "")) ? 200 : 404, "text/plain", "");
  });
  server.on("/api/wifi/move", HTTP_POST, [] {  // body: {ssid, dir: -1 | 1}
    AUTH();
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) return server.send(400, "text/plain", "bad json");
    server.send(wifiMove(String(d["ssid"] | ""), (int)(d["dir"] | 0)) ? 200 : 400, "text/plain", "");
  });
  // Make a saved network the top priority and restart so the device joins it now.
  server.on("/api/wifi/use", HTTP_POST, [] {
    AUTH();
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) return server.send(400, "text/plain", "bad json");
    String ssid = d["ssid"] | "";
    int i = wifiIndexOf(ssid);
    if (i < 0) return server.send(404, "text/plain", "not saved");
    wifiSave(ssid, "", true, true);
    server.send(200, "text/plain", "restarting");
    delay(500);
    ESP.restart();
  });

  // Entity list for the editor's picker. ?refresh=1 reloads it from Home Assistant.
  server.on("/api/entities", HTTP_GET, [] {
    AUTH();
    if (server.hasArg("refresh")) haRefreshEntities();
    JsonDocument d;
    d["connected"] = haConnected();
    d["endpoint"] = haEndpointName();
    d["loading"] = haEntitiesLoading();
    JsonArray a = d["entities"].to<JsonArray>();
    for (auto& e : haEntities()) {
      JsonArray row = a.add<JsonArray>();
      row.add(e.first);
      row.add(e.second);
    }
    String out;
    serializeJson(d, out);
    server.send(200, "application/json", out);
  });
  server.on("/update", HTTP_GET, [] { AUTH(); server.send_P(200, "text/html", UPDATE_HTML); });
  server.on("/api/version", HTTP_GET, [] { AUTH(); server.send(200, "text/plain", FW_VERSION); });
  server.on("/update", HTTP_POST,
    [] {
      if (!authed()) return;
      server.send(updateOk ? 200 : 500, "text/plain", updateOk ? "ok, rebooting" : Update.errorString());
      if (updateOk) { delay(500); ESP.restart(); }
    },
    [] {
      if (!server.authenticate("admin", cfg.adminPass.c_str())) return;
      HTTPUpload& u = server.upload();
      if (u.status == UPLOAD_FILE_START) {
        updateOk = false;
        Update.begin(UPDATE_SIZE_UNKNOWN);
      } else if (u.status == UPLOAD_FILE_WRITE) {
        Update.write(u.buf, u.currentSize);
      } else if (u.status == UPLOAD_FILE_END) {
        updateOk = Update.end(true);
      } else if (u.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
      }
    });
  server.begin();
}

void webLoop() { server.handleClient(); }
