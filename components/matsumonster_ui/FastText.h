// FastText — direct-to-framebuffer text rendering that bypasses pax's
// per-pixel CW rotation transform.  Shared by MatsuMonsterTerminal and
// MeshtasticChatView for smooth scrolling on the rotated Tanmatsu display.
//
// Call fast_text_init() once at startup with the pax framebuffer.  All
// subsequent fast_text_blit() calls write RGB565 pixels directly into
// the raw buffer using the known CW rotation mapping.

#pragma once

#include <stdint.h>
#include "pax_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build the glyph cache from pax_font_sky_mono at FONT_PX=12.
// Safe to call multiple times — only initialises on the first call.
void fast_text_init(pax_buf_t *fb);

// Blit a NUL-terminated string at logical position (lx, ly) in the
// given ARGB8888 color.  Stops at NUL, max_x, or non-printable ASCII.
void fast_text_blit(pax_buf_t *fb, int lx, int ly, const char *text,
                    uint32_t color_argb, int max_x);

// Glyph dimensions (0 if not yet initialised).
int fast_text_glyph_w(void);
int fast_text_glyph_h(void);

// Draw a horizontal line directly into the raw framebuffer.
void fast_hline(pax_buf_t *fb, int lx0, int lx1, int ly, uint32_t color_argb);

// Fill a small rectangle directly into the raw framebuffer.
void fast_rect(pax_buf_t *fb, int lx, int ly, int lw, int lh, uint32_t color_argb);

#ifdef __cplusplus
}
#endif
