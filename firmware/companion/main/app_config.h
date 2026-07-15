#pragma once
// ---- Claude Deck device config ----
// WiFi credentials are NEVER compiled in. A prior version of this file supported a local-only
// wifi_secret.h fallback (#define WIFI_SSID/WIFI_PASSWORD) for developer convenience — but a locally
// built binary with that file present silently bakes the real SSID/password into the compiled image
// as plaintext strings, and that image is exactly what gets committed to firmware/dist/ and published
// (git history, GitHub Releases, the web flasher, Homebrew). That happened. WiFi must always be
// configured on-device, via the SoftAP setup portal (first boot / STA connect-fail / Settings ->
// hold: WiFi portal) or re-entered the same way after a factory reset — never baked into a binary.
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// Bridge (the Mac running bridge.mjs) — LAN IP + port. Not a secret (a LAN address), unlike WiFi creds.
// Empty (the default) = auto-discover every bridge on the LAN via mDNS (_claudeq._tcp).
// Set this here only to force one fixed address; can also be set on the device setup screen.
#ifndef BRIDGE_HOST
#define BRIDGE_HOST    ""
#endif
#ifndef BRIDGE_PORT
#define BRIDGE_PORT    8787
#endif
#define BRIDGE_WS_URI  "ws://" BRIDGE_HOST ":8787/"

#define DEVICE_NAME    "claudeq"
#define DEVICE_FW      "2.1.5"
