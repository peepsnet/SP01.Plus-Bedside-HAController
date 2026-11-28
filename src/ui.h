#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>
#include "ha_client.h"

// Screen is 480x320 landscape by default. Buttons live in a canvas above a 40px bar at the bottom
// that holds page tabs and the connection status icons.
// Rotating the screen to portrait swaps this to 320x480. The dashboard canvas is the screen minus the 40 px bar.
int uiW();
int uiH();
int uiCanvasH();
bool uiPortrait();

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

// Full-screen progress card (firmware update). pct < 0 shows no percentage. uiProgress repaints right away, so it works
// from inside blocking download/upload loops. uiProgressFail shows a message with a Close button.
void uiProgress(const char* title, const char* phase, int pct);
void uiProgressFail(const char* title, const String& why);
// Set when the layout has a camera button: opens that camera full screen (see camera.h).

// Backlight in percent, clamped to the configured min..max. save=true also stores it in /config.json.
void uiSetBrightnessPct(int pct, bool save);
// Temporarily drives the backlight to 100 % (on=true) or back to the saved brightness (on=false). Not saved.
void uiSetFullBrightness(bool on);
// Implemented in menu.cpp: true while the Settings overlay is showing (swipe gestures are ignored then).
bool menuIsOpen();

// Status icons in the bottom bar (Wi-Fi arcs and an outlined "HA" pill), replacing the old status text.
// wifi: 0 off, 1 setup access point, 2 connecting, 3 connected (rssi in dBm used for the color)
void uiSetWifiState(int state, int rssi);
void uiSetHaState(HaState state);
String uiStatusText();  // last Home Assistant status text, shown on the About screen
