#pragma once
#include <Arduino.h>

// Full-screen camera view. Shows MJPEG (multipart/x-mixed-replace) streams, and falls back to polling single JPEG
// snapshots. Source is either a Home Assistant camera entity ("camera.front_door", fetched through HA's camera proxy
// with the long-lived token) or a direct http(s) URL of an IP camera (user:pass@host in the URL is sent as basic auth).
// RTSP and H.264 are not supported. Untested on hardware.
// fullBrightness: force 100 % backlight while the view is open; the user's brightness comes back on close.
void cameraOpen(const String& label, const String& entityId, const String& url, bool fullBrightness);
void cameraClose();
bool cameraIsOpen();
void cameraTick();  // call from the main loop before lv_timer_handler(); swaps in newly decoded frames
