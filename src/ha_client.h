#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

// Numeric attributes we track per entity. NAN means "not reported".
struct HaLevels {
  float brightness = NAN;   // light, 0-100 %
  float kelvin = NAN;       // light color temperature
  float position = NAN;     // cover, 0-100 %
  float percentage = NAN;   // fan, 0-100 %
  float volume = NAN;       // media_player, 0-100 %
  float temperature = NAN;  // climate target temperature
  float value = NAN;        // number / input_number state
};

// attrs: the attributes carried by this update (all of them in the first snapshot, only changed ones afterwards).
using StateCb = std::function<void(const String& entityId, const String& state, const HaLevels& levels, JsonObjectConst attrs)>;
using StatusCb = std::function<void(const String& status)>;

void haBegin(StateCb onState, StatusCb onStatus);
void haLoop();
// (Re)subscribe to these entities. Safe to call before connected; applied on auth_ok.
void haSetEntities(const std::vector<String>& ids);
// action: toggle | on | off | press | scene | script | open | close | stop | lock | unlock | play_pause
void haCallAction(const String& entityId, const String& action);
// Set a slider value. attr: brightness | color_temp | (empty: by domain). value is in the slider's own unit.
void haCallSlider(const String& entityId, const String& attr, float value);
bool haConnected();
void haReconnect();  // after settings change

// Entity picker data (id + friendly name). Loaded from HA the first time the web editor asks for it (haEnsureEntities) or on
// haRefreshEntities, and kept in PSRAM. haEntitiesNext walks the list: start with cursor = 0.
bool haEntitiesNext(size_t& cursor, String& id, String& name);
bool haEntitiesLoading();
void haEnsureEntities();
void haRefreshEntities();
enum HaState { HA_NOT_CONFIGURED, HA_CONNECTING, HA_LOCAL, HA_EXTERNAL, HA_AUTH_ERROR };
HaState haState();
String haEndpointName();  // "local" | "external" | ""
