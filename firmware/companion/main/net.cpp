// WiFi STA + WebSocket client -> Claude Deck bridge.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_websocket_client.h"
#include "mdns.h"
#include "cJSON.h"
#include "app.h"
#include "app_config.h"

static const char *TAG = "net";
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0

static claudeq_cfg_t g_cfg;          // active config (NVS or compile defaults)
static bool s_provisioning = false;  // true once we hand off to the setup portal
static bool s_got_ip = false;

// The deck connects to EVERY Claudeq bridge it discovers on the LAN — one slot each.
#define MAX_BRIDGES 4
typedef struct {
    esp_websocket_client_handle_t client;
    char ip[40];
    int  port;
    bool used;
    bool connected;
    uint32_t last_seen;  // tick mDNS last advertised this bridge — used to reclaim a vanished slot
    char *rx;        // per-bridge reassembly buffer (frames from different bridges interleave)
    int   rx_op;     // op-code of the in-progress message: 0x1 text, 0x2 binary
} bridge_t;
static bridge_t s_br[MAX_BRIDGES];
static int s_focus_bridge = 0;       // where net_send_text/binary (macros/voice) go

// WiFi icon = "at least one bridge connected", derived by scanning so there's no shared-counter race.
static void update_conn_icon(void) {
    bool any = false;
    for (int i = 0; i < MAX_BRIDGES; i++) if (s_br[i].used && s_br[i].connected) { any = true; break; }
    ui_set_connection(any);
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ui_set_connection(false);
        if (!s_provisioning) { vTaskDelay(pdMS_TO_TICKS(1000)); esp_wifi_connect(); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_got_ip = true;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
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
        char hello[160];
        int n = snprintf(hello, sizeof(hello),
                         "{\"type\":\"hello\",\"name\":\"%s\",\"fw\":\"%s\",\"caps\":[\"decide\",\"macros\",\"voice\",\"hud\",\"sessions\"]}",
                         DEVICE_NAME, DEVICE_FW);
        esp_websocket_client_send_text(b->client, hello, n, portMAX_DELAY);
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:                  // graceful/server-initiated close tears down too
        b->connected = false; update_conn_icon();
        free(b->rx); b->rx = NULL; b->rx_op = 0;  // discard any partial reassembly
        ui_bridge_gone(bi);                       // drop this bridge's sessions from the deck
        break;
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
            if (b->rx_op == 0x2) {                    // binary -> audio clip
                audio_play_pcm(b->rx, d->payload_len);
            } else {                                  // text -> JSON, tagged with the source bridge
                b->rx[d->payload_len] = '\0';
                cJSON *root = cJSON_Parse(b->rx);
                if (root) { ui_handle_message(root, bi); cJSON_Delete(root); }
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

// Open a WS to ip:port if we don't already have a slot for it.
static void ensure_bridge(const char *ip, int port) {
    for (int i = 0; i < MAX_BRIDGES; i++)
        if (s_br[i].used && s_br[i].port == port && strcmp(s_br[i].ip, ip) == 0) { s_br[i].last_seen = xTaskGetTickCount(); return; }  // already known
    int slot = -1;
    for (int i = 0; i < MAX_BRIDGES; i++) if (!s_br[i].used) { slot = i; break; }
    if (slot < 0) return;  // all slots in use
    bridge_t *b = &s_br[slot];
    b->used = true; b->connected = false; b->port = port; b->last_seen = xTaskGetTickCount();
    snprintf(b->ip, sizeof(b->ip), "%s", ip);
    char uri[80]; snprintf(uri, sizeof(uri), "ws://%s:%d/", ip, port);
    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.reconnect_timeout_ms = 4000;
    cfg.network_timeout_ms = 5000;
    cfg.buffer_size = 2048;
    b->client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(b->client, WEBSOCKET_EVENT_ANY, ws_evt, (void *)(intptr_t)slot);
    esp_websocket_client_start(b->client);
    ESP_LOGI(TAG, "bridge %d -> %s", slot, uri);
}

static void discovery_task(void *arg) {
    // give STA up to 20s to get an IP; if it can't, the saved network is wrong/out of range -> portal
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, false, true, pdMS_TO_TICKS(20000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "no IP within 20s -> entering setup portal");
        s_provisioning = true;
        provision_start();
        vTaskDelete(NULL);
        return;
    }
    mdns_init();
    for (;;) {
        if (g_cfg.bridge[0]) ensure_bridge(g_cfg.bridge, g_cfg.port > 0 ? g_cfg.port : 8787);  // explicit override
        mdns_result_t *res = NULL;
        if (mdns_query_ptr("_claudeq", "_tcp", 3000, MAX_BRIDGES * 2, &res) == ESP_OK) {
            for (mdns_result_t *r = res; r; r = r->next) {
                char ip[40]; int port = g_cfg.port > 0 ? g_cfg.port : 8787;
                if (result_ipv4(r, ip, sizeof(ip), &port)) ensure_bridge(ip, port);
            }
            mdns_query_results_free(res);
        }
        // reclaim slots whose bridge stopped advertising and isn't connected (vanished / changed IP)
        uint32_t now = xTaskGetTickCount();
        for (int i = 0; i < MAX_BRIDGES; i++) {
            bridge_t *b = &s_br[i];
            if (b->used && !b->connected && (now - b->last_seen) > pdMS_TO_TICKS(30000)) {
                ESP_LOGW(TAG, "reclaiming stale bridge slot %d (%s:%d)", i, b->ip, b->port);
                if (b->client) { esp_websocket_client_stop(b->client); esp_websocket_client_destroy(b->client); }
                free(b->rx);
                b->client = NULL; b->rx = NULL; b->used = false; b->connected = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(8000));   // keep discovering: pick up bridges that start later
    }
}

extern "C" void net_set_focus_bridge(int bridge) { if (bridge >= 0 && bridge < MAX_BRIDGES) s_focus_bridge = bridge; }

extern "C" void net_send_to(int bridge, const char *json) {
    if (bridge < 0 || bridge >= MAX_BRIDGES) return;
    bridge_t *b = &s_br[bridge];
    if (b->client && b->connected) esp_websocket_client_send_text(b->client, json, strlen(json), pdMS_TO_TICKS(1000));
}

extern "C" void net_send_text(const char *json) { net_send_to(s_focus_bridge, json); }

extern "C" void net_send_binary(const void *data, size_t len) {
    bridge_t *b = &s_br[s_focus_bridge];
    if (b->client && b->connected) esp_websocket_client_send_bin(b->client, (const char *)data, len, pdMS_TO_TICKS(200));
}

extern "C" void net_start(void) {
    s_wifi_eg = xEventGroupCreate();
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
    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, g_cfg.ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, g_cfg.pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = g_cfg.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(discovery_task, "net", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "wifi starting, ssid=%s; discovering bridges via mDNS", g_cfg.ssid);
}
