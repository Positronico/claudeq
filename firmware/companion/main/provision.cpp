// On-device provisioning: a SoftAP captive portal to set WiFi + bridge address, saved to NVS.
// No rebuild needed to change networks. Entered on first boot (no config), when STA can't connect,
// or on demand (the "WiFi setup" button writes a force flag and reboots).
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "app.h"
#include "app_config.h"

static const char *TAG = "prov";
#define NVS_NS  "claudeq"
#define AP_SSID "Claudeq-setup"
#define AP_IP   "192.168.4.1"
static httpd_handle_t s_httpd = NULL;

// ---------- config storage (NVS) ----------
bool cfg_load(claudeq_cfg_t *out) {
    // start from compile-time defaults (may be placeholders from app_config.h)
    snprintf(out->ssid, sizeof(out->ssid), "%s", WIFI_SSID);
    snprintf(out->pass, sizeof(out->pass), "%s", WIFI_PASSWORD);
    snprintf(out->bridge, sizeof(out->bridge), "%s", BRIDGE_HOST);
    out->port = BRIDGE_PORT;

    bool forced = false, have_nvs = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t f = 0; if (nvs_get_u8(h, "force", &f) == ESP_OK && f) forced = true;
        size_t n;   // read straight into the correctly-sized destination fields (defaults kept on miss)
        n = sizeof(out->ssid);   if (nvs_get_str(h, "ssid", out->ssid, &n) == ESP_OK && out->ssid[0]) have_nvs = true;
        n = sizeof(out->pass);   nvs_get_str(h, "pass", out->pass, &n);
        n = sizeof(out->bridge); nvs_get_str(h, "bridge", out->bridge, &n);
        uint16_t p; if (nvs_get_u16(h, "port", &p) == ESP_OK && p) out->port = p;
        nvs_close(h);
    }
    if (forced) return false;                 // user asked to re-run setup
    if (have_nvs) return true;                 // provisioned creds present
    return strlen(out->ssid) > 0 && strcmp(out->ssid, "YOUR_WIFI_SSID") != 0;  // else only usable if compiled-in
}

void cfg_save(const char *ssid, const char *pass, const char *bridge, int port) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid ? ssid : "");
    nvs_set_str(h, "pass", pass ? pass : "");
    nvs_set_str(h, "bridge", bridge ? bridge : "");
    nvs_set_u16(h, "port", (uint16_t)(port > 0 ? port : BRIDGE_PORT));
    nvs_set_u8(h, "force", 0);
    nvs_commit(h); nvs_close(h);
}

void cfg_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h); nvs_commit(h); nvs_close(h);
}

void net_enter_setup(void) {                  // called from the UI: force the portal on next boot
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "force", 1); nvs_commit(h); nvs_close(h); }
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

// ---------- captive portal HTTP ----------
static void urldecode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *o++ = ' '; }
        else if (*p == '%' && p[1] && p[2]) {
            int hi = (p[1] <= '9' ? p[1] - '0' : (p[1] | 0x20) - 'a' + 10);
            int lo = (p[2] <= '9' ? p[2] - '0' : (p[2] | 0x20) - 'a' + 10);
            *o++ = (char)((hi << 4) | lo); p += 2;
        } else { *o++ = *p; }
    }
    *o = 0;
}

static const char *FORM_HTML =
"<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Claudeq setup</title><style>body{font-family:-apple-system,Segoe UI,sans-serif;background:#0a0b0f;color:#e9edf5;max-width:420px;margin:0 auto;padding:24px}"
"h1{color:#d6743a;font-size:21px}label{display:block;margin:14px 0 4px;font-size:13px;color:#8a93a6}"
"input{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;border:1px solid #222633;background:#12141c;color:#e9edf5;font-size:16px}"
"button{margin-top:22px;width:100%;padding:14px;border:0;border-radius:8px;background:#d6743a;color:#1a1206;font-weight:700;font-size:16px}"
"p{color:#8a93a6;font-size:12px}</style></head><body><h1>Claudeq setup</h1>"
"<p>Connect your deck to your WiFi and to the Mac running the bridge.</p>"
"<form method=POST action=/save>"
"<label>WiFi network (2.4 GHz only)</label><input name=ssid value=\"%s\" maxlength=32 autofocus>"
"<label>WiFi password</label><input name=pass type=password placeholder=\"(leave blank if open)\">"
"<label>Bridge address (your Mac's LAN IP)</label><input name=bridge value=\"%s\">"
"<label>Bridge port</label><input name=port value=\"%d\">"
"<button type=submit>Save &amp; restart</button></form></body></html>";

static esp_err_t root_get(httpd_req_t *req) {
    claudeq_cfg_t c; cfg_load(&c);
    const char *ssid_disp = strcmp(c.ssid, "YOUR_WIFI_SSID") == 0 ? "" : c.ssid;
    char *buf = (char *)malloc(2048);
    if (!buf) return ESP_FAIL;
    int n = snprintf(buf, 2048, FORM_HTML, ssid_disp, c.bridge, c.port);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, n);
    free(buf);
    return ESP_OK;
}

static void restart_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(1200)); esp_restart(); }

static esp_err_t save_post(httpd_req_t *req) {
    char body[512];
    int want = req->content_len < sizeof(body) - 1 ? (int)req->content_len : (int)sizeof(body) - 1;
    int total = 0;
    while (total < want) {
        int r = httpd_req_recv(req, body + total, want - total);
        if (r < 0) return ESP_FAIL;          // socket error/timeout -> persist nothing
        if (r == 0) break;
        total += r;
    }
    body[total] = 0;
    char ssid[65] = {0}, pass[65] = {0}, bridge[64] = {0}, port[8] = {0};
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    httpd_query_key_value(body, "bridge", bridge, sizeof(bridge));
    httpd_query_key_value(body, "port", port, sizeof(port));
    urldecode(ssid); urldecode(pass); urldecode(bridge); urldecode(port);
    if (!ssid[0] || strlen(ssid) > 32) {     // 802.11 SSID is <=32 octets; also reject empty
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Enter a WiFi network name (max 32 characters).");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saving config: ssid=%s bridge=%s:%s", ssid, bridge, port);
    cfg_save(ssid, pass, bridge, atoi(port));
    const char *ok = "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font-family:-apple-system,sans-serif;background:#0a0b0f;color:#e9edf5;text-align:center;padding-top:64px'>"
        "<h2 style='color:#46c46a'>Saved.</h2><p>Claudeq is restarting and will join your network.</p></body>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, ok, HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "rb", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ---------- captive DNS (answer every query with our AP IP -> phones auto-open the portal) ----------
static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(53); sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(sock); vTaskDelete(NULL); return; }
    uint8_t buf[256];
    const uint8_t ans[] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04, 192, 168, 4, 1 };
    while (1) {
        struct sockaddr_in cli; socklen_t cl = sizeof(cli);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &cl);
        if (n < 12) continue;
        buf[2] = 0x81; buf[3] = 0x80;                    // QR=1, AA=1, RA=1
        buf[6] = 0x00; buf[7] = 0x01;                    // ANCOUNT = 1
        buf[8] = buf[9] = buf[10] = buf[11] = 0;         // NSCOUNT/ARCOUNT = 0
        if (n + (int)sizeof(ans) > (int)sizeof(buf)) continue;
        memcpy(buf + n, ans, sizeof(ans));
        sendto(sock, buf, n + sizeof(ans), 0, (struct sockaddr *)&cli, cl);
    }
}

// ---------- bring up the portal ----------
static void setup_ui_task(void *arg) { ui_show_setup(AP_SSID, AP_IP); vTaskDelete(NULL); }

void provision_start(void) {
    ESP_LOGW(TAG, "starting SoftAP provisioning portal (%s @ %s)", AP_SSID, AP_IP);
    esp_wifi_stop();                                     // no-op/harmless if STA was never started
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", AP_SSID);
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.lru_purge_enable = true;
    hc.max_uri_handlers = 8;
    if (httpd_start(&s_httpd, &hc) == ESP_OK) {
        httpd_uri_t save = {}; save.uri = "/save"; save.method = HTTP_POST; save.handler = save_post;
        httpd_register_uri_handler(s_httpd, &save);
        httpd_uri_t root = {}; root.uri = "/*"; root.method = HTTP_GET; root.handler = root_get;
        httpd_register_uri_handler(s_httpd, &root);
    }
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    xTaskCreate(setup_ui_task, "setupui", 4096, NULL, 4, NULL);
}
