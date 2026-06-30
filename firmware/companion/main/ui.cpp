// Claudeq UI (LVGL 8.4, native portrait 172x640).
// Top: status strip + session chip row. Body: Decide / Macros / Voice / HUD as bottom-tab pages.
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "lvgl.h"
#include "cJSON.h"
#include "esp_log.h"
#include "app.h"

static const char *TAG = "ui";

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

#define SCR_W 172

// status bar
static lv_obj_t *s_dot, *s_state, *s_conn;
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
#define MAX_BRIDGES 4
static char bridge_host[MAX_BRIDGES][24];  // machine name per bridge (shown on a chip only on a title clash)
// pages + bottom nav grid
static lv_obj_t *t_decide, *t_macros, *t_voice, *t_hud;
static lv_obj_t *nav_btns[4], *nav_icons[4], *nav_lbls[4];
static int cur_page = 0;
static const char *NAV_ICON[4] = { LV_SYMBOL_OK, LV_SYMBOL_KEYBOARD, LV_SYMBOL_AUDIO, LV_SYMBOL_LIST };
static const char *NAV_TEXT[4] = { "Decide", "Macros", "Voice", "HUD" };
static void set_page(int idx);
// decide (supports multi-question asks)
static lv_obj_t *s_header, *s_question, *s_opts, *s_placeholder;
static char cur_id[80];
static cJSON *s_ask = NULL;       // cloned questions payload, kept alive across taps
static cJSON *s_answers = NULL;   // accumulated {question: label} across the ask's questions
static int  cur_q_idx = 0, cur_q_total = 0;
// macros
static lv_obj_t *s_macros_cont;
static char cur_macro_id[12][32];
// hud
static lv_obj_t *hud_model, *hud_elapsed, *hud_tool, *hud_todos;
// voice
static lv_obj_t *s_voice_btn, *s_voice_lbl;

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

// ---------- decide (supports multi-question asks) ----------
static void opt_clicked(lv_event_t *e);

static void clear_ask(void) {
    if (s_ask) { cJSON_Delete(s_ask); s_ask = NULL; }
    if (s_answers) { cJSON_Delete(s_answers); s_answers = NULL; }
    cur_q_idx = 0; cur_q_total = 0; cur_id[0] = 0;
    if (s_opts) lv_obj_clean(s_opts);
    if (s_header) lv_obj_add_flag(s_header, LV_OBJ_FLAG_HIDDEN);
    if (s_question) lv_obj_add_flag(s_question, LV_OBJ_FLAG_HIDDEN);
    if (s_placeholder) lv_obj_clear_flag(s_placeholder, LV_OBJ_FLAG_HIDDEN);
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
    set_page(0);  // jump to Decide
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

// ---------- voice ----------
static void voice_evt(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        lv_obj_set_style_bg_color(s_voice_btn, lv_color_hex(0x3a1414), 0);
        lv_label_set_text(s_voice_lbl, "REC...");
        net_send_text("{\"type\":\"voice_start\"}");
        audio_record_start();
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        audio_record_stop();
        lv_obj_set_style_bg_color(s_voice_btn, lv_color_hex(COL_PANEL), 0);
        lv_label_set_text(s_voice_lbl, "HOLD");
        net_send_text("{\"type\":\"voice_end\"}");
    }
}

// ---------- hud ----------
static void show_hud(cJSON *root) {
    cJSON *model = cJSON_GetObjectItem(root, "model");
    cJSON *el = cJSON_GetObjectItem(root, "elapsedMs");
    cJSON *tool = cJSON_GetObjectItem(root, "lastTool");
    cJSON *todos = cJSON_GetObjectItem(root, "todos");
    char buf[96];
    snprintf(buf, sizeof(buf), "model:  %s", cJSON_IsString(model) ? model->valuestring : "-");
    lv_label_set_text(hud_model, buf);
    int secs = cJSON_IsNumber(el) ? (int)(el->valuedouble / 1000.0) : 0;
    snprintf(buf, sizeof(buf), "elapsed:  %ds", secs);
    lv_label_set_text(hud_elapsed, buf);
    snprintf(buf, sizeof(buf), "last tool:  %s", cJSON_IsString(tool) ? tool->valuestring : "-");
    lv_label_set_text(hud_tool, buf);
    int tn = cJSON_IsArray(todos) ? cJSON_GetArraySize(todos) : 0;
    snprintf(buf, sizeof(buf), "todos:  %d", tn);
    lv_label_set_text(hud_todos, buf);
}

// ---------- sessions (chip strip, aggregated across bridges) ----------
static void render_chips(void);

static void chip_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= sess_n || !sess_sid[idx][0]) return;
    snprintf(focus_sid, sizeof(focus_sid), "%s", sess_sid[idx]);
    focus_bridge = sess_bridge[idx];
    net_set_focus_bridge(focus_bridge);              // route macros/voice to this session's bridge
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "focus");
    cJSON_AddStringToObject(m, "sid", sess_sid[idx]);
    char *s = cJSON_PrintUnformatted(m);
    if (s) { net_send_to(focus_bridge, s); ESP_LOGI(TAG, "focus -> b%d %s", focus_bridge, sess_sid[idx]); cJSON_free(s); }
    cJSON_Delete(m);
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
        if (sess_n > 0) { snprintf(focus_sid, sizeof(focus_sid), "%s", sess_sid[0]); focus_bridge = sess_bridge[0]; net_set_focus_bridge(focus_bridge); }
        else { focus_sid[0] = 0; focus_bridge = -1; }
    }
    if (!focus_sid[0] && sess_n > 0) {   // no focus yet -> adopt the first session
        snprintf(focus_sid, sizeof(focus_sid), "%s", sess_sid[0]); focus_bridge = sess_bridge[0]; net_set_focus_bridge(focus_bridge);
    }
    render_chips();
}

void ui_bridge_gone(int bridge) {
    if (!ui_lock(1000)) return;
    drop_bridge_sessions(bridge);
    if (focus_bridge == bridge) { focus_sid[0] = 0; focus_bridge = -1; clear_ask(); }
    render_chips();
    ui_unlock();
}

// ---------- bottom nav (2x2 grid) ----------
static void set_page(int idx) {
    if (idx < 0 || idx > 3) return;
    cur_page = idx;
    lv_obj_t *pgs[4] = { t_decide, t_macros, t_voice, t_hud };
    for (int i = 0; i < 4; i++) {
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
    set_page((int)(intptr_t)lv_event_get_user_data(e));
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
        if (cJSON_IsString(sid)) {           // a question pulls focus to its session, on whichever bridge
            snprintf(focus_sid, sizeof(focus_sid), "%s", sid->valuestring);
            focus_bridge = bridge; net_set_focus_bridge(bridge);
        }
        show_ask(root); set_state("waiting", "tap to choose"); render_chips();
    } else if (!strcmp(t, "ask_cancel")) {
        if (concerns_focus(root, bridge)) clear_ask();
    } else if (!strcmp(t, "hud")) {
        if (concerns_focus(root, bridge)) show_hud(root);
    } else if (!strcmp(t, "macros")) {
        show_macros(root);
    } else if (!strcmp(t, "alert")) {
        cJSON *lvl = cJSON_GetObjectItem(root, "level");
        const char *lv = cJSON_IsString(lvl) ? lvl->valuestring : "info";
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(!strcmp(lv, "error") ? 0x3a1414 : 0x10161f), 0);
    }
    ui_unlock();
}

void ui_set_connection(bool connected) {
    if (!s_conn) return;
    if (!ui_lock(1000)) return;
    lv_label_set_text(s_conn, connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(s_conn, lv_color_hex(connected ? COL_OK : COL_ERR), 0);
    ui_unlock();
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

// A full-screen content page in the area between the chip strip and the bottom nav.
static lv_obj_t *make_page(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, SCR_W, 434);
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_bg_color(p, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 6, 0);
    lv_obj_set_style_pad_gap(p, 0, 0);
    return p;
}

void ui_init(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // --- status bar ---
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, SCR_W, 30);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    s_dot = lv_obj_create(bar);
    lv_obj_set_size(s_dot, 11, 11);
    lv_obj_set_style_radius(s_dot, 6, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_align(s_dot, LV_ALIGN_LEFT_MID, 0, 0);
    s_state = mklabel(bar, "connecting...", &lv_font_montserrat_16, COL_INK);
    lv_obj_set_width(s_state, 116);
    lv_label_set_long_mode(s_state, LV_LABEL_LONG_DOT);
    lv_obj_align(s_state, LV_ALIGN_LEFT_MID, 18, 0);
    s_conn = mklabel(bar, LV_SYMBOL_CLOSE, &lv_font_montserrat_16, COL_ERR);
    lv_obj_align(s_conn, LV_ALIGN_RIGHT_MID, 0, 0);

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

    // --- content pages (one visible at a time) ---
    t_decide = make_page(scr);
    t_macros = make_page(scr);
    t_voice  = make_page(scr);
    t_hud    = make_page(scr);

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
        nav_icons[i] = mklabel(b, NAV_ICON[i], &lv_font_montserrat_16, COL_DIM);
        nav_lbls[i]  = mklabel(b, NAV_TEXT[i], &lv_font_montserrat_12, COL_DIM);
        nav_btns[i]  = b;
    }

    // --- Decide page ---
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
    lv_obj_set_size(s_opts, 158, 340);
    lv_obj_align(s_opts, LV_ALIGN_TOP_LEFT, 0, 74);
    lv_obj_set_style_bg_opa(s_opts, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_opts, 0, 0);
    lv_obj_set_style_pad_all(s_opts, 0, 0);
    lv_obj_set_style_pad_gap(s_opts, 6, 0);
    lv_obj_set_flex_flow(s_opts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_opts, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    s_placeholder = mklabel(t_decide, "Claudeq ready.\nWaiting for a\nquestion...", &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_style_text_align(s_placeholder, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_placeholder);

    // --- Macros page ---
    s_macros_cont = lv_obj_create(t_macros);
    lv_obj_set_size(s_macros_cont, 158, 410);
    lv_obj_align(s_macros_cont, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_macros_cont, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_macros_cont, 0, 0);
    lv_obj_set_style_pad_all(s_macros_cont, 0, 0);
    lv_obj_set_style_pad_gap(s_macros_cont, 7, 0);
    lv_obj_set_flex_flow(s_macros_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_macros_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_macros_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_macros_cont, LV_SCROLLBAR_MODE_AUTO);

    // --- Voice page ---
    s_voice_btn = lv_obj_create(t_voice);
    lv_obj_set_size(s_voice_btn, 120, 120);
    lv_obj_set_style_radius(s_voice_btn, 60, 0);
    style_card(s_voice_btn);
    lv_obj_set_style_border_color(s_voice_btn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(s_voice_btn, 2, 0);
    lv_obj_align(s_voice_btn, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(s_voice_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_voice_btn, voice_evt, LV_EVENT_ALL, NULL);
    s_voice_lbl = mklabel(s_voice_btn, "HOLD", &lv_font_montserrat_16, COL_ACCENT);
    lv_obj_center(s_voice_lbl);
    lv_obj_t *vh = mklabel(t_voice, "Hold to talk " LV_SYMBOL_RIGHT "\ndictated into the\nfocused session.", &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_style_text_align(vh, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(vh, LV_ALIGN_CENTER, 0, 70);

    // --- HUD page ---
    hud_model   = mklabel(t_hud, "model:  -",     &lv_font_montserrat_16, COL_INK);
    hud_elapsed = mklabel(t_hud, "elapsed:  0s",  &lv_font_montserrat_16, COL_INK);
    hud_tool    = mklabel(t_hud, "last tool:  -", &lv_font_montserrat_16, COL_INK);
    hud_todos   = mklabel(t_hud, "todos:  0",     &lv_font_montserrat_16, COL_INK);
    lv_label_set_long_mode(hud_tool, LV_LABEL_LONG_WRAP); lv_obj_set_width(hud_tool, 156);
    lv_obj_align(hud_model,   LV_ALIGN_TOP_LEFT, 0, 6);
    lv_obj_align(hud_elapsed, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_obj_align(hud_tool,    LV_ALIGN_TOP_LEFT, 0, 62);
    lv_obj_align(hud_todos,   LV_ALIGN_TOP_LEFT, 0, 90);
    lv_obj_t *setup_btn = lv_obj_create(t_hud);
    lv_obj_set_size(setup_btn, 152, 46);
    style_card(setup_btn);
    lv_obj_align(setup_btn, LV_ALIGN_TOP_LEFT, 0, 130);
    lv_obj_add_flag(setup_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(setup_btn, setup_btn_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *sl = mklabel(setup_btn, LV_SYMBOL_WIFI "  hold: WiFi setup", &lv_font_montserrat_12, COL_DIM);
    lv_obj_center(sl);

    set_page(0);
    ESP_LOGI(TAG, "ui ready (portrait, 2x2 nav)");
}
