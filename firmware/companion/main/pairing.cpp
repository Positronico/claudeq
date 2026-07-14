// Device-side pairing (live numeric-comparison / SAS) + per-connection authentication + AES-256-GCM
// envelope crypto for the device<->bridge WebSocket link. Mirrors bridge/crypto.mjs (node:crypto) and
// bridge/mock-device.html (WebCrypto SubtleCrypto) bit-for-bit -- three independent implementations of
// the same wire protocol, see docs/PROTOCOL.md's "Pairing" / "Authentication & encryption" sections.
//
// Crypto building blocks used here, all already linked for this firmware (no new sdkconfig flags):
//   - X25519 ECDH: the small vendored implementation in microlink's x25519.c/h (main already REQUIRES
//     microlink, whose CMakeLists exposes "src" as a public include dir) -- not mbedtls's generic
//     Weierstrass-curve-oriented ECDH context API.
//   - HMAC-SHA256 (the one-step KDF used everywhere below -- there is no HKDF component enabled) and
//     AES-256-GCM: mbedtls_md_hmac / mbedtls_gcm_*, both already enabled (used by microlink/OTA).
//   - Base64 for JSON-safe key/nonce/ciphertext transport: mbedtls_base64_* (always compiled in).
//
// All context strings below (e.g. "claudeq-psk-v1") are hashed via strlen() at runtime, NEVER a
// hand-counted byte length -- a single off-by-one there would silently break interop with the bridge
// and mock, since HMAC/AES-GCM inputs must be byte-identical across all three implementations.
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "app.h"
// x25519.h has no extern "C" guard, and every other consumer in the tree (ml_noise.c, ml_wg_mgr.c) is
// plain C -- this is the first C++ translation unit to include it, so without this wrapper the compiler
// mangles the declaration (_Z6x25519...) while the linked object exports the plain C symbol, and the
// link fails with "undefined reference to `x25519(...)`".
extern "C" {
#include "x25519.h"
}

static const char *TAG = "pairing";
#define PAIR_TIMEOUT_MS 90000

// ---------- generic crypto helpers ----------
static int hmac_sha256(const uint8_t *key, size_t keylen, const uint8_t *data, size_t datalen, uint8_t out[32]) {
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keylen, data, datalen, out);
}
// Byte-lexicographic order (matches C memcmp / Node Buffer.compare / JS byte compare exactly) so both
// peers hash the two ephemeral pubkeys in the same order regardless of who sent pair_request first.
static void sort_pubs(const uint8_t a[32], const uint8_t b[32], const uint8_t **first, const uint8_t **second) {
    if (memcmp(a, b, 32) <= 0) { *first = a; *second = b; } else { *first = b; *second = a; }
}
static void compute_sas(const uint8_t shared[32], const uint8_t pub_a[32], const uint8_t pub_b[32], char out[7]) {
    const uint8_t *x, *y; sort_pubs(pub_a, pub_b, &x, &y);
    uint8_t buf[64]; memcpy(buf, x, 32); memcpy(buf + 32, y, 32);
    uint8_t mac[32]; hmac_sha256(shared, 32, buf, sizeof(buf), mac);
    uint32_t n = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
    snprintf(out, 7, "%06u", (unsigned)(n % 1000000));
}
static void derive_psk(const uint8_t shared[32], uint8_t psk[32]) {
    static const char *ctx = "claudeq-psk-v1";
    hmac_sha256(shared, 32, (const uint8_t *)ctx, strlen(ctx), psk);
}
// transcript = device_nonce || bridge_nonce || sorted(device_eph_pub, bridge_eph_pub)
static size_t build_auth_transcript(const uint8_t nonce_d[16], const uint8_t nonce_b[16],
                                     const uint8_t pub_d[32], const uint8_t pub_b[32], uint8_t out[96]) {
    const uint8_t *x, *y; sort_pubs(pub_d, pub_b, &x, &y);
    memcpy(out, nonce_d, 16); memcpy(out + 16, nonce_b, 16); memcpy(out + 32, x, 32); memcpy(out + 64, y, 32);
    return 96;
}
static void derive_session_key(const uint8_t psk[32], const uint8_t ecdh_shared[32],
                                const uint8_t *transcript, size_t tlen, uint8_t session_key[32]) {
    static const char *ctx = "claudeq-session-v1";
    uint8_t buf[32 + 96 + 32]; size_t n = 0;
    memcpy(buf, ecdh_shared, 32); n += 32;
    memcpy(buf + n, transcript, tlen); n += tlen;
    memcpy(buf + n, ctx, strlen(ctx)); n += strlen(ctx);
    hmac_sha256(psk, 32, buf, n, session_key);
}
static void derive_directional_keys(const uint8_t session_key[32], uint8_t k_d2b[32], uint8_t k_b2d[32]) {
    hmac_sha256(session_key, 32, (const uint8_t *)"d2b", 3, k_d2b);
    hmac_sha256(session_key, 32, (const uint8_t *)"b2d", 3, k_b2d);
}
static void compute_auth_tag(const uint8_t session_key[32], const char *role,
                              const uint8_t *transcript, size_t tlen, uint8_t out[32]) {
    uint8_t buf[16 + 96]; size_t rl = strlen(role);
    memcpy(buf, role, rl); memcpy(buf + rl, transcript, tlen);
    hmac_sha256(session_key, 32, buf, rl + tlen, out);
}
static void counter_iv(uint64_t n, uint8_t iv[12]) {
    memset(iv, 0, 12);
    for (int i = 0; i < 8; i++) iv[4 + i] = (uint8_t)(n >> (8 * (7 - i)));
}
static int b64_encode(const uint8_t *data, size_t len, char *out, size_t out_cap) {
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, out_cap, &olen, data, len) != 0) return -1;
    out[olen] = 0;
    return (int)olen;
}
static int b64_decode(const char *s, uint8_t *out, size_t out_cap) {
    size_t olen = 0;
    if (mbedtls_base64_decode(out, out_cap, &olen, (const unsigned char *)s, strlen(s)) != 0) return -1;
    return (int)olen;
}

// ---------- per-connection auth + envelope state (one per bridge slot, indexed like net.cpp's s_br[]) ----------
typedef struct {
    bool authenticated;
    uint8_t send_key[32], recv_key[32];   // active directional AES-256 keys (post-auth)
    uint64_t send_ctr, recv_ctr;
    char bridge_id_snapshot[40];          // learned from hello_ack; identifies which trust entry to use

    bool auth_in_flight;
    uint8_t auth_priv[32], auth_pub[32], nonce_device[16];
    // scratch while waiting for the bridge's own auth_verify:
    uint8_t pending_session_key[32];
    uint8_t pending_send_key[32], pending_recv_key[32];
    uint8_t pending_transcript[96];
    size_t  pending_transcript_len;
} pairing_conn_t;
static pairing_conn_t s_conn[MAX_BRIDGES];

static void start_auth_handshake(int bi) {
    pairing_conn_t *c = &s_conn[bi];
    if (trust_find(c->bridge_id_snapshot) < 0) return;
    esp_fill_random(c->auth_priv, 32);
    x25519_base(c->auth_pub, c->auth_priv, 1);
    esp_fill_random(c->nonce_device, 16);
    c->auth_in_flight = true;
    char nb64[32], pb64[64];
    if (b64_encode(c->nonce_device, 16, nb64, sizeof(nb64)) < 0) return;
    if (b64_encode(c->auth_pub, 32, pb64, sizeof(pb64)) < 0) return;
    char msg[200];
    snprintf(msg, sizeof(msg), "{\"type\":\"auth_hello\",\"nonce\":\"%s\",\"pub\":\"%s\"}", nb64, pb64);
    net_send_raw(bi, msg);
}

static void handle_auth_challenge(cJSON *root, int bi) {
    pairing_conn_t *c = &s_conn[bi];
    if (!c->auth_in_flight) return;
    cJSON *nonce_item = cJSON_GetObjectItem(root, "nonce");
    cJSON *pub_item = cJSON_GetObjectItem(root, "pub");
    if (!cJSON_IsString(nonce_item) || !cJSON_IsString(pub_item)) return;
    uint8_t nonce_bridge[16], pub_bridge[32];
    if (b64_decode(nonce_item->valuestring, nonce_bridge, sizeof(nonce_bridge)) != 16) return;
    if (b64_decode(pub_item->valuestring, pub_bridge, sizeof(pub_bridge)) != 32) return;

    int idx = trust_find(c->bridge_id_snapshot);
    trust_bridge_t entry;
    if (idx < 0 || !trust_get(idx, &entry)) return;

    uint8_t ecdh_shared[32];
    if (x25519(ecdh_shared, c->auth_priv, pub_bridge, 1) != 0) { ESP_LOGW(TAG, "bridge %d: non-contributory auth pubkey", bi); return; }
    c->pending_transcript_len = build_auth_transcript(c->nonce_device, nonce_bridge, c->auth_pub, pub_bridge, c->pending_transcript);
    derive_session_key(entry.psk, ecdh_shared, c->pending_transcript, c->pending_transcript_len, c->pending_session_key);
    derive_directional_keys(c->pending_session_key, c->pending_send_key, c->pending_recv_key);   // d2b, b2d
    memset(ecdh_shared, 0, sizeof(ecdh_shared));
    memset(&entry, 0, sizeof(entry));   // scrub the PSK copy off the stack promptly

    uint8_t my_tag[32];
    compute_auth_tag(c->pending_session_key, "device", c->pending_transcript, c->pending_transcript_len, my_tag);
    char mac_b64[64];
    if (b64_encode(my_tag, 32, mac_b64, sizeof(mac_b64)) < 0) return;
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"type\":\"auth_verify\",\"mac\":\"%s\"}", mac_b64);
    net_send_raw(bi, msg);
}

static void handle_auth_verify(cJSON *root, int bi) {
    pairing_conn_t *c = &s_conn[bi];
    if (!c->auth_in_flight) return;
    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    if (!cJSON_IsString(mac_item)) return;
    uint8_t got[32];
    if (b64_decode(mac_item->valuestring, got, sizeof(got)) != 32) return;
    uint8_t expected[32];
    compute_auth_tag(c->pending_session_key, "bridge", c->pending_transcript, c->pending_transcript_len, expected);
    if (memcmp(got, expected, 32) != 0) {
        ESP_LOGW(TAG, "bridge %d: auth FAILED (bad bridge MAC)", bi);
        c->auth_in_flight = false;
        memset(c->pending_session_key, 0, 32);
        return;
    }
    memcpy(c->send_key, c->pending_send_key, 32);
    memcpy(c->recv_key, c->pending_recv_key, 32);
    c->send_ctr = 0; c->recv_ctr = 0;
    c->authenticated = true;
    c->auth_in_flight = false;
    memset(c->pending_session_key, 0, 32);
    memset(c->auth_priv, 0, 32);
    ESP_LOGI(TAG, "bridge %d authenticated", bi);
}

static void handle_auth_reject(cJSON *root, int bi) {
    cJSON *reason = cJSON_GetObjectItem(root, "reason");
    ESP_LOGW(TAG, "bridge %d: auth rejected (%s)", bi, cJSON_IsString(reason) ? reason->valuestring : "?");
    s_conn[bi].auth_in_flight = false;
}

void pairing_on_connect(int bi) {
    if (bi < 0 || bi >= MAX_BRIDGES) return;
    memset(&s_conn[bi], 0, sizeof(s_conn[bi]));
}

bool pairing_is_authenticated(int bi) {
    return bi >= 0 && bi < MAX_BRIDGES && s_conn[bi].authenticated;
}

// ---------- SAS pairing ceremony (one global ceremony at a time -- matches the single on-screen flow) ----------
typedef struct {
    pairing_state_t state;
    int bridge;
    uint8_t my_priv[32], my_pub[32], their_pub[32], shared[32];
    char code[8];
    bool confirmed_by_me, confirmed_by_peer;
    int64_t deadline_us;
} pairing_ceremony_t;
static pairing_ceremony_t s_pair;

static void abort_pairing(pairing_state_t final_state) {
    memset(s_pair.my_priv, 0, sizeof(s_pair.my_priv));
    memset(s_pair.shared, 0, sizeof(s_pair.shared));
    s_pair.state = final_state;
}

void pairing_on_disconnect(int bi) {
    if (bi < 0 || bi >= MAX_BRIDGES) return;
    memset(&s_conn[bi], 0, sizeof(s_conn[bi]));
    if (s_pair.bridge == bi && s_pair.state != PAIR_IDLE && s_pair.state != PAIR_DONE && s_pair.state != PAIR_FAILED) {
        ESP_LOGW(TAG, "pairing aborted: bridge %d disconnected mid-ceremony", bi);
        abort_pairing(PAIR_FAILED);
    }
}

void pairing_start_as_device(int bridge_slot) {
    if (bridge_slot < 0 || bridge_slot >= MAX_BRIDGES || s_pair.state != PAIR_IDLE) return;
    memset(&s_pair, 0, sizeof(s_pair));
    s_pair.bridge = bridge_slot;
    esp_fill_random(s_pair.my_priv, 32);
    x25519_base(s_pair.my_pub, s_pair.my_priv, 1);
    s_pair.state = PAIR_WAIT_RESPONSE;
    s_pair.deadline_us = esp_timer_get_time() + (int64_t)PAIR_TIMEOUT_MS * 1000;
    char pub_b64[64];
    if (b64_encode(s_pair.my_pub, 32, pub_b64, sizeof(pub_b64)) < 0) { abort_pairing(PAIR_FAILED); return; }
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"type\":\"pair_request\",\"pub\":\"%s\"}", pub_b64);
    net_send_raw(bridge_slot, msg);
}

static void handle_pair_request(cJSON *root, int bi) {   // bridge-initiated (operator ran `claudeq pair`)
    if (s_pair.state != PAIR_IDLE) { net_send_raw(bi, "{\"type\":\"pair_reject\",\"reason\":\"busy\"}"); return; }
    cJSON *pub_item = cJSON_GetObjectItem(root, "pub");
    if (!cJSON_IsString(pub_item)) return;
    uint8_t their_pub[32];
    if (b64_decode(pub_item->valuestring, their_pub, sizeof(their_pub)) != 32) return;
    memset(&s_pair, 0, sizeof(s_pair));
    s_pair.bridge = bi;
    esp_fill_random(s_pair.my_priv, 32);
    x25519_base(s_pair.my_pub, s_pair.my_priv, 1);
    memcpy(s_pair.their_pub, their_pub, 32);
    if (x25519(s_pair.shared, s_pair.my_priv, s_pair.their_pub, 1) != 0) { abort_pairing(PAIR_FAILED); return; }
    compute_sas(s_pair.shared, s_pair.my_pub, s_pair.their_pub, s_pair.code);
    s_pair.state = PAIR_SHOW_SAS;
    s_pair.deadline_us = esp_timer_get_time() + (int64_t)PAIR_TIMEOUT_MS * 1000;
    char pub_b64[64];
    if (b64_encode(s_pair.my_pub, 32, pub_b64, sizeof(pub_b64)) < 0) { abort_pairing(PAIR_FAILED); return; }
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"type\":\"pair_response\",\"pub\":\"%s\"}", pub_b64);
    net_send_raw(bi, msg);
}

static void handle_pair_response(cJSON *root, int bi) {   // reply to OUR pair_request
    if (s_pair.state != PAIR_WAIT_RESPONSE || s_pair.bridge != bi) return;
    cJSON *pub_item = cJSON_GetObjectItem(root, "pub");
    if (!cJSON_IsString(pub_item)) return;
    uint8_t their_pub[32];
    if (b64_decode(pub_item->valuestring, their_pub, sizeof(their_pub)) != 32) return;
    memcpy(s_pair.their_pub, their_pub, 32);
    if (x25519(s_pair.shared, s_pair.my_priv, s_pair.their_pub, 1) != 0) { abort_pairing(PAIR_FAILED); return; }
    compute_sas(s_pair.shared, s_pair.my_pub, s_pair.their_pub, s_pair.code);
    s_pair.state = PAIR_SHOW_SAS;
    s_pair.deadline_us = esp_timer_get_time() + (int64_t)PAIR_TIMEOUT_MS * 1000;
}

static void try_finalize_pairing(void) {
    if (!s_pair.confirmed_by_me || !s_pair.confirmed_by_peer) return;
    pairing_conn_t *c = &s_conn[s_pair.bridge];
    if (!c->bridge_id_snapshot[0]) { abort_pairing(PAIR_FAILED); return; }
    uint8_t psk[32];
    derive_psk(s_pair.shared, psk);
    trust_add(c->bridge_id_snapshot, NULL, psk);
    memset(psk, 0, sizeof(psk));
    net_send_raw(s_pair.bridge, "{\"type\":\"pair_ack\",\"ok\":true}");
    int bridge = s_pair.bridge;
    s_pair.state = PAIR_DONE;
    memset(s_pair.my_priv, 0, sizeof(s_pair.my_priv));
    memset(s_pair.shared, 0, sizeof(s_pair.shared));
    start_auth_handshake(bridge);   // authenticate this now-paired connection immediately, no reconnect needed
}

static void handle_pair_confirm(cJSON *root, int bi) {
    if (s_pair.bridge != bi || (s_pair.state != PAIR_SHOW_SAS && s_pair.state != PAIR_WAIT_PEER)) return;
    s_pair.confirmed_by_peer = true;
    try_finalize_pairing();
}

static void handle_pair_reject(cJSON *root, int bi) {
    if (s_pair.bridge != bi) return;
    cJSON *reason = cJSON_GetObjectItem(root, "reason");
    ESP_LOGW(TAG, "pairing rejected by peer (%s)", cJSON_IsString(reason) ? reason->valuestring : "?");
    abort_pairing(PAIR_FAILED);
}

static void handle_pair_ack(cJSON *root, int bi) {
    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    ESP_LOGI(TAG, "bridge %d confirmed pairing complete (ok=%d)", bi, cJSON_IsTrue(ok) ? 1 : 0);
}

void pairing_confirm(void) {
    if (s_pair.state != PAIR_SHOW_SAS) return;
    s_pair.confirmed_by_me = true;
    net_send_raw(s_pair.bridge, "{\"type\":\"pair_confirm\"}");
    s_pair.state = PAIR_WAIT_PEER;
    s_pair.deadline_us = esp_timer_get_time() + (int64_t)PAIR_TIMEOUT_MS * 1000;
    try_finalize_pairing();
}

void pairing_reject(void) {
    if (s_pair.state == PAIR_IDLE || s_pair.state == PAIR_DONE || s_pair.state == PAIR_FAILED) return;
    net_send_raw(s_pair.bridge, "{\"type\":\"pair_reject\",\"reason\":\"user\"}");
    abort_pairing(PAIR_FAILED);
}

void pairing_dismiss(void) {
    if (s_pair.state == PAIR_DONE || s_pair.state == PAIR_FAILED) s_pair.state = PAIR_IDLE;
}

pairing_state_t pairing_get_state(void) {
    if ((s_pair.state == PAIR_WAIT_RESPONSE || s_pair.state == PAIR_SHOW_SAS || s_pair.state == PAIR_WAIT_PEER)
        && esp_timer_get_time() > s_pair.deadline_us) {
        ESP_LOGW(TAG, "pairing ceremony timed out");
        if (s_pair.bridge >= 0) net_send_raw(s_pair.bridge, "{\"type\":\"pair_reject\",\"reason\":\"timeout\"}");
        abort_pairing(PAIR_FAILED);
    }
    return s_pair.state;
}
const char *pairing_get_code(void) { return s_pair.code; }
int pairing_get_bridge(void) { return s_pair.state == PAIR_IDLE ? -1 : s_pair.bridge; }

// ---------- dispatch (net.cpp forwards every hello_ack/pair_*/auth_* message here) ----------
void pairing_on_message(cJSON *root, int bi) {
    if (bi < 0 || bi >= MAX_BRIDGES) return;
    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type_item)) return;
    const char *t = type_item->valuestring;
    if (!strcmp(t, "hello_ack")) {
        cJSON *bid = cJSON_GetObjectItem(root, "bridge_id");
        cJSON *paired = cJSON_GetObjectItem(root, "paired");
        if (cJSON_IsString(bid)) snprintf(s_conn[bi].bridge_id_snapshot, sizeof(s_conn[bi].bridge_id_snapshot), "%s", bid->valuestring);
        if (cJSON_IsTrue(paired) && !s_conn[bi].authenticated && !s_conn[bi].auth_in_flight) start_auth_handshake(bi);
    }
    else if (!strcmp(t, "pair_request"))  handle_pair_request(root, bi);
    else if (!strcmp(t, "pair_response")) handle_pair_response(root, bi);
    else if (!strcmp(t, "pair_confirm"))  handle_pair_confirm(root, bi);
    else if (!strcmp(t, "pair_reject"))   handle_pair_reject(root, bi);
    else if (!strcmp(t, "pair_ack"))      handle_pair_ack(root, bi);
    else if (!strcmp(t, "auth_challenge")) handle_auth_challenge(root, bi);
    else if (!strcmp(t, "auth_verify"))    handle_auth_verify(root, bi);
    else if (!strcmp(t, "auth_reject"))    handle_auth_reject(root, bi);
}

// ---------- envelope crypto (wraps/unwraps every non-handshake message once authenticated) ----------
bool pairing_wrap_outgoing(int bi, const char *json, char **out_json, size_t *out_len) {
    if (bi < 0 || bi >= MAX_BRIDGES || !json || !out_json || !out_len) return false;
    pairing_conn_t *c = &s_conn[bi];
    if (!c->authenticated) return false;
    size_t plen = strlen(json);
    uint8_t *combined = (uint8_t *)malloc(plen + 16);   // ciphertext || tag
    if (!combined) return false;
    uint64_t n = ++c->send_ctr;
    uint8_t iv[12]; counter_iv(n, iv);
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, c->send_key, 256);
    int rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plen, iv, sizeof(iv), NULL, 0,
                                        (const uint8_t *)json, combined, 16, combined + plen);
    mbedtls_gcm_free(&gcm);
    if (rc != 0) { free(combined); return false; }
    size_t b64cap = 4 * ((plen + 16 + 2) / 3) + 4;
    char *b64 = (char *)malloc(b64cap);
    if (!b64) { free(combined); return false; }
    int b64len = b64_encode(combined, plen + 16, b64, b64cap);
    free(combined);
    if (b64len < 0) { free(b64); return false; }
    size_t jcap = (size_t)b64len + 64;
    char *msg = (char *)malloc(jcap);
    if (!msg) { free(b64); return false; }
    int mn = snprintf(msg, jcap, "{\"type\":\"sec\",\"n\":%llu,\"ct\":\"%s\"}", (unsigned long long)n, b64);
    free(b64);
    if (mn < 0) { free(msg); return false; }
    *out_json = msg;
    *out_len = (size_t)mn;
    return true;
}

bool pairing_wrap_outgoing_binary(int bi, const uint8_t *data, size_t len, uint8_t **out, size_t *out_len) {
    if (bi < 0 || bi >= MAX_BRIDGES || !data || !out || !out_len) return false;
    pairing_conn_t *c = &s_conn[bi];
    if (!c->authenticated) return false;
    uint8_t *buf = (uint8_t *)malloc(8 + len + 16);
    if (!buf) return false;
    uint64_t n = ++c->send_ctr;
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)(n >> (8 * (7 - i)));   // big-endian counter header
    uint8_t iv[12]; counter_iv(n, iv);
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, c->send_key, 256);
    int rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, iv, sizeof(iv), NULL, 0, data, buf + 8, 16, buf + 8 + len);
    mbedtls_gcm_free(&gcm);
    if (rc != 0) { free(buf); return false; }
    *out = buf;
    *out_len = 8 + len + 16;
    return true;
}

cJSON *pairing_unwrap_incoming(cJSON *sec_root, int bi) {
    if (bi < 0 || bi >= MAX_BRIDGES) return NULL;
    pairing_conn_t *c = &s_conn[bi];
    if (!c->authenticated) return NULL;
    cJSON *n_item = cJSON_GetObjectItem(sec_root, "n");
    cJSON *ct_item = cJSON_GetObjectItem(sec_root, "ct");
    if (!cJSON_IsNumber(n_item) || !cJSON_IsString(ct_item)) return NULL;
    uint64_t n = (uint64_t)n_item->valuedouble;
    if (n <= c->recv_ctr) { ESP_LOGW(TAG, "bridge %d: sec replay/reorder (n=%llu <= %llu)", bi, (unsigned long long)n, (unsigned long long)c->recv_ctr); return NULL; }
    const char *ct_b64 = ct_item->valuestring;
    size_t b64_len = strlen(ct_b64);
    size_t raw_cap = (b64_len / 4 + 1) * 3;
    uint8_t *raw = (uint8_t *)malloc(raw_cap);
    if (!raw) return NULL;
    int raw_len = b64_decode(ct_b64, raw, raw_cap);
    if (raw_len < 16) { free(raw); ESP_LOGW(TAG, "bridge %d: sec bad base64", bi); return NULL; }
    size_t ct_len = (size_t)raw_len - 16;
    uint8_t *pt = (uint8_t *)malloc(ct_len + 1);
    if (!pt) { free(raw); return NULL; }
    uint8_t iv[12]; counter_iv(n, iv);
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, c->recv_key, 256);
    int rc = mbedtls_gcm_auth_decrypt(&gcm, ct_len, iv, sizeof(iv), NULL, 0, raw + ct_len, 16, raw, pt);
    mbedtls_gcm_free(&gcm);
    free(raw);
    if (rc != 0) { free(pt); ESP_LOGW(TAG, "bridge %d: sec auth-tag FAILED (tampered/wrong key)", bi); return NULL; }
    c->recv_ctr = n;   // advance now: decryption succeeded, so this counter value is "used" regardless of parse outcome below
    pt[ct_len] = 0;
    cJSON *inner = cJSON_Parse((const char *)pt);
    free(pt);
    return inner;
}

bool pairing_unwrap_incoming_binary(int bi, const uint8_t *data, size_t len, uint8_t **out, size_t *out_len) {
    if (bi < 0 || bi >= MAX_BRIDGES || !data || !out || !out_len || len < 8 + 16) return false;
    pairing_conn_t *c = &s_conn[bi];
    if (!c->authenticated) return false;
    uint64_t n = 0;
    for (int i = 0; i < 8; i++) n = (n << 8) | data[i];
    if (n <= c->recv_ctr) { ESP_LOGW(TAG, "bridge %d: binary replay/reorder", bi); return false; }
    size_t ct_len = len - 8 - 16;
    const uint8_t *ct = data + 8;
    const uint8_t *tag = data + 8 + ct_len;
    uint8_t *pt = (uint8_t *)malloc(ct_len ? ct_len : 1);
    if (!pt) return false;
    uint8_t iv[12]; counter_iv(n, iv);
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, c->recv_key, 256);
    int rc = mbedtls_gcm_auth_decrypt(&gcm, ct_len, iv, sizeof(iv), NULL, 0, tag, 16, ct, pt);
    mbedtls_gcm_free(&gcm);
    if (rc != 0) { free(pt); ESP_LOGW(TAG, "bridge %d: binary auth-tag FAILED", bi); return false; }
    c->recv_ctr = n;
    *out = pt;
    *out_len = ct_len;
    return true;
}
