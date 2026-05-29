// SPDX-License-Identifier: GPL-3.0-or-later
//
// FastText — direct-to-framebuffer glyph renderer.
//
// Pre-renders every printable ASCII character (32–127) into a 1-byte-per-pixel
// mask at init time, then blits text by writing RGB565 pixels directly into the
// raw framebuffer using the Tanmatsu's CW rotation mapping:
//
//   logical(lx, ly) → physical(479-ly, lx) → offset = lx * 480 + (479 - ly)
//
// This bypasses pax's per-pixel orientation transform, which costs ~20ms per
// pax_draw_text call through PAX_O_ROT_CW.

#include "FastText.h"

#include <stdlib.h>
#include <string.h>

extern "C" {
#include "esp_log.h"
}
#include "pax_fonts.h"
#include "pax_text.h"

static const char *TAG = "ftext";

static constexpr int FONT_PX  = 12;
static constexpr int PHYS_W   = 480;   // portrait buffer width
static constexpr int NUM_GLYPHS = 96;  // printable ASCII 32–127

static uint8_t *s_mask   = nullptr;
static int      s_gw     = 0;
static int      s_gh     = 0;

static inline uint16_t argb_to_565(uint32_t argb) {
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >>  8) & 0xFF;
    uint8_t b =  argb        & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void fast_text_init(pax_buf_t *fb)
{
    if (s_mask) return;   // already initialised
    if (!fb) return;

    pax_vec2f sz = pax_text_size(pax_font_sky_mono, FONT_PX, "X");
    s_gw = (int)(sz.x + 0.5f);
    s_gh = (int)(sz.y + 0.5f);
    if (s_gw <= 0 || s_gh <= 0) return;

    int glyph_pixels = s_gw * s_gh;
    s_mask = (uint8_t *)calloc(NUM_GLYPHS * glyph_pixels, 1);
    if (!s_mask) {
        ESP_LOGE(TAG, "glyph cache alloc failed");
        return;
    }

    // Render each glyph into a tiny unrotated temp buffer, extract mask.
    pax_buf_t tmp;
    pax_buf_init(&tmp, NULL, s_gw, s_gh, pax_buf_get_type(fb));

    char str[2] = {0, 0};
    for (int ch = 32; ch < 128; ch++) {
        pax_background(&tmp, 0xFF000000);
        str[0] = (char)ch;
        pax_draw_text(&tmp, 0xFFFFFFFF, pax_font_sky_mono, FONT_PX, 0, 0, str);

        uint16_t *pixels = (uint16_t *)pax_buf_get_pixels(&tmp);
        uint8_t  *mask   = &s_mask[(ch - 32) * glyph_pixels];
        for (int i = 0; i < glyph_pixels; i++)
            mask[i] = (pixels[i] != 0) ? 1 : 0;
    }

    pax_buf_destroy(&tmp);
    ESP_LOGI(TAG, "glyph cache ready: %dx%d per glyph, %d bytes",
             s_gw, s_gh, NUM_GLYPHS * glyph_pixels);
}

void fast_text_blit(pax_buf_t *fb, int lx, int ly, const char *text,
                    uint32_t color_argb, int max_x)
{
    if (!s_mask || !fb || !text) return;
    uint16_t *raw   = (uint16_t *)pax_buf_get_pixels(fb);
    uint16_t  color = argb_to_565(color_argb);
    int glyph_pixels = s_gw * s_gh;

    int x = lx;
    for (int ci = 0; text[ci] && x + s_gw <= max_x; ci++) {
        char ch = text[ci];
        if (ch < 32 || ch > 127) ch = '?';

        const uint8_t *mask = &s_mask[(ch - 32) * glyph_pixels];

        for (int gx = 0; gx < s_gw; gx++) {
            int base = (x + gx) * PHYS_W + (PHYS_W - 1 - ly);
            for (int gy = 0; gy < s_gh; gy++) {
                if (mask[gy * s_gw + gx])
                    raw[base - gy] = color;
            }
        }
        x += s_gw;
    }
}

int fast_text_glyph_w(void) { return s_gw; }
int fast_text_glyph_h(void) { return s_gh; }
