// ota.cpp — over-the-air firmware update.
//
// Fully bridge-independent: the deck pulls directly from GitHub Pages over HTTPS.
//   1. ota_check_async() fetches OTA_JSON_URL -> { "version", "app", ... }, compares to DEVICE_FW.
//   2. ota_start() streams OTA_APP_URL (the app-partition image) into the inactive OTA slot via
//      esp_https_ota, reports progress, sets the boot partition, and reboots.
//   3. The new image boots PENDING_VERIFY; main.cpp calls ota_confirm_valid() once healthy so the
//      bootloader won't roll back. A boot-loop before that auto-reverts to the previous slot.
//
// TLS uses the mbedtls certificate bundle (esp_crt_bundle_attach) already enabled for microlink.
// The image is NOT signature-verified (secure boot is off) — TLS + a trusted CA is the trust bar.
// All work runs in short-lived worker tasks; the UI only polls the state getters below (it never
// blocks on the network, and this module never touches LVGL).

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "app.h"
#include "app_config.h"

static const char *TAG = "ota";

#define OTA_BASE_URL "https://positronico.github.io/claudeq/"
#define OTA_JSON_URL OTA_BASE_URL "ota.json"
#define OTA_APP_URL  OTA_BASE_URL "claudeq-app.bin"

static volatile ota_state_t s_state = OTA_IDLE;
static volatile int         s_pct   = 0;
static char                 s_avail[24] = {0};   // version string from ota.json
static char                 s_err[64]   = {0};   // short error message
static volatile bool        s_busy      = false; // a check/download task is running
static bool                 s_confirmed = false; // rollback already cancelled this boot

ota_state_t ota_get_state(void)        { return s_state; }
int         ota_get_pct(void)          { return s_pct; }
const char *ota_get_avail_version(void){ return s_avail; }
const char *ota_get_error(void)        { return s_err; }
bool        ota_in_progress(void)      { return s_state == OTA_CHECKING || s_state == OTA_DOWNLOADING; }

static void set_error(const char *msg) {
    strlcpy(s_err, msg ? msg : "update failed", sizeof(s_err));
    s_state = OTA_ERROR;
}

// Parse a dotted numeric version ("v1.2.3" / "1.2.3") into up to 3 components. Non-numeric tails ignored.
static void parse_ver(const char *v, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!v) return;
    if (*v == 'v' || *v == 'V') v++;
    sscanf(v, "%d.%d.%d", &out[0], &out[1], &out[2]);
}
// >0 if a is newer than b, <0 if older, 0 if equal.
static int ver_cmp(const char *a, const char *b) {
    int va[3], vb[3];
    parse_ver(a, va); parse_ver(b, vb);
    for (int i = 0; i < 3; i++) if (va[i] != vb[i]) return va[i] - vb[i];
    return 0;
}

// Blocking HTTPS GET of a small text resource into buf. Returns bytes read, or -1 on error.
static int https_get_text(const char *url, char *buf, int buflen) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    int total = -1;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            int n = 0;
            while (n < buflen - 1) {
                int r = esp_http_client_read(c, buf + n, buflen - 1 - n);
                if (r <= 0) break;
                n += r;
            }
            buf[n] = 0;
            total = n;
        } else {
            ESP_LOGW(TAG, "check: HTTP %d", status);
        }
    } else {
        ESP_LOGW(TAG, "check: open failed: %s", esp_err_to_name(err));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return total;
}

static void check_task(void *arg) {
    (void)arg;
    s_state = OTA_CHECKING;
    s_pct = 0;
    char body[512];
    int n = https_get_text(OTA_JSON_URL, body, sizeof(body));
    if (n <= 0) {
        set_error("can't reach update server");
        s_busy = false; vTaskDelete(NULL); return;
    }
    cJSON *root = cJSON_Parse(body);
    cJSON *ver = root ? cJSON_GetObjectItem(root, "version") : NULL;
    if (!ver || !cJSON_IsString(ver)) {
        if (root) cJSON_Delete(root);
        set_error("bad update manifest");
        s_busy = false; vTaskDelete(NULL); return;
    }
    strlcpy(s_avail, ver->valuestring, sizeof(s_avail));
    cJSON_Delete(root);
    if (ver_cmp(s_avail, DEVICE_FW) > 0) {
        ESP_LOGI(TAG, "update available: %s (running %s)", s_avail, DEVICE_FW);
        s_state = OTA_AVAILABLE;
    } else {
        ESP_LOGI(TAG, "up to date (%s, latest %s)", DEVICE_FW, s_avail);
        s_state = OTA_UPTODATE;
    }
    s_busy = false;
    vTaskDelete(NULL);
}

static void download_task(void *arg) {
    (void)arg;
    s_state = OTA_DOWNLOADING;
    s_pct = 0;

    esp_http_client_config_t http = {};
    http.url = OTA_APP_URL;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = 20000;
    http.keep_alive_enable = true;

    esp_https_ota_config_t ota = {};
    ota.http_config = &http;

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&ota, &h);
    if (err != ESP_OK || !h) {
        set_error("download failed to start");
        s_busy = false; vTaskDelete(NULL); return;
    }
    int total = esp_https_ota_get_image_size(h);
    while (true) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int read = esp_https_ota_get_image_len_read(h);
        s_pct = (total > 0) ? (int)((int64_t)read * 100 / total) : 0;
    }
    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        err = esp_https_ota_finish(h);   // validates image + sets the boot partition
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA done -> rebooting into new image");
            s_pct = 100;
            s_state = OTA_REBOOTING;
            vTaskDelay(pdMS_TO_TICKS(1200));   // let the UI show "Rebooting…"
            esp_restart();
        } else {
            set_error("image validation failed");
        }
    } else {
        esp_https_ota_abort(h);
        set_error("download interrupted");
    }
    s_busy = false;
    vTaskDelete(NULL);
}

void ota_check_async(void) {
    if (s_busy) return;
    s_busy = true;
    s_err[0] = 0;
    xTaskCreate(check_task, "ota_chk", 8192, NULL, 5, NULL);
}

void ota_start(void) {
    if (s_busy) return;
    s_busy = true;
    s_err[0] = 0;
    xTaskCreate(download_task, "ota", 8192, NULL, 5, NULL);
}

// Called once the running image is proven healthy (UI up + network reachable). If we booted a
// freshly-OTA'd image that's still PENDING_VERIFY, mark it valid so the bootloader keeps it.
void ota_confirm_valid(void) {
    if (s_confirmed) return;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "new image marked valid (rollback cancelled)");
        }
    }
    s_confirmed = true;
}
