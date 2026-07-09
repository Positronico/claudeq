#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

// --- LVGL thread-safe access (implemented in main.cpp) ---
bool ui_lock(int timeout_ms);
void ui_unlock(void);

// --- UI (ui.cpp) ---
void ui_init(void);                      // build the UI once; caller already holds the LVGL lock
void ui_handle_message(cJSON *root, int bridge);  // handle a message from bridge index `bridge`
void ui_bridge_gone(int bridge);         // a bridge disconnected -> drop its sessions
void ui_set_net_status(bool online, int bridges, bool ts_configured, bool ts_up); // top-bar net icons; takes the lock
bool ui_set_battery(int pct, bool charging, bool present); // update status-bar battery gauge; false if UI not ready
void ui_show_setup(const char *ap_ssid, const char *ap_ip); // overlay shown while provisioning
void ui_show_lock_notice(bool show);     // brief "Locked" flash before the screen blanks (BOOT long-press)

// --- Provisioning / config (provision.cpp) ---
typedef struct {
    char ssid[33]; char pass[65]; char bridge[64]; int port; char tailscale_authkey[128];
    uint8_t wifi_enabled;       // 0 = user disabled WiFi in Settings (deck stays offline); default 1
    uint8_t tailscale_enabled;  // 0 = user disabled Tailscale in Settings; default 1
    uint8_t auto_standby;       // 0 = user disabled auto screen-off in Settings; default 1
    uint8_t sound_enabled;      // 0 = user muted alert/reply sounds in Settings; default 1
} claudeq_cfg_t;
bool cfg_load(claudeq_cfg_t *out);       // fill out (NVS or compile defaults); true if a usable SSID exists
void cfg_save(const char *ssid, const char *pass, const char *bridge, int port, const char *authkey);
void cfg_set_wifi_enabled(uint8_t v);        // persist the WiFi on/off flag only
void cfg_set_tailscale_enabled(uint8_t v);   // persist the Tailscale on/off flag only
void cfg_set_auto_standby(uint8_t v);        // persist the auto screen-off flag only
void cfg_set_sound_enabled(uint8_t v);       // persist the sound on/off flag only
void cfg_clear(void);                    // wipe saved config
void provision_start(void);              // bring up the SoftAP captive portal
void net_enter_setup(void);              // flag setup + reboot into the portal (called from the UI)

// --- Net (net.cpp) ---
#define MAX_BRIDGES 8                    // max simultaneous bridge connections (LAN mDNS + tailnet); shared by net.cpp + ui.cpp
void net_preload_config(void);           // load saved config into g_cfg before ui_init reads it (Settings switches)
void net_start(void);                    // start wifi + multi-bridge discovery/websockets
void net_send_to(int bridge, const char *json);     // send text to a specific bridge connection
void net_send_text(const char *json);    // send text to the currently focused bridge
void net_send_binary(const void *data, size_t len); // send a binary frame (mic PCM) to the focused bridge
void net_set_focus_bridge(int bridge);   // route net_send_text/binary to this bridge (UI sets on focus change)
void net_wifi_set_enabled(bool on);      // persist WiFi flag + reboot to apply (Settings toggle)
void net_tailnet_set_enabled(bool on);   // join/leave the tailnet live, no reboot (Settings toggle)
void net_get_flags(bool *wifi_en, bool *ts_en, bool *ts_has_key); // current toggle state for the Settings UI
void net_get_prefs(bool *auto_standby, bool *sound_en);          // standby/sound toggle state for the Settings UI

// --- Battery (battery.cpp) ---
void battery_init(void);                  // set up ADC1 + calibration for the BAT_ADC divider (GPIO4)
bool battery_read(float *volts, int *pct, bool *charging); // false until ADC ready; averages a few samples

// --- Audio (audio.cpp) ---
void audio_init(void);                   // ES8311 speaker setup
void audio_play_alert(const char *sound);// synth fallback: "chirp" | "soft" | "error" | NULL
void audio_play_pcm(const void *data, size_t bytes); // play a streamed mono/16-bit/16k PCM clip
void audio_record_start(void);           // begin streaming mic PCM to the bridge
void audio_record_stop(void);            // stop mic capture
void audio_set_muted(bool muted);        // true = drop all alert/reply playback (Settings "Sounds" off)

// --- Board IO (main.cpp) ---
void app_io_set_pa(bool on);             // enable/disable speaker amp (TCA9554 bit7)
void app_set_rotation(bool landscape);   // portrait <-> landscape (for the full-text reader)

// --- Power / standby (main.cpp) ---
// Screen off = backlight + panel asleep; WiFi/WS stay up so the deck remains pingable and wakes on events.
void app_note_user_activity(void);       // touch/interaction: reset the idle timer, wake if asleep
void app_wake_for_event(void);           // a bridge event worth attention (ask/alert/reply): wake + reset timer
void app_set_auto_standby(bool enabled); // enable/disable the idle screen-off timer (Settings toggle, live)
bool ui_has_pending_ask(void);           // (ui.cpp) true while a question overlay is up -> don't auto-sleep

#ifdef __cplusplus
}
#endif
