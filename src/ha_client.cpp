#include "ha_client.h"
#include "config.h"
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

static WebSocketsClient ws;
static StateCb stateCb;
static StatusCb statusCb;
static std::vector<String> wanted;
static bool authed = false;
static uint32_t msgId = 1;
static uint32_t subId = 0;

static void sendJson(JsonDocument& d) {
  String out;
  serializeJson(d, out);
  ws.sendTXT(out);
}

static void subscribe() {
  if (!authed || wanted.empty()) return;
  JsonDocument d;
  subId = ++msgId;
  d["id"] = subId;
  d["type"] = "subscribe_entities";
  JsonArray a = d["entity_ids"].to<JsonArray>();
  for (auto& e : wanted) a.add(e);
  sendJson(d);
}

static void handleMessage(const char* payload, size_t len) {
  // Only keep what we need; entity payloads can be large.
  JsonDocument filter;
  filter["type"] = true;
  filter["event"] = true;
  filter["success"] = true;
  JsonDocument d;
  if (deserializeJson(d, payload, len, DeserializationOption::Filter(filter)) &&
      !d["type"].is<const char*>()) {
    // Fall back to unfiltered parse for auth messages (small).
    d.clear();
    if (deserializeJson(d, payload, len)) return;
  }
  String type = d["type"] | "";
  if (type == "auth_required") {
    JsonDocument a;
    a["type"] = "auth";
    a["access_token"] = cfg.haToken;
    sendJson(a);
  } else if (type == "auth_ok") {
    authed = true;
    if (statusCb) statusCb("HA connected");
    subscribe();
  } else if (type == "auth_invalid") {
    authed = false;
    if (statusCb) statusCb("HA token rejected");
  } else if (type == "event") {
    JsonObject ev = d["event"];
    // Initial snapshot: {"a": {entity: {"s": state, ...}}}
    for (JsonPair kv : ev["a"].as<JsonObject>()) {
      const char* s = kv.value()["s"];
      if (s && stateCb) stateCb(kv.key().c_str(), s);
    }
    // Changes: {"c": {entity: {"+": {"s": newState, ...}}}}
    for (JsonPair kv : ev["c"].as<JsonObject>()) {
      const char* s = kv.value()["+"]["s"];
      if (s && stateCb) stateCb(kv.key().c_str(), s);
    }
  }
}

static void onWs(WStype_t t, uint8_t* p, size_t len) {
  switch (t) {
    case WStype_CONNECTED:
      if (statusCb) statusCb("HA socket open");
      break;
    case WStype_DISCONNECTED:
      authed = false;
      if (statusCb) statusCb("HA offline");
      break;
    case WStype_TEXT:
      handleMessage((const char*)p, len);
      break;
    default:
      break;
  }
}

void haBegin(StateCb onState, StatusCb onStatus) {
  stateCb = onState;
  statusCb = onStatus;
  haReconnect();
}

void haReconnect() {
  ws.disconnect();
  authed = false;
  if (cfg.haHost.isEmpty() || cfg.haToken.isEmpty()) {
    if (statusCb) statusCb("HA not configured");
    return;
  }
  if (cfg.haTls) ws.beginSSL(cfg.haHost.c_str(), cfg.haPort, "/api/websocket");  // untested; no cert pinning
  else ws.begin(cfg.haHost.c_str(), cfg.haPort, "/api/websocket");
  ws.onEvent(onWs);
  ws.setReconnectInterval(5000);
  ws.enableHeartbeat(25000, 5000, 2);
}

void haLoop() { ws.loop(); }

void haSetEntities(const std::vector<String>& ids) {
  wanted = ids;
  subscribe();  // a second subscription may overlap the old one until reconnect; harmless for state updates
}

bool haConnected() { return authed; }

void haCallAction(const String& entityId, const String& action) {
  if (!authed) return;
  int dot = entityId.indexOf('.');
  if (dot < 0) return;
  String domain = entityId.substring(0, dot);
  String service;
  if (action == "toggle") service = "toggle";
  else if (action == "on") service = "turn_on";
  else if (action == "off") service = "turn_off";
  else if (action == "press") service = "press";
  else if (action == "scene" || action == "script") service = "turn_on";
  else return;
  JsonDocument d;
  d["id"] = ++msgId;
  d["type"] = "call_service";
  d["domain"] = domain;
  d["service"] = service;
  d["target"]["entity_id"] = entityId;
  sendJson(d);
}
