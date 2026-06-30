#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

// --- LVGL thread-safe access (implemented in main.cpp) ---
bool ui_lock(int timeout_ms);
void ui_unlock(void);

// --- UI (ui.cpp) ---
void ui_init(void);                      // build the UI once; caller already holds the LVGL lock
void ui_handle_message(cJSON *root, int bridge);  // handle a message from bridge index `bridge`
void ui_bridge_gone(int bridge);         // a bridge disconnected -> drop its sessions
void ui_set_connection(bool connected);  // update WiFi/WS indicator; takes the lock internally
void ui_show_setup(const char *ap_ssid, const char *ap_ip); // overlay shown while provisioning

// --- Provisioning / config (provision.cpp) ---
typedef struct { char ssid[33]; char pass[65]; char bridge[64]; int port; char tailscale_authkey[128]; } claudeq_cfg_t;
bool cfg_load(claudeq_cfg_t *out);       // fill out (NVS or compile defaults); true if a usable SSID exists
void cfg_save(const char *ssid, const char *pass, const char *bridge, int port, const char *authkey);
void cfg_clear(void);                    // wipe saved config
void provision_start(void);              // bring up the SoftAP captive portal
void net_enter_setup(void);              // flag setup + reboot into the portal (called from the UI)

// --- Net (net.cpp) ---
#define MAX_BRIDGES 8                    // max simultaneous bridge connections (LAN mDNS + tailnet); shared by net.cpp + ui.cpp
void net_start(void);                    // start wifi + multi-bridge discovery/websockets
void net_send_to(int bridge, const char *json);     // send text to a specific bridge connection
void net_send_text(const char *json);    // send text to the currently focused bridge
void net_send_binary(const void *data, size_t len); // send a binary frame (mic PCM) to the focused bridge
void net_set_focus_bridge(int bridge);   // route net_send_text/binary to this bridge (UI sets on focus change)

// --- Audio (audio.cpp) ---
void audio_init(void);                   // ES8311 speaker setup
void audio_play_alert(const char *sound);// synth fallback: "chirp" | "soft" | "error" | NULL
void audio_play_pcm(const void *data, size_t bytes); // play a streamed mono/16-bit/16k PCM clip
void audio_record_start(void);           // begin streaming mic PCM to the bridge
void audio_record_stop(void);            // stop mic capture

// --- Board IO (main.cpp) ---
void app_io_set_pa(bool on);             // enable/disable speaker amp (TCA9554 bit7)

#ifdef __cplusplus
}
#endif
