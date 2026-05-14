// SPDX-License-Identifier: GPL-3.0-or-later
//
// notification_overlay — Session 3c of the P4 Meshtastic port.
//
// Paints a transient banner across the top of the landscape view when
// a Meshtastic text message arrives. Lets the user see incoming chat
// without leaving the Game Boy emulator.
//
// Rendering: wraps render_buf_a and render_buf_b in dedicated
// pax_buf_t instances so we can use pax_draw_text + pax_font_sky_mono
// directly on the GBC framebuffer. The banner overlaps a sliver of the
// emulator content; when the timer expires we stop redrawing and
// gnuboy's per-frame raster naturally overwrites the pixels.
//
// Hook into the proto layer:
//   meshtastic_proto_set_chat_notify_cb(on_chat_notify);
// is called from notification_overlay_init().

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the overlay. Must be called AFTER the GBC framebuffers
// have been allocated in main.c. Caller passes the two render buffer
// pointers; the overlay wraps each in a pax_buf_t for drawing. Safe
// to call once; subsequent calls are no-ops.
void notification_overlay_init(uint8_t *render_buf_a, uint8_t *render_buf_b);

// Per-frame render. Called by blit_task with the framebuffer it's
// about to send to the panel. If a notification is currently active
// and not expired, paints onto this frame; otherwise no-op.
void notification_overlay_render(uint8_t *gbc_buf);

// Manually clear / dismiss any active notification. Useful when
// switching to chat (the user has obviously seen the message now).
void notification_overlay_dismiss(void);

#ifdef __cplusplus
}
#endif
