// WiFi STA + WebSocket client -> Claude Deck bridge.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_websocket_client.h"
#include "mdns.h"
#include "cJSON.h"
#include "microlink.h"
#include "app.h"
#include "app_config.h"

static const char *TAG = "net";
static microlink_t *s_ml = NULL;     // Tailscale node (only when an auth key is configured)
// Each esp_websocket_client runs its OWN FreeRTOS task, so slot tasks run in parallel on the dual-core
// S3. last_sessions is the one freeable pointer read across slots (the failover replay reads a peer
// slot's buffer), so guard every free/strdup/read of it. Other slot fields are word-sized + never freed,
// so their benign cross-task races self-correct on the next message.
static SemaphoreHandle_t s_ls_mux = NULL;
static inline void ls_lock(void)   { if (s_ls_mux) xSemaphoreTake(s_ls_mux, portMAX_DELAY); }
static inline void ls_unlock(void) { if (s_ls_mux) xSemaphoreGive(s_ls_mux); }
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0

static claudeq_cfg_t g_cfg;          // active config (NVS or compile defaults)
static bool s_provisioning = false;  // true once we hand off to the setup portal
static bool s_got_ip = false;

// The deck connects to EVERY Claudeq bridge it discovers — on the LAN (mDNS) and across the tailnet.
// MAX_BRIDGES is defined in app.h (shared with ui.cpp).
typedef struct {
    esp_websocket_client_handle_t client;
    char ip[40];
    int  port;
    bool used;
    bool connected;
    uint32_t last_seen;  // tick mDNS last advertised this bridge — used to reclaim a vanished slot
    char *rx;        // per-bridge reassembly buffer (frames from different bridges interleave)
    int   rx_op;     // op-code of the in-progress message: 0x1 text, 0x2 binary
    // dedup: the same machine can be reached via LAN (mDNS) and the tailnet at different IPs. Both slots
    // stay connected; only the elected `primary` forwards to the UI, so its sessions show once.
    char  bridge_id[40];   // stable per-process id the bridge sends in 'sessions' (empty until first seen)
    bool  is_lan;          // discovered via LAN mDNS (preferred) vs the tailnet
    bool  primary;         // forward this connection's messages to the UI? (optimistic true until demoted)
    char *last_sessions;   // cached last 'sessions' JSON, replayed if this slot is promoted on a failover
} bridge_t;
static bridge_t s_br[MAX_BRIDGES];
static int s_focus_bridge = 0;       // where net_send_text/binary (macros/voice) go
static TaskHandle_t s_disc_task = NULL;   // discovery_task handle; the tailnet toggle command rides its
                                          // notification value (1 enable, 2 disable) so it's read atomically

// Push the top-bar network state (WiFi link + distinct-bridge count + tailscale), derived by scanning so
// there's no shared-counter race. WiFi icon = s_got_ip (the actual STA/IP link state) -- independent of
// whether any bridge happens to be reachable, so it stays accurate even with WiFi up but zero paired/
// reachable bridges. Bridge count = connected PRIMARY slots (a machine reachable via both LAN + tailnet
// keeps one primary, so it isn't double-counted).
static void update_conn_icon(void) {
    int bridges = 0;
    for (int i = 0; i < MAX_BRIDGES; i++) {
        if (s_br[i].used && s_br[i].connected && s_br[i].primary) bridges++;
    }
    bool ts_configured = (g_cfg.tailscale_authkey[0] != 0 && g_cfg.tailscale_enabled != 0);
    ui_set_net_status(s_got_ip, bridges, ts_configured, s_ml != NULL);
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;    // WiFi actually dropped -- reflect that immediately, independent of bridge state
        ui_set_net_status(false, 0, (g_cfg.tailscale_authkey[0] != 0 && g_cfg.tailscale_enabled != 0), s_ml != NULL);
        if (!s_provisioning) { vTaskDelay(pdMS_TO_TICKS(1000)); esp_wifi_connect(); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_got_ip = true;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        update_conn_icon();   // WiFi just came up -- refresh the icon now, don't wait for the next bridge event
    }
}

// Among connected slots sharing `id`, elect one primary (prefer a LAN path, else the lowest slot index —
// deterministic + idempotent). Demote the rest; a slot that loses a primary role it was showing has its
// chips pulled from the UI. Returns the winner slot, or -1 if no connected slot carries that id.
static int elect_primary(const char *id) {
    if (!id || !id[0]) return -1;
    int winner = -1;
    for (int i = 0; i < MAX_BRIDGES; i++) {
        bridge_t *b = &s_br[i];
        if (!b->used || !b->connected || strcmp(b->bridge_id, id) != 0) continue;
        if (winner < 0 || (b->is_lan && !s_br[winner].is_lan)) winner = i;
    }
    if (winner < 0) return -1;
    for (int i = 0; i < MAX_BRIDGES; i++) {
        bridge_t *b = &s_br[i];
        if (!b->used || strcmp(b->bridge_id, id) != 0) continue;
        bool was_primary = b->primary;
        b->primary = (i == winner);
        if (was_primary && !b->primary && b->connected) ui_bridge_gone(i);  // pull the demoted slot's chips
    }
    return winner;
}

static void ws_evt(void *args, esp_event_base_t base, int32_t id, void *data) {
    int bi = (int)(intptr_t)args;
    if (bi < 0 || bi >= MAX_BRIDGES) return;
    bridge_t *b = &s_br[bi];
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "bridge %d connected (%s:%d)", bi, b->ip, b->port);
        b->connected = true; update_conn_icon();
        pairing_on_connect(bi);                   // fresh connection -> fresh auth/pairing state for this slot
        char hello[220];
        int n = snprintf(hello, sizeof(hello),
                         "{\"type\":\"hello\",\"name\":\"%s\",\"fw\":\"%s\",\"device_id\":\"%s\",\"caps\":[\"session\",\"macros\",\"voice\",\"settings\",\"sessions\"]}",
                         DEVICE_NAME, DEVICE_FW, device_get_id());
        esp_websocket_client_send_text(b->client, hello, n, portMAX_DELAY);
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED: {                // graceful/server-initiated close tears down too
        b->connected = false; update_conn_icon();
        pairing_on_disconnect(bi);                // wipe this slot's session keys + abort any live ceremony with it
        free(b->rx); b->rx = NULL; b->rx_op = 0;  // discard any partial reassembly
        ui_bridge_gone(bi);                       // drop this bridge's sessions from the deck
        // If this was the primary path for a dual-homed bridge, promote a still-connected shadow (e.g.
        // the deck left home: LAN died, the tailnet path takes over) and replay its cached chips at once.
        char gone_id[40]; snprintf(gone_id, sizeof(gone_id), "%s", b->bridge_id);
        bool was_primary = b->primary;
        b->bridge_id[0] = 0; b->primary = false;
        ls_lock(); free(b->last_sessions); b->last_sessions = NULL; ls_unlock();
        if (was_primary && gone_id[0]) {
            int w = elect_primary(gone_id);
            char *snap = NULL;                        // copy the survivor's chips under the lock, then
            if (w >= 0 && w != bi) {                  // parse the copy — never the live buffer its own
                ls_lock();                            // task may free/replace concurrently.
                if (s_br[w].primary && s_br[w].last_sessions) snap = strdup(s_br[w].last_sessions);
                ls_unlock();
            }
            if (snap) {
                cJSON *c = cJSON_Parse(snap);
                if (c) { ui_handle_message(c, w); cJSON_Delete(c); }
                free(snap);
            }
        }
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        // reassemble by payload_offset/len; text (0x1) -> JSON, binary (0x2) -> PCM audio
        if (d->payload_len == 0 || d->op_code == 0x8 /*close*/) break;
        if (d->payload_offset == 0) {
            free(b->rx);
            b->rx = (char *)heap_caps_malloc(d->payload_len + 1, MALLOC_CAP_SPIRAM);
            b->rx_op = d->op_code;
        }
        if (b->rx && d->data_len > 0) memcpy(b->rx + d->payload_offset, d->data_ptr, d->data_len);
        if (b->rx && (d->payload_offset + d->data_len) >= d->payload_len) {
            if (b->rx_op == 0x2) {                    // binary -> encrypted audio clip envelope (see pairing.cpp)
                if (b->primary) {
                    uint8_t *pcm = NULL; size_t pcm_len = 0;
                    if (pairing_unwrap_incoming_binary(bi, (const uint8_t *)b->rx, d->payload_len, &pcm, &pcm_len)) {
                        audio_play_pcm(pcm, pcm_len);
                        free(pcm);
                    }
                }
            } else {                                  // text -> JSON, tagged with the source bridge
                b->rx[d->payload_len] = '\0';
                cJSON *root = cJSON_Parse(b->rx);
                if (root) {
                    cJSON *type = cJSON_GetObjectItem(root, "type");
                    const char *t = cJSON_IsString(type) ? type->valuestring : "";
                    if (!strcmp(t, "hello_ack")) {
                        // identity + dedup: stamp this slot's bridge_id (now known ~1 RTT after connect,
                        // in the clear, rather than waiting for the encrypted 'sessions' message) and
                        // (re-)elect which path is primary for this machine.
                        cJSON *bid = cJSON_GetObjectItem(root, "bridge_id");
                        if (cJSON_IsString(bid)) snprintf(b->bridge_id, sizeof(b->bridge_id), "%s", bid->valuestring);
                        elect_primary(b->bridge_id);
                        pairing_on_message(root, bi);   // may auto-start the per-connection auth handshake
                    } else if (!strncmp(t, "pair_", 5) || !strncmp(t, "auth_", 5)) {
                        pairing_on_message(root, bi);
                    } else if (!strcmp(t, "sec")) {
                        cJSON *inner = pairing_unwrap_incoming(root, bi);
                        if (inner) {
                            cJSON *itype = cJSON_GetObjectItem(inner, "type");
                            if (cJSON_IsString(itype) && strcmp(itype->valuestring, "sessions") == 0) {
                                cJSON *bid2 = cJSON_GetObjectItem(inner, "bridge_id");
                                if (cJSON_IsString(bid2)) snprintf(b->bridge_id, sizeof(b->bridge_id), "%s", bid2->valuestring);
                                // cache the DECRYPTED inner text (not the outer 'sec' envelope) so a later
                                // failover replay hands ui_handle_message real session data, not ciphertext.
                                char *inner_txt = cJSON_PrintUnformatted(inner);
                                if (inner_txt) { ls_lock(); free(b->last_sessions); b->last_sessions = inner_txt; ls_unlock(); }
                                elect_primary(b->bridge_id);
                            }
                            if (b->primary) ui_handle_message(inner, bi);   // shadow connections forward nothing
                            cJSON_Delete(inner);
                        }
                    }
                    // else: unauthenticated socket sent a non-handshake plaintext type -> dropped silently
                    cJSON_Delete(root);
                }
            }
            free(b->rx); b->rx = NULL;
        }
        break;
    default: break;
    }
}

// Extract an IPv4 + port from an mDNS result (resolving the hostname if no inline A record).
static bool result_ipv4(mdns_result_t *r, char *ip, size_t iplen, int *port) {
    for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            snprintf(ip, iplen, IPSTR, IP2STR(&a->addr.u_addr.ip4));
            if (r->port) *port = r->port;
            return true;
        }
    }
    if (r->hostname) {
        esp_ip4_addr_t v4;
        if (mdns_query_a(r->hostname, 2000, &v4) == ESP_OK) {
            snprintf(ip, iplen, IPSTR, IP2STR(&v4));
            if (r->port) *port = r->port;
            return true;
        }
    }
    return false;
}

// Open a WS to ip:port if we don't already have a slot for it. is_lan = discovered via LAN mDNS (the
// preferred path) vs the tailnet; used to elect the primary connection when a machine is reachable both ways.
static void ensure_bridge(const char *ip, int port, bool is_lan) {
    for (int i = 0; i < MAX_BRIDGES; i++)
        if (s_br[i].used && s_br[i].port == port && strcmp(s_br[i].ip, ip) == 0) { s_br[i].last_seen = xTaskGetTickCount(); return; }  // already known
    int slot = -1;
    for (int i = 0; i < MAX_BRIDGES; i++) if (!s_br[i].used) { slot = i; break; }
    if (slot < 0) return;  // all slots in use
    bridge_t *b = &s_br[slot];
    b->used = true; b->connected = false; b->port = port; b->last_seen = xTaskGetTickCount();
    b->is_lan = is_lan; b->primary = true; b->bridge_id[0] = 0; b->last_sessions = NULL;
    snprintf(b->ip, sizeof(b->ip), "%s", ip);
    char uri[80]; snprintf(uri, sizeof(uri), "ws://%s:%d/", ip, port);
    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.reconnect_timeout_ms = 4000;
    cfg.network_timeout_ms = 5000;
    cfg.buffer_size = 2048;
    // Default task_stack is only 4KB (esp_websocket_client's WEBSOCKET_TASK_STACK) -- this event
    // callback now also does the pairing/auth envelope crypto (a stack-resident mbedtls_gcm_context
    // carries ~300+ bytes of GHASH tables alone) before handing off to ui_handle_message's cJSON/LVGL
    // work, all on this one task. That combination plausibly overflows the tight default; give it the
    // same headroom already used elsewhere in this firmware for similarly-loaded tasks (discovery_task).
    cfg.task_stack = 8192;
    b->client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(b->client, WEBSOCKET_EVENT_ANY, ws_evt, (void *)(intptr_t)slot);
    esp_websocket_client_start(b->client);
    ESP_LOGI(TAG, "bridge %d -> %s", slot, uri);
}

// Bring the deck onto the user's tailnet when an auth key is configured. microlink rides the existing
// WiFi STA connection (it does NOT manage WiFi). No-op when no key is set, so the LAN/mDNS path is
// unchanged for users who don't use Tailscale.
static void tailnet_start(void) {
    if (s_ml || !g_cfg.tailscale_authkey[0] || !g_cfg.tailscale_enabled) return;
    microlink_config_t mc = {};
    mc.auth_key     = g_cfg.tailscale_authkey;
    mc.device_name  = "claudeq";
    mc.enable_derp  = true;
    mc.enable_stun  = true;
    mc.enable_disco = true;
    mc.max_peers    = 16;
    s_ml = microlink_init(&mc);
    if (s_ml && microlink_start(s_ml) == ESP_OK) {
        ESP_LOGI(TAG, "tailnet: joining as 'claudeq'");
    } else {
        ESP_LOGW(TAG, "tailnet: microlink init/start failed");
        s_ml = NULL;
    }
}

// Leave the tailnet: stop microlink and drop every tailnet (non-LAN) bridge slot. Runs ONLY inside
// discovery_task (the sole owner of s_ml + the tailnet slots), so there is no new cross-task race.
static void tailnet_stop(void) {
    if (!s_ml) return;
    for (int i = 0; i < MAX_BRIDGES; i++) {
        bridge_t *b = &s_br[i];
        if (!b->used || b->is_lan) continue;          // keep LAN slots; only tear down tailnet paths
        char gone_id[40]; snprintf(gone_id, sizeof(gone_id), "%s", b->bridge_id);
        bool was_primary = b->primary;
        if (b->client) { esp_websocket_client_stop(b->client); esp_websocket_client_destroy(b->client); }
        free(b->rx); b->rx = NULL; b->rx_op = 0;
        ls_lock(); free(b->last_sessions); b->last_sessions = NULL; ls_unlock();
        b->client = NULL; b->used = false; b->connected = false;
        b->bridge_id[0] = 0; b->primary = false;
        ui_bridge_gone(i);                            // pull this slot's chips from the deck
        if (was_primary && gone_id[0]) {              // dual-homed & the tailnet path was primary -> promote LAN
            int w = elect_primary(gone_id);
            char *snap = NULL;
            if (w >= 0 && w != i) { ls_lock(); if (s_br[w].primary && s_br[w].last_sessions) snap = strdup(s_br[w].last_sessions); ls_unlock(); }
            if (snap) { cJSON *c = cJSON_Parse(snap); if (c) { ui_handle_message(c, w); cJSON_Delete(c); } free(snap); }
        }
    }
    microlink_stop(s_ml);
    microlink_destroy(s_ml);
    s_ml = NULL;
    update_conn_icon();
    ESP_LOGI(TAG, "tailnet: left");
}

// Discover Claudeq bridges reachable over the tailnet: any online peer running a bridge on :8787.
// ensure_bridge() de-dupes by ip:port, so a bridge seen on both the LAN (mDNS) and the tailnet connects
// once. (Filtering peers by tag/hostname is a refinement — see docs/TAILNET.md.)
static void tailnet_discover_bridges(void) {
    if (!s_ml) return;
    int n = microlink_get_peer_count(s_ml);
    for (int i = 0; i < n; i++) {
        microlink_peer_info_t info;
        if (microlink_get_peer_info(s_ml, i, &info) != ESP_OK || !info.online) continue;
        char ip[16];
        microlink_ip_to_str(info.vpn_ip, ip);
        ensure_bridge(ip, 8787, false);   // tailnet path: shadow when the same machine is also on the LAN
    }
}

// UI "Disconnect" on a paired bridge: tear the slot down explicitly (same shape as the stale-slot-reclaim
// block below) rather than relying on esp_websocket_client_stop() to fire WEBSOCKET_EVENT_DISCONNECTED on
// its own. Runs ONLY inside discovery_task, so it's safe to block on esp_websocket_client_stop() here —
// this is exactly why net_disconnect_bridge() hands off via a notification instead of doing this directly
// from the (LVGL-lock-holding) UI task.
static void disconnect_bridge_slot(int idx) {
    if (idx < 0 || idx >= MAX_BRIDGES || !s_br[idx].used) return;
    bridge_t *b = &s_br[idx];
    ESP_LOGI(TAG, "disconnecting bridge slot %d (%s:%d) on user request", idx, b->ip, b->port);
    pairing_on_disconnect(idx);
    if (b->client) { esp_websocket_client_stop(b->client); esp_websocket_client_destroy(b->client); }
    free(b->rx); b->rx = NULL; b->rx_op = 0;
    ls_lock(); free(b->last_sessions); b->last_sessions = NULL; ls_unlock();
    b->client = NULL; b->used = false; b->connected = false;
    b->bridge_id[0] = 0; b->primary = false;
    ui_bridge_gone(idx);
    update_conn_icon();
}

static void discovery_task(void *arg) {
    s_disc_task = xTaskGetCurrentTaskHandle();   // Settings toggles notify this task to apply tailnet changes
    // give STA up to 20s to get an IP; if it can't, the saved network is wrong/out of range -> portal
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, false, true, pdMS_TO_TICKS(20000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "no IP within 20s -> entering setup portal");
        s_provisioning = true;
        provision_start();
        s_disc_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    // Sync the wall clock via SNTP. HTTPS OTA (TLS to GitHub) rejects certs as "not yet valid" if the
    // clock is at 1970, surfacing as ESP_ERR_HTTP_CONNECT. WireGuard/Tailscale doesn't need real time,
    // so this gap only bites TLS. Non-blocking: time lands within a few seconds of getting an IP.
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);

    mdns_init();
    tailnet_start();                       // join the tailnet if an auth key is configured
    for (;;) {
        if (g_cfg.bridge[0]) ensure_bridge(g_cfg.bridge, g_cfg.port > 0 ? g_cfg.port : 8787, true);  // explicit override
        mdns_result_t *res = NULL;
        if (mdns_query_ptr("_claudeq", "_tcp", 3000, MAX_BRIDGES * 2, &res) == ESP_OK) {
            for (mdns_result_t *r = res; r; r = r->next) {
                char ip[40]; int port = g_cfg.port > 0 ? g_cfg.port : 8787;
                if (result_ipv4(r, ip, sizeof(ip), &port)) ensure_bridge(ip, port, true);
            }
            mdns_query_results_free(res);
        }
        if (s_ml) tailnet_discover_bridges();  // also pick up bridges reachable over the tailnet
        // reclaim slots whose bridge stopped advertising and isn't connected (vanished / changed IP)
        uint32_t now = xTaskGetTickCount();
        for (int i = 0; i < MAX_BRIDGES; i++) {
            bridge_t *b = &s_br[i];
            if (b->used && !b->connected && (now - b->last_seen) > pdMS_TO_TICKS(30000)) {
                ESP_LOGW(TAG, "reclaiming stale bridge slot %d (%s:%d)", i, b->ip, b->port);
                if (b->client) { esp_websocket_client_stop(b->client); esp_websocket_client_destroy(b->client); }
                free(b->rx);
                ls_lock(); free(b->last_sessions); b->last_sessions = NULL; ls_unlock();
                b->client = NULL; b->rx = NULL; b->used = false; b->connected = false;
            }
        }
        // re-discover every ~8s, but wake early on a Settings toggle or a UI "Disconnect" tap. The command
        // rides the notification value (atomically read + cleared here), so nothing posted from the LVGL
        // task is ever lost. Disconnect-slot commands are encoded as 100+slot (slot < MAX_BRIDGES <= 8,
        // so this never collides with the tailnet on/off values 1/2).
        uint32_t cmd = 0;
        xTaskNotifyWait(0, 0xFFFFFFFF, &cmd, pdMS_TO_TICKS(8000));
        if (cmd == 1) tailnet_start();
        else if (cmd == 2) tailnet_stop();
        else if (cmd >= 100 && cmd < 100 + MAX_BRIDGES) disconnect_bridge_slot((int)(cmd - 100));
        update_conn_icon();   // keep the top-bar tailscale badge / bridge count fresh even without a WS event
    }
}

extern "C" void net_set_focus_bridge(int bridge) { if (bridge >= 0 && bridge < MAX_BRIDGES) s_focus_bridge = bridge; }

// --- Settings toggles ---
// WiFi is the deck's only uplink and the disconnect handler auto-reconnects, so a live stop fights itself;
// persist the flag and reboot (net_start honours it on boot). The UI keeps running so re-enabling is possible.
extern "C" void net_wifi_set_enabled(bool on) {
    cfg_set_wifi_enabled(on ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

// Tailscale can start/stop live: hand the command to discovery_task (the sole owner of s_ml + tailnet slots)
// and wake it so the change applies within a couple of seconds — no reboot.
extern "C" void net_tailnet_set_enabled(bool on) {
    cfg_set_tailscale_enabled(on ? 1 : 0);
    g_cfg.tailscale_enabled = on ? 1 : 0;
    // Command rides the notification value (overwrite = last toggle wins); discovery_task reads it atomically.
    if (s_disc_task) xTaskNotify(s_disc_task, on ? 1 : 2, eSetValueWithOverwrite);
}

extern "C" void net_get_flags(bool *wifi_en, bool *ts_en, bool *ts_has_key) {
    if (wifi_en)    *wifi_en    = g_cfg.wifi_enabled != 0;
    if (ts_en)      *ts_en      = g_cfg.tailscale_enabled != 0;
    if (ts_has_key) *ts_has_key = g_cfg.tailscale_authkey[0] != 0;
}

extern "C" void net_get_prefs(bool *auto_standby, bool *sound_en) {
    if (auto_standby) *auto_standby = g_cfg.auto_standby != 0;
    if (sound_en)     *sound_en     = g_cfg.sound_enabled != 0;
}

// UNWRAPPED send -- used only by pairing.cpp for the handshake messages that must travel in the clear
// (hello/pair_*/auth_*), and internally above for `hello` itself.
extern "C" void net_send_raw(int bridge, const char *json) {
    if (bridge < 0 || bridge >= MAX_BRIDGES) return;
    bridge_t *b = &s_br[bridge];
    if (b->client && b->connected) esp_websocket_client_send_text(b->client, json, strlen(json), pdMS_TO_TICKS(1000));
}

// Auth-gated + AES-256-GCM-encrypted send: every real app message (focus/answer/macro/voice/...) goes
// through here. An unauthenticated bridge slot carries no app traffic in either direction -- the message
// is simply dropped, not queued, matching the receive side's equivalent gate in ws_evt.
extern "C" void net_send_to(int bridge, const char *json) {
    if (!pairing_is_authenticated(bridge)) return;
    char *wrapped = NULL; size_t wrapped_len = 0;
    if (pairing_wrap_outgoing(bridge, json, &wrapped, &wrapped_len)) {
        net_send_raw(bridge, wrapped);
        free(wrapped);
    }
}

extern "C" void net_send_text(const char *json) { net_send_to(s_focus_bridge, json); }

extern "C" void net_send_binary(const void *data, size_t len) {
    if (!pairing_is_authenticated(s_focus_bridge)) return;
    bridge_t *b = &s_br[s_focus_bridge];
    if (!b->client || !b->connected) return;
    uint8_t *wrapped = NULL; size_t wrapped_len = 0;
    if (pairing_wrap_outgoing_binary(s_focus_bridge, (const uint8_t *)data, len, &wrapped, &wrapped_len)) {
        esp_websocket_client_send_bin(b->client, (const char *)wrapped, wrapped_len, pdMS_TO_TICKS(200));
        free(wrapped);
    }
}

// For the "Paired Bridges" / "Pair new bridge" Settings screens (ui.cpp), which need to enumerate live
// bridge slots but can't see s_br[] directly (private to this file).
extern "C" int net_bridge_count(void) { return MAX_BRIDGES; }

extern "C" bool net_bridge_info(int idx, bool *used, bool *connected, char *bridge_id, size_t bridge_id_len, char *host, size_t host_len) {
    if (idx < 0 || idx >= MAX_BRIDGES) return false;
    bridge_t *b = &s_br[idx];
    if (used) *used = b->used;
    if (connected) *connected = b->connected;
    if (bridge_id && bridge_id_len) snprintf(bridge_id, bridge_id_len, "%s", b->bridge_id);
    if (host && host_len) snprintf(host, host_len, "%s", b->ip);   // no separate machine-name field on this slot; IP is always available
    return true;
}

// Hands off to discovery_task (see disconnect_bridge_slot) rather than tearing the slot down here directly —
// esp_websocket_client_stop() can block, and this is called from the LVGL-lock-holding UI task.
extern "C" bool net_disconnect_bridge(int idx) {
    if (idx < 0 || idx >= MAX_BRIDGES || !s_br[idx].used) return false;
    if (s_disc_task) xTaskNotify(s_disc_task, 100 + idx, eSetValueWithOverwrite);
    return true;
}

// Load saved config into g_cfg EARLY — before ui_init() builds the Settings page and reads it via
// net_get_flags(). (app_main runs ui_init before net_start, so without this the switches show zeroed
// state: WiFi "off" though it's on, Tailscale "disabled".) net_start re-reads it for the usable/force call.
extern "C" void net_preload_config(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    cfg_load(&g_cfg);
}

extern "C" void net_start(void) {
    s_wifi_eg = xEventGroupCreate();
    s_ls_mux = xSemaphoreCreateMutex();   // guards bridge_t.last_sessions; created before any ws task starts
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL);

    bool usable = cfg_load(&g_cfg);
    if (!usable) {                          // no saved/compiled creds, or setup was forced -> portal
        ESP_LOGW(TAG, "no usable WiFi config -> entering setup portal");
        s_provisioning = true;
        provision_start();
        return;
    }
    if (!g_cfg.wifi_enabled) {               // user turned WiFi off in Settings -> stay offline (UI still runs)
        ESP_LOGW(TAG, "WiFi disabled in Settings; staying offline (re-enable on the deck)");
        ui_set_net_status(false, 0, (g_cfg.tailscale_authkey[0] != 0 && g_cfg.tailscale_enabled != 0), s_ml != NULL);
        return;
    }
    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, g_cfg.ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, g_cfg.pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = g_cfg.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem power-save: the radio naps between DTIM beacons but stays associated, so the WebSocket stays
    // up and incoming frames still wake us. Cheap standing win; the screen backlight is the bigger draw.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    xTaskCreate(discovery_task, "net", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "wifi starting, ssid=%s; discovering bridges via mDNS", g_cfg.ssid);
}
