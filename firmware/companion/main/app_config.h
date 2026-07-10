#pragma once
// ---- Claude Deck device config ----
// WiFi credentials (2.4 GHz only — the ESP32-S3 has no 5 GHz radio).
// Put real creds in a local-only "wifi_secret.h" next to this file; it just needs
//   #define WIFI_SSID "..."  and  #define WIFI_PASSWORD "..."
// That file is picked up automatically and never has to be shared in chat.
#if defined(__has_include)
#  if __has_include("wifi_secret.h")
#    include "wifi_secret.h"
#  endif
#endif
#ifndef WIFI_SSID
#define WIFI_SSID      "YOUR_WIFI_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"
#endif

// Bridge (the Mac running bridge.mjs) — LAN IP + port.
// Empty (the default) = auto-discover every bridge on the LAN via mDNS (_claudeq._tcp).
// Set this (here, in wifi_secret.h, or on the device setup screen) only to force one fixed address.
#ifndef BRIDGE_HOST
#define BRIDGE_HOST    ""
#endif
#ifndef BRIDGE_PORT
#define BRIDGE_PORT    8787
#endif
#define BRIDGE_WS_URI  "ws://" BRIDGE_HOST ":8787/"

#define DEVICE_NAME    "claudeq"
#define DEVICE_FW      "2.0.0"
