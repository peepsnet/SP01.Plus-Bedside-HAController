#include "web.h"
#include "config.h"
#include "web_pages.h"
#include <ArduinoJson.h>
#include <WebServer.h>

static WebServer server(80);
static std::function<bool(const String&)> layoutCb;

void webBegin(std::function<bool(const String&)> onLayout) {
  layoutCb = onLayout;

  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", EDITOR_HTML); });
  server.on("/settings", HTTP_GET, [] { server.send_P(200, "text/html", SETTINGS_HTML); });

  server.on("/api/layout", HTTP_GET, [] { server.send(200, "application/json", layoutRead()); });
  server.on("/api/layout", HTTP_POST, [] {
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
    JsonDocument d;
    d["wifi_ssid"] = cfg.wifiSsid;
    d["ha_host"] = cfg.haHost;
    d["ha_port"] = cfg.haPort;
    d["ha_tls"] = cfg.haTls;
    String out;
    serializeJson(d, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/config", HTTP_POST, [] {
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
  server.begin();
}

void webLoop() { server.handleClient(); }
