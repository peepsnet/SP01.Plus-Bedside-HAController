#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>

// Screen is 480x320 landscape. Buttons live in a 480x280 canvas; a 40px bar at the bottom
// holds page tabs and a connection status label.
constexpr int UI_CANVAS_W = 480;
constexpr int UI_CANVAS_H = 280;

using ButtonHandler = std::function<void(const String& entityId, const String& action)>;

void uiInit(ButtonHandler onPress);
// Rebuild all pages from layout JSON. Returns false (and keeps the old UI) if the JSON is invalid.
bool uiBuild(const String& layoutJson);
void uiSetState(const String& entityId, const String& state);
void uiSetStatus(const String& text);
// Unique entity ids referenced by the current layout (used for the HA subscription).
std::vector<String> uiEntities();
void uiTick();
