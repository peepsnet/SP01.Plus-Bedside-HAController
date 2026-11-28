#include "web.h"
#include "config.h"
#include "ota.h"
#include "web_pages.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include "version.h"
#include "ha_client.h"

static WebServer server(80);
static std::function<bool(const String&)> layoutCb;
static bool updateOk = false;
static String updateErr;
static bool installPending = false;
static size_t updateWritten = 0;
static int updateTotal = 0;

// HTTP basic auth (user "admin"). Credentials are sent unencrypted over plain HTTP: LAN use only.
static bool authed() {
  if (server.authenticate("admin", cfg.adminPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Bedside");
  return false;
}
#define AUTH() if (!authed()) return

void webBegin(std::function<bool(const String&)> onLayout) {
  layoutCb = onLayout;
  otaPumpHook = [] { server.handleClient(); };

  server.on("/", HTTP_GET, [] { AUTH(); server.send_P(200, "text/html", EDITOR_HTML); });
  server.on("/settings", HTTP_GET, [] { AUTH(); server.send_P(200, "text/html", SETTINGS_HTML); });

  // ?o=landscape|portrait picks which layout; without it, the one for the current screen rotation.
  auto wantPortrait = [] { return server.hasArg("o") ? server.arg("o") == "portrait" : cfgPortrait(); };
  server.on("/api/layout", HTTP_GET, [wantPortrait] { AUTH(); server.send(200, "application/json", layoutRead(wantPortrait())); });
  server.on("/api/layout", HTTP_POST, [wantPortrait] {
    AUTH();
    String body = server.arg("plain");
    bool portrait = wantPortrait();
    bool ok;
    if (portrait == cfgPortrait()) {
      ok = layoutCb && layoutCb(body);  // callback validates + rebuilds the UI
    } else {  // editing the other orientation: only check it, it shows up after a rotation
      JsonDocument chk;
      ok = !deserializeJson(chk, body) && chk["pages"].is<JsonArray>();
    }
    if (!ok) {
      server.send(400, "text/plain", "invalid layout");
      return;
    }
    layoutWrite(body, portrait);
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
    d["rotation"] = cfg.rotation;
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
    cfg.rotation = constrain((int)(d["rotation"] | cfg.rotation), 0, 3);
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
    else haEnsureEntities();  // first request after boot loads the list from Home Assistant
    // Streamed in pieces: the list can be thousands of entities and must not be built in memory in one go.
    auto esc = [](const String& in) {
      String o;
      o.reserve(in.length() + 4);
      for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
      }
      return o;
    };
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    String head = String("{\"connected\":") + (haConnected() ? "true" : "false") + ",\"endpoint\":\"" + haEndpointName() +
                  "\",\"loading\":" + (haEntitiesLoading() ? "true" : "false") + ",\"entities\":[";
    server.sendContent(head);
    String chunk, id, name;
    size_t cursor = 0;
    bool first = true;
    while (haEntitiesNext(cursor, id, name)) {
      if (!first) chunk += ',';
      first = false;
      chunk += "[\"" + esc(id) + "\",\"" + esc(name) + "\"]";
      if (chunk.length() > 1000) { server.sendContent(chunk); chunk = ""; }
    }
    chunk += "]}";
    server.sendContent(chunk);
    server.sendContent("");
  });
  server.on("/update", HTTP_GET, [] { AUTH(); server.send_P(200, "text/html", UPDATE_HTML); });
  server.on("/api/ota/check", HTTP_GET, [] {
    AUTH();
    String tag = otaLatestTag();
    JsonDocument d;
    d["current"] = FW_VERSION;
    d["latest"] = tag;
    d["newer"] = !tag.isEmpty() && otaIsNewer(tag);
    String out;
    serializeJson(d, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/ota/install", HTTP_POST, [] {  // starts after this reply; poll /api/ota/status
    AUTH();
    installPending = true;
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/ota/status", HTTP_GET, [] {
    AUTH();
    OtaState s = otaState();
    JsonDocument d;
    d["active"] = s.active;
    d["phase"] = s.phase;
    d["pct"] = s.pct;
    d["error"] = s.error;
    String out;
    serializeJson(d, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/reboot", HTTP_POST, [] {
    AUTH();
    server.send(200, "text/plain", "ok");
    delay(500);
    ESP.restart();
  });
  // Factory reset: erases saved Wi-Fi, Home Assistant settings, the web password and both layouts. The page asks twice first.
  server.on("/api/factory-reset", HTTP_POST, [] {
    AUTH();
    LittleFS.remove("/config.json");
    LittleFS.remove("/layout.json");
    LittleFS.remove("/layout_portrait.json");
    server.send(200, "text/plain", "ok");
    delay(500);
    ESP.restart();
  });
  server.on("/api/version", HTTP_GET, [] { AUTH(); server.send(200, "text/plain", FW_VERSION); });
  server.on("/update", HTTP_POST,
    [] {
      if (!authed()) return;
      server.send(updateOk ? 200 : 500, "text/plain", updateOk ? "ok, rebooting" : (updateErr.length() ? updateErr : Update.errorString()));
      if (updateOk) {
        otaReport("Rebooting", 100);
        delay(800);
        ESP.restart();
      }
    },
    [] {
      if (!server.authenticate("admin", cfg.adminPass.c_str())) return;
      HTTPUpload& u = server.upload();
      if (u.status == UPLOAD_FILE_START) {
        updateOk = false;
        updateErr = "";
        updateWritten = 0;
        updateTotal = server.clientContentLength();  // includes a little multipart overhead
        otaReport("Receiving and writing", 0);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          updateErr = Update.errorString();
          otaFail(updateErr);
        }
      } else if (u.status == UPLOAD_FILE_WRITE) {
        if (updateErr.length()) return;
        if (Update.write(u.buf, u.currentSize) != u.currentSize) {
          updateErr = Update.errorString();
          Update.abort();
          otaFail(updateErr);
          return;
        }
        updateWritten += u.currentSize;
        int pct = updateTotal > 0 ? (int)((uint64_t)updateWritten * 100 / updateTotal) : -1;
        otaReport("Receiving and writing", pct > 99 ? 99 : pct);
      } else if (u.status == UPLOAD_FILE_END) {
        if (updateErr.length()) return;
        otaReport("Verifying", 100);
        updateOk = Update.end(true);
        if (!updateOk) {
          updateErr = Update.errorString();
          otaFail(updateErr);
        }
      } else if (u.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        updateErr = "Upload interrupted";
        otaFail(updateErr);
      }
    });
  server.begin();
}

void webLoop() {
  server.handleClient();
  if (installPending) {  // GitHub install requested from the web page; runs here, outside the request handler
    installPending = false;
    String err = otaInstallLatest();
    if (!err.isEmpty()) otaFail(err);
  }
}
