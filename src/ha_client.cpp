#include "ha_client.h"
#include "config.h"
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

// Untested against a live Home Assistant. Protocol: https://developers.home-assistant.io/docs/api/websocket

static WebSocketsClient ws;
static StateCb stateCb;
static StatusCb statusCb;
static std::vector<String> wanted;
static bool authed = false;
static uint32_t msgId = 1;

// --- local / external endpoint fallback -------------------------------------------------------
static int endpoint = 0;  // 0 local, 1 external
static uint32_t connectStart = 0;
static uint32_t authedAt = 0;
static bool authFailed = false;
static constexpr uint32_t AUTH_TIMEOUT_MS = 12000;      // give up on an endpoint after this
static constexpr uint32_t RETRY_LOCAL_MS = 5 * 60000;   // while on external, try local again this often

static bool hasLocal() { return !cfg.haHost.isEmpty(); }
static bool hasExternal() { return !cfg.haExtHost.isEmpty(); }

// --- entity picker ------------------------------------------------------------------------------
// The picker list lives in one packed buffer ("entity_id|name\n" lines) in PSRAM, not in a vector of Strings on the internal
// heap: with sensors and binary_sensors a big Home Assistant has thousands of entities, and filling the small internal heap
// with them starved LVGL and the Wi-Fi stack. It is only loaded when the web editor asks for it.
static char* pickBuf = nullptr;
static size_t pickLen = 0, pickCap = 0;
static bool pickLoaded = false;
static bool loadingEntities = false;
static const char* PICK_DOMAINS[] = {"light", "switch", "cover", "fan", "climate", "media_player", "scene", "script",
                                     "input_boolean", "input_number", "number", "lock", "button", "input_button", "camera", "sensor", "binary_sensor"};
static constexpr int PICK_CHUNK = 100;  // entities per request: keeps each reply well under the 15 KB WebSocket limit
static int pickDomain = 0, pickOffset = 0;
static uint32_t tplId = 0;
static uint32_t pickReqAt = 0;

static void sendJson(JsonDocument& d) {
  String out;
  serializeJson(d, out);
  ws.sendTXT(out);
}

static uint32_t subId = 0;  // current subscribe_entities subscription (0 = none on this connection)

static void subscribe() {
  if (!authed || wanted.empty()) return;
  if (subId) {  // replace the old subscription instead of stacking another one (every layout save used to add one)
    JsonDocument u;
    u["id"] = ++msgId;
    u["type"] = "unsubscribe_events";
    u["subscription"] = subId;
    sendJson(u);
  }
  JsonDocument d;
  subId = ++msgId;
  d["id"] = subId;
  d["type"] = "subscribe_entities";
  JsonArray a = d["entity_ids"].to<JsonArray>();
  for (auto& e : wanted) a.add(e);
  sendJson(d);
}

static void pickRequest() {
  if (pickDomain >= (int)(sizeof(PICK_DOMAINS) / sizeof(PICK_DOMAINS[0]))) {
    loadingEntities = false;
    return;
  }
  JsonDocument d;
  tplId = ++msgId;
  pickReqAt = millis();
  d["id"] = tplId;
  d["type"] = "render_template";
  String t = String("{% for s in (states.") + PICK_DOMAINS[pickDomain] + " | sort(attribute='entity_id') | list)[" +
             pickOffset + ":" + (pickOffset + PICK_CHUNK) + "] %}{{ s.entity_id }}|{{ s.name }}\n{% endfor %}";
  d["template"] = t;
  d["timeout"] = 5;
  sendJson(d);
}

void haRefreshEntities() {
  if (!authed) return;
  if (!pickBuf) {
    pickCap = 256 * 1024;
    pickBuf = (char*)ps_malloc(pickCap);
    if (!pickBuf) {  // no PSRAM: a small list is better than none
      pickCap = 24 * 1024;
      pickBuf = (char*)malloc(pickCap);
    }
    if (!pickBuf) { pickCap = 0; return; }
  }
  pickLen = 0;
  pickLoaded = false;
  loadingEntities = true;
  pickDomain = 0;
  pickOffset = 0;
  pickRequest();
}

static void handlePickResult(const char* text) {
  int lines = 0;
  const char* pos = text;
  while (*pos) {
    const char* nl = strchr(pos, '\n');
    size_t n = nl ? (size_t)(nl - pos) : strlen(pos);
    const char* bar = (const char*)memchr(pos, '|', n);
    if (bar && bar > pos) {
      lines++;
      if (pickBuf && pickLen + n + 1 <= pickCap) {  // when the buffer is full the rest is dropped
        memcpy(pickBuf + pickLen, pos, n);
        pickLen += n;
        pickBuf[pickLen++] = '\n';
      }
    }
    if (!nl) break;
    pos = nl + 1;
  }
  // Stop the render_template subscription, then continue with the next chunk or domain.
  JsonDocument u;
  u["id"] = ++msgId;
  u["type"] = "unsubscribe_events";
  u["subscription"] = tplId;
  sendJson(u);
  if (lines >= PICK_CHUNK) pickOffset += PICK_CHUNK;
  else { pickDomain++; pickOffset = 0; }
  pickRequest();
  if (!loadingEntities) pickLoaded = true;
}

static void startEndpoint(int ep) {
  endpoint = ep;
  authFailed = false;
  ws.disconnect();
  authed = false;
  subId = 0;
  connectStart = millis();
  bool ext = ep == 1;
  const String& host = ext ? cfg.haExtHost : cfg.haHost;
  uint16_t port = ext ? cfg.haExtPort : cfg.haPort;
  bool tls = ext ? cfg.haExtTls : cfg.haTls;
  if (tls) ws.beginSSL(host.c_str(), port, "/api/websocket");  // untested; certificates are not verified
  else ws.begin(host.c_str(), port, "/api/websocket");
  ws.setReconnectInterval(5000);
  ws.enableHeartbeat(25000, 5000, 2);
  if (statusCb) statusCb(String("connecting (") + (ext ? "external" : "local") + ")");
}

static void handleLevels(const String& entity, JsonObject a, const String& state, HaLevels& lv) {
  int dot = entity.indexOf('.');
  String domain = dot > 0 ? entity.substring(0, dot) : "";
  if (!a.isNull()) {
    if (a["brightness"].is<float>()) lv.brightness = roundf(a["brightness"].as<float>() * 100.0f / 255.0f);
    if (a["color_temp_kelvin"].is<float>()) lv.kelvin = a["color_temp_kelvin"].as<float>();
    if (a["current_position"].is<float>()) lv.position = a["current_position"].as<float>();
    if (a["percentage"].is<float>()) lv.percentage = a["percentage"].as<float>();
    if (a["volume_level"].is<float>()) lv.volume = roundf(a["volume_level"].as<float>() * 100.0f);
    if (domain == "climate" && a["temperature"].is<float>()) lv.temperature = a["temperature"].as<float>();
  }
  if (domain == "light" && state == "off") lv.brightness = 0;
  if (domain == "number" || domain == "input_number") {
    char* end;
    float v = strtof(state.c_str(), &end);
    if (end != state.c_str()) lv.value = v;
  }
}

static void deliver(const char* entity, JsonObject o) {
  const char* s = o["s"];
  if (!s || !stateCb) return;
  HaLevels lv;
  handleLevels(entity, o["a"].as<JsonObject>(), s, lv);
  stateCb(entity, s, lv, o["a"].as<JsonObjectConst>());
}

static void handleMessage(const char* payload, size_t len) {
  JsonDocument filter;
  filter["type"] = true;
  filter["id"] = true;
  filter["event"] = true;
  JsonDocument d;
  if (deserializeJson(d, payload, len, DeserializationOption::Filter(filter)) &&
      !d["type"].is<const char*>()) {
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
    authFailed = false;
    authedAt = millis();
    if (statusCb) statusCb(String("connected (") + (endpoint ? "external" : "local") + ")");
    subId = 0;  // a new connection has no subscriptions yet
    subscribe();
  } else if (type == "auth_invalid") {
    authed = false;
    authFailed = true;
    if (statusCb) statusCb("token rejected");
  } else if (type == "event") {
    JsonObject ev = d["event"];
    if (tplId && (uint32_t)(d["id"] | 0) == tplId) {  // render_template result for the entity picker
      if (ev["result"].is<const char*>()) handlePickResult(ev["result"].as<const char*>());
      return;
    }
    // Initial snapshot: {"a": {entity: {"s": state, "a": {attributes}}}}
    for (JsonPair kv : ev["a"].as<JsonObject>()) deliver(kv.key().c_str(), kv.value().as<JsonObject>());
    // Changes: {"c": {entity: {"+": {"s": newState, "a": {changed attributes}}}}}
    for (JsonPair kv : ev["c"].as<JsonObject>()) deliver(kv.key().c_str(), kv.value()["+"].as<JsonObject>());
  }
}

static void onWs(WStype_t t, uint8_t* p, size_t len) {
  switch (t) {
    case WStype_CONNECTED:
      if (statusCb) statusCb("socket open");
      break;
    case WStype_DISCONNECTED:
      authed = false;
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
  ws.onEvent(onWs);
  haReconnect();
}

void haReconnect() {
  ws.disconnect();
  authed = false;
  subId = 0;
  if ((!hasLocal() && !hasExternal()) || cfg.haToken.isEmpty()) {
    connectStart = 0;
    if (statusCb) statusCb("not configured");
    return;
  }
  startEndpoint(hasLocal() ? 0 : 1);
}

void haLoop() {
  if (connectStart == 0) return;  // not configured
  ws.loop();
  uint32_t now = millis();
  // A picker load that lost its connection or its reply must not block later loads: give it up so the next request restarts it.
  if (loadingEntities && (!authed || now - pickReqAt > 20000)) loadingEntities = false;
  if (!authed && now - connectStart > AUTH_TIMEOUT_MS) {
    // This endpoint did not come up: try the other one if configured, else retry the same one.
    int other = endpoint == 0 ? 1 : 0;
    bool otherOk = other == 0 ? hasLocal() : hasExternal();
    startEndpoint(otherOk ? other : endpoint);
  } else if (authed && endpoint == 1 && hasLocal() && now - authedAt > RETRY_LOCAL_MS) {
    startEndpoint(0);  // prefer local again; falls back to external after the auth timeout if it is unreachable
  }
}

void haSetEntities(const std::vector<String>& ids) {
  wanted = ids;
  subscribe();  // a second subscription may overlap the old one until reconnect; harmless for state updates
}

bool haConnected() { return authed; }
HaState haState() {
  if (connectStart == 0) return HA_NOT_CONFIGURED;
  if (authed) return endpoint ? HA_EXTERNAL : HA_LOCAL;
  return authFailed ? HA_AUTH_ERROR : HA_CONNECTING;
}
String haEndpointName() { return !authed ? "" : (endpoint ? "external" : "local"); }
bool haEntitiesNext(size_t& cursor, String& id, String& name) {
  if (!pickBuf) return false;
  while (cursor < pickLen) {
    const char* line = pickBuf + cursor;
    const char* end = (const char*)memchr(line, '\n', pickLen - cursor);
    size_t n = end ? (size_t)(end - line) : pickLen - cursor;
    cursor += n + 1;
    const char* bar = (const char*)memchr(line, '|', n);
    if (!bar) continue;
    id = "";
    name = "";
    id.concat(line, (unsigned int)(bar - line));  // the buffer is not NUL-terminated per line: copy exactly n bytes
    name.concat(bar + 1, (unsigned int)(n - (bar - line) - 1));
    return true;
  }
  return false;
}
bool haEntitiesLoading() { return loadingEntities; }
void haEnsureEntities() {
  if (!pickLoaded && !loadingEntities) haRefreshEntities();
}

static void callService(const String& domain, const String& service, const String& entity, JsonDocument* data = nullptr) {
  JsonDocument d;
  d["id"] = ++msgId;
  d["type"] = "call_service";
  d["domain"] = domain;
  d["service"] = service;
  if (data) d["service_data"] = *data;
  d["target"]["entity_id"] = entity;
  sendJson(d);
}

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
  else if (action == "open") service = "open_cover";
  else if (action == "close") service = "close_cover";
  else if (action == "stop") service = "stop_cover";
  else if (action == "lock") service = "lock";
  else if (action == "unlock") service = "unlock";
  else if (action == "play_pause") service = "media_play_pause";
  else return;
  callService(domain, service, entityId);
}

void haCallSlider(const String& entityId, const String& attr, float v) {
  if (!authed) return;
  int dot = entityId.indexOf('.');
  if (dot < 0) return;
  String domain = entityId.substring(0, dot);
  JsonDocument data;
  if (domain == "light") {
    if (attr == "color_temp") data["color_temp_kelvin"] = (int)v;
    else data["brightness_pct"] = (int)v;
    callService("light", "turn_on", entityId, &data);
  } else if (domain == "cover") {
    data["position"] = (int)v;
    callService("cover", "set_cover_position", entityId, &data);
  } else if (domain == "fan") {
    data["percentage"] = (int)v;
    callService("fan", "set_percentage", entityId, &data);
  } else if (domain == "media_player") {
    data["volume_level"] = v / 100.0f;
    callService("media_player", "volume_set", entityId, &data);
  } else if (domain == "climate") {
    data["temperature"] = v;
    callService("climate", "set_temperature", entityId, &data);
  } else if (domain == "number" || domain == "input_number") {
    data["value"] = v;
    callService(domain, "set_value", entityId, &data);
  }
}
