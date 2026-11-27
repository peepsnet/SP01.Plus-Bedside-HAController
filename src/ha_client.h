#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>

using StateCb = std::function<void(const String& entityId, const String& state)>;
using StatusCb = std::function<void(const String& status)>;

void haBegin(StateCb onState, StatusCb onStatus);
void haLoop();
// (Re)subscribe to these entities. Safe to call before connected; applied on auth_ok.
void haSetEntities(const std::vector<String>& ids);
// action: toggle | on | off | scene | script | press
void haCallAction(const String& entityId, const String& action);
bool haConnected();
void haReconnect();  // after settings change
