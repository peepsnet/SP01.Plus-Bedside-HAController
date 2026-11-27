#pragma once
// FW_VERSION comes from the /VERSION file via scripts/version.py (single source of truth).
#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif
// GitHub repo used by the on-device "check for update" feature.
#define GH_REPO "peepsnet/SP01.Plus-Bedside-HAController"
// Release asset that on-device updates download (the app-only image).
#define GH_OTA_SUFFIX "-ota.bin"
