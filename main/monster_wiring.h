// monster_wiring.h — C ABI shim that lets the C-language main.c instantiate
// and drive the C++-only monster_core / meshtastic_radio / matsumonster_ui
// components. All cross-language plumbing for Step 6 lives here.
//
// Lifetime: the C++ objects are file-scope statics in monster_wiring.cpp,
// constructed on first call to monster_init() and never destroyed.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "pax_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MONSTER_STATE_EMULATOR = 0,
    MONSTER_STATE_TERMINAL = 1,
    MONSTER_STATE_CHAT     = 2,   // Path #3 Session 5: full Meshtastic chat UI
} monster_state_t;

// Construct radio + daycare + battle + terminal. Call once after BSP +
// pax_buf are initialised and the input queue has been fetched.
//   fb        : the same pax_buf_t already used by main.c (e.g. &fb_pax)
//   canvas_w  : pax buffer logical width
//   canvas_h  : pax buffer logical height
//   input_q   : the queue handle returned by bsp_input_get_queue()
void monster_init(pax_buf_t *fb, int canvas_w, int canvas_h, QueueHandle_t input_q);

// Step 2 specifically requires SRAM init AFTER gnuboy has loaded a ROM
// (so ram.sbank is allocated by the loader). Call this from main.c right
// after sram_load() returns.
void monster_init_sram(void);

// Auto check-in to the MonsterMesh daycare using the live ROM's save data.
// Reads the trainer name from Gen 1 SRAM offset 0x2598 and feeds the whole
// party to daycare.checkIn(). Must be called AFTER monster_init_sram().
// shortName is the 4-char Meshtastic-style local node name; until the C6
// radio is wired this is hardcoded by the caller (e.g. "MM01").
// No-op if SRAM is not bound or the ROM has no battery RAM.
void monster_auto_checkin(const char *shortName);

// Periodic call from the emulator frame loop. Internally rate-limits to
// PokemonDaycare's expected ~10s cadence — cheap to call every frame.
void monster_daycare_tick(void);

// State accessor + transitions.
monster_state_t monster_get_state(void);

// Called by main.c when it detects Fn+T while in the emulator. Mutes audio
// and switches the state machine; the next call to monster_terminal_pump()
// drives the terminal frame.
void monster_enter_terminal(void);

// Called by main.c every frame while state == MONSTER_STATE_TERMINAL.
// Drains the input queue into the terminal, redraws if dirty, and on exit
// flips state back to MONSTER_STATE_EMULATOR + un-mutes audio.
// Returns true if the state just transitioned back to emulator this frame.
bool monster_terminal_pump(void);

// Path #3 Session 5: full Meshtastic chat UI.
//
// Called by main.c when it detects Fn+M (from any state). Mutes audio
// the same way `monster_enter_terminal` does and switches the state
// machine; the next call to `monster_chat_pump` drives the chat frame.
void monster_enter_chat(void);

// Per-frame chat-mode pump. Drains the input queue, redraws if dirty,
// and on exit flips state back to MONSTER_STATE_EMULATOR + un-mutes
// audio. Returns true on the frame the state transitions back out.
bool monster_chat_pump(void);

#ifdef __cplusplus
}
#endif
