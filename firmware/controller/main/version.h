#pragma once
// Single source of the firmware version. Every released binary must carry a
// unique FW_BUILD: scripts/Build-Release.ps1 refuses to reuse a number already
// recorded in build-artifacts/*/manifest.json.
#include "sdkconfig.h"
#define FW_VERSION_BASE "0.4.4"
#define FW_BUILD_BASE 40400
#if CONFIG_M2S_SINGLE_USB_RCP_OTA
#define FW_VERSION FW_VERSION_BASE "-singleusb"
#define FW_BUILD (FW_BUILD_BASE + 1)
#else
#define FW_VERSION FW_VERSION_BASE
#define FW_BUILD FW_BUILD_BASE
#endif
