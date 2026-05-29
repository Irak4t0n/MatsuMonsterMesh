// SPDX-License-Identifier: GPL-3.0-or-later

#include "MeshtasticChatView.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/display.h"
#include "bsp/input.h"
}
#include "pax_fonts.h"
#include "pax_text.h"

#include "FastText.h"
#include "meshtastic_proto.h"
#include "mqtt_transport.h"

static const char *TAG = "mchat";

// ── Layout constants (logical px) ─────────────────────────────────────
static constexpr int      FONT_PX        = 12;
static constexpr int      LINE_PX        = 14;
static constexpr int      MARGIN_X       = 6;
static constexpr int      TITLE_H        = 20;
static constexpr int      COMPOSE_H      = 20;
static constexpr int      HELP_H         = 16;
static constexpr int      BLINK_MS       = 500;
static constexpr int      STATUS_HOLD_MS = 2500;

static constexpr uint32_t COLOR_BG       = 0xFF000000;
static constexpr uint32_t COLOR_TITLE    = 0xFF00FFAA;   // mint
static constexpr uint32_t COLOR_DIVIDER  = 0xFF303030;
static constexpr uint32_t COLOR_SELF     = 0xFFFFFF66;   // pale yellow — our messages
static constexpr uint32_t COLOR_PEER     = 0xFFE0E0E0;
static constexpr uint32_t COLOR_NAME     = 0xFF66CCFF;   // sky blue — sender label
static constexpr uint32_t COLOR_TIME     = 0xFF707070;
static constexpr uint32_t COLOR_PROMPT   = 0xFFFFFF00;
static constexpr uint32_t COLOR_COMPOSE  = 0xFFFFFFFF;
static constexpr uint32_t COLOR_CURSOR   = 0xFFFFFF00;
static constexpr uint32_t COLOR_HELP     = 0xFF505050;
static constexpr uint32_t COLOR_STATUS   = 0xFF66FF66;
static constexpr uint32_t COLOR_PANEL_BG = 0xFF101010;
static constexpr uint32_t COLOR_PANEL_HL = 0xFF00FF88;   // active channel highlight
static constexpr uint32_t COLOR_PANEL_DIM= 0xFF606060;

// Width reserved for the right-side channel panel
static constexpr int      PANEL_W        = 180;

static inline uint32_t now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ──────────────────────────────────────────────────────────────────────

void MeshtasticChatView::setRenderTarget(pax_buf_t *fb, int canvas_w, int canvas_h)
{
    fb_       = fb;
    canvas_w_ = canvas_w;
    canvas_h_ = canvas_h;
    dirty_    = true;
    fast_text_init(fb);
}

void MeshtasticChatView::setInputQueue(QueueHandle_t q)
{
    input_q_ = q;
}

void MeshtasticChatView::begin()
{
    compose_len_  = 0;
    compose_[0]   = '\0';
    scroll_offset_= 0;
    cursor_on_    = true;
    blink_ms_     = now_ms();
    dirty_        = true;
    render();
}

// ── Input handling ────────────────────────────────────────────────────

void MeshtasticChatView::handleInput()
{
    if (!input_q_) return;

    // Drain everything queued. Each event either toggles state or
    // mutates the compose buffer; we mark dirty and let render() do
    // the work.
    bsp_input_event_t ev;
    while (xQueueReceive(input_q_, &ev, 0) == pdTRUE) {
        if (ev.type == INPUT_EVENT_TYPE_KEYBOARD) {
            // Fn+T from within the chat → jump straight to the terminal.
            // Main.c picks this up via wantsTerminalJump() and routes.
            if ((ev.args_keyboard.modifiers & BSP_INPUT_MODIFIER_FUNCTION) &&
                (ev.args_keyboard.ascii == 't' || ev.args_keyboard.ascii == 'T')) {
                wants_terminal_ = true;
                continue;
            }
            onKeyboard((char)ev.args_keyboard.ascii,
                       ev.args_keyboard.modifiers);
        } else if (ev.type == INPUT_EVENT_TYPE_NAVIGATION) {
            // Only act on key-down (state != 0). Without this, release
            // events would re-fire Enter and submit an empty compose
            // every keystroke. Same pattern MatsuMonsterTerminal uses.
            if (ev.args_navigation.state) {
                onNavigation(ev.args_navigation.key);
            }
        }
    }
}

void MeshtasticChatView::onKeyboard(char ascii, uint32_t modifiers)
{
    // Fn+1..Fn+8: switch active TX channel
    if ((modifiers & BSP_INPUT_MODIFIER_FUNCTION) && ascii >= '1' && ascii <= '8') {
        uint8_t idx = (uint8_t)(ascii - '1');
        const meshtastic_channel_t *ch = meshtastic_channel_get(idx);
        if (ch) {
            meshtastic_channel_set_tx(idx);
            setStatus("Channel: %s", ch->name);
        } else {
            setStatus("Channel %d: empty", idx + 1);
        }
        dirty_ = true;
        return;
    }

    // Enter and Esc come through both as keyboard ASCII AND as
    // navigation events on the Tanmatsu — we accept both here because
    // submitCompose / wants_exit_ are idempotent.
    if (ascii == '\r' || ascii == '\n') {
        submitCompose();
        return;
    }
    if (ascii == 27) {                       // Esc
        wants_exit_ = true;
        return;
    }
    if (ascii >= 0x20 && ascii < 0x7F) {     // Printable ASCII
        if (compose_len_ + 1 < COMPOSE_MAX) {
            compose_[compose_len_++] = ascii;
            compose_[compose_len_]   = '\0';
            dirty_ = true;
        }
    }
}

void MeshtasticChatView::onNavigation(int key)
{
    // bsp_input_navigation_key_t values come from badge-bsp/bsp/input.h.
    // The Tanmatsu BSP fires Enter / Backspace / ESC as navigation
    // events (not as keyboard ASCII) — that's why the v1 chat view's
    // Enter key did nothing.
    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            submitCompose();
            break;
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
            if (compose_len_ > 0) {
                compose_[--compose_len_] = '\0';
                dirty_ = true;
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_UP:
            scroll_offset_++;
            if (scroll_offset_ > MESHTASTIC_CHAT_DEPTH) {
                scroll_offset_ = MESHTASTIC_CHAT_DEPTH;
            }
            dirty_ = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            if (scroll_offset_ > 0) {
                scroll_offset_--;
                dirty_ = true;
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            wants_exit_ = true;
            break;
        default:
            break;
    }
}

void MeshtasticChatView::submitCompose()
{
    if (compose_len_ == 0) {
        setStatus("(empty)");
        return;
    }
    esp_err_t err = meshtastic_send_text(compose_);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "sent \"%s\"", compose_);
        setStatus("sent (%u bytes)", (unsigned)compose_len_);
    } else {
        ESP_LOGW(TAG, "send_text failed: %d", (int)err);
        setStatus("send failed (err=%d)", (int)err);
    }
    compose_len_   = 0;
    compose_[0]    = '\0';
    scroll_offset_ = 0;     // jump back to newest after sending
    dirty_         = true;
}

void MeshtasticChatView::setStatus(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_line_, sizeof(status_line_), fmt, ap);
    va_end(ap);
    status_until_ms_ = now_ms() + STATUS_HOLD_MS;
    dirty_           = true;
}

// ── Render ────────────────────────────────────────────────────────────

void MeshtasticChatView::render()
{
    if (!fb_ || canvas_w_ <= 0 || canvas_h_ <= 0) return;

    // Cap render rate so we don't burn the SPI display on idle frames.
    uint32_t t = now_ms();
    bool blink_due = (t - blink_ms_) >= BLINK_MS;
    bool status_expired = (status_until_ms_ != 0 && t >= status_until_ms_);
    if (!dirty_ && !blink_due && !status_expired) return;

    if (blink_due) {
        cursor_on_ = !cursor_on_;
        blink_ms_  = t;
    }
    if (status_expired) {
        status_line_[0]  = '\0';
        status_until_ms_ = 0;
    }

    // Clear the entire framebuffer directly — CW rotation maps logical
    // x-columns to contiguous physical rows, so a single memset works.
    uint16_t *raw = (uint16_t *)pax_buf_get_pixels(fb_);
    memset(raw, 0, canvas_w_ * 480 * sizeof(uint16_t));

    drawTitleBar();
    drawChatLog();
    drawComposeBox();
    drawHelpLine();
    drawSidePanel();

    bsp_display_blit(0, 0, canvas_h_, canvas_w_, pax_buf_get_pixels(fb_));

    dirty_          = false;
    last_render_ms_ = t;
}

void MeshtasticChatView::drawTitleBar()
{
    int text_w = canvas_w_ - PANEL_W;  // left region only

    char who[16];
    meshtastic_format_node(meshtastic_proto_node_id(), who, sizeof(who));

    // Show current channel in title
    const meshtastic_channel_t *tx_ch = meshtastic_channel_get(meshtastic_channel_get_tx());
    const char *ch_name = tx_ch ? tx_ch->name : "???";

    char title[80];
    snprintf(title, sizeof(title), "MatsuMesh: %s [%s]", who, ch_name);
    fast_text_blit(fb_, MARGIN_X, 4, title, COLOR_TITLE, text_w);

    // Right-aligned counters (within the left region).
    meshtastic_node_entry_t nodes[MESHTASTIC_NODEDB_CAP];
    size_t nn = meshtastic_nodedb_snapshot(nodes, MESHTASTIC_NODEDB_CAP);
    uint32_t total = meshtastic_chat_total();
    char r[48];
    snprintf(r, sizeof(r), "%u nodes  %u msgs",
             (unsigned)nn, (unsigned)total);
    int rw = (int)strlen(r) * fast_text_glyph_w();
    fast_text_blit(fb_, text_w - MARGIN_X - rw, 4, r, COLOR_TIME, text_w);

    pax_draw_rect(fb_, COLOR_DIVIDER, 0, TITLE_H, text_w, 1);
}

void MeshtasticChatView::drawChatLog()
{
    int text_w    = canvas_w_ - PANEL_W;
    int top       = TITLE_H + 4;
    int compose_y = canvas_h_ - COMPOSE_H - HELP_H;
    int bottom    = compose_y - 2;

    int line_cap = (bottom - top) / LINE_PX;
    if (line_cap <= 0) return;

    meshtastic_chat_entry_t entries[MESHTASTIC_CHAT_DEPTH];
    size_t n = meshtastic_chat_snapshot(entries, MESHTASTIC_CHAT_DEPTH);
    if (n == 0) {
        fast_text_blit(fb_, MARGIN_X, top,
                       "(no messages yet -- type and Enter)",
                       COLOR_HELP, text_w);
        return;
    }

    int first_idx = scroll_offset_;
    if (first_idx >= (int)n) first_idx = (int)n - 1;
    if (first_idx < 0)       first_idx = 0;

    // Fill from bottom up.
    int y = bottom - LINE_PX;
    uint32_t now_v = now_ms();
    uint8_t active_tx = meshtastic_channel_get_tx();
    for (int i = first_idx; i < (int)n && y >= top; i++) {
        const meshtastic_chat_entry_t &e = entries[i];
        char who[16];
        if (e.is_self) {
            strncpy(who, "me", sizeof(who) - 1);
            who[sizeof(who) - 1] = '\0';
        } else {
            meshtastic_format_node(e.from_node, who, sizeof(who));
        }
        uint32_t age = (now_v - e.when_ms) / 1000u;
        char age_s[12];
        if (age < 60)        snprintf(age_s, sizeof(age_s), "%us",  (unsigned)age);
        else if (age < 3600) snprintf(age_s, sizeof(age_s), "%um",  (unsigned)(age / 60));
        else                 snprintf(age_s, sizeof(age_s), "%uh",  (unsigned)(age / 3600));

        // Show channel tag for messages not on the active TX channel
        char ch_tag[8] = "";
        if (e.channel_idx >= 0 && e.channel_idx != (int8_t)active_tx) {
            snprintf(ch_tag, sizeof(ch_tag), "[%d]", e.channel_idx + 1);
        }

        char line[180];
        snprintf(line, sizeof(line), "%s[%s] %s: %s", ch_tag, age_s, who, e.text);

        uint32_t color = e.is_self ? COLOR_SELF : COLOR_PEER;
        fast_text_blit(fb_, MARGIN_X, y, line, color, text_w);
        y -= LINE_PX;
    }
}

void MeshtasticChatView::drawComposeBox()
{
    int text_w = canvas_w_ - PANEL_W;
    int y = canvas_h_ - COMPOSE_H - HELP_H;
    pax_draw_rect(fb_, COLOR_DIVIDER, 0, y, text_w, 1);

    int text_y = y + 4;
    int gw = fast_text_glyph_w();
    fast_text_blit(fb_, MARGIN_X, text_y, ">", COLOR_PROMPT, text_w);
    int compose_x = MARGIN_X + gw + gw;

    // How many chars fit in the visible compose area
    int avail_px = text_w - compose_x - gw;   // leave room for cursor
    int vis_chars = (gw > 0) ? (avail_px / gw) : 0;
    if (vis_chars < 1) vis_chars = 1;

    // Scroll the visible window so the cursor (end of text) stays on screen
    const char *vis = compose_;
    int vis_len = (int)compose_len_;
    if (vis_len > vis_chars) {
        int skip = vis_len - vis_chars;
        vis     += skip;
        vis_len  = vis_chars;
    }

    fast_text_blit(fb_, compose_x, text_y, vis, COLOR_COMPOSE, text_w);

    if (cursor_on_) {
        int cx = compose_x + vis_len * gw + 1;
        if (cx < text_w)
            pax_draw_rect(fb_, COLOR_CURSOR, cx, text_y + 2, 6, FONT_PX);
    }
}

void MeshtasticChatView::drawHelpLine()
{
    int text_w = canvas_w_ - PANEL_W;
    int y = canvas_h_ - HELP_H + 2;
    pax_draw_rect(fb_, COLOR_DIVIDER, 0, canvas_h_ - HELP_H, text_w, 1);

    if (status_line_[0]) {
        fast_text_blit(fb_, MARGIN_X, y, status_line_, COLOR_STATUS, text_w);
    } else {
        fast_text_blit(fb_, MARGIN_X, y,
                       "Enter=send  Esc=back  Fn+N=ch  Fn+T=terminal",
                       COLOR_HELP, text_w);
    }
}

void MeshtasticChatView::drawSidePanel()
{
    int panel_x = canvas_w_ - PANEL_W;
    int gw = fast_text_glyph_w();
    int y = 4;
    uint8_t active = meshtastic_channel_get_tx();

    // Vertical divider
    pax_draw_rect(fb_, COLOR_DIVIDER, panel_x, 0, 1, canvas_h_);

    int px = panel_x + 6;

    // Header
    fast_text_blit(fb_, px, y, "-- Channels --", COLOR_TITLE, canvas_w_);
    y += LINE_PX + 4;

    // List all channels
    for (int i = 0; i < MESHTASTIC_MAX_CHANNELS; i++) {
        const meshtastic_channel_t *ch = meshtastic_channel_get(i);
        if (!ch) continue;

        char line[40];
        snprintf(line, sizeof(line), "%s%d: %s",
                 (i == active) ? ">" : " ", i + 1, ch->name);

        uint32_t color = (i == active) ? COLOR_PANEL_HL : COLOR_PEER;
        fast_text_blit(fb_, px, y, line, color, canvas_w_);
        y += LINE_PX;
    }

    // Channel info
    y += 8;
    fast_text_blit(fb_, px, y, "-- Info --", COLOR_TITLE, canvas_w_);
    y += LINE_PX + 2;

    const meshtastic_channel_t *tx = meshtastic_channel_get(active);
    if (tx) {
        char hash_s[16];
        snprintf(hash_s, sizeof(hash_s), "Hash: 0x%02X", tx->hash);
        fast_text_blit(fb_, px, y, hash_s, COLOR_PANEL_DIM, canvas_w_);
        y += LINE_PX;

        const char *enc = tx->psk_len > 0 ? "AES-128" : "None";
        char enc_s[20];
        snprintf(enc_s, sizeof(enc_s), "PSK: %s", enc);
        fast_text_blit(fb_, px, y, enc_s, COLOR_PANEL_DIM, canvas_w_);
        y += LINE_PX;
    }

    // Node count
    y += 8;
    meshtastic_node_entry_t nodes[MESHTASTIC_NODEDB_CAP];
    size_t nn = meshtastic_nodedb_snapshot(nodes, MESHTASTIC_NODEDB_CAP);
    char ns[24];
    snprintf(ns, sizeof(ns), "Nodes: %u", (unsigned)nn);
    fast_text_blit(fb_, px, y, ns, COLOR_PANEL_DIM, canvas_w_);
    y += LINE_PX;

    // MQTT status
    bool mqtt = mqtt_transport_is_connected();
    char ms[24];
    snprintf(ms, sizeof(ms), "MQTT: %s", mqtt ? "on" : "off");
    fast_text_blit(fb_, px, y, ms, mqtt ? COLOR_PANEL_HL : COLOR_PANEL_DIM, canvas_w_);
    y += LINE_PX + 8;

    // Help
    fast_text_blit(fb_, px, y, "Fn+1..8 = switch", COLOR_HELP, canvas_w_);
    y += LINE_PX;
    fast_text_blit(fb_, px, y, "Fn+T = terminal", COLOR_HELP, canvas_w_);
}
