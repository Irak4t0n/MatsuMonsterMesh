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

#include "meshtastic_proto.h"

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
    (void)modifiers;
    // Enter and Esc come through both as keyboard ASCII AND as
    // navigation events on the Tanmatsu — we accept both here because
    // submitCompose / wants_exit_ are idempotent. Backspace is
    // intentionally NOT handled here, only in onNavigation, because
    // the BSP fires both event types simultaneously for a single
    // press and the keyboard path would double-delete. Same rule as
    // MatsuMonsterTerminal::onKeyboard.
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

    pax_background(fb_, COLOR_BG);
    drawTitleBar();
    drawChatLog();
    drawComposeBox();
    drawHelpLine();

    // Same blit convention as the terminal: PAX renders into a buffer
    // rotated 90° CW from the panel's native portrait orientation, so
    // we swap width/height when handing to the blit.
    bsp_display_blit(0, 0, canvas_h_, canvas_w_, pax_buf_get_pixels(fb_));

    dirty_          = false;
    last_render_ms_ = t;
}

void MeshtasticChatView::drawTitleBar()
{
    // Resolve our own short name through the NodeDB so the title reflects
    // any future per-device customisation cleanly.
    char who[16];
    meshtastic_format_node(meshtastic_proto_node_id(), who, sizeof(who));

    char title[64];
    snprintf(title, sizeof(title), "MatsuMesh: %s", who);

    pax_draw_text(fb_, COLOR_TITLE, pax_font_sky_mono, FONT_PX,
                  MARGIN_X, 4, title);

    // Right-aligned counters: node count + total messages.
    meshtastic_node_entry_t nodes[MESHTASTIC_NODEDB_CAP];
    size_t nn = meshtastic_nodedb_snapshot(nodes, MESHTASTIC_NODEDB_CAP);
    uint32_t total = meshtastic_chat_total();
    char r[48];
    snprintf(r, sizeof(r), "%u nodes  %u msgs",
             (unsigned)nn, (unsigned)total);
    pax_vec1_t sz = pax_text_size(pax_font_sky_mono, FONT_PX, r);
    pax_draw_text(fb_, COLOR_TIME, pax_font_sky_mono, FONT_PX,
                  canvas_w_ - MARGIN_X - (int)sz.x, 4, r);

    pax_draw_rect(fb_, COLOR_DIVIDER, 0, TITLE_H, canvas_w_, 1);
}

void MeshtasticChatView::drawChatLog()
{
    int top       = TITLE_H + 4;
    int compose_y = canvas_h_ - COMPOSE_H - HELP_H;
    int bottom    = compose_y - 2;

    int line_cap = (bottom - top) / LINE_PX;
    if (line_cap <= 0) return;

    meshtastic_chat_entry_t entries[MESHTASTIC_CHAT_DEPTH];
    size_t n = meshtastic_chat_snapshot(entries, MESHTASTIC_CHAT_DEPTH);
    if (n == 0) {
        pax_draw_text(fb_, COLOR_HELP, pax_font_sky_mono, FONT_PX,
                      MARGIN_X, top,
                      "(no messages yet — wait for an announce or type and Enter)");
        return;
    }

    // We have `n` messages newest-first. We want to draw newest at the
    // bottom, scrolling older up. With a scroll_offset_, the bottom
    // visible message is the (scroll_offset_)'th newest.
    int first_idx = scroll_offset_;
    if (first_idx >= (int)n) first_idx = (int)n - 1;
    if (first_idx < 0)       first_idx = 0;

    // Fill from bottom up.
    int y = bottom - LINE_PX;
    uint32_t now_v = now_ms();
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

        // Format: "[age]  WHO: text"
        char line[160];
        snprintf(line, sizeof(line), "[%s] %s: %s", age_s, who, e.text);

        uint32_t color = e.is_self ? COLOR_SELF : COLOR_PEER;
        pax_draw_text(fb_, color, pax_font_sky_mono, FONT_PX,
                      MARGIN_X, y, line);
        y -= LINE_PX;
    }
}

void MeshtasticChatView::drawComposeBox()
{
    int y = canvas_h_ - COMPOSE_H - HELP_H;
    pax_draw_rect(fb_, COLOR_DIVIDER, 0, y, canvas_w_, 1);

    int text_y = y + 4;
    pax_draw_text(fb_, COLOR_PROMPT, pax_font_sky_mono, FONT_PX,
                  MARGIN_X, text_y, ">");
    pax_draw_text(fb_, COLOR_COMPOSE, pax_font_sky_mono, FONT_PX,
                  MARGIN_X + 14, text_y, compose_);

    if (cursor_on_) {
        pax_vec1_t sz = pax_text_size(pax_font_sky_mono, FONT_PX, compose_);
        int cx = MARGIN_X + 14 + (int)sz.x + 1;
        pax_draw_rect(fb_, COLOR_CURSOR, cx, text_y + 2, 6, FONT_PX);
    }
}

void MeshtasticChatView::drawHelpLine()
{
    int y = canvas_h_ - HELP_H + 2;
    pax_draw_rect(fb_, COLOR_DIVIDER, 0, canvas_h_ - HELP_H, canvas_w_, 1);

    // If a status is active, show it (in green) — otherwise show keys.
    if (status_line_[0]) {
        pax_draw_text(fb_, COLOR_STATUS, pax_font_sky_mono, FONT_PX,
                      MARGIN_X, y, status_line_);
    } else {
        pax_draw_text(fb_, COLOR_HELP, pax_font_sky_mono, FONT_PX,
                      MARGIN_X, y,
                      "Enter=send  Esc=back  Up/Dn=scroll  Fn+T=terminal");
    }
}
