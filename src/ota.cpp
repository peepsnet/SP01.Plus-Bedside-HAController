#include "ota.h"
#include "version.h"
#include "ui.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// NOTE: TLS certificates are NOT verified (setInsecure) to avoid shipping a CA bundle in milestone two.
// Anyone able to MITM your network could serve a malicious image. Firmware images are not signed either.
// Works for public repositories only. All of this is untested on hardware.

void otaReport(const char* phase, int pct) { uiProgress("Updating firmware", phase, pct); }
void otaFail(const String& why) { uiProgressFail("Update failed", why); }

String otaLatestTag() {
  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!h.begin(c, "https://api.github.com/repos/" GH_REPO "/releases/latest")) return "";
  h.addHeader("User-Agent", "bedside-controller");
  h.addHeader("Accept", "application/vnd.github+json");
  if (h.GET() != 200) return "";
  JsonDocument filter;
  filter["tag_name"] = true;
  JsonDocument d;
  if (deserializeJson(d, h.getStream(), DeserializationOption::Filter(filter))) return "";
  return String(d["tag_name"] | "");
}

static bool parseVer(const char* s, int v[3]) {
  if (*s == 'v' || *s == 'V') s++;
  v[0] = v[1] = v[2] = 0;
  return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) >= 1;
}

bool otaIsNewer(const String& tag) {
  int a[3], b[3];
  if (!parseVer(tag.c_str(), a) || !parseVer(FW_VERSION, b)) return false;
  for (int i = 0; i < 3; i++) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return false;
}

// Finds the "*-ota.bin" asset of the latest release and flashes it. Reboots on success.
String otaInstallLatest() {
  String url;
  {
    WiFiClientSecure c;
    c.setInsecure();
    HTTPClient h;
    h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    otaReport("Looking up the latest release", -1);
    if (!h.begin(c, "https://api.github.com/repos/" GH_REPO "/releases/latest")) return "begin failed";
    h.addHeader("User-Agent", "bedside-controller");
    if (h.GET() != 200) return "release lookup failed";
    JsonDocument filter;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;
    JsonDocument d;
    if (deserializeJson(d, h.getStream(), DeserializationOption::Filter(filter))) return "bad release json";
    for (JsonObject a : d["assets"].as<JsonArray>()) {
      String n = a["name"] | "";
      if (n.endsWith(GH_OTA_SUFFIX)) url = String(a["browser_download_url"] | "");
    }
  }
  if (url.isEmpty()) return "no " GH_OTA_SUFFIX " asset in latest release";
  WiFiClientSecure c;
  c.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // GitHub redirects to a CDN
  httpUpdate.rebootOnUpdate(false);  // so the "Rebooting" status can be shown first
  httpUpdate.onStart([] { otaReport("Downloading and writing", 0); });
  httpUpdate.onProgress([](int cur, int total) {
    if (total <= 0) return;
    int pct = (int)((int64_t)cur * 100 / total);
    otaReport(pct >= 100 ? "Verifying" : "Downloading and writing", pct);
  });
  otaReport("Connecting to download", -1);
  t_httpUpdate_return r = httpUpdate.update(c, url);
  if (r == HTTP_UPDATE_NO_UPDATES) return "No update";
  if (r == HTTP_UPDATE_FAILED) return httpUpdate.getLastErrorString();
  otaReport("Rebooting", 100);
  delay(1200);
  ESP.restart();
  return "";
}
