// Claudeq UI (LVGL 8.4, native portrait 172x640).
// Top: status strip + session chip row. Body: Session / Macros / (mic action) / Settings bottom-nav.
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "lvgl.h"
#include "cJSON.h"
#include "esp_log.h"
#include "app.h"
#include "app_config.h"

static const char *TAG = "ui";
LV_FONT_DECLARE(font_mic_18);        // 1-glyph custom font: FontAwesome microphone (U+F130), for the Voice nav icon
#define SYM_MIC "\xEF\x84\xB0"        // UTF-8 for U+F130, rendered with font_mic_18
// Deck text fonts: Montserrat + Latin-1 + typographic punctuation (em-dash, curly quotes, ellipsis,
// bullet, arrow) + the FontAwesome icon glyphs we use — so Claude's replies render real punctuation
// instead of tofu. Replaces lv_font_montserrat_12/16 everywhere below.
LV_FONT_DECLARE(font_deck_16);
LV_FONT_DECLARE(font_deck_12);
LV_FONT_DECLARE(font_back_32);       // big back-chevron (U+F053) for the reader's slim Back button
#define lv_font_montserrat_16 font_deck_16
#define lv_font_montserrat_12 font_deck_12

#define COL_BG      0x0a0b0f
#define COL_PANEL   0x12141c
#define COL_LINE    0x222633
#define COL_INK     0xe9edf5
#define COL_DIM     0x8a93a6
#define COL_ACCENT  0xd6743a
#define COL_BLUE    0x7aa2ff
#define COL_OK      0x46c46a
#define COL_WARN    0xe8b84b
#define COL_ERR     0xef5a5a
#define COL_TSOFF   0x333a4a   // dim "off" dots of the Tailscale logo (much darker than the green lit dots)
#define COL_CLAUDE  0xcb8b6a   // Claude mascot clay/terracotta

#define SCR_W 172

// status bar
static lv_obj_t *s_dot, *s_state, *s_conn;
static lv_obj_t *s_ts_cell = NULL, *s_bridges = NULL;     // top-bar Tailscale cell (hide/show) + bridge count
static lv_obj_t *s_ts_dots[4] = {0};                     // the 4 "lit" dots of the Tailscale logo (recolored by state)
static lv_obj_t *s_botbar = NULL;                         // bottom status strip (dot + state text) above the nav
static lv_obj_t *s_bat_body = NULL, *s_bat_fill = NULL;   // drawn battery gauge
#define BAT_FILL_MAX 18                                   // inner fill width at 100% (px) — spans the body inner width
// sessions (chip strip) — aggregated across every connected bridge
static lv_obj_t *s_sessbar;
#define MAX_SESS 24      // headroom for several bridges (MAX_BRIDGES) x several sessions each
static char sess_sid[MAX_SESS][48];
static char sess_title[MAX_SESS][24];
static bool sess_needs[MAX_SESS];
static int  sess_bridge[MAX_SESS];   // which bridge connection owns each session
static int  sess_n = 0;
static char focus_sid[48];
static int  focus_bridge = -1;       // the focused session lives on this bridge
// MAX_BRIDGES is defined in app.h (shared with net.cpp)
static char bridge_host[MAX_BRIDGES][24];  // machine name per bridge (shown on a chip only on a title clash)
// pages + bottom nav grid. Nav index 2 (Voice) is an ACTION button (starts listening), not a page.
static lv_obj_t *t_decide, *t_macros, *t_settings;
static lv_obj_t *nav_btns[4], *nav_icons[4], *nav_lbls[4];
static int cur_page = 0;
static const char *NAV_ICON[4] = { LV_SYMBOL_OK, LV_SYMBOL_KEYBOARD, SYM_MIC, LV_SYMBOL_SETTINGS };
static const char *NAV_TEXT[4] = { "Session", "Macros", "Voice", "Settings" };
static void set_page(int idx);
static void voice_toggle(void);
// decide (supports multi-question asks)
static lv_obj_t *s_header, *s_question, *s_opts, *s_placeholder;
static lv_obj_t *s_feed;              // live activity feed on the Session page (when no question)
static int s_feed_n = 0;
#define FEED_MAX_ROWS 30
static char cur_id[80];
static cJSON *s_ask = NULL;       // cloned questions payload, kept alive across taps
static cJSON *s_answers = NULL;   // accumulated {question: label} across the ask's questions
static int  cur_q_idx = 0, cur_q_total = 0;
// macros
static lv_obj_t *s_macros_cont;
static char cur_macro_id[12][32];
// hud: a compact one-line telemetry strip pinned to the bottom of the Session page
static lv_obj_t *hud_line;
// voice capture + on-device confirm (tap mic -> record -> transcribe -> Send/Cancel)
typedef enum { VOICE_IDLE, VOICE_REC, VOICE_WAIT, VOICE_CONFIRM } voice_state_t;
static voice_state_t s_vstate = VOICE_IDLE;
static int  s_voice_bridge = -1;     // bridge that owns this capture (transcript/commit must go back to it)
static int  s_voice_id = 0;          // per-capture id; ignores stale transcript messages
static lv_obj_t *s_voice_ov = NULL;  // full-screen overlay (Listening / Transcribing / confirm)
static lv_timer_t *s_voice_timeout = NULL;

static lv_obj_t *mklabel(lv_obj_t *p, const char *t, const lv_font_t *f, uint32_t c) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    return l;
}
static void style_card(lv_obj_t *o) {
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(o, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}
// Explicit on/off colors — no LVGL theme is installed, so the default switch is nearly invisible on the
// dark panel. Off = grey track, on = green fill, knob = light; disabled = dim.
static void style_switch(lv_obj_t *sw) {
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2a2f3a), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_OK), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_INK), LV_PART_KNOB);
}
// Forward decl: a filled-dot icon primitive, defined further down (near make_page) but also needed by
// the Paired Bridges screen, defined earlier in the file (in the Settings-callbacks section).
static lv_obj_t *mkdot(lv_obj_t *parent, int d, uint32_t color);

// ---------- decide (supports multi-question asks) ----------
static void opt_clicked(lv_event_t *e);

// ---------- activity feed (live "what's Claude doing" on the Session page) ----------
// The colored icon is the ONLY kind indicator now (no label word), so glyph + colour must read at a
// glance: you=orange ›, Claude's tool=blue ›, done=green ✓, Claude's reply=green ‹, needs-you=yellow bell.
static const char *feed_sym(const char *kind) {
    if (!kind) return LV_SYMBOL_RIGHT;
    if (!strcmp(kind, "done"))   return LV_SYMBOL_OK;
    if (!strcmp(kind, "reply"))  return LV_SYMBOL_LEFT;
    if (!strcmp(kind, "notify")) return LV_SYMBOL_BELL;
    if (!strcmp(kind, "prompt")) return LV_SYMBOL_RIGHT;
    if (!strcmp(kind, "start"))  return LV_SYMBOL_PLAY;
    return LV_SYMBOL_RIGHT;            // tool
}
static uint32_t feed_col(const char *kind) {
    if (kind && !strcmp(kind, "done"))   return COL_OK;
    if (kind && !strcmp(kind, "reply"))  return COL_OK;
    if (kind && !strcmp(kind, "notify")) return COL_WARN;
    if (kind && !strcmp(kind, "prompt")) return COL_ACCENT;
    if (kind && !strcmp(kind, "start"))  return COL_DIM;
    return COL_BLUE;                  // tool
}
// Show the feed if it has rows, else the idle placeholder (called when no question is pending).
static void show_idle_view(void) {
    if (s_feed_n > 0) {
        if (s_feed) lv_obj_clear_flag(s_feed, LV_OBJ_FLAG_HIDDEN);
        if (s_placeholder) lv_obj_add_flag(s_placeholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (s_feed) lv_obj_add_flag(s_feed, LV_OBJ_FLAG_HIDDEN);
        if (s_placeholder) lv_obj_clear_flag(s_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}
static void feed_clear(void) { if (s_feed) { lv_obj_clean(s_feed); s_feed_n = 0; } }

// ---------- reader: full-text landscape view (tap a reply row) ----------
static lv_obj_t *s_reader_ov = NULL;
static char *s_last_reply = NULL;     // full text of the most recent reply (shown in the reader)
static void reader_close_cb(lv_event_t *e) {
    (void)e;
    if (s_reader_ov) { lv_obj_del(s_reader_ov); s_reader_ov = NULL; }
    app_set_rotation(false);          // back to portrait
}
static void ui_show_reader(const char *text) {
    if (!text || !text[0]) return;
    app_set_rotation(true);           // landscape: logical becomes 640 wide x 172 tall
    lv_coord_t W = lv_disp_get_hor_res(NULL), H = lv_disp_get_ver_res(NULL);
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_reader_ov = ov;
    lv_obj_set_size(ov, W, H);
    lv_obj_align(ov, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 6, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);   // block taps to the (portrait) UI underneath
    // Text fills the left at FULL height; Back is a slim full-height strip on the right, so it doesn't
    // eat the scarce vertical space in landscape.
    lv_obj_t *tc = lv_obj_create(ov);
    lv_obj_set_size(tc, W - 50, H - 12);         // wider text: only the thin 40px Back strip is reserved
    lv_obj_align(tc, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(tc, LV_OPA_0, 0);
    lv_obj_set_style_border_width(tc, 0, 0);
    lv_obj_set_style_pad_all(tc, 0, 0);
    lv_obj_set_scroll_dir(tc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(tc, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t *l = mklabel(tc, text, &lv_font_montserrat_16, COL_INK);
    lv_obj_set_width(l, W - 62);            // ~578px wide lines (vs ~156 in portrait)
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_t *back = lv_obj_create(ov);
    lv_obj_set_size(back, 40, H - 12);           // thin full-height strip, just a big chevron
    style_card(back);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, reader_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mklabel(back, LV_SYMBOL_LEFT, &font_back_32, COL_INK));
}
static void reply_row_clicked(lv_event_t *e) { (void)e; if (s_last_reply) ui_show_reader(s_last_reply); }

static void feed_add_row(const char *kind, const char *label, const char *detail, const char *full, bool current) {
    if (!s_feed) return;
    // cap: drop oldest rows beyond the limit
    while (s_feed_n >= FEED_MAX_ROWS) {
        lv_obj_t *first = lv_obj_get_child(s_feed, 0);
        if (!first) break;
        lv_obj_del(first); s_feed_n--;
    }
    // un-highlight the previous "current" row so only the newest is marked
    uint32_t cnt = lv_obj_get_child_cnt(s_feed);
    if (current && cnt > 0) {
        lv_obj_t *last = lv_obj_get_child(s_feed, cnt - 1);
        if (last) lv_obj_set_style_bg_opa(last, LV_OPA_0, 0);
    }
    lv_obj_t *row = lv_obj_create(s_feed);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_bg_opa(row, current ? LV_OPA_20 : LV_OPA_0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_hor(row, 2, 0);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_style_pad_column(row, 5, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);  // icon at the first text line
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    // Colored icon ONLY — the label word is dropped so the detail keeps (almost) the full width and
    // wraps instead of being squeezed into a narrow column.
    (void)label;
    lv_obj_t *pf = mklabel(row, feed_sym(kind), &lv_font_montserrat_16, feed_col(kind));
    lv_obj_set_width(pf, 20);
    lv_obj_t *dt = mklabel(row, detail ? detail : "", &lv_font_montserrat_12, COL_INK);
    lv_label_set_long_mode(dt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(dt, 128);
    // A reply carries the full text: stash it and make the row tappable -> landscape reader.
    if (kind && !strcmp(kind, "reply") && full && full[0]) {
        free(s_last_reply); s_last_reply = strdup(full);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, reply_row_clicked, LV_EVENT_CLICKED, NULL);
    }
    s_feed_n++;
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);   // keep newest visible
}
static void feed_replace(cJSON *arr) {
    feed_clear();
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    int start = n > FEED_MAX_ROWS ? n - FEED_MAX_ROWS : 0;
    for (int i = start; i < n; i++) {
        cJSON *ln = cJSON_GetArrayItem(arr, i);
        cJSON *k = cJSON_GetObjectItem(ln, "kind");
        cJSON *l = cJSON_GetObjectItem(ln, "label");
        cJSON *d = cJSON_GetObjectItem(ln, "detail");
        cJSON *fu = cJSON_GetObjectItem(ln, "full");
        feed_add_row(cJSON_IsString(k) ? k->valuestring : NULL,
                     cJSON_IsString(l) ? l->valuestring : "",
                     cJSON_IsString(d) ? d->valuestring : "",
                     cJSON_IsString(fu) ? fu->valuestring : NULL, i == n - 1);
    }
}

// Single entry point for changing which (sid,bridge) the deck follows. Wipes the activity feed
// whenever the target actually changes, so a new session never inherits/appends onto the previous
// one's rows. Pass sid=NULL to clear focus. The newly-focused bridge re-sends its feed snapshot.
static void set_local_focus(const char *sid, int bridge) {
    bool changed = sid ? (bridge != focus_bridge || strcmp(focus_sid, sid) != 0)
                       : (focus_sid[0] != 0);
    if (sid) { snprintf(focus_sid, sizeof(focus_sid), "%s", sid); focus_bridge = bridge; net_set_focus_bridge(bridge); }
    else { focus_sid[0] = 0; focus_bridge = -1; }
    if (changed) feed_clear();
}

static void clear_ask(void) {
    if (s_ask) { cJSON_Delete(s_ask); s_ask = NULL; }
    if (s_answers) { cJSON_Delete(s_answers); s_answers = NULL; }
    cur_q_idx = 0; cur_q_total = 0; cur_id[0] = 0;
    if (s_opts) lv_obj_clean(s_opts);
    if (s_header) lv_obj_add_flag(s_header, LV_OBJ_FLAG_HIDDEN);
    if (s_question) lv_obj_add_flag(s_question, LV_OBJ_FLAG_HIDDEN);
    show_idle_view();                 // back to the activity feed (or placeholder if empty)
}

// Render the question at cur_q_idx of the kept-alive s_ask payload.
static void render_question(void) {
    if (!s_ask) return;
    cJSON *qs = cJSON_GetObjectItem(s_ask, "questions");
    cJSON *q = cJSON_GetArrayItem(qs, cur_q_idx);
    if (!q) return;
    cJSON *question = cJSON_GetObjectItem(q, "question");
    cJSON *header = cJSON_GetObjectItem(q, "header");
    cJSON *opts = cJSON_GetObjectItem(q, "options");

    lv_obj_add_flag(s_placeholder, LV_OBJ_FLAG_HIDDEN);
    if (s_feed) lv_obj_add_flag(s_feed, LV_OBJ_FLAG_HIDDEN);   // question takes over the screen
    const char *htext = cJSON_IsString(header) ? header->valuestring : "QUESTION";
    char hbuf[56];
    if (cur_q_total > 1) snprintf(hbuf, sizeof(hbuf), "%s  (%d/%d)", htext, cur_q_idx + 1, cur_q_total);
    else snprintf(hbuf, sizeof(hbuf), "%s", htext);
    lv_label_set_text(s_header, hbuf);
    lv_obj_clear_flag(s_header, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_question, cJSON_IsString(question) ? question->valuestring : "?");
    lv_obj_clear_flag(s_question, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clean(s_opts);
    int n = cJSON_IsArray(opts) ? cJSON_GetArraySize(opts) : 0;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_GetArrayItem(opts, i);
        cJSON *lab = cJSON_GetObjectItem(o, "label");
        cJSON *des = cJSON_GetObjectItem(o, "description");
        lv_obj_t *btn = lv_obj_create(s_opts);
        lv_obj_set_size(btn, LV_PCT(100), LV_SIZE_CONTENT);
        style_card(btn);
        lv_obj_set_style_pad_all(btn, 8, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, opt_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *ll = mklabel(btn, cJSON_IsString(lab) ? lab->valuestring : "?", &lv_font_montserrat_16, COL_INK);
        lv_obj_set_width(ll, LV_PCT(100));
        if (cJSON_IsString(des) && des->valuestring[0]) {
            lv_obj_t *dl = mklabel(btn, des->valuestring, &lv_font_montserrat_12, COL_DIM);
            lv_label_set_long_mode(dl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(dl, LV_PCT(100));
        }
    }
}

static void send_answers(void) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "answer");
    cJSON_AddStringToObject(m, "id", cur_id);
    cJSON_AddItemToObject(m, "answers", s_answers ? cJSON_Duplicate(s_answers, 1) : cJSON_CreateObject());
    char *s = cJSON_PrintUnformatted(m);
    if (s) { net_send_text(s); ESP_LOGI(TAG, "answer -> %s", s); cJSON_free(s); }
    cJSON_Delete(m);
}

static void opt_clicked(lv_event_t *e) {
    if (!s_ask) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    cJSON *qs = cJSON_GetObjectItem(s_ask, "questions");
    cJSON *q = cJSON_GetArrayItem(qs, cur_q_idx);
    cJSON *opts = cJSON_GetObjectItem(q, "options");
    cJSON *o = cJSON_GetArrayItem(opts, idx);
    if (!o) return;
    cJSON *question = cJSON_GetObjectItem(q, "question");
    cJSON *lab = cJSON_GetObjectItem(o, "label");
    if (s_answers) cJSON_AddStringToObject(s_answers,
        cJSON_IsString(question) ? question->valuestring : "?",
        cJSON_IsString(lab) ? lab->valuestring : "?");
    cur_q_idx++;
    if (cur_q_idx < cur_q_total) render_question();   // more questions in this ask -> show the next
    else { send_answers(); clear_ask(); }
}

static void show_ask(cJSON *root) {
    cJSON *qs = cJSON_GetObjectItem(root, "questions");
    if (!cJSON_IsArray(qs) || cJSON_GetArraySize(qs) == 0) return;
    cJSON *id = cJSON_GetObjectItem(root, "id");
    clear_ask();                        // free any prior ask
    s_ask = cJSON_Duplicate(root, 1);   // keep the payload alive across taps (caller frees root)
    if (!s_ask) return;
    s_answers = cJSON_CreateObject();
    snprintf(cur_id, sizeof(cur_id), "%s", cJSON_IsString(id) ? id->valuestring : "ask");
    cur_q_total = cJSON_GetArraySize(cJSON_GetObjectItem(s_ask, "questions"));
    cur_q_idx = 0;
    render_question();
    set_page(0);  // jump to Session
}

// ---------- macros ----------
static void macro_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= 12 || !cur_macro_id[idx][0]) return;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "macro");
    cJSON_AddStringToObject(m, "id", cur_macro_id[idx]);
    char *s = cJSON_PrintUnformatted(m);
    if (s) { net_send_text(s); ESP_LOGI(TAG, "macro -> %s", s); cJSON_free(s); }
    cJSON_Delete(m);
}
static void show_macros(cJSON *root) {
    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) return;
    lv_obj_clean(s_macros_cont);
    memset(cur_macro_id, 0, sizeof(cur_macro_id));
    int n = cJSON_GetArraySize(items);
    if (n > 12) n = 12;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(items, i);
        cJSON *id = cJSON_GetObjectItem(it, "id");
        cJSON *lab = cJSON_GetObjectItem(it, "label");
        snprintf(cur_macro_id[i], sizeof(cur_macro_id[i]), "%s", cJSON_IsString(id) ? id->valuestring : "");
        lv_obj_t *btn = lv_obj_create(s_macros_cont);
        lv_obj_set_size(btn, LV_PCT(100), 52);
        style_card(btn);
        lv_obj_set_style_pad_all(btn, 6, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, macro_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = mklabel(btn, cJSON_IsString(lab) ? lab->valuestring : "?", &lv_font_montserrat_16, COL_INK);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
    }
}

// ---------- voice (tap mic -> record -> on-device confirm -> Send/Cancel) ----------
static void voice_close_overlay(void) {
    if (s_voice_timeout) { lv_timer_del(s_voice_timeout); s_voice_timeout = NULL; }
    if (s_voice_ov) { lv_obj_del(s_voice_ov); s_voice_ov = NULL; }
}
static void voice_reset(void) {   // back to IDLE: drop the overlay, un-redden the mic icon
    voice_close_overlay();
    s_vstate = VOICE_IDLE;
    if (nav_icons[2]) lv_obj_set_style_text_color(nav_icons[2], lv_color_hex(COL_DIM), 0);
}
static void voice_send_msg(const char *type) {   // {"type":..,"id":N} back to the capture's own bridge
    if (s_voice_bridge < 0) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"id\":%d}", type, s_voice_id);
    net_send_to(s_voice_bridge, buf);
}
// Full-screen overlay with a status/transcript body and up to two action buttons (bottom-stacked).
static void voice_render(const char *body, uint32_t body_col,
                         const char *b1, lv_event_cb_t c1,
                         const char *b2, lv_event_cb_t c2) {
    voice_close_overlay();
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_voice_ov = ov;
    lv_obj_set_size(ov, SCR_W, 640);
    lv_obj_align(ov, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 12, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);   // eat taps so they don't fall through to the nav below
    lv_obj_t *t = mklabel(ov, SYM_MIC, &font_mic_18, COL_ACCENT);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);
    // scrollable body area (a long transcript won't overrun the buttons)
    lv_obj_t *bc = lv_obj_create(ov);
    lv_obj_set_size(bc, 150, b1 ? 468 : 560);
    lv_obj_align(bc, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_opa(bc, LV_OPA_0, 0);
    lv_obj_set_style_border_width(bc, 0, 0);
    lv_obj_set_style_pad_all(bc, 0, 0);
    lv_obj_set_scroll_dir(bc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(bc, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t *l = mklabel(bc, body, &lv_font_montserrat_16, body_col);
    lv_obj_set_width(l, 146);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (b1) {
        lv_obj_t *btn = lv_obj_create(ov);
        lv_obj_set_size(btn, 148, 52);
        style_card(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, c1, LV_EVENT_CLICKED, NULL);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, b2 ? -66 : -8);
        lv_obj_center(mklabel(btn, b1, &lv_font_montserrat_16, COL_INK));
    }
    if (b2) {
        lv_obj_t *btn = lv_obj_create(ov);
        lv_obj_set_size(btn, 148, 52);
        style_card(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, c2, LV_EVENT_CLICKED, NULL);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_center(mklabel(btn, b2, &lv_font_montserrat_16, COL_INK));
    }
}
static void voice_commit_cb(lv_event_t *e)  { (void)e; voice_send_msg("voice_commit"); voice_reset(); }
static void voice_cancel_cb(lv_event_t *e)  { (void)e; voice_send_msg("voice_cancel"); voice_reset(); }
static void voice_timeout_cb(lv_timer_t *t) {   // no transcript came back in time
    (void)t; s_voice_timeout = NULL;
    if (s_vstate != VOICE_WAIT) return;
    s_vstate = VOICE_CONFIRM;
    voice_render("Transcription\nfailed.", COL_ERR, "Dismiss", voice_cancel_cb, NULL, NULL);
}
static void voice_stop_cb(lv_event_t *e) {   // "Stop" in the Listening overlay
    (void)e;
    if (s_vstate != VOICE_REC) return;
    audio_record_stop();
    voice_send_msg("voice_end");
    s_vstate = VOICE_WAIT;
    voice_render("Transcribing...", COL_DIM, NULL, NULL, NULL, NULL);
    s_voice_timeout = lv_timer_create(voice_timeout_cb, 15000, NULL);
    lv_timer_set_repeat_count(s_voice_timeout, 1);
}
static void voice_notice_cb(lv_event_t *e) { (void)e; voice_reset(); }   // dismiss the no-session notice
// mic nav tap: start listening (only from IDLE; the overlay covers the nav while active).
static void voice_toggle(void) {
    if (s_vstate != VOICE_IDLE) return;
    if (focus_bridge < 0 || !focus_sid[0]) {   // no session to dictate into -> tell the user, don't sit silent
        s_voice_bridge = -1;
        voice_render("No active session.\nStart Claude with\n'claudeq' first.", COL_WARN, "OK", voice_notice_cb, NULL, NULL);
        return;
    }
    s_voice_bridge = focus_bridge;                   // PCM already routes here (net_send_binary -> focus bridge)
    s_voice_id++;
    s_vstate = VOICE_REC;
    if (nav_icons[2]) lv_obj_set_style_text_color(nav_icons[2], lv_color_hex(COL_ERR), 0);
    voice_send_msg("voice_start");
    audio_record_start();
    voice_render("Listening...", COL_OK, "Stop", voice_stop_cb, NULL, NULL);
}
// Called from ui_handle_message when a transcript arrives for this capture.
static void voice_on_transcript(const char *text) {
    if (s_voice_timeout) { lv_timer_del(s_voice_timeout); s_voice_timeout = NULL; }
    s_vstate = VOICE_CONFIRM;
    if (text && text[0]) voice_render(text, COL_INK, "Send", voice_commit_cb, "Cancel", voice_cancel_cb);
    else                 voice_render("(no speech)", COL_DIM, "Dismiss", voice_cancel_cb, NULL, NULL);
}

// ---------- hud: compact telemetry strip at the bottom of the Session page ----------
static void show_hud(cJSON *root) {
    if (!hud_line) return;
    cJSON *model = cJSON_GetObjectItem(root, "model");
    cJSON *el = cJSON_GetObjectItem(root, "elapsedMs");
    cJSON *tool = cJSON_GetObjectItem(root, "lastTool");
    cJSON *todos = cJSON_GetObjectItem(root, "todos");
    int secs = cJSON_IsNumber(el) ? (int)(el->valuedouble / 1000.0) : 0;
    int tn = cJSON_IsArray(todos) ? cJSON_GetArraySize(todos) : 0;
    char buf[128];   // ASCII-only separators (the device font has no middle-dot glyph)
    snprintf(buf, sizeof(buf), "%s | %ds | %s | %d todo",
             cJSON_IsString(model) && model->valuestring[0] ? model->valuestring : "-", secs,
             cJSON_IsString(tool) && tool->valuestring[0] ? tool->valuestring : "-", tn);
    lv_label_set_text(hud_line, buf);
}

// ---------- sessions (chip strip, aggregated across bridges) ----------
static void render_chips(void);

static void chip_clicked(lv_event_t *e) {
    if (s_vstate == VOICE_REC) return;   // don't move focus mid-capture (mic PCM routes to the focus bridge)
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= sess_n || !sess_sid[idx][0]) return;
    set_local_focus(sess_sid[idx], sess_bridge[idx]);   // clears the feed on switch; routes macros/voice
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "focus");
    cJSON_AddStringToObject(m, "sid", sess_sid[idx]);
    char *s = cJSON_PrintUnformatted(m);
    if (s) { net_send_to(focus_bridge, s); ESP_LOGI(TAG, "focus -> b%d %s", focus_bridge, sess_sid[idx]); cJSON_free(s); }
    cJSON_Delete(m);
    if (!s_ask) show_idle_view();   // feed was cleared by set_local_focus; its bridge will resend it
    render_chips();
}

static void render_chips(void) {
    lv_obj_clean(s_sessbar);
    for (int i = 0; i < sess_n; i++) {
        bool focused = focus_sid[0] && sess_bridge[i] == focus_bridge && !strcmp(sess_sid[i], focus_sid);
        uint32_t border = focused ? COL_ACCENT : (sess_needs[i] ? COL_WARN : COL_LINE);
        uint32_t fg     = focused ? COL_INK    : (sess_needs[i] ? COL_WARN : COL_DIM);
        // only label the machine when this title also exists on a *different* bridge (a real clash)
        bool clash = false;
        for (int j = 0; j < sess_n; j++)
            if (j != i && sess_bridge[j] != sess_bridge[i] && !strcmp(sess_title[j], sess_title[i])) { clash = true; break; }
        lv_obj_t *chip = lv_obj_create(s_sessbar);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, 34);
        lv_obj_set_style_bg_color(chip, lv_color_hex(focused ? 0x2a1c12 : COL_PANEL), 0);
        lv_obj_set_style_border_color(chip, lv_color_hex(border), 0);
        lv_obj_set_style_border_width(chip, focused ? 2 : 1, 0);
        lv_obj_set_style_radius(chip, 16, 0);
        lv_obj_set_style_pad_hor(chip, 14, 0);
        lv_obj_set_style_pad_ver(chip, 0, 0);
        lv_obj_set_style_pad_column(chip, 5, 0);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chip, chip_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        mklabel(chip, sess_title[i], &lv_font_montserrat_16, fg);
        if (clash && bridge_host[sess_bridge[i]][0]) {
            char hb[26]; snprintf(hb, sizeof(hb), "@%s", bridge_host[sess_bridge[i]]);
            mklabel(chip, hb, &lv_font_montserrat_12, COL_DIM);
        }
    }
}

// drop the rows belonging to `bridge`, compacting the flat arrays in place
static void drop_bridge_sessions(int bridge) {
    int w = 0;
    for (int i = 0; i < sess_n; i++) {
        if (sess_bridge[i] == bridge) continue;
        if (w != i) {
            memcpy(sess_sid[w], sess_sid[i], sizeof(sess_sid[0]));
            memcpy(sess_title[w], sess_title[i], sizeof(sess_title[0]));
            sess_needs[w] = sess_needs[i];
            sess_bridge[w] = sess_bridge[i];
        }
        w++;
    }
    sess_n = w;
}

static void show_sessions(cJSON *root, int bridge) {
    cJSON *list = cJSON_GetObjectItem(root, "list");
    cJSON *host = cJSON_GetObjectItem(root, "host");
    if (cJSON_IsString(host) && bridge >= 0 && bridge < MAX_BRIDGES)
        snprintf(bridge_host[bridge], sizeof(bridge_host[0]), "%s", host->valuestring);
    drop_bridge_sessions(bridge);                    // replace this bridge's slice
    int n = cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0;
    for (int i = 0; i < n && sess_n < MAX_SESS; i++) {
        cJSON *it = cJSON_GetArrayItem(list, i);
        cJSON *sid = cJSON_GetObjectItem(it, "sid");
        cJSON *title = cJSON_GetObjectItem(it, "title");
        cJSON *needs = cJSON_GetObjectItem(it, "needs");
        snprintf(sess_sid[sess_n], sizeof(sess_sid[0]), "%s", cJSON_IsString(sid) ? sid->valuestring : "");
        snprintf(sess_title[sess_n], sizeof(sess_title[0]), "%s", cJSON_IsString(title) ? title->valuestring : "?");
        sess_needs[sess_n] = cJSON_IsTrue(needs);
        sess_bridge[sess_n] = bridge;
        sess_n++;
    }
    // Is the focused session still present? When THIS update is from the focus's own bridge, trust the
    // incoming list (authoritative, never truncated); otherwise check the merged array.
    bool found = false;
    if (focus_sid[0] && bridge == focus_bridge) {
        for (int i = 0; i < n; i++) {
            cJSON *sid = cJSON_GetObjectItem(cJSON_GetArrayItem(list, i), "sid");
            if (cJSON_IsString(sid) && !strcmp(sid->valuestring, focus_sid)) { found = true; break; }
        }
    } else if (focus_sid[0]) {
        for (int i = 0; i < sess_n; i++) if (sess_bridge[i] == focus_bridge && !strcmp(sess_sid[i], focus_sid)) { found = true; break; }
    }
    // Only the focus's own bridge may invalidate its focus, and never wipe a live ask here
    // (that's the job of ask_cancel / ui_bridge_gone). An unrelated bridge's refresh must not touch focus.
    if (!found && focus_sid[0] && bridge == focus_bridge && !s_ask) {
        if (sess_n > 0) set_local_focus(sess_sid[0], sess_bridge[0]);
        else set_local_focus(NULL, -1);
    }
    if (!focus_sid[0] && sess_n > 0)     // no focus yet -> adopt the first session
        set_local_focus(sess_sid[0], sess_bridge[0]);
    render_chips();
}

void ui_bridge_gone(int bridge) {
    if (!ui_lock(1000)) return;
    if (s_vstate != VOICE_IDLE && bridge == s_voice_bridge) voice_reset();   // capture's bridge vanished
    drop_bridge_sessions(bridge);
    if (focus_bridge == bridge) { set_local_focus(NULL, -1); clear_ask(); }
    render_chips();
    ui_unlock();
}

// ---------- bottom nav (2x2 grid) ----------
static void set_page(int idx) {
    if (idx < 0 || idx > 3 || idx == 2) return;   // 2 = mic action button, not a page
    cur_page = idx;
    lv_obj_t *pgs[4] = { t_decide, t_macros, NULL, t_settings };
    for (int i = 0; i < 4; i++) {
        if (i == 2) continue;                     // mic button keeps its own (idle/recording) icon color
        if (pgs[i]) { if (i == idx) lv_obj_clear_flag(pgs[i], LV_OBJ_FLAG_HIDDEN);
                      else lv_obj_add_flag(pgs[i], LV_OBJ_FLAG_HIDDEN); }
        bool a = (i == idx);
        if (nav_btns[i])  lv_obj_set_style_border_color(nav_btns[i], lv_color_hex(a ? COL_ACCENT : COL_LINE), 0);
        if (nav_btns[i])  lv_obj_set_style_bg_color(nav_btns[i], lv_color_hex(a ? 0x1f1610 : COL_PANEL), 0);
        if (nav_icons[i]) lv_obj_set_style_text_color(nav_icons[i], lv_color_hex(a ? COL_ACCENT : COL_DIM), 0);
        if (nav_lbls[i])  lv_obj_set_style_text_color(nav_lbls[i], lv_color_hex(a ? COL_INK : COL_DIM), 0);
    }
}
static void nav_clicked(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == 2) { voice_toggle(); return; }       // mic: start listening instead of switching pages
    set_page(i);
}

// ---------- status ----------
static void set_state(const char *state, const char *text) {
    uint32_t c = COL_DIM;
    if (!strcmp(state, "thinking") || !strcmp(state, "working")) c = COL_BLUE;
    else if (!strcmp(state, "waiting")) c = COL_WARN;
    else if (!strcmp(state, "done")) c = COL_OK;
    else if (!strcmp(state, "error")) c = COL_ERR;
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(c), 0);
    lv_label_set_text(s_state, text && text[0] ? text : state);
}

// Does a sid-tagged message concern the currently displayed (focused) session?
static bool concerns_focus(cJSON *root, int bridge) {
    if (bridge != focus_bridge || !focus_sid[0]) return false;
    cJSON *sid = cJSON_GetObjectItem(root, "sid");
    return !cJSON_IsString(sid) || !strcmp(sid->valuestring, focus_sid);
}

void ui_handle_message(cJSON *root, int bridge) {
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) return;
    const char *t = type->valuestring;
    if (!ui_lock(1000)) { ESP_LOGW(TAG, "ui_lock timeout, drop %s", t); return; }
    if (!strcmp(t, "status")) {
        if (concerns_focus(root, bridge)) {
            cJSON *st = cJSON_GetObjectItem(root, "state");
            cJSON *tx = cJSON_GetObjectItem(root, "text");
            set_state(cJSON_IsString(st) ? st->valuestring : "idle", cJSON_IsString(tx) ? tx->valuestring : NULL);
        }
    } else if (!strcmp(t, "sessions")) {
        show_sessions(root, bridge);
    } else if (!strcmp(t, "ask")) {
        cJSON *sid = cJSON_GetObjectItem(root, "sid");
        if (cJSON_IsString(sid))             // a question pulls focus to its session, on whichever bridge
            set_local_focus(sid->valuestring, bridge);   // clears stale feed; bridge resends snapshot after the ask
        show_ask(root); set_state("waiting", "tap to choose"); render_chips();
        app_wake_for_event();                // a question needs you -> wake the screen from standby
    } else if (!strcmp(t, "ask_cancel")) {
        if (concerns_focus(root, bridge)) clear_ask();
    } else if (!strcmp(t, "hud")) {
        if (concerns_focus(root, bridge)) show_hud(root);
    } else if (!strcmp(t, "transcript")) {   // voice: bridge returned the transcript for confirmation
        cJSON *id = cJSON_GetObjectItem(root, "id");
        cJSON *txt = cJSON_GetObjectItem(root, "text");
        if (s_vstate == VOICE_WAIT && bridge == s_voice_bridge &&
            cJSON_IsNumber(id) && (int)id->valuedouble == s_voice_id)
            voice_on_transcript(cJSON_IsString(txt) ? txt->valuestring : "");
    } else if (!strcmp(t, "activity")) {
        if (concerns_focus(root, bridge)) {
            cJSON *feed = cJSON_GetObjectItem(root, "feed");
            cJSON *line = cJSON_GetObjectItem(root, "line");
            if (cJSON_IsArray(feed)) {
                feed_replace(feed);
            } else if (line) {
                cJSON *k = cJSON_GetObjectItem(line, "kind");
                cJSON *l = cJSON_GetObjectItem(line, "label");
                cJSON *d = cJSON_GetObjectItem(line, "detail");
                cJSON *fu = cJSON_GetObjectItem(line, "full");
                const char *kind = cJSON_IsString(k) ? k->valuestring : NULL;
                feed_add_row(kind,
                             cJSON_IsString(l) ? l->valuestring : "",
                             cJSON_IsString(d) ? d->valuestring : "",
                             cJSON_IsString(fu) ? fu->valuestring : NULL, true);
                // Wake on the "something new" lines (Claude replied / finished / is calling for you); stay
                // dark through routine tool-use churn (tool/prompt/start) so standby actually saves power.
                if (kind && (!strcmp(kind, "reply") || !strcmp(kind, "done") || !strcmp(kind, "notify")))
                    app_wake_for_event();
            }
            if (!s_ask) show_idle_view();
        }
    } else if (!strcmp(t, "macros")) {
        show_macros(root);
    } else if (!strcmp(t, "alert")) {
        cJSON *lvl = cJSON_GetObjectItem(root, "level");
        const char *lv = cJSON_IsString(lvl) ? lvl->valuestring : "info";
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(!strcmp(lv, "error") ? 0x3a1414 : 0x10161f), 0);
        // Built-in tone: the bridge omits `sound` when it streams a custom PCM clip instead, so the two
        // never double-play. With no bridge-side sound files (the default install), this IS the alert sound.
        cJSON *snd = cJSON_GetObjectItem(root, "sound");
        if (cJSON_IsString(snd) && snd->valuestring[0]) audio_play_alert(snd->valuestring);
        app_wake_for_event();                // an alert wants your attention -> wake the screen
    }
    ui_unlock();
}

// True while a question overlay is up: the power task keeps the screen awake so the ask can't be missed.
bool ui_has_pending_ask(void) { return s_ask != NULL; }

void ui_set_net_status(bool wifi_up, int bridges, bool ts_configured, bool ts_up) {
    if (!s_conn) return;
    if (!ui_lock(1000)) return;
    // WiFi icon reflects the actual STA/IP link state -- independent of bridges/pairing, so it stays
    // accurate even with WiFi up but zero reachable bridges (e.g. right after a fresh pairing wipe).
    lv_label_set_text(s_conn, wifi_up ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(s_conn, lv_color_hex(wifi_up ? COL_OK : COL_ERR), 0);
    if (s_ts_cell) {                                // Tailscale logo: shown only when configured
        if (ts_configured) lv_obj_clear_flag(s_ts_cell, LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_add_flag(s_ts_cell, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 4; i++)                     // light the logo's lit dots green when the tailnet is up
        if (s_ts_dots[i]) lv_obj_set_style_bg_color(s_ts_dots[i], lv_color_hex(ts_up ? COL_OK : COL_TSOFF), 0);
    if (s_bridges) {                                // bridge count: its own signal, independent of the WiFi icon
        char b[12]; snprintf(b, sizeof(b), "%d", bridges);
        lv_label_set_text(s_bridges, b);
        lv_obj_set_style_text_color(s_bridges, lv_color_hex(bridges > 0 ? COL_INK : COL_DIM), 0);
    }
    // With no session in focus there's no per-session status, so the strip would keep its stale "connecting..."
    // init text even while online — reflect the actual WiFi link state instead.
    if (s_state && !focus_sid[0])
        lv_label_set_text(s_state, wifi_up ? "ready - no session" : "connecting...");
    ui_unlock();
}

bool ui_set_battery(int pct, bool charging, bool present) {
    if (!s_bat_body || !s_bat_fill) return false;    // UI not built yet -> caller retries
    if (!ui_lock(1000)) return false;
    if (!present) {                       // no reading yet -> keep the gauge hidden
        lv_obj_add_flag(s_bat_body, LV_OBJ_FLAG_HIDDEN);
        ui_unlock();
        return true;
    }
    lv_obj_clear_flag(s_bat_body, LV_OBJ_FLAG_HIDDEN);
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    int w = 1 + (BAT_FILL_MAX - 1) * pct / 100;      // keep a sliver visible even near 0%
    lv_obj_set_width(s_bat_fill, w);
    uint32_t col = pct <= 15 ? COL_ERR : (pct <= 35 ? COL_WARN : COL_OK);
    lv_obj_set_style_bg_color(s_bat_fill, lv_color_hex(col), 0);
    (void)charging;   // no reliable USB/charge signal on this board (VBAT barely moves; SYS_OUT is constant)
    ui_unlock();
    return true;
}

// Full-screen overlay shown while the SoftAP setup portal is up.
void ui_show_setup(const char *ap_ssid, const char *ap_ip) {
    if (!ui_lock(2000)) return;
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov, SCR_W, 640);
    lv_obj_align(ov, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 12, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = mklabel(ov, LV_SYMBOL_WIFI "  WiFi setup", &lv_font_montserrat_16, COL_ACCENT);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);
    char buf[220];
    snprintf(buf, sizeof(buf), "On your phone:\n\n1. Join WiFi\n\"%s\"\n\n2. Open\nhttp://%s\n\n3. Enter your\nWiFi + bridge IP", ap_ssid, ap_ip);
    lv_obj_t *l = mklabel(ov, buf, &lv_font_montserrat_16, COL_INK);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 148);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 14);
    ui_unlock();
}
static void setup_btn_cb(lv_event_t *e) { (void)e; net_enter_setup(); }   // long-press -> reboot into portal

// ---------- settings ----------
static lv_obj_t *s_modal = NULL;
static void (*s_modal_yes)(void) = NULL;
static void modal_close(void) { if (s_modal) { lv_obj_del(s_modal); s_modal = NULL; } s_modal_yes = NULL; }
static void modal_yes_cb(lv_event_t *e) { (void)e; void (*fn)(void) = s_modal_yes; modal_close(); if (fn) fn(); }
static void modal_no_cb(lv_event_t *e)  { (void)e; modal_close(); }
static void show_modal(const char *title, const char *body, const char *yes, void (*onyes)(void)) {
    modal_close();
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_modal = ov;
    lv_obj_set_size(ov, SCR_W, 640);
    lv_obj_align(ov, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 12, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);   // eat taps so they don't fall through to the nav below
    lv_obj_t *tl = mklabel(ov, title, &lv_font_montserrat_16, COL_ACCENT);
    lv_obj_align(tl, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *l = mklabel(ov, body, &lv_font_montserrat_16, COL_INK);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 148);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 40);
    s_modal_yes = onyes;
    lv_obj_t *y = lv_obj_create(ov);
    lv_obj_set_size(y, 148, 52); style_card(y);
    lv_obj_add_flag(y, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(y, modal_yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(y, LV_ALIGN_BOTTOM_MID, 0, -66);
    lv_obj_center(mklabel(y, yes, &lv_font_montserrat_16, COL_ERR));
    lv_obj_t *no = lv_obj_create(ov);
    lv_obj_set_size(no, 148, 52); style_card(no);
    lv_obj_add_flag(no, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(no, modal_no_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(no, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_center(mklabel(no, "Cancel", &lv_font_montserrat_16, COL_INK));
}
// ---------- OTA overlay ----------
// Opened from Settings "Check for update". A polling lv_timer reflects the ota.cpp state machine
// onto the widgets; the single action button is "Update" (available) or "Close" (up-to-date/error),
// and is hidden while checking/downloading/rebooting so a flash can't be interrupted from the screen.
static lv_obj_t *s_ota_ov = NULL, *s_ota_msg = NULL, *s_ota_bar = NULL, *s_ota_btn = NULL, *s_ota_btnlbl = NULL;
static lv_timer_t *s_ota_timer = NULL;
static uint32_t s_ota_elapsed_ms = 0;   // watchdog: surface a timeout if the check never returns (e.g. DNS hang)
#define OTA_CHECK_TIMEOUT_MS 15000
static void ota_close(void) {
    if (s_ota_timer) { lv_timer_del(s_ota_timer); s_ota_timer = NULL; }
    if (s_ota_ov) { lv_obj_del(s_ota_ov); s_ota_ov = NULL; }
    s_ota_msg = s_ota_bar = s_ota_btn = s_ota_btnlbl = NULL;
}
static void ota_action_cb(lv_event_t *e) {
    (void)e;
    if (ota_get_state() == OTA_AVAILABLE) ota_start();   // begin download+flash (overlay stays, polls progress)
    else ota_close();                                    // up-to-date / error -> dismiss
}
static void ota_tick(lv_timer_t *t) {
    (void)t;
    if (!s_ota_ov) return;
    char buf[96];
    ota_state_t st = ota_get_state();
    bool show_bar = false, show_btn = false;
    const char *btntxt = "Close";
    // Watchdog: the network task can stall on DNS (not bounded by the HTTP timeout). If we're still
    // checking after OTA_CHECK_TIMEOUT_MS, tell the user instead of spinning forever.
    s_ota_elapsed_ms += 250;
    if ((st == OTA_CHECKING || st == OTA_IDLE) && s_ota_elapsed_ms >= OTA_CHECK_TIMEOUT_MS) {
        lv_label_set_text(s_ota_msg, "Check timed out.\nNo internet?");
        lv_obj_add_flag(s_ota_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ota_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ota_btnlbl, "Close");
        return;
    }
    switch (st) {
        case OTA_CHECKING:    strlcpy(buf, "Checking for\nupdates\xE2\x80\xA6", sizeof(buf)); break;
        case OTA_UPTODATE:    snprintf(buf, sizeof(buf), "Up to date\nv%s", DEVICE_FW); show_btn = true; break;
        case OTA_AVAILABLE:   snprintf(buf, sizeof(buf), "Update available\nv%s " LV_SYMBOL_RIGHT " v%s", DEVICE_FW, ota_get_avail_version()); show_btn = true; btntxt = "Update"; break;
        case OTA_DOWNLOADING: snprintf(buf, sizeof(buf), "Downloading\xE2\x80\xA6\n%d%%", ota_get_pct()); show_bar = true; break;
        case OTA_REBOOTING:   strlcpy(buf, "Installed.\nRebooting\xE2\x80\xA6", sizeof(buf)); show_bar = true; break;
        case OTA_ERROR:       snprintf(buf, sizeof(buf), "Update failed:\n%s", ota_get_error()); show_btn = true; break;
        default:              return;
    }
    lv_label_set_text(s_ota_msg, buf);
    if (show_bar) { lv_obj_clear_flag(s_ota_bar, LV_OBJ_FLAG_HIDDEN); lv_bar_set_value(s_ota_bar, ota_get_pct(), LV_ANIM_OFF); }
    else lv_obj_add_flag(s_ota_bar, LV_OBJ_FLAG_HIDDEN);
    if (show_btn) { lv_obj_clear_flag(s_ota_btn, LV_OBJ_FLAG_HIDDEN); lv_label_set_text(s_ota_btnlbl, btntxt); }
    else lv_obj_add_flag(s_ota_btn, LV_OBJ_FLAG_HIDDEN);
}
// Invoked from the Settings "Check for update" click callback, which already runs under the LVGL
// lock (lv_timer_handler holds it) — so DO NOT take ui_lock here (the mutex is non-recursive; taking
// it would deadlock-timeout and the overlay would never appear). Same convention as show_modal().
void ui_show_ota(void) {
    ota_close();
    s_ota_elapsed_ms = 0;
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_ota_ov = ov;
    lv_obj_set_size(ov, SCR_W, 640);
    lv_obj_align(ov, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 12, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);   // eat taps so they don't fall through to the nav

    lv_obj_t *tl = mklabel(ov, "Firmware", &lv_font_montserrat_16, COL_ACCENT);
    lv_obj_align(tl, LV_ALIGN_TOP_MID, 0, 6);

    s_ota_msg = mklabel(ov, "Checking\xE2\x80\xA6", &lv_font_montserrat_16, COL_INK);
    lv_obj_set_style_text_align(s_ota_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_ota_msg, 148);
    lv_label_set_long_mode(s_ota_msg, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_ota_msg, LV_ALIGN_CENTER, 0, -30);

    s_ota_bar = lv_bar_create(ov);
    lv_obj_set_size(s_ota_bar, 140, 14);
    lv_obj_align(s_ota_bar, LV_ALIGN_CENTER, 0, 24);
    lv_bar_set_range(s_ota_bar, 0, 100);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(COL_OK), LV_PART_INDICATOR);
    lv_obj_add_flag(s_ota_bar, LV_OBJ_FLAG_HIDDEN);

    s_ota_btn = lv_obj_create(ov);
    lv_obj_set_size(s_ota_btn, 148, 52); style_card(s_ota_btn);
    lv_obj_add_flag(s_ota_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ota_btn, ota_action_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(s_ota_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    s_ota_btnlbl = mklabel(s_ota_btn, "Close", &lv_font_montserrat_16, COL_INK);
    lv_obj_center(s_ota_btnlbl);
    lv_obj_add_flag(s_ota_btn, LV_OBJ_FLAG_HIDDEN);

    s_ota_timer = lv_timer_create(ota_tick, 250, NULL);
    ota_check_async();   // kick the version check; the timer reflects its progress
}
static void checkupd_btn_cb(lv_event_t *e) { (void)e; ui_show_ota(); }

static void do_wifi_off(void) { net_wifi_set_enabled(false); }
static void wifi_sw_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (on) { net_wifi_set_enabled(true); return; }        // enabling: persist + reboot back online
    lv_obj_add_state(sw, LV_STATE_CHECKED);                // keep it visually ON until the user confirms
    show_modal("WiFi", "Disable WiFi?\nThe deck goes\noffline until you\nre-enable it here.", "Disable", do_wifi_off);
}
static void tailscale_sw_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    net_tailnet_set_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));   // live, no reboot
}
static void standby_sw_cb(lv_event_t *e) {
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    cfg_set_auto_standby(on ? 1 : 0);
    app_set_auto_standby(on);                                         // live, no reboot
}
static void sounds_sw_cb(lv_event_t *e) {
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);   // ON = sounds play
    cfg_set_sound_enabled(on ? 1 : 0);
    audio_set_muted(!on);                                            // live, no reboot
}

// ---------- pairing: SAS confirm overlay (auto-popup, either side can start the ceremony) ----------
// Backed by pairing.cpp's global ceremony state machine (PAIR_IDLE..PAIR_FAILED) — this timer just
// reflects it onto widgets, the same "state machine lives in a .cpp, UI polls it" idiom as ota_tick.
// Runs from the moment ui_init() finishes for the device's whole lifetime (not opened/closed like the
// OTA overlay), since a bridge-initiated pairing request (an operator running `claudeq pair`) can arrive
// at any time, from any screen.
static lv_obj_t *s_pair_ov = NULL, *s_pair_msg = NULL, *s_pair_code = NULL, *s_pair_confirm_btn = NULL, *s_pair_reject_btn = NULL;
static int s_pair_done_ticks = 0;
static void pair_confirm_cb(lv_event_t *e) { (void)e; pairing_confirm(); }
static void pair_reject_cb(lv_event_t *e)  { (void)e; pairing_reject(); }
static void pairing_overlay_close(void) {
    if (s_pair_ov) { lv_obj_del(s_pair_ov); s_pair_ov = NULL; }
    s_pair_msg = s_pair_code = s_pair_confirm_btn = s_pair_reject_btn = NULL;
}
static void pairing_overlay_open(void) {
    if (s_pair_ov) return;
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_pair_ov = ov;
    lv_obj_set_size(ov, SCR_W, 640);
    lv_obj_align(ov, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 12, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);   // eat taps so they don't fall through to the nav below

    lv_obj_t *tl = mklabel(ov, "Pair bridge", &lv_font_montserrat_16, COL_ACCENT);
    lv_obj_align(tl, LV_ALIGN_TOP_MID, 0, 6);

    s_pair_msg = mklabel(ov, "", &lv_font_montserrat_16, COL_INK);
    lv_obj_set_style_text_align(s_pair_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_pair_msg, 148);
    lv_label_set_long_mode(s_pair_msg, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_pair_msg, LV_ALIGN_CENTER, 0, -70);

    s_pair_code = mklabel(ov, "", &lv_font_montserrat_16, COL_ACCENT);
    lv_obj_align(s_pair_code, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(s_pair_code, LV_OBJ_FLAG_HIDDEN);

    s_pair_confirm_btn = lv_obj_create(ov);
    lv_obj_set_size(s_pair_confirm_btn, 148, 52); style_card(s_pair_confirm_btn);
    lv_obj_add_flag(s_pair_confirm_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pair_confirm_btn, pair_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(s_pair_confirm_btn, LV_ALIGN_BOTTOM_MID, 0, -66);
    lv_obj_center(mklabel(s_pair_confirm_btn, "Confirm", &lv_font_montserrat_16, COL_OK));
    lv_obj_add_flag(s_pair_confirm_btn, LV_OBJ_FLAG_HIDDEN);

    s_pair_reject_btn = lv_obj_create(ov);
    lv_obj_set_size(s_pair_reject_btn, 148, 52); style_card(s_pair_reject_btn);
    lv_obj_add_flag(s_pair_reject_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pair_reject_btn, pair_reject_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(s_pair_reject_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_center(mklabel(s_pair_reject_btn, "Reject", &lv_font_montserrat_16, COL_ERR));
    lv_obj_add_flag(s_pair_reject_btn, LV_OBJ_FLAG_HIDDEN);
}
static void pairing_watch_tick(lv_timer_t *t) {
    (void)t;
    pairing_state_t st = pairing_get_state();   // also lazily applies the ceremony timeout
    if (st != PAIR_DONE && st != PAIR_FAILED) s_pair_done_ticks = 0;
    switch (st) {
    case PAIR_IDLE:
        if (s_pair_ov) pairing_overlay_close();
        break;
    case PAIR_WAIT_RESPONSE:
        pairing_overlay_open();
        lv_label_set_text(s_pair_msg, "Waiting for the\nbridge to respond\xE2\x80\xA6");
        lv_obj_add_flag(s_pair_code, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pair_confirm_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pair_reject_btn, LV_OBJ_FLAG_HIDDEN);
        break;
    case PAIR_SHOW_SAS:
        pairing_overlay_open();
        lv_label_set_text(s_pair_msg, "Confirm this code\nmatches the bridge:");
        lv_label_set_text(s_pair_code, pairing_get_code());
        lv_obj_clear_flag(s_pair_code, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_pair_confirm_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_pair_reject_btn, LV_OBJ_FLAG_HIDDEN);
        break;
    case PAIR_WAIT_PEER:
        pairing_overlay_open();
        lv_label_set_text(s_pair_msg, "Waiting for the\nbridge to confirm\xE2\x80\xA6");
        lv_obj_add_flag(s_pair_confirm_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pair_reject_btn, LV_OBJ_FLAG_HIDDEN);
        break;
    case PAIR_DONE:
        pairing_overlay_open();
        lv_label_set_text(s_pair_msg, "Paired!");
        lv_obj_add_flag(s_pair_code, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pair_confirm_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pair_reject_btn, LV_OBJ_FLAG_HIDDEN);
        if (++s_pair_done_ticks > 6) pairing_dismiss();   // ~1.5s at the 250ms tick rate, then back to idle
        break;
    case PAIR_FAILED:
        pairing_overlay_open();
        lv_label_set_text(s_pair_msg, "Pairing failed\nor was rejected.");
        lv_obj_add_flag(s_pair_code, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pair_confirm_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pair_reject_btn, LV_OBJ_FLAG_HIDDEN);
        if (++s_pair_done_ticks > 6) pairing_dismiss();
        break;
    }
}

// ---------- Paired Bridges screen: list (with live-connected dot) + pair/disconnect/forget ----------
static lv_obj_t *s_pb_ov = NULL;
static void ui_show_paired_bridges(void);
static void pb_close(void) { if (s_pb_ov) { lv_obj_del(s_pb_ov); s_pb_ov = NULL; } }
static void pb_close_cb(lv_event_t *e) { (void)e; pb_close(); }
static int find_bridge_slot_by_id(const char *bridge_id) {
    if (!bridge_id || !bridge_id[0]) return -1;
    int n = net_bridge_count();
    for (int i = 0; i < n; i++) {
        bool used = false, connected = false; char bid[40] = {0}, host[40] = {0};
        if (!net_bridge_info(i, &used, &connected, bid, sizeof(bid), host, sizeof(host))) continue;
        if (used && bid[0] && !strcmp(bid, bridge_id)) return i;
    }
    return -1;
}
static void pb_disconnect_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0) net_disconnect_bridge(idx);
    ui_show_paired_bridges();   // rebuild to reflect the change
}
static void pb_forget_cb(lv_event_t *e) {
    const char *bridge_id = (const char *)lv_event_get_user_data(e);
    int slot = find_bridge_slot_by_id(bridge_id);
    if (slot >= 0) net_disconnect_bridge(slot);
    trust_forget(bridge_id);
    ui_show_paired_bridges();   // rebuild
}
static void pb_pair_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    pb_close();                // one overlay at a time -- the SAS confirm overlay takes over from here
    pairing_start_as_device(idx);
}
static void ui_show_paired_bridges(void) {
    pb_close();
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_pb_ov = ov;
    lv_obj_set_size(ov, SCR_W, 640);
    lv_obj_align(ov, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 10, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *tl = mklabel(ov, "Paired Bridges", &lv_font_montserrat_16, COL_ACCENT);
    lv_obj_align(tl, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *list = lv_obj_create(ov);
    lv_obj_set_size(list, 150, 500);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_bg_opa(list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_gap(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    // bridge_id strings must outlive this function for the Forget button callbacks -- a small bounded
    // static table (MAX_TRUSTED_BRIDGES is tiny) reused across rebuilds, not a per-call heap allocation.
    static char forget_ids[MAX_TRUSTED_BRIDGES][40];
    for (int i = 0; i < MAX_TRUSTED_BRIDGES; i++) {
        trust_bridge_t entry;
        if (!trust_get(i, &entry)) continue;
        int slot = find_bridge_slot_by_id(entry.bridge_id);
        bool live = slot >= 0 && pairing_is_authenticated(slot);

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, 150, 74); style_card(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(row, 6, 0);

        lv_obj_t *dot = mkdot(row, 8, live ? COL_OK : COL_DIM);
        lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 0, 3);
        lv_obj_t *lbl = mklabel(row, entry.label, &lv_font_montserrat_12, COL_INK);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 14, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, 120);

        lv_obj_t *disc = lv_obj_create(row);
        lv_obj_set_size(disc, 70, 28); style_card(disc);
        lv_obj_align(disc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_flag(disc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(disc, pb_disconnect_cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
        lv_obj_center(mklabel(disc, "Disc.", &lv_font_montserrat_12, COL_INK));

        lv_obj_t *forget = lv_obj_create(row);
        lv_obj_set_size(forget, 70, 28); style_card(forget);
        lv_obj_align(forget, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_add_flag(forget, LV_OBJ_FLAG_CLICKABLE);
        snprintf(forget_ids[i], sizeof(forget_ids[i]), "%s", entry.bridge_id);
        lv_obj_add_event_cb(forget, pb_forget_cb, LV_EVENT_CLICKED, forget_ids[i]);
        lv_obj_center(mklabel(forget, "Forget", &lv_font_montserrat_12, COL_ERR));
    }

    // discovered-but-unpaired bridges -- tap to start the pairing ceremony
    int n = net_bridge_count();
    for (int i = 0; i < n; i++) {
        bool used = false, connected = false; char bid[40] = {0}, host[40] = {0};
        if (!net_bridge_info(i, &used, &connected, bid, sizeof(bid), host, sizeof(host))) continue;
        if (!used || !connected || !bid[0] || trust_find(bid) >= 0) continue;   // already paired, or not ready yet
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, 150, 54); style_card(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, pb_pair_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = mklabel(row, "New bridge", &lv_font_montserrat_12, COL_INK);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *tap = mklabel(row, "tap to pair \xE2\x86\x92", &lv_font_montserrat_12, COL_ACCENT);
        lv_obj_align(tap, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    if (lv_obj_get_child_cnt(list) == 0) {
        lv_obj_t *ph = mklabel(list, "No bridges paired\nyet. Bridges you're\nconnected to will\nappear here to pair.", &lv_font_montserrat_12, COL_DIM);
        lv_label_set_long_mode(ph, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ph, 148);
    }

    lv_obj_t *close = lv_obj_create(ov);
    lv_obj_set_size(close, 148, 46); style_card(close);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close, pb_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_center(mklabel(close, "Close", &lv_font_montserrat_16, COL_INK));
}
static void pb_open_cb(lv_event_t *e) { (void)e; ui_show_paired_bridges(); }

// A full-screen content page in the area between the chip strip and the bottom status strip.
static lv_obj_t *make_page(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, SCR_W, 406);   // chip strip ends at 74; bottom status strip starts at 480
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_bg_color(p, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 6, 0);
    lv_obj_set_style_pad_gap(p, 0, 0);
    // The page is a static frame — only its inner content containers (feed / macros / options) scroll.
    // Leaving it scrollable made its AUTO scrollbar show permanently (inner containers overflow the
    // 394 px content box by a few px), and doubled up with the inner bar once content overflowed.
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

// A transparent, borderless, non-scrollable container — used as an evenly-spaced top-bar cell.
static lv_obj_t *mkcell(lv_obj_t *parent, int w, int h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_opa(c, LV_OPA_0, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

// A small solid rectangle (icon primitive).
static lv_obj_t *mkrect(lv_obj_t *parent, int w, int h, uint32_t color) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

// A filled dot (icon primitive).
static lv_obj_t *mkdot(lv_obj_t *parent, int d, uint32_t color) {
    lv_obj_t *o = mkrect(parent, d, d, color);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    return o;
}

// Brief "Locked" flash shown right before the screen blanks on a BOOT long-press (pocket mode). The deck
// font has no padlock glyph, so the lock is drawn from primitives (a ring shackle behind a rounded body).
// The power task shows it, lets it render, then blanks; ui_show_lock_notice(false) tears it down on unlock.
static lv_obj_t *s_lock_ov = NULL;
void ui_show_lock_notice(bool show) {
    if (!ui_lock(1000)) return;
    if (s_lock_ov) { lv_obj_del(s_lock_ov); s_lock_ov = NULL; }
    if (show) {
        lv_obj_t *ov = lv_obj_create(lv_layer_top());
        s_lock_ov = ov;
        lv_obj_set_size(ov, SCR_W, 640);
        lv_obj_align(ov, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(ov, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_border_width(ov, 0, 0);
        lv_obj_set_style_radius(ov, 0, 0);
        lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);   // eat any stray tap during the flash

        // padlock shackle: a ring (border only) whose lower arc the body covers
        lv_obj_t *sh = lv_obj_create(ov);
        lv_obj_set_size(sh, 30, 30);
        lv_obj_set_style_radius(sh, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(sh, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sh, 5, 0);
        lv_obj_set_style_border_color(sh, lv_color_hex(COL_WARN), 0);
        lv_obj_set_style_pad_all(sh, 0, 0);
        lv_obj_clear_flag(sh, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(sh, LV_ALIGN_CENTER, 0, -34);

        // padlock body (drawn after the shackle so it sits on top of the ring's lower half)
        lv_obj_t *body = mkrect(ov, 46, 36, COL_WARN);
        lv_obj_set_style_radius(body, 7, 0);
        lv_obj_align(body, LV_ALIGN_CENTER, 0, -12);
        lv_obj_t *kh = mkdot(body, 8, COL_BG);        // keyhole
        lv_obj_align(kh, LV_ALIGN_CENTER, 0, -1);

        lv_obj_t *t = mklabel(ov, "Locked", &lv_font_montserrat_16, COL_INK);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 24);
        lv_obj_t *h = mklabel(ov, "hold to unlock", &lv_font_montserrat_12, COL_DIM);
        lv_obj_align(h, LV_ALIGN_CENTER, 0, 48);
    }
    ui_unlock();
}


// Draw the simplified Claude mascot (clay body, two eyes, side ears, little legs) into an 18x15 canvas cell.
static void draw_mascot(lv_obj_t *canvas) {
    lv_obj_t *body = mkrect(canvas, 14, 11, COL_CLAUDE);   // rounded body, centred; ears poke out the sides
    lv_obj_set_style_radius(body, 2, 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_align(mkrect(canvas, 2, 4, COL_CLAUDE), LV_ALIGN_TOP_LEFT, 0, 4);    // left ear
    lv_obj_align(mkrect(canvas, 2, 4, COL_CLAUDE), LV_ALIGN_TOP_RIGHT, 0, 4);   // right ear
    lv_obj_align(mkrect(canvas, 2, 4, 0x000000), LV_ALIGN_TOP_LEFT, 5, 3);      // left eye
    lv_obj_align(mkrect(canvas, 2, 4, 0x000000), LV_ALIGN_TOP_LEFT, 11, 3);     // right eye
    for (int i = 0; i < 4; i++)                                                 // four little legs
        lv_obj_align(mkrect(canvas, 2, 3, COL_CLAUDE), LV_ALIGN_TOP_LEFT, 3 + i * 4, 11);
}

void ui_init(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // --- top bar: four icons evenly spaced, left->right: bridge count, Tailscale (tunnel), WiFi, battery ---
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, SCR_W, 30);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // (1) bridge count — the Claude mascot followed by the number of connected bridges
    lv_obj_t *br_cell = mkcell(bar, 36, 26);
    lv_obj_set_flex_flow(br_cell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(br_cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(br_cell, 3, 0);
    draw_mascot(mkcell(br_cell, 18, 15));
    s_bridges = mklabel(br_cell, "0", &lv_font_montserrat_16, COL_DIM);

    // (2) Tailscale — its 3x3 dot logo; the 4 "lit" dots (middle row + bottom-centre) turn green when up,
    // dim when configured-but-down. Whole cell hidden when Tailscale isn't configured.
    s_ts_cell = mkcell(bar, 18, 26);
    lv_obj_t *grid = mkcell(s_ts_cell, 16, 16);
    lv_obj_align(grid, LV_ALIGN_CENTER, 0, 0);
    int di = 0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            lv_obj_t *dot = mkdot(grid, 4, COL_TSOFF);
            lv_obj_align(dot, LV_ALIGN_TOP_LEFT, c * 6, r * 6);
            if ((r == 1 || (r == 2 && c == 1)) && di < 4) s_ts_dots[di++] = dot;      // the lit pattern
        }
    lv_obj_add_flag(s_ts_cell, LV_OBJ_FLAG_HIDDEN);

    // (3) WiFi link
    lv_obj_t *wifi_cell = mkcell(bar, 18, 26);
    s_conn = mklabel(wifi_cell, LV_SYMBOL_CLOSE, &lv_font_montserrat_16, COL_ERR);
    lv_obj_center(s_conn);

    // (4) battery gauge (drawn with rects; the deck font lacks the FontAwesome battery glyphs) — body outline
    // + terminal nub + an inner fill whose width/colour reflect charge. Hidden until the first reading.
    lv_obj_t *bat_cell = mkcell(bar, 26, 26);
    s_bat_body = lv_obj_create(bat_cell);
    lv_obj_set_size(s_bat_body, 22, 12);
    lv_obj_set_style_radius(s_bat_body, 2, 0);
    lv_obj_set_style_bg_opa(s_bat_body, LV_OPA_0, 0);
    lv_obj_set_style_border_color(s_bat_body, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_border_width(s_bat_body, 1, 0);
    lv_obj_set_style_pad_all(s_bat_body, 0, 0);
    lv_obj_clear_flag(s_bat_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_bat_body, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(s_bat_body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align_to(mkrect(bat_cell, 2, 6, COL_DIM), s_bat_body, LV_ALIGN_OUT_RIGHT_MID, 0, 0);   // + terminal
    s_bat_fill = mkrect(s_bat_body, BAT_FILL_MAX, 8, COL_OK);
    lv_obj_set_style_radius(s_bat_fill, 1, 0);
    lv_obj_align(s_bat_fill, LV_ALIGN_LEFT_MID, 1, 0);

    // --- bottom status strip: session state dot + text, sitting directly on top of the nav ---
    s_botbar = lv_obj_create(scr);
    lv_obj_set_size(s_botbar, SCR_W, 28);
    lv_obj_align(s_botbar, LV_ALIGN_BOTTOM_MID, 0, -132);   // nav is the bottom 132px
    lv_obj_set_style_bg_color(s_botbar, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(s_botbar, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(s_botbar, 1, 0);
    lv_obj_set_style_border_side(s_botbar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(s_botbar, 0, 0);
    lv_obj_set_style_pad_hor(s_botbar, 8, 0);
    lv_obj_clear_flag(s_botbar, LV_OBJ_FLAG_SCROLLABLE);
    s_dot = lv_obj_create(s_botbar);
    lv_obj_set_size(s_dot, 10, 10);
    lv_obj_set_style_radius(s_dot, 5, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_align(s_dot, LV_ALIGN_LEFT_MID, 0, 0);
    s_state = mklabel(s_botbar, "connecting...", &lv_font_montserrat_16, COL_INK);
    lv_obj_set_width(s_state, 140);
    lv_label_set_long_mode(s_state, LV_LABEL_LONG_DOT);
    lv_obj_align(s_state, LV_ALIGN_LEFT_MID, 16, 0);

    // --- session chip strip (bigger, horizontally scrollable) ---
    s_sessbar = lv_obj_create(scr);
    lv_obj_set_size(s_sessbar, SCR_W, 44);
    lv_obj_align(s_sessbar, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(s_sessbar, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(s_sessbar, 0, 0);
    lv_obj_set_style_pad_all(s_sessbar, 5, 0);
    lv_obj_set_style_pad_gap(s_sessbar, 5, 0);
    lv_obj_set_flex_flow(s_sessbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sessbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_sessbar, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_sessbar, LV_SCROLLBAR_MODE_OFF);

    // --- content pages (one visible at a time; nav index 2 is the mic action, no page) ---
    t_decide   = make_page(scr);
    t_macros   = make_page(scr);
    t_settings = make_page(scr);

    // --- bottom nav: 2x2 grid of big buttons ---
    lv_obj_t *nav = lv_obj_create(scr);
    lv_obj_set_size(nav, SCR_W, 132);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nav, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_color(nav, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(nav, 1, 0);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_style_pad_all(nav, 6, 0);
    lv_obj_set_style_pad_gap(nav, 6, 0);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(nav);
        lv_obj_set_size(b, 74, 54);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_PANEL), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(COL_LINE), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_pad_all(b, 2, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(b, nav_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        const lv_font_t *icf = (i == 2) ? &font_mic_18 : &lv_font_montserrat_16;   // mic glyph needs its own font
        nav_icons[i] = mklabel(b, NAV_ICON[i], icf, COL_DIM);
        nav_lbls[i]  = mklabel(b, NAV_TEXT[i], &lv_font_montserrat_12, COL_DIM);
        nav_btns[i]  = b;
    }

    // --- Session page (question/answer + activity feed + telemetry strip) ---
    s_header = mklabel(t_decide, "QUESTION", &lv_font_montserrat_12, COL_ACCENT);
    lv_obj_align(s_header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_header, LV_OBJ_FLAG_HIDDEN);
    s_question = lv_label_create(t_decide);
    lv_obj_set_style_text_font(s_question, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_question, lv_color_hex(COL_INK), 0);
    lv_label_set_long_mode(s_question, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_question, 156);
    lv_obj_align(s_question, LV_ALIGN_TOP_LEFT, 0, 16);
    lv_obj_add_flag(s_question, LV_OBJ_FLAG_HIDDEN);
    s_opts = lv_obj_create(t_decide);
    lv_obj_set_size(s_opts, 158, 320);   // fits the page content box (394) below the question (y=74)
    lv_obj_align(s_opts, LV_ALIGN_TOP_LEFT, 0, 74);
    lv_obj_set_style_bg_opa(s_opts, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_opts, 0, 0);
    lv_obj_set_style_pad_all(s_opts, 0, 0);
    lv_obj_set_style_pad_gap(s_opts, 6, 0);
    lv_obj_set_flex_flow(s_opts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_opts, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    s_placeholder = mklabel(t_decide, "Claudeq ready.\nFollowing\nClaude...", &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_style_text_align(s_placeholder, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_placeholder);
    // live activity feed — fills the Session page; shown when idle (no question) and has rows
    s_feed = lv_obj_create(t_decide);
    lv_obj_set_size(s_feed, 160, 394);   // fills the page content box; scrolls internally
    lv_obj_align(s_feed, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_feed, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_feed, 0, 0);
    lv_obj_set_style_pad_all(s_feed, 0, 0);
    lv_obj_set_style_pad_row(s_feed, 1, 0);
    lv_obj_set_flex_flow(s_feed, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_feed, LV_OBJ_FLAG_HIDDEN);
    // (model/elapsed/tool telemetry strip removed — the session state now lives in the bottom status strip;
    // hud_line stays NULL so show_hud() is a no-op.)

    // --- Macros page ---
    s_macros_cont = lv_obj_create(t_macros);
    lv_obj_set_size(s_macros_cont, 158, 394);   // fills the page content box; scrolls internally
    lv_obj_align(s_macros_cont, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_macros_cont, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_macros_cont, 0, 0);
    lv_obj_set_style_pad_all(s_macros_cont, 0, 0);
    lv_obj_set_style_pad_gap(s_macros_cont, 7, 0);
    lv_obj_set_flex_flow(s_macros_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_macros_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_macros_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_macros_cont, LV_SCROLLBAR_MODE_AUTO);

    // --- Settings page (WiFi / Tailscale toggles + on-demand WiFi portal) ---
    bool wifi_en = true, ts_en = true, ts_has_key = false;
    net_get_flags(&wifi_en, &ts_en, &ts_has_key);
    lv_obj_t *wl = mklabel(t_settings, "WiFi", &lv_font_montserrat_16, COL_INK);
    lv_obj_align(wl, LV_ALIGN_TOP_LEFT, 0, 10);
    lv_obj_t *wsw = lv_switch_create(t_settings);
    style_switch(wsw);
    lv_obj_align(wsw, LV_ALIGN_TOP_RIGHT, 0, 4);
    if (wifi_en) lv_obj_add_state(wsw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(wsw, wifi_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *tsl = mklabel(t_settings, "Tailscale", &lv_font_montserrat_16, COL_INK);
    lv_obj_align(tsl, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_t *tsw = lv_switch_create(t_settings);
    style_switch(tsw);
    lv_obj_align(tsw, LV_ALIGN_TOP_RIGHT, 0, 52);
    if (ts_en && ts_has_key) lv_obj_add_state(tsw, LV_STATE_CHECKED);
    if (!ts_has_key) {
        lv_obj_add_state(tsw, LV_STATE_DISABLED);
        lv_obj_t *th = mklabel(t_settings, "add an auth key in WiFi setup", &lv_font_montserrat_12, COL_DIM);
        lv_label_set_long_mode(th, LV_LABEL_LONG_WRAP); lv_obj_set_width(th, 156);
        lv_obj_align(th, LV_ALIGN_TOP_LEFT, 0, 82);
    }
    lv_obj_add_event_cb(tsw, tailscale_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Auto screen-off + Sounds toggles (both live, no reboot)
    bool auto_standby = true, sound_en = true;
    net_get_prefs(&auto_standby, &sound_en);
    lv_obj_t *stl = mklabel(t_settings, "Auto sleep", &lv_font_montserrat_16, COL_INK);
    lv_obj_align(stl, LV_ALIGN_TOP_LEFT, 0, 118);
    lv_obj_t *stsw = lv_switch_create(t_settings);
    style_switch(stsw);
    lv_obj_align(stsw, LV_ALIGN_TOP_RIGHT, 0, 112);
    if (auto_standby) lv_obj_add_state(stsw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(stsw, standby_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *sndl = mklabel(t_settings, "Sounds", &lv_font_montserrat_16, COL_INK);
    lv_obj_align(sndl, LV_ALIGN_TOP_LEFT, 0, 162);
    lv_obj_t *sndsw = lv_switch_create(t_settings);
    style_switch(sndsw);
    lv_obj_align(sndsw, LV_ALIGN_TOP_RIGHT, 0, 156);
    if (sound_en) lv_obj_add_state(sndsw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sndsw, sounds_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *setup_btn = lv_obj_create(t_settings);
    lv_obj_set_size(setup_btn, 152, 46);
    style_card(setup_btn);
    lv_obj_align(setup_btn, LV_ALIGN_TOP_LEFT, 0, 208);
    lv_obj_add_flag(setup_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(setup_btn, setup_btn_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_center(mklabel(setup_btn, LV_SYMBOL_WIFI "  hold: WiFi portal", &lv_font_montserrat_12, COL_DIM));

    lv_obj_t *upd_btn = lv_obj_create(t_settings);
    lv_obj_set_size(upd_btn, 152, 46);
    style_card(upd_btn);
    lv_obj_align(upd_btn, LV_ALIGN_TOP_LEFT, 0, 262);
    lv_obj_add_flag(upd_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(upd_btn, checkupd_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mklabel(upd_btn, "Check for update", &lv_font_montserrat_12, COL_INK));

    lv_obj_t *pb_btn = lv_obj_create(t_settings);
    lv_obj_set_size(pb_btn, 152, 46);
    style_card(pb_btn);
    lv_obj_align(pb_btn, LV_ALIGN_TOP_LEFT, 0, 316);
    lv_obj_add_flag(pb_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pb_btn, pb_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mklabel(pb_btn, "Paired Bridges", &lv_font_montserrat_12, COL_INK));

    lv_obj_t *fw = mklabel(t_settings, "claudeq  v" DEVICE_FW, &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(fw, LV_ALIGN_BOTTOM_MID, 0, -2);

    set_page(0);
    lv_timer_create(pairing_watch_tick, 250, NULL);   // persistent: a bridge-initiated pairing can arrive from any screen
    ESP_LOGI(TAG, "ui ready (portrait, 2x2 nav)");
}
