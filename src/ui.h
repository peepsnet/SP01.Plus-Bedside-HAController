#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>
#include "ha_client.h"

// Screen is 480x320 landscape. Buttons live in a 480x280 canvas; a 40px bar at the bottom
// holds page tabs and a connection status label.
constexpr int UI_CANVAS_W = 480;
constexpr int UI_CANVAS_H = 280;

using ButtonHandler = std::function<void(const String& entityId, const String& action)>;
using SliderHandler = std::function<void(const String& entityId, const String& attr, float value)>;

void uiInit(ButtonHandler onPress, SliderHandler onSlide);
// Rebuild all pages from layout JSON. Returns false (and keeps the old UI) if the JSON is invalid.
bool uiBuild(const String& layoutJson);
void uiSetState(const String& entityId, const String& state, const HaLevels& levels);
void uiSetStatus(const String& text);
// Large message card over the home screen (first-run / Wi-Fi help). Empty text hides it.
void uiSetBanner(const String& text);
// Unique entity ids referenced by the current layout (used for the HA subscription).
std::vector<String> uiEntities();
void uiTick();

// Backlight in percent, clamped to the configured min..max. save=true also stores it in /config.json.
void uiSetBrightnessPct(int pct, bool save);
// Implemented in menu.cpp: true while the Settings overlay is showing (swipe gestures are ignored then).
bool menuIsOpen();

// Status icons in the bottom bar (Wi-Fi arcs and an outlined "HA" pill), replacing the old status text.
// wifi: 0 off, 1 setup access point, 2 connecting, 3 connected (rssi in dBm used for the color)
void uiSetWifiState(int state, int rssi);
void uiSetHaState(HaState state);
String uiStatusText();  // last Home Assistant status text, shown on the About screen
