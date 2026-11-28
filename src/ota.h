#pragma once
#include <Arduino.h>

// Latest GitHub release tag (e.g. "v0.2.0"), or "" on error.
String otaLatestTag();
// True when `tag` is a higher semantic version than the running firmware.
bool otaIsNewer(const String& tag);
// Download the latest release's firmware.bin and flash it. Reboots on success; returns an error text otherwise.
String otaInstallLatest();
// Progress for any firmware install (GitHub download or web upload), shown full screen on the touchscreen.
// pct < 0 means "working, no percentage". Phases used: Looking up release, Downloading, Writing, Verifying, Rebooting.
void otaReport(const char* phase, int pct);
// Latest install state, for the web page to poll.
struct OtaState { bool active; String phase; int pct; String error; };
OtaState otaState();
// While a GitHub install runs, this is called so the web server can keep answering status requests.
extern void (*otaPumpHook)();
void otaFail(const String& why);  // shows the error with a Close button
