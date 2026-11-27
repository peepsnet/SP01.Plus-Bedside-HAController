#include "ota.h"
#include "version.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// NOTE: TLS certificates are NOT verified (setInsecure) to avoid shipping a CA bundle in milestone two.
// Anyone able to MITM your network could serve a malicious image. Firmware images are not signed either.
// Works for public repositories only. All of this is untested on hardware.

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

String otaInstallLatest() {
  WiFiClientSecure c;
  c.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // GitHub redirects to a CDN
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r =
      httpUpdate.update(c, "https://github.com/" GH_REPO "/releases/latest/download/" GH_FIRMWARE_ASSET);
  if (r == HTTP_UPDATE_NO_UPDATES) return "No update";
  if (r == HTTP_UPDATE_FAILED) return httpUpdate.getLastErrorString();
  return "";
}
