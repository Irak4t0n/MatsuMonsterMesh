// SPDX-License-Identifier: GPL-3.0-or-later
//
// MeshtasticChatView — fullscreen Meshtastic chat UI. Session 5 of the
// P4-side Meshtastic port (Path #3). Renders into the same pax_buf_t /
// LCD the emulator and the terminal already use.
//
// Layout (logical px on the rotated 800x480 canvas):
//
//   ┌──────────────────────────────────────────────────────┐ ← title bar
//   │ MatsuMesh: HBM!         3 nodes  · 7 msgs            │
//   ├──────────────────────────────────────────────────────┤
//   │                                                      │
//   │  chat log (newest at bottom, scrolls up with Up/Dn)  │
//   │                                                      │
//   │                                                      │
//   ├──────────────────────────────────────────────────────┤
//   │ > [compose buffer here]_                             │ ← compose box
//   ├──────────────────────────────────────────────────────┤
//   │ Enter=send  Esc=back  Up/Dn=scroll  Fn+T=terminal    │ ← help line
//   └──────────────────────────────────────────────────────┘
//
// Input handling:
//   - Printable ASCII → compose buffer
//   - Enter         → meshtastic_send_text(compose); clear buffer
//   - Backspace     → drop one char
//   - Esc           → exit (back to caller's previous state)
//   - Up / Down     → scroll the chat log
//   - Fn+T          → cross-jump to MatsuMonsterTerminal (handled by main.c)

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "pax_gfx.h"

class MeshtasticChatView {
public:
    MeshtasticChatView() = default;

    void setRenderTarget(pax_buf_t *fb, int canvas_w, int canvas_h);
    void setInputQueue(QueueHandle_t input_q);

    void begin();          // initial render
    void handleInput();    // drain queued events; non-blocking
    void render();         // draw current state to fb + blit
    bool wantsToExit() const   { return wants_exit_; }
    void clearExitFlag()       { wants_exit_ = false; }
    bool wantsTerminalJump() const { return wants_terminal_; }
    void clearTerminalJump()       { wants_terminal_ = false; }

private:
    pax_buf_t     *fb_       = nullptr;
    int            canvas_w_ = 0;
    int            canvas_h_ = 0;
    QueueHandle_t  input_q_  = nullptr;

    // ── Compose buffer ──────────────────────────────────────────────
    static constexpr size_t COMPOSE_MAX = 120;
    char     compose_[COMPOSE_MAX] = {};
    size_t   compose_len_          = 0;

    // ── UI state ────────────────────────────────────────────────────
    int      scroll_offset_   = 0;     // 0 = bottom (newest visible)
    bool     wants_exit_      = false;
    bool     wants_terminal_  = false;
    bool     dirty_           = true;
    uint32_t blink_ms_        = 0;
    bool     cursor_on_       = true;
    uint32_t last_render_ms_  = 0;
    char     status_line_[64] = {};   // optional one-shot status (e.g. "sent OK")
    uint32_t status_until_ms_ = 0;

    // ── Internal helpers ───────────────────────────────────────────
    void onKeyboard(char ascii, uint32_t modifiers);
    void onNavigation(int key);
    void submitCompose();
    void setStatus(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    void drawTitleBar();
    void drawChatLog();
    void drawComposeBox();
    void drawHelpLine();
};
