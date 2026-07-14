// Persistent device identity + a small bounded table of paired-bridge trust records (bridge_id -> PSK),
// stored in NVS namespace "claudeq" (same namespace as provision.cpp's WiFi/Tailscale config, so
// cfg_clear()'s nvs_erase_all wipes this too, for free -- a factory reset also forgets every pairing).
// See docs/PROTOCOL.md's "Pairing" section for the ceremony that populates this table.
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "app.h"

static const char *TAG = "trust";
#define NVS_NS "claudeq"

typedef struct {
    uint16_t version;
    uint16_t count;
    trust_bridge_t entries[MAX_TRUSTED_BRIDGES];
} trust_blob_t;

static trust_blob_t s_trust;
static char s_device_id[40];
static bool s_loaded = false;

static void save_trust(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "trust", &s_trust, sizeof(s_trust));
    nvs_commit(h); nvs_close(h);
}

static void save_device_id(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "dev_id", s_device_id);
    nvs_commit(h); nvs_close(h);
}

static void recompute_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_TRUSTED_BRIDGES; i++) if (s_trust.entries[i].used) n++;
    s_trust.count = (uint16_t)n;
}

static void load_once(void) {
    if (s_loaded) return;
    s_loaded = true;
    memset(&s_trust, 0, sizeof(s_trust));
    s_trust.version = 1;
    s_device_id[0] = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_device_id);
        nvs_get_str(h, "dev_id", s_device_id, &n);
        size_t blen = sizeof(s_trust);
        if (nvs_get_blob(h, "trust", &s_trust, &blen) != ESP_OK || blen != sizeof(s_trust)) {
            memset(&s_trust, 0, sizeof(s_trust));
            s_trust.version = 1;
        }
        nvs_close(h);
    }
    if (!s_device_id[0]) {
        uint8_t raw[16];
        esp_fill_random(raw, sizeof(raw));
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 16; i++) {
            s_device_id[i * 2]     = hex[raw[i] >> 4];
            s_device_id[i * 2 + 1] = hex[raw[i] & 0xF];
        }
        s_device_id[32] = 0;
        save_device_id();
        ESP_LOGI(TAG, "generated new device_id: %s", s_device_id);
    }
    ESP_LOGI(TAG, "loaded trust table: %d paired bridge(s)", trust_count());
}

const char *device_get_id(void) {
    load_once();
    return s_device_id;
}

int trust_find(const char *bridge_id) {
    load_once();
    if (!bridge_id || !bridge_id[0]) return -1;
    for (int i = 0; i < MAX_TRUSTED_BRIDGES; i++)
        if (s_trust.entries[i].used && strcmp(s_trust.entries[i].bridge_id, bridge_id) == 0) return i;
    return -1;
}

bool trust_get(int idx, trust_bridge_t *out) {
    load_once();
    if (idx < 0 || idx >= MAX_TRUSTED_BRIDGES || !s_trust.entries[idx].used || !out) return false;
    *out = s_trust.entries[idx];
    return true;
}

int trust_count(void) {
    load_once();
    int n = 0;
    for (int i = 0; i < MAX_TRUSTED_BRIDGES; i++) if (s_trust.entries[i].used) n++;
    return n;
}

bool trust_add(const char *bridge_id, const char *label, const uint8_t psk[32]) {
    load_once();
    if (!bridge_id || !bridge_id[0] || !psk) return false;
    int idx = trust_find(bridge_id);
    if (idx < 0) {
        for (int i = 0; i < MAX_TRUSTED_BRIDGES; i++) if (!s_trust.entries[i].used) { idx = i; break; }
    }
    if (idx < 0) {   // table full -> evict the oldest pairing to make room for the new one
        idx = 0;
        for (int i = 1; i < MAX_TRUSTED_BRIDGES; i++)
            if (s_trust.entries[i].paired_at < s_trust.entries[idx].paired_at) idx = i;
        ESP_LOGW(TAG, "trust table full; evicting oldest entry (%s)", s_trust.entries[idx].bridge_id);
    }
    trust_bridge_t *e = &s_trust.entries[idx];
    e->used = true;
    snprintf(e->bridge_id, sizeof(e->bridge_id), "%s", bridge_id);
    snprintf(e->label, sizeof(e->label), "%s", (label && label[0]) ? label : bridge_id);
    memcpy(e->psk, psk, 32);
    e->paired_at = (uint32_t)(esp_timer_get_time() / 1000000);
    recompute_count();
    save_trust();
    ESP_LOGI(TAG, "paired with bridge %s (%s)", bridge_id, e->label);
    return true;
}

void trust_forget_by_index(int idx) {
    load_once();
    if (idx < 0 || idx >= MAX_TRUSTED_BRIDGES || !s_trust.entries[idx].used) return;
    ESP_LOGI(TAG, "forgetting bridge %s", s_trust.entries[idx].bridge_id);
    memset(&s_trust.entries[idx], 0, sizeof(s_trust.entries[idx]));
    recompute_count();
    save_trust();
}

void trust_forget(const char *bridge_id) {
    trust_forget_by_index(trust_find(bridge_id));
}
