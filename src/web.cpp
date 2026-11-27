#include "web.h"
#include "config.h"
#include "web_pages.h"
#include <ArduinoJson.h>
#include <Update.h>
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
    d["wifi_ssid"] = cfg.wifiSsid;
    d["ha_host"] = cfg.haHost;
    d["ha_port"] = cfg.haPort;
    d["ha_tls"] = cfg.haTls;
    d["ha_ext_host"] = cfg.haExtHost;
    d["ha_ext_port"] = cfg.haExtPort;
    d["ha_ext_tls"] = cfg.haExtTls;
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
    cfg.wifiSsid = d["wifi_ssid"] | cfg.wifiSsid;
    String pw = d["wifi_pass"] | "";
    if (pw.length()) cfg.wifiPass = pw;
    cfg.haHost = d["ha_host"] | cfg.haHost;
    cfg.haPort = d["ha_port"] | cfg.haPort;
    cfg.haTls = d["ha_tls"] | cfg.haTls;
    cfg.haExtHost = d["ha_ext_host"] | cfg.haExtHost;
    cfg.haExtPort = d["ha_ext_port"] | cfg.haExtPort;
    cfg.haExtTls = d["ha_ext_tls"] | cfg.haExtTls;
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
