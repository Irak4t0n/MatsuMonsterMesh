// MatsuMonsterTerminal — fullscreen text terminal for the MonsterMesh side
// of the app. Renders into the same pax_buf_t / display the emulator already
// uses (see main/main.c — pax_gfx + bsp_display_blit + pax_font_sky_mono).
//
// Step 5 only: this class is self-contained. Step 6 wires it into a state
// machine so Fn+T toggles between EMULATOR and TERMINAL states.

#pragma once

#include <stddef.h>
#include <stdint.h>

// pax_gfx.h transitively pulls in C++ stdlib headers, so include without
// wrapping in extern "C" — each upstream header has its own guards.
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "pax_gfx.h"
#include "PokemonData.h"   // Gen1Party — used as the cached battle party

class PokemonDaycare;
class MonsterMeshTextBattle;
class MeshtasticRadio;
struct IEmulatorSRAM;

class MatsuMonsterTerminal {
public:
    // Per Step 5 spec — exactly these four wired-system pointers.
    MatsuMonsterTerminal(PokemonDaycare        *daycare,
                         MonsterMeshTextBattle *battle,
                         MeshtasticRadio       *radio,
                         IEmulatorSRAM         *sram);

    // Step 5 spec calls for begin() / handleInput() / render() / wantsToExit().
    // The two extra setters below keep the spec'd ctor signature pure while
    // still letting main.c hand us the platform render target — see the note
    // at the top of MatsuMonsterTerminal.cpp explaining why.
    void setRenderTarget(pax_buf_t *fb,
                         int        canvas_w,
                         int        canvas_h);
    void setInputQueue(QueueHandle_t input_queue);

    void begin();         // clear scrollback, print banner
    void handleInput();   // drain queued events; non-blocking
    void render();        // draw current state to fb + bsp_display_blit
    bool wantsToExit() const { return wants_exit_; }

    // Called by main.c after wantsToExit() goes true, so the next entry
    // starts fresh.
    void clearExitFlag() { wants_exit_ = false; }

    // Called from monster_enter_terminal on every Fn+T entry. Bails out of
    // any stale battle left running when the user exited mid-fight, and
    // clears the input buffer. Without this, on re-entry battle_->isActive()
    // is still true and the engine swallows every keystroke at the `>`
    // prompt while waiting on a move that never comes.
    void prepareForReentry();

    // Append a message to the scrollback (also used by command handlers and,
    // optionally, by main.c to surface state-machine events to the user).
    void println(const char *line);
    void printf_line(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

private:
    // ── Wired system ────────────────────────────────────────────────────────
    PokemonDaycare        *daycare_ = nullptr;
    MonsterMeshTextBattle *battle_  = nullptr;
    MeshtasticRadio       *radio_   = nullptr;
    IEmulatorSRAM         *sram_    = nullptr;

    // ── Render target (set by main.c) ───────────────────────────────────────
    pax_buf_t     *fb_       = nullptr;
    int            canvas_w_ = 0;
    int            canvas_h_ = 0;
    QueueHandle_t  input_q_  = nullptr;

    // ── Scrollback ──────────────────────────────────────────────────────────
    static constexpr int SCROLL_LINES = 96;       // ring buffer depth
    static constexpr int LINE_WIDTH   = 64;       // chars per line (incl null)
    char    scroll_[SCROLL_LINES][LINE_WIDTH] = {};
    int     scroll_head_ = 0;     // next write slot
    int     scroll_fill_ = 0;     // valid line count (<= SCROLL_LINES)
    int     scroll_offset_ = 0;   // user scroll offset (0 = bottom)

    // ── Input line ──────────────────────────────────────────────────────────
    static constexpr int INPUT_MAX = 96;
    char    input_buf_[INPUT_MAX] = {};
    int     input_len_ = 0;

    // ── UI state ────────────────────────────────────────────────────────────
    bool     wants_exit_       = false;
    bool     dirty_            = true;   // full redraw needed (header + scrollback + input)
    bool     input_only_dirty_ = false;  // only the input line + cursor changed (typing / blink)
    uint32_t blink_ms_         = 0;      // last blink toggle time
    bool     cursor_on_        = true;
    uint32_t last_render_ms_   = 0;      // cap render rate so heavy frames don't pile up

    // ── Command handling ────────────────────────────────────────────────────
    void handleCommand(const char *cmd);
    void cmdHelp();
    void cmdParty();
    void cmdStatus();
    void cmdFight(const char *args);
    void cmdRun();
    void cmdCatch();
    // Path #3 Session 1 — raw-LoRa bring-up smoke tests.
    void cmdLoraSend(const char *args);
    void cmdLoraStats();
    void cmdLoraReinit();
    void cmdLoraProbe();
    // Path #3 Session 2a — Meshtastic protocol layer.
    void cmdMeshRecent();
    // Path #3 Session 2c — text-message-only view.
    void cmdMeshMessages();
    // Path #3 Session 2d — Meshtastic TX path.
    void cmdMeshSend(const char *args);
    void cmdMeshAnnounce(const char *args);
    // Path #3 Session 3b — NodeDB view.
    void cmdMeshNodes();
    void cmdQuit();

    // ── Internal helpers ────────────────────────────────────────────────────
    void onKeyboard(char ascii, uint32_t modifiers);
    void onNavigation(int key);          // bsp_input_navigation_key_t value
    void submitInput();
    void backspaceInput();
    void appendInputChar(char c);

    void drawHeader();
    void drawScrollback();
    void drawInputLine();

    // Right-side context panel — populated based on the current state:
    // a battle in progress shows the player's moves 1-4 + HP for both
    // sides; otherwise it shows the live party + a quick command list.
    void drawSidePanel();
    void drawBattlePanel(int x, int y, int right);
    void drawIdlePanel  (int x, int y, int right);

    // Battle integration: ticks the battle engine while it's running and
    // drains newly-revealed log lines into the terminal scrollback so the
    // user can see turn-by-turn output.
    void pumpBattle();

    // Last battle log line we've already mirrored into our scrollback —
    // used to detect when the battle's scroll-throttle has revealed a new
    // line. Cleared each time a new battle starts via cmdRun() / cmdFight().
    char    last_battle_line_[64] = {};
    bool    battle_was_active_    = false;

    // Cached Gen1Party built from the live ROM's SRAM via DaycareSavPatcher.
    // Mirrors upstream MonsterMeshTerminal::savParty_. Lazily populated the
    // first time a battle command needs it — see loadSavParty().
    Gen1Party sav_party_       = {};
    bool      sav_party_ready_ = false;
    bool      loadSavParty();   // returns true if sav_party_.count > 0

    // Last wild encounter set up by cmdRun — recorded so pumpBattle can
    // grant the player XP based on the foe's species/level when the engine
    // reports P1_WIN. Cleared after XP is paid out.
    uint8_t   last_foe_species_ = 0;
    uint8_t   last_foe_level_   = 0;
    bool      xp_pending_       = false;

    static const char *speciesName(uint8_t dexNum);
};
