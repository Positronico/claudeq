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
void ui_set_net_status(bool wifi_up, int bridges, bool ts_configured, bool ts_up); // top-bar net icons; takes the lock. wifi_up = actual STA/IP link state (independent of bridge/pairing reachability)
bool ui_set_battery(int pct, bool charging, bool present); // update status-bar battery gauge; false if UI not ready
void ui_show_setup(const char *ap_ssid, const char *ap_ip); // overlay shown while provisioning
void ui_show_lock_notice(bool show);     // brief "Locked" flash before the screen blanks (BOOT long-press)
void ui_show_ota(void);                  // open the OTA overlay (Check for update -> download -> reboot)

// --- OTA firmware update (ota.cpp) ---
typedef enum {
    OTA_IDLE = 0,     // nothing happening
    OTA_CHECKING,     // fetching ota.json
    OTA_UPTODATE,     // checked: already newest
    OTA_AVAILABLE,    // checked: a newer version exists (ota_get_avail_version())
    OTA_DOWNLOADING,  // streaming + writing the image (ota_get_pct())
    OTA_REBOOTING,    // image written + boot partition set; about to restart
    OTA_ERROR,        // failed (ota_get_error()); current image untouched
} ota_state_t;
void ota_check_async(void);              // fetch ota.json, compare vs DEVICE_FW; updates ota_get_state()
void ota_start(void);                    // download + flash the available image, then reboot
ota_state_t ota_get_state(void);         // current OTA state (polled by the UI overlay)
int  ota_get_pct(void);                  // 0..100 download progress while OTA_DOWNLOADING
const char *ota_get_avail_version(void); // version from ota.json (valid when OTA_AVAILABLE)
const char *ota_get_error(void);         // short message when OTA_ERROR
bool ota_in_progress(void);              // true while checking/downloading -> suppress auto-standby
void ota_confirm_valid(void);            // cancel pending rollback once the running image is healthy

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

// --- Trust store / device identity (trust.cpp) ---
// Bounded paired-bridge table, stored as a single blob in the "claudeq" NVS namespace (cfg_clear() wipes
// it too, for free). See docs/PROTOCOL.md's "Pairing" section.
#define MAX_TRUSTED_BRIDGES 8
typedef struct {
    bool     used;
    char     bridge_id[40];    // matches bridge_t.bridge_id's size in net.cpp
    char     label[24];        // display label (the bridge's host name at pairing time)
    uint8_t  psk[32];          // persistent PSK = HMAC-SHA256(pairing shared secret, "claudeq-psk-v1")
    uint32_t paired_at;        // uptime-seconds at pairing time; display/audit only, no RTC battery
} trust_bridge_t;
const char *device_get_id(void);                            // this unit's persistent device_id (generated once, first boot)
int  trust_find(const char *bridge_id);                       // -1 if untrusted, else index
bool trust_get(int idx, trust_bridge_t *out);
int  trust_count(void);
bool trust_add(const char *bridge_id, const char *label, const uint8_t psk[32]);
void trust_forget(const char *bridge_id);
void trust_forget_by_index(int idx);

// --- Pairing + per-connection auth + envelope crypto (pairing.cpp) ---
// Live numeric-comparison (SAS) pairing ceremony over the existing WebSocket, then a PSK + fresh-ephemeral-
// ECDH auth handshake on every reconnect, then AES-256-GCM envelope encryption for every app message.
typedef enum { PAIR_IDLE, PAIR_WAIT_RESPONSE, PAIR_SHOW_SAS, PAIR_WAIT_PEER, PAIR_DONE, PAIR_FAILED } pairing_state_t;
void pairing_init(void);                                     // allocate PSRAM-backed connection state; call once, before net_start()
void pairing_start_as_device(int bridge_slot);              // UI: user tapped "Pair" on a discovered-but-untrusted bridge
void pairing_confirm(void);                                 // UI: Confirm tapped on the SAS screen
void pairing_reject(void);                                  // UI: Reject tapped
void pairing_dismiss(void);                                 // UI: acknowledge a PAIR_DONE/PAIR_FAILED toast -> back to idle
pairing_state_t pairing_get_state(void);                     // also lazily applies the ceremony timeout
const char *pairing_get_code(void);                          // valid once state has reached PAIR_SHOW_SAS
int  pairing_get_bridge(void);                                // bridge slot the current ceremony is with, else -1
void pairing_on_connect(int bridge_slot);                    // fresh WS connection on this slot -> reset auth state
void pairing_on_disconnect(int bridge_slot);                 // slot dropped -> wipe session keys, abort any ceremony with it
void pairing_on_message(cJSON *root, int bridge_slot);        // net.cpp forwards hello_ack/auth_*/pair_* here
bool pairing_is_authenticated(int bridge_slot);
bool pairing_wrap_outgoing(int bridge_slot, const char *json, char **out_json, size_t *out_len);              // heap-allocates *out_json
bool pairing_wrap_outgoing_binary(int bridge_slot, const uint8_t *data, size_t len, uint8_t **out, size_t *out_len);
cJSON *pairing_unwrap_incoming(cJSON *sec_root, int bridge_slot);                    // NULL on failure; caller cJSON_Delete()s the result
bool pairing_unwrap_incoming_binary(int bridge_slot, const uint8_t *data, size_t len, uint8_t **out, size_t *out_len);

// --- Net (net.cpp) ---
#define MAX_BRIDGES 8                    // max simultaneous bridge connections (LAN mDNS + tailnet); shared by net.cpp + ui.cpp
void net_preload_config(void);           // load saved config into g_cfg before ui_init reads it (Settings switches)
void net_start(void);                    // start wifi + multi-bridge discovery/websockets
void net_send_to(int bridge, const char *json);     // send text to a specific bridge connection (auth-gated + encrypted)
void net_send_text(const char *json);    // send text to the currently focused bridge (auth-gated + encrypted)
void net_send_binary(const void *data, size_t len); // send a binary frame (mic PCM) to the focused bridge (auth-gated + encrypted)
void net_send_raw(int bridge, const char *json);    // UNWRAPPED send -- pairing/auth handshake messages only (pairing.cpp)
void net_set_focus_bridge(int bridge);   // route net_send_text/binary to this bridge (UI sets on focus change)
void net_wifi_set_enabled(bool on);      // persist WiFi flag + reboot to apply (Settings toggle)
void net_tailnet_set_enabled(bool on);   // join/leave the tailnet live, no reboot (Settings toggle)
void net_get_flags(bool *wifi_en, bool *ts_en, bool *ts_has_key); // current toggle state for the Settings UI
void net_get_prefs(bool *auto_standby, bool *sound_en);          // standby/sound toggle state for the Settings UI
int  net_bridge_count(void);                                     // total known bridge slots (used + unused loop bound for the UI)
bool net_bridge_info(int idx, bool *used, bool *connected, char *bridge_id, size_t bridge_id_len, char *host, size_t host_len);
bool net_disconnect_bridge(int idx);      // UI "Disconnect": tear down this slot's live connection (trust untouched, reconnects later)

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
