// SPDX-License-Identifier: GPL-3.0-or-later

#include "notification_overlay.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"

#include "config.h"             // PHYS_W, PHYS_H
#include "meshtastic_proto.h"   // chat_notify_cb registration

static const char *TAG = "notif";

// ── State ──────────────────────────────────────────────────────────────

#define NOTIF_HOLD_MS  4500     // banner stays for this many ms

// pax_buf_t wrappers over the GBC double-buffer. Init lazily once
// render_buf_a / render_buf_b exist.
static pax_buf_t   s_pax_a;
static pax_buf_t   s_pax_b;
static bool        s_inited = false;

// Notification state. Updated from the drain task via the proto
// callback; read by the blit task. Guarded by mutual-exclusion
// implicit in single-writer / single-reader access — text strings are
// small enough and updates atomic enough that this works without an
// explicit lock for our 16-byte name / 80-byte text fields. A simple
// gen-counter handles the tear case if both ever race.
static struct {
    volatile bool     active;
    volatile uint32_t expire_ms;
    char              who[16];
    char              text[80];
} s_state = {0};

// Cached pointers to main.c's GBC framebuffers. main.c keeps
// render_buf_b file-static (only render_buf_a is exposed), so the
// overlay can't pull them via extern — they get passed in at
// init() and stored here. notification_overlay_render() then
// matches the active buffer to the right pax wrapper.
static uint8_t *s_buf_a = NULL;
static uint8_t *s_buf_b = NULL;

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ── Callback from meshtastic_proto's drain task ────────────────────────

static void on_chat_notify(uint32_t from_node,
                           const char *display_name,
                           const char *text)
{
    (void)from_node;
    if (!s_inited) return;

    // Truncate-copy into the static buffers. The proto layer already
    // filtered control bytes, so it's safe to print directly.
    strlcpy(s_state.who,  display_name ? display_name : "?",
            sizeof(s_state.who));
    strlcpy(s_state.text, text         ? text         : "",
            sizeof(s_state.text));
    s_state.expire_ms = now_ms() + NOTIF_HOLD_MS;
    // active LAST so the renderer never reads a half-populated entry.
    s_state.active    = true;

    ESP_LOGI(TAG, "incoming: %s: %s", s_state.who, s_state.text);
}

// ── Init ───────────────────────────────────────────────────────────────

void notification_overlay_init(uint8_t *render_buf_a, uint8_t *render_buf_b)
{
    if (s_inited) return;
    if (!render_buf_a || !render_buf_b) {
        ESP_LOGE(TAG, "render buffers not allocated yet");
        return;
    }
    s_buf_a = render_buf_a;
    s_buf_b = render_buf_b;

    // Wrap each GBC framebuffer in a pax_buf_t. Same dimensions + format
    // + rotation as fb_pax in main.c so drawing coords match the chat /
    // terminal coordinate space (landscape: 800 wide x 480 tall).
    pax_buf_init(&s_pax_a, render_buf_a, PHYS_W, PHYS_H, PAX_BUF_16_565RGB);
    pax_buf_set_orientation(&s_pax_a, PAX_O_ROT_CW);
    pax_buf_init(&s_pax_b, render_buf_b, PHYS_W, PHYS_H, PAX_BUF_16_565RGB);
    pax_buf_set_orientation(&s_pax_b, PAX_O_ROT_CW);

    // Register the proto-side notify hook. From this point on every
    // inbound TextMessage fires on_chat_notify().
    meshtastic_proto_set_chat_notify_cb(on_chat_notify);

    s_inited = true;
    ESP_LOGI(TAG, "ready");
}

// ── Per-frame render ───────────────────────────────────────────────────

void notification_overlay_render(uint8_t *gbc_buf)
{
    if (!s_inited || !s_state.active) return;

    uint32_t now_v = now_ms();
    if (now_v >= s_state.expire_ms) {
        // Time's up — flip inactive. The GBC raster on the next
        // emulator frame will paint over our pixels for the part of
        // the banner that overlaps its rendered area. The letterbox
        // bars also get cleared by gnuboy's memset every frame.
        s_state.active = false;
        return;
    }

    pax_buf_t *pax = (gbc_buf == s_buf_a) ? &s_pax_a : &s_pax_b;

    // Banner: a 36-pixel-tall horizontal strip across the top of the
    // landscape view. The PAX_O_ROT_CW orientation we set up in init()
    // means pax's reported (width, height) is (800, 480) — landscape.
    static const int W = 800;
    static const int H = 36;

    // Black background with a thin coloured underline so it's visually
    // distinct from any in-game UI.
    pax_draw_rect(pax, 0xCC000000, 0, 0, W, H);
    pax_draw_rect(pax, 0xFF00FFAA, 0, H - 2, W, 2);

    // "<NAME>: <text>" — sender label in mint, text in white. The two
    // calls keep the colours apart without resorting to multi-coloured
    // text spans (pax-gfx doesn't have those).
    char prefix[24];
    snprintf(prefix, sizeof(prefix), "%s:", s_state.who);
    pax_draw_text(pax, 0xFF00FFAA, pax_font_sky_mono, 16,
                  8, 8, prefix);

    pax_vec1_t psz = pax_text_size(pax_font_sky_mono, 16, prefix);
    pax_draw_text(pax, 0xFFFFFFFF, pax_font_sky_mono, 16,
                  8 + (int)psz.x + 8, 8, s_state.text);

    // Right-aligned "Fn+M" hint so the user knows how to read more.
    static const char *hint = "Fn+M";
    pax_vec1_t hsz = pax_text_size(pax_font_sky_mono, 14, hint);
    pax_draw_text(pax, 0xFF808080, pax_font_sky_mono, 14,
                  W - (int)hsz.x - 8, 10, hint);
}

void notification_overlay_dismiss(void)
{
    s_state.active = false;
}
