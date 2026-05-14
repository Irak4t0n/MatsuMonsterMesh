// SPDX-License-Identifier: GPL-3.0-or-later
//
// MatsuMonsterTerminal — fullscreen text terminal.
//
// Render target: pax_buf_t already initialised by main.c (uses pax-gfx with
// orientation = PAX_O_ROT_CW so logical coords match the portrait viewport).
// Font: pax_font_sky_mono (the only font main.c links). No new graphics deps.
//
// Note on the constructor signature: Step 5's spec hard-pins it to (daycare,
// battle, radio, sram). The terminal also needs the pax buffer + the input
// queue handle, both of which live in main.c. Rather than break the spec'd
// ctor, we expose setRenderTarget() / setInputQueue() that main.c calls
// once at boot, before begin().

#include "MatsuMonsterTerminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// BSP headers are plain C with no `extern "C"` guards of their own — wrap
// here so bsp_display_blit() etc. link to their C definitions. PAX headers
// (below) are C++-aware and must NOT be wrapped (they pull in <initializer_list>).
extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"   // wild-encounter species + level jitter
#include "bsp/display.h"
#include "bsp/input.h"

// Defined in main/main.c — Alt+M from the terminal forwards here so the
// user can swap to the Meshtastic UI app without first leaving the
// terminal. main.c's reboot helper sets the AppFS bootsel and resets.
bool restart_to_meshtastic(void);
}
static inline bool mm_restart_to_meshtastic(void) {
    return restart_to_meshtastic();
}
#include "pax_fonts.h"
#include "pax_text.h"

#include "PokemonDaycare.h"
#include "MonsterMeshTextBattle.h"
#include "MeshtasticRadio.h"
#include "meshtastic_lora.h"     // Path #3 smoke-test commands
#include "meshtastic_proto.h"    // Path #3 Session 2a parsed-header view
#include "emulator_sram_iface.h"
#include "DaycareData.h"        // daycareSpeciesNames[]
#include "DaycareSavPatcher.h"  // buildGen1Party
#include "PokemonData.h"        // Gen1Party

// ── Layout constants (logical px) ───────────────────────────────────────────
static constexpr int   FONT_PX      = 12;     // pax font size in px
static constexpr int   LINE_PX      = 14;     // line stride
static constexpr int   MARGIN_X     = 6;
static constexpr int   HEADER_Y     = 4;
static constexpr int   HEADER_H     = 18;
static constexpr int   INPUT_H      = 18;
// Width reserved on the right edge of the canvas for the context panel
// (moves / party / commands). Scrollback + input line are clipped to the
// area left of this column. Sized for ~38 monospace chars at FONT_PX=12.
static constexpr int   PANEL_W      = 280;

static constexpr uint32_t COLOR_BG     = 0xFF000000;
static constexpr uint32_t COLOR_HEADER = 0xFF00FFAA;   // mint green
static constexpr uint32_t COLOR_TEXT   = 0xFFE0E0E0;
static constexpr uint32_t COLOR_INPUT  = 0xFFFFFFFF;
static constexpr uint32_t COLOR_PROMPT = 0xFFFFFF00;
static constexpr uint32_t COLOR_CURSOR = 0xFFFFFF00;
static constexpr uint32_t COLOR_DIM    = 0xFF707070;

static inline uint32_t now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// Forward decl — defined later in the file, used by cmdFight/cmdRun.
static void fill_placeholder_mon(Gen1Pokemon &m,
                                 uint8_t species, uint8_t level,
                                 uint8_t move_id, uint8_t move_pp);

// ── Wild encounter pool ────────────────────────────────────────────────────
// Common Gen 1 wilds from the Viridian / Pallet / Route 1 / Viridian Forest
// area — direct port of upstream MonsterMesh's AREA_POOL_0
// (_refs_monster_mesh/.../MonsterMeshTerminal.cpp:24). Keeps the "early-game
// roguelike" feel for `run` without a full progression system yet.
static const uint8_t kWildPool[] = {
    16,  // Pidgey
    19,  // Rattata
    21,  // Spearow
    10,  // Caterpie
    13,  // Weedle
    32,  // NidoranM
    29,  // NidoranF
    23,  // Ekans
    46,  // Paras
    48,  // Venonat
};
static constexpr size_t kWildPoolLen = sizeof(kWildPool) / sizeof(kWildPool[0]);

// Port of upstream's pickMovesForSpecies: pick up to 4 species-appropriate
// moves (STAB first, then filler Normal moves). Falls back to Tackle+Growl
// if nothing else fits. Matches MonsterMeshTerminal.cpp:1011 behaviour.
//
// Level-clamped ceiling: a L3 wild shouldn't roll Razor Leaf (55, high-crit)
// or Slam (80) — that's how Paras one-shots a same-level lead. Cap is
// `min(100, 30 + level*7)`:
//   L3 → 51   (Tackle/Scratch/Pound/Mega Drain qualify; Razor Leaf doesn't)
//   L5 → 65   (Razor Leaf in)
//   L7 → 79   (Headbutt-70 in; Slam-80 still blocked)
//   L10 → 100 (everything legal)
// A floor of `min(40, 20 + level*4)` keeps the move list non-empty at very
// low levels so the engine doesn't reject the wild for having no moves.
static void pick_moves_for_species(uint8_t species, uint8_t level,
                                   uint8_t out_moves[4])
{
    const Gen1BaseStats &b = GEN1_BASE_STATS[species < 152 ? species : 0];
    uint8_t picked = 0;
    out_moves[0] = out_moves[1] = out_moves[2] = out_moves[3] = 0;

    // Power ceiling by level (see comment above).
    int ceil_i = 30 + (int)level * 7;
    if (ceil_i > 100) ceil_i = 100;
    uint8_t pmax = (uint8_t)ceil_i;

    // STAB moves first — same min as upstream so STAB still beats filler.
    uint8_t stab_min = 40;
    if (stab_min > pmax) stab_min = pmax;   // tiny levels: relax floor
    for (uint16_t i = 0; i < GEN1_MOVE_COUNT && picked < 4; ++i) {
        const Gen1MoveData &m = GEN1_MOVES[i];
        if (m.power == 0) continue;
        if (m.type != b.type1 && m.type != b.type2) continue;
        if (m.power < stab_min || m.power > pmax) continue;
        out_moves[picked++] = m.num;
    }
    // Fill with Normal-type moves under the same ceiling.
    uint8_t fill_min = 30;
    if (fill_min > pmax) fill_min = pmax;
    for (uint16_t i = 0; i < GEN1_MOVE_COUNT && picked < 4; ++i) {
        const Gen1MoveData &m = GEN1_MOVES[i];
        if (m.power == 0 || m.type != 0) continue;
        if (m.power < fill_min || m.power > pmax) continue;
        bool dup = false;
        for (uint8_t j = 0; j < picked; ++j) if (out_moves[j] == m.num) dup = true;
        if (!dup) out_moves[picked++] = m.num;
    }
    if (picked == 0) { out_moves[0] = 33; out_moves[1] = 45; }  // Tackle, Growl
}

// Build a fully-statted wild Gen1Pokemon: derive max HP / ATK / DEF / SPD /
// SPC from base stats + level via Gen1BattleEngine::initBattlePokeFromBase,
// then mirror the result into the 44-byte save struct so the engine reads
// the same numbers back at battle start. Direct port of upstream's
// writeBattlePokeToSave (MonsterMeshTerminal.cpp:1036).
static void build_wild_mon(Gen1Pokemon &p, uint8_t species, uint8_t level,
                           const uint8_t moves[4])
{
    memset(&p, 0, sizeof(p));
    Gen1BattleEngine::BattlePoke tmp;
    Gen1BattleEngine::initBattlePokeFromBase(tmp, species, level, moves);
    p.species  = species;
    p.boxLevel = level;
    p.level    = level;
    p.maxHp[0] = tmp.maxHp >> 8; p.maxHp[1] = tmp.maxHp & 0xFF;
    p.hp[0]    = tmp.hp    >> 8; p.hp[1]    = tmp.hp    & 0xFF;
    p.atk[0]   = tmp.atk   >> 8; p.atk[1]   = tmp.atk   & 0xFF;
    p.def[0]   = tmp.def   >> 8; p.def[1]   = tmp.def   & 0xFF;
    p.spd[0]   = tmp.spd   >> 8; p.spd[1]   = tmp.spd   & 0xFF;
    p.spc[0]   = tmp.spc   >> 8; p.spc[1]   = tmp.spc   & 0xFF;
    // Use a moderate DV byte so wilds aren't max-rolled but aren't trivially
    // weak either (upstream defaults to 0x88 = 8/8 for wild encounters).
    p.dvs[0] = 0x88;
    p.dvs[1] = 0x88;
    memcpy(p.moves, moves, 4);
    for (int i = 0; i < 4; ++i) {
        const Gen1MoveData *m = gen1Move(moves[i]);
        p.pp[i] = m ? m->pp : 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────

MatsuMonsterTerminal::MatsuMonsterTerminal(PokemonDaycare        *daycare,
                                           MonsterMeshTextBattle *battle,
                                           MeshtasticRadio       *radio,
                                           IEmulatorSRAM         *sram)
  : daycare_(daycare), battle_(battle), radio_(radio), sram_(sram)
{
    // Subscribe to the battle's log stream so every turn line lands in our
    // scrollback the instant it's generated. Without this we'd poll the
    // engine's visible window and miss intermediate turn lines.
    if (battle_) {
        battle_->setExternalLogCallback(
            [](const char *line, void *ctx) {
                static_cast<MatsuMonsterTerminal *>(ctx)->println(line);
            },
            this);
    }
}

void MatsuMonsterTerminal::setRenderTarget(pax_buf_t *fb, int canvas_w, int canvas_h)
{
    fb_       = fb;
    canvas_w_ = canvas_w;
    canvas_h_ = canvas_h;
}

void MatsuMonsterTerminal::setInputQueue(QueueHandle_t q)
{
    input_q_ = q;
}

void MatsuMonsterTerminal::prepareForReentry()
{
    // Cancel any battle the user left mid-fight before exiting via Fn+T.
    // Without this, battle_->isActive() stays true on re-entry and the
    // engine intercepts every keystroke at the `>` prompt — typing letters
    // does nothing because the engine only handles 1-4/s/f/ESC.
    if (battle_ && battle_->isActive()) {
        battle_->exit();
        println("(Previous battle abandoned.)");
    }
    // Reset the input line so a re-entry always lands at a clean prompt.
    input_len_ = 0;
    input_buf_[0]      = '\0';
    cursor_on_         = true;
    blink_ms_          = now_ms();
    xp_pending_        = false;
    battle_was_active_ = false;
    dirty_             = true;   // force a full redraw on next render
}

void MatsuMonsterTerminal::begin()
{
    scroll_head_ = scroll_fill_ = scroll_offset_ = 0;
    input_len_   = 0;
    input_buf_[0] = '\0';
    wants_exit_  = false;
    dirty_       = true;

    println("MonsterMesh Terminal");
    println("Commands: party, status, fight <name>, run, quit, help");
    println("Fn+T or ESC returns to emulator.");
    println("");
}

// ── Scrollback append ───────────────────────────────────────────────────────

void MatsuMonsterTerminal::println(const char *line)
{
    if (!line) line = "";
    snprintf(scroll_[scroll_head_], LINE_WIDTH, "%s", line);
    scroll_head_ = (scroll_head_ + 1) % SCROLL_LINES;
    if (scroll_fill_ < SCROLL_LINES) scroll_fill_++;
    scroll_offset_ = 0;   // any new output snaps view to bottom
    dirty_ = true;
}

void MatsuMonsterTerminal::printf_line(const char *fmt, ...)
{
    char buf[LINE_WIDTH * 2];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // Word-naive line wrap: break at LINE_WIDTH-1 chars.
    int len = (int)strlen(buf);
    int pos = 0;
    while (pos < len) {
        char chunk[LINE_WIDTH];
        int take = len - pos;
        if (take > LINE_WIDTH - 1) take = LINE_WIDTH - 1;
        memcpy(chunk, buf + pos, take);
        chunk[take] = '\0';
        println(chunk);
        pos += take;
    }
}

// ── Input pump ──────────────────────────────────────────────────────────────

void MatsuMonsterTerminal::handleInput()
{
    if (!input_q_) return;
    bsp_input_event_t ev;
    while (xQueueReceive(input_q_, &ev, 0) == pdTRUE) {
        switch (ev.type) {
            case INPUT_EVENT_TYPE_KEYBOARD:
                if (ev.args_keyboard.ascii) {
                    onKeyboard(ev.args_keyboard.ascii, ev.args_keyboard.modifiers);
                }
                break;
            case INPUT_EVENT_TYPE_NAVIGATION:
                if (ev.args_navigation.state) {
                    onNavigation((int)ev.args_navigation.key);
                }
                break;
            default:
                break;
        }
    }

    // Drive the battle engine + drain any newly-revealed log lines into
    // the terminal scrollback so the user can read turn-by-turn output.
    pumpBattle();

    // Cursor blink — toggle every 500ms. Only the input line needs to
    // redraw, NOT the whole scrollback / header.
    uint32_t t = now_ms();
    if (t - blink_ms_ >= 500) {
        blink_ms_         = t;
        cursor_on_        = !cursor_on_;
        input_only_dirty_ = true;
    }
}

void MatsuMonsterTerminal::pumpBattle()
{
    if (!battle_) return;
    // Drive the battle's per-frame state (CPU action submission in LOCAL
    // mode, retry timers in NETWORKED mode, scroll throttle decrement).
    // Log output now arrives via the ExternalLogCallback registered in the
    // ctor — no polling needed here.
    if (battle_->isActive()) {
        battle_->tick(now_ms());
    }

    // Auto-dismiss the result screen the moment the engine flips to FINISHED.
    // Without this the user has to press a key to call exit(), which delays
    // the XP-payout printout below (it's gated on the just_finished edge).
    // appendLog has already forwarded "You won!" / "You blacked out..." into
    // our scrollback synchronously, so there's nothing left for the user to
    // read on the engine-owned screen.
    if (battle_->isActive() &&
        battle_->phase() == MonsterMeshTextBattle::Phase::FINISHED) {
        battle_->exit();
        dirty_ = true;
    }

    // XP payout: when a battle that was ours-to-win just ended in P1_WIN,
    // write the gained XP into the live SRAM (so it carries back into the
    // emulator save) AND tell the daycare so its session counters
    // (totalXpGained / totalLevelsGained) reflect battle wins too.
    bool just_finished = battle_was_active_ && !battle_->isActive();
    if (just_finished && xp_pending_) {
        xp_pending_ = false;
        bool won = battle_->engine().result() == Gen1BattleEngine::Result::P1_WIN;
        if (won && sram_ && daycare_) {
            const auto &state = daycare_->getState();
            if (state.partyCount > 0) {
                uint8_t lead_dex = state.pokemon[0].speciesDex;
                uint8_t prev_total_level = state.pokemon[0].savLevel +
                                            (uint8_t)state.pokemon[0].totalLevelsGained;
                // Gen-1-flavoured XP curve approximation: foe_level * 8 —
                // ~24 XP at L3, 40 at L5, 80 at L10. Tuned to feel right
                // against the early-game wild pool without breaking pacing.
                uint32_t xp = (uint32_t)last_foe_level_ * 8u;

                uint8_t *buf = iemu_sram_data(sram_);
                if (buf && iemu_sram_size(sram_) >=
                               DaycareSavPatcher::SAV_MIN_BYTES) {
                    // Use patchPokemon directly (instead of checkout()) so
                    // we get the new level back for the daycare counters.
                    uint8_t new_level =
                        DaycareSavPatcher::patchPokemon(buf, 0, lead_dex, xp);
                    DaycareSavPatcher::fixChecksum(buf);
                    iemu_sram_mark_dirty(sram_);

                    // Tell the daycare — updates totalXpGained and
                    // totalLevelsGained without re-writing SRAM.
                    daycare_->recordBattleWin(0, xp, new_level);

                    printf_line("%s gained %u XP!",
                                speciesName(lead_dex), (unsigned)xp);
                    if (new_level > prev_total_level) {
                        printf_line("%s reached L%u!",
                                    speciesName(lead_dex), (unsigned)new_level);
                    }
                } else {
                    println("(XP write failed - SRAM unavailable)");
                }
            }
        }
    }
    battle_was_active_ = battle_->isActive();
}

void MatsuMonsterTerminal::onKeyboard(char ascii, uint32_t modifiers)
{
    // Fn+T always returns to emulator, regardless of battle state.
    if ((modifiers & BSP_INPUT_MODIFIER_FUNCTION) &&
        (ascii == 't' || ascii == 'T')) {
        wants_exit_ = true;
        return;
    }
    // Alt+M → reboot into the Tanmatsu Meshtastic UI app. Same shortcut
    // main.c registers for the emulator state — duplicated here so the
    // user can swap apps without first leaving the terminal.
    if ((modifiers & BSP_INPUT_MODIFIER_ALT) &&
        (ascii == 'm' || ascii == 'M')) {
        if (mm_restart_to_meshtastic()) return;   // unreachable on success
        println("(Alt+M: Meshtastic UI app not installed.)");
        println("Install Nicolai-Electronics/tanmatsu-meshtastic-ui");
        println("via BadgeLink first.");
        return;
    }

    // Battle just ended (Phase::FINISHED) — any keystroke dismisses the
    // result screen. Do that here AND fall through so the same keystroke
    // also reaches the terminal-input path. Otherwise the user loses the
    // first character of the command they're trying to type next.
    if (battle_ && battle_->isActive() &&
        battle_->phase() == MonsterMeshTextBattle::Phase::FINISHED) {
        battle_->exit();
        dirty_ = true;
        // Intentionally fall through.
    }

    // Mid-battle key routing. Goal: keep all the upstream battle hotkeys
    // (1-4 move, s switch, f flee, ESC forfeit) responsive on a single
    // press, BUT also let the user type multi-letter terminal commands
    // like `catch` while the foe is on the field. Rules:
    //   1. WAIT_SWITCH: route everything to the engine (the switch menu
    //      uses w/s/space/enter and we don't want typing to interfere).
    //   2. Otherwise, if the input buffer is empty AND the keystroke is a
    //      recognised battle hotkey → engine.
    //   3. Otherwise (non-hotkey on empty buffer, OR any key with text
    //      already buffered) → fall through to the terminal input path,
    //      so the user can spell out `catch`+Enter.
    if (battle_ && battle_->isActive()) {
        auto is_battle_hotkey = [](char c) -> bool {
            return c == '1' || c == '2' || c == '3' || c == '4' ||
                   c == 's' || c == 'S' || c == 'f' || c == 'F' ||
                   c == 27 /* ESC */;
        };
        bool buffer_empty = (input_len_ == 0);
        bool in_switch    = (battle_->phase() ==
                             MonsterMeshTextBattle::Phase::WAIT_SWITCH);
        if (in_switch ||
            (buffer_empty && is_battle_hotkey(ascii))) {
            battle_->handleKey((uint8_t)ascii);
            dirty_ = true;
            return;
        }
        // Fall through: keystroke goes to the terminal input buffer.
    }

    if (ascii == '\n' || ascii == '\r') { submitInput(); return; }
    // NOTE: backspace is intentionally NOT handled here. The Tanmatsu BSP
    // fires *both* a NAVIGATION event (key=BACKSPACE) and a KEYBOARD event
    // (ascii='\b') for a single physical press. submitInput / wants_exit_
    // are idempotent so Enter/ESC double-firing is invisible, but
    // backspaceInput is destructive — handling both paths deletes two
    // characters per press. Navigation path owns backspace.
    if (ascii == 27 /* ESC */)          { wants_exit_ = true; return; }

    if (ascii >= 0x20 && ascii < 0x7F) {
        appendInputChar(ascii);
    }
}

void MatsuMonsterTerminal::onNavigation(int key)
{
    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            wants_exit_ = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            submitInput();
            break;
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
            backspaceInput();
            break;
        case BSP_INPUT_NAVIGATION_KEY_UP: {
            int max_off = scroll_fill_ - 1;
            if (max_off < 0) max_off = 0;
            if (scroll_offset_ < max_off) { scroll_offset_++; dirty_ = true; }
            break;
        }
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            if (scroll_offset_ > 0) { scroll_offset_--; dirty_ = true; }
            break;
        case BSP_INPUT_NAVIGATION_KEY_PGUP: {
            int max_off = scroll_fill_ - 1;
            if (max_off < 0) max_off = 0;
            scroll_offset_ += 10;
            if (scroll_offset_ > max_off) scroll_offset_ = max_off;
            dirty_ = true;
            break;
        }
        case BSP_INPUT_NAVIGATION_KEY_PGDN:
            scroll_offset_ -= 10;
            if (scroll_offset_ < 0) scroll_offset_ = 0;
            dirty_ = true;
            break;
        default: break;
    }
}

void MatsuMonsterTerminal::appendInputChar(char c)
{
    if (input_len_ < INPUT_MAX - 1) {
        input_buf_[input_len_++] = c;
        input_buf_[input_len_]   = '\0';
        // Only the bottom input line needs to redraw — keep the scrollback
        // pixels intact. Avoids a full pax_background + 30 text calls per
        // keystroke, which is what makes typing feel sluggish.
        input_only_dirty_ = true;
    }
}

void MatsuMonsterTerminal::backspaceInput()
{
    if (input_len_ > 0) {
        input_buf_[--input_len_] = '\0';
        input_only_dirty_ = true;
    }
}

void MatsuMonsterTerminal::submitInput()
{
    // Use printf_line so over-LINE_WIDTH input wraps cleanly instead of
    // tripping -Werror=format-truncation; first 2 chars are the prompt.
    printf_line("> %s", input_buf_);
    char cmdcopy[INPUT_MAX];
    memcpy(cmdcopy, input_buf_, input_len_ + 1);
    input_len_ = 0;
    input_buf_[0] = '\0';
    handleCommand(cmdcopy);
}

// ── Command dispatch ────────────────────────────────────────────────────────

static const char *trim_ws(const char *s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

static bool starts_with(const char *s, const char *prefix, const char **rest) {
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) != 0) return false;
    if (s[n] != '\0' && s[n] != ' ' && s[n] != '\t') return false;
    if (rest) *rest = trim_ws(s + n);
    return true;
}

void MatsuMonsterTerminal::handleCommand(const char *raw)
{
    const char *cmd = trim_ws(raw);
    if (*cmd == '\0') return;

    const char *args = nullptr;
    if      (starts_with(cmd, "party",  &args))  cmdParty();
    else if (starts_with(cmd, "status", &args))  cmdStatus();
    else if (starts_with(cmd, "fight",  &args))  cmdFight(args);
    else if (starts_with(cmd, "run",    &args))  cmdRun();
    else if (starts_with(cmd, "catch",  &args))  cmdCatch();
    else if (starts_with(cmd, "lora_send",  &args)) cmdLoraSend(args);
    else if (starts_with(cmd, "lora_stats", &args)) cmdLoraStats();
    else if (starts_with(cmd, "lora_reinit", &args)) cmdLoraReinit();
    else if (starts_with(cmd, "lora_probe", &args)) cmdLoraProbe();
    else if (starts_with(cmd, "mesh_recent", &args)) cmdMeshRecent();
    else if (starts_with(cmd, "mesh_messages", &args)) cmdMeshMessages();
    else if (starts_with(cmd, "mesh_send", &args)) cmdMeshSend(args);
    else if (starts_with(cmd, "mesh_announce", &args)) cmdMeshAnnounce(args);
    else if (starts_with(cmd, "mesh_nodes", &args)) cmdMeshNodes();
    else if (starts_with(cmd, "daycare_beacon", &args)) cmdDaycareBeacon();
    else if (starts_with(cmd, "lora_tx_test", &args)) cmdLoraTxTest(args);
    else if (starts_with(cmd, "quit",   &args))  cmdQuit();
    else if (starts_with(cmd, "exit",   &args))  cmdQuit();
    else if (starts_with(cmd, "help",   &args))  cmdHelp();
    else if (starts_with(cmd, "?",      &args))  cmdHelp();
    else                                         printf_line("Unknown command: %s", cmd);
}

void MatsuMonsterTerminal::cmdHelp()
{
    println("Commands:");
    println("  party         — list daycare party");
    println("  status        — daycare status + neighbor count");
    println("  fight <name>  — challenge a mesh neighbour by short name");
    println("  run           — start a local roguelike battle vs CPU");
    println("  catch         — try to catch the current wild Pokemon");
    println("  lora_send <s> — send raw probe (≥8 chars) on LongFast US");
    println("  lora_stats    — show LoRa init / TX / RX counters");
    println("  lora_reinit   — retry LoRa bring-up (handy after startup race)");
    println("  lora_probe    — exercise each LoRa primitive to find a bad one");
    println("  mesh_recent   — last N parsed Meshtastic headers + decoded body");
    println("  mesh_messages — text messages only, newest first");
    println("  mesh_send <t> — broadcast text message <t> on LongFast");
    println("  mesh_announce — broadcast NodeInfo (so T-Deck shows our name)");
    println("  mesh_nodes    — list nodes we've heard NodeInfo from");
    println("  daycare_beacon— force an immediate daycare beacon broadcast");
    println("  lora_tx_test N— send N-byte dummy packet (find TX size limit)");
    println("  quit          — back to emulator");
}

void MatsuMonsterTerminal::cmdParty()
{
    // Live-read the running emulator's SRAM via DaycareSavPatcher rather
    // than the cached daycare state from boot-time auto-checkin. This way
    // `party` always reflects the user's CURRENT in-game progress, even
    // if they saved in-game after the daycare last checked in.
    if (!sram_) {
        println("(emulator SRAM not bound — load a Gen 1 ROM first)");
        return;
    }
    if (iemu_sram_size(sram_) == 0 || iemu_sram_data(sram_) == nullptr) {
        println("(this ROM has no battery RAM — try a Pokemon game)");
        return;
    }

    DaycarePartyInfo party[6];
    uint8_t count = DaycareSavPatcher::readParty(iemu_sram_data(sram_), party);
    if (count == 0) {
        println("Party empty.");
        println("Tip: in-game, walk to a Pokemon Center or use the");
        println("     START menu's SAVE option, then re-open the terminal.");
        return;
    }

    printf_line("Party (%u):", (unsigned)count);
    for (uint8_t i = 0; i < count; ++i) {
        const auto &p = party[i];
        const char *nick = (p.nickname[0] != '\0')
                               ? p.nickname
                               : speciesName(p.dexNum);
        printf_line("  %u. %-10s L%-3u  (%s)",
                    (unsigned)(i + 1), nick, (unsigned)p.level,
                    speciesName(p.dexNum));
    }
}

void MatsuMonsterTerminal::cmdStatus()
{
    if (!daycare_)  { println("(no daycare wired)"); return; }
    if (!radio_)    { println("(no radio wired)");   return; }

    const auto &state = daycare_->getState();
    printf_line("Daycare:    %s", daycare_->isActive() ? "active" : "idle");
    printf_line("Trainer:    %s  Node: %s",
                daycare_->getGameName(), daycare_->getShortName());
    printf_line("Party:      %u", (unsigned)state.partyCount);
    printf_line("Neighbors:  %d (mesh nodes: %d)",
                (int)daycare_->getNeighborCount(),
                radio_->getNeighborCount());
    printf_line("Achievements: 0x%016llx",
                (unsigned long long)state.achievementFlags);

    // Per-Pokémon session counters — updated by cmdRun's XP payout +
    // (eventually) daycare events. Skip slots that haven't earned anything
    // so the status line stays readable.
    for (uint8_t i = 0; i < state.partyCount; ++i) {
        const auto &p = state.pokemon[i];
        if (p.totalXpGained == 0 && p.totalLevelsGained == 0) continue;
        const char *nick = (p.nickname[0] != '\0') ? p.nickname : speciesName(p.speciesDex);
        printf_line("  %s  +%u XP  +%u lvl",
                    nick,
                    (unsigned)p.totalXpGained,
                    (unsigned)p.totalLevelsGained);
    }

    // Last event summary
    const auto &evt = daycare_->getLastEvent();
    if (evt.message[0]) {
        printf_line("Last event: %s", evt.message);
    }
}

void MatsuMonsterTerminal::cmdFight(const char *args)
{
    if (!battle_ || !radio_ || !daycare_) {
        println("(battle subsystem not wired)");
        return;
    }
    if (!*args) {
        println("Usage: fight <neighbor short name>");
        return;
    }

    // Stub-radio path: getNeighborCount() returns 0 → user feedback per spec.
    if (radio_->getNeighborCount() == 0 && daycare_->getNeighborCount() == 0) {
        println("No mesh neighbors. (Radio is in stub mode — see");
        println("CONFIG_MATSUMONSTER_MESH_STUB in menuconfig.)");
        return;
    }

    // Look the target up in the daycare's known-neighbours list.
    const auto *nbrs = daycare_->getNeighbors();
    uint8_t cnt = daycare_->getNeighborCount();
    const auto *match = (decltype(nbrs))nullptr;
    for (uint8_t i = 0; i < cnt; ++i) {
        if (strncasecmp(nbrs[i].shortName, args, sizeof(nbrs[i].shortName)) == 0) {
            match = &nbrs[i]; break;
        }
    }
    if (!match) {
        printf_line("No neighbour named '%s'.", args);
        return;
    }

    // Player party from the live save when available; falls back to
    // placeholder Pikachu only if the ROM has no Gen 1 SAV.
    Gen1Party myParty;
    if (loadSavParty()) {
        myParty = sav_party_;
    } else {
        myParty = {};
        myParty.count       = 1;
        myParty.species[0]  = 25;
        fill_placeholder_mon(myParty.mons[0], 25, 5, 33, 35);
        // Non-empty nickname so the engine's [POKEMON] templates render.
        snprintf((char *)myParty.nicknames[0], 11, "%s", speciesName(25));
    }

    last_battle_line_[0] = '\0';    // fresh tracker for the new battle
    battle_->startNetworkedAsInitiator(match->nodeId, myParty);
    printf_line("Challenging %s (0x%08x)...", match->shortName, (unsigned)match->nodeId);
}

// Re-read the live Gen1Party from the bound IEmulatorSRAM each call.
// Originally cached on first use (mirroring upstream's pattern), but that
// meant `run`/`fight` could fire with stale data after the player saved
// in-game. The read is cheap (~800 bytes from PSRAM) so there's no
// reason to cache. Returns true if at least one Pokémon was read.
bool MatsuMonsterTerminal::loadSavParty()
{
    if (!sram_) return false;
    uint8_t n = DaycareSavPatcher::buildGen1Party(sram_, &sav_party_);
    sav_party_ready_ = (n > 0);   // kept for any future caller that wants the flag
    return n > 0;
}

// Fallback: fill a Gen1Pokemon with the minimum fields Gen1BattleEngine
// needs to build a working BattlePoke via initBattlePokeFromSave (species,
// level, one valid move + PP, max DVs for a tougher placeholder). hp[]==0
// makes the engine auto-fill to maxHp. Used only when no live save party
// is available (e.g. ROM has no SRAM).
static void fill_placeholder_mon(Gen1Pokemon &m,
                                 uint8_t species, uint8_t level,
                                 uint8_t move_id, uint8_t move_pp)
{
    memset(&m, 0, sizeof(m));
    m.species  = species;
    m.boxLevel = level;
    m.level    = level;
    m.moves[0] = move_id;
    m.pp[0]    = move_pp;
    m.dvs[0]   = 0xFF;             // max Atk/Def DVs for a tougher placeholder
    m.dvs[1]   = 0xFF;             // max Spd/Spc DVs
    // hp[0..1] left zero → engine sets hp = maxHp on init
}

void MatsuMonsterTerminal::cmdRun()
{
    if (!battle_) { println("(battle subsystem not wired)"); return; }

    // Player party comes from the live save when available; placeholder
    // Pikachu w/ Tackle when there's no Gen 1 SAV in cart RAM (e.g. you
    // booted a non-Pokémon ROM).
    Gen1Party myParty;
    bool real_party = loadSavParty();
    if (real_party) {
        myParty = sav_party_;
        const char *leadNick = (myParty.nicknames[0][0] != 0)
                                   ? (const char *)myParty.nicknames[0]
                                   : speciesName(myParty.species[0]);
        // Prefer the in-game trainer name captured at auto-checkin; fall
        // back to the Meshtastic short name; final fallback "your" so the
        // sentence still reads cleanly if neither is available yet.
        const char *trainer = (daycare_ && daycare_->getGameName()[0])
                                  ? daycare_->getGameName()
                                  : (daycare_ && daycare_->getShortName()[0])
                                      ? daycare_->getShortName()
                                      : nullptr;
        if (trainer) {
            printf_line("Sending out %s from %s's party.", leadNick, trainer);
        } else {
            printf_line("Sending out %s from your party.", leadNick);
        }
    } else {
        myParty = {};
        myParty.count       = 1;
        myParty.species[0]  = 25;
        fill_placeholder_mon(myParty.mons[0], 25, 5, 33, 35);
        println("(No live save detected — using placeholder Pikachu.)");
    }

    // CPU opponent: pick a random species from the early-game wild pool
    // (port of upstream MonsterMesh's AREA_POOL_0). Level is the player's
    // lead minus 3 so the fight is winnable for a Gen-1 starter team.
    Gen1Party cpuParty = {};
    cpuParty.count = 1;

    uint8_t cpuLevel = 3;
    if (real_party && myParty.mons[0].level > 0) {
        int target = (int)myParty.mons[0].level - 3;
        // Add a small ±2 jitter so identical levels still produce some variety.
        target += ((int)(esp_random() % 5) - 2);
        if (target < 2)  target = 2;
        if (target > 50) target = 50;
        cpuLevel = (uint8_t)target;
    }

    uint8_t cpuSpecies = kWildPool[esp_random() % kWildPoolLen];
    uint8_t cpuMoves[4];
    pick_moves_for_species(cpuSpecies, cpuLevel, cpuMoves);

    cpuParty.species[0] = cpuSpecies;
    build_wild_mon(cpuParty.mons[0], cpuSpecies, cpuLevel, cpuMoves);
    snprintf((char *)cpuParty.nicknames[0], 11, "%s", speciesName(cpuSpecies));

    // Player placeholder fallback (no live save): give it a real nickname
    // so the engine's "[POKEMON] used X!" template doesn't render blank.
    if (!real_party) {
        snprintf((char *)myParty.nicknames[0], 11, "%s", speciesName(25));
    }

    last_battle_line_[0] = '\0';
    last_foe_species_    = cpuSpecies;
    last_foe_level_      = cpuLevel;
    xp_pending_          = true;
    battle_->startLocal(myParty, cpuParty);

    // Announce the matchup BEFORE handing keys to the engine, so the user
    // can see who they're up against without having to spend a turn first.
    // Engine emits "A wild battle begins!" itself; we add the specifics.
    printf_line("A wild %s (L%u) appeared!",
                speciesName(cpuSpecies), (unsigned)cpuLevel);
    const auto &lead = myParty.mons[0];
    const char *leadNick = (myParty.nicknames[0][0] != 0)
                               ? (const char *)myParty.nicknames[0]
                               : speciesName(myParty.species[0]);
    printf_line("Go! %s (L%u)!", leadNick, (unsigned)lead.level);
    println("1-4 = move, S = switch, F = flee, ESC = forfeit.");
}

void MatsuMonsterTerminal::cmdCatch()
{
    if (!battle_ || !battle_->isActive()) {
        println("No wild Pokemon to catch — try `run` first.");
        return;
    }
    if (battle_->mode() != MonsterMeshTextBattle::Mode::LOCAL_ROGUELIKE) {
        println("Can't catch a trainer's Pokemon!");
        return;
    }
    if (!sram_) {
        println("(catch: no SRAM bound — can't write to save)");
        return;
    }

    // ── Catch chance ──────────────────────────────────────────────────
    // Hand-tuned for the roguelike pacing — Gen 1's actual formula uses
    // ball type, status, and a divisor that's brutal at full HP. Ours:
    //   base 30% + 1% per missing HP%, +25% if foe is statused, capped 95%.
    // Quick reference at L5 vs full-HP foe → 30%; at 1HP → 95%.
    const auto &foe = battle_->engine().party(1).mons[
                          battle_->engine().party(1).active];
    if (foe.species == 0 || foe.maxHp == 0) {
        println("(catch: foe state invalid)");
        return;
    }
    uint32_t hp_pct = (uint32_t)foe.hp * 100u / foe.maxHp;
    int chance = 30 + (int)(100u - hp_pct);
    if (foe.status != Gen1BattleEngine::ST_NONE) chance += 25;
    if (chance > 95) chance = 95;
    if (chance < 5)  chance = 5;

    bool caught = ((int)(esp_random() % 100u)) < chance;
    const char *foeName = foe.nickname[0]
                              ? foe.nickname
                              : speciesName(foe.species);

    if (!caught) {
        printf_line("Threw a ball at %s... it broke free!", foeName);
        printf_line("(catch chance was %d%%)", chance);
        return;
    }

    // ── Build the CaughtMon record from the live BattlePoke ───────────
    DaycareSavPatcher::CaughtMon cm = {};
    cm.dexNum = foe.species;       // build_wild_mon stores dex# directly
    cm.level  = foe.level;
    cm.hp     = foe.hp;
    cm.maxHp  = foe.maxHp;
    cm.atk    = foe.atk;
    cm.def    = foe.def;
    cm.spd    = foe.spd;
    cm.spc    = foe.spc;
    cm.type1  = foe.type1;
    cm.type2  = foe.type2;
    memcpy(cm.moves, foe.moves, 4);
    memcpy(cm.pp,    foe.pp,    4);
    // BattlePoke doesn't track DVs after init — re-use the same default
    // build_wild_mon writes (0x88/0x88, the upstream wild-encounter pair).
    cm.dvs[0] = 0x88;
    cm.dvs[1] = 0x88;
    snprintf(cm.nickname, sizeof(cm.nickname), "%s",
             foe.nickname[0] ? foe.nickname : speciesName(foe.species));

    // ── Write the record + checksum ───────────────────────────────────
    int dest = DaycareSavPatcher::addCaughtMon(sram_, cm);
    switch (dest) {
        case 0:
            printf_line("Gotcha! %s was added to your party.", foeName);
            break;
        case 1:
            printf_line("Gotcha! %s was sent to your PC.", foeName);
            println("(Party full — check the current Bill's PC box.)");
            break;
        case -1:
            println("Caught it... but party AND current PC box are full!");
            println("(Switch to an empty box in Bill's PC, then try again.)");
            return;   // leave battle running so the player can flee/forfeit
        default:
            println("(catch: SRAM write failed)");
            return;
    }

    uint8_t *buf = iemu_sram_data(sram_);
    if (buf) DaycareSavPatcher::fixChecksum(buf);

    // No XP for caught mons (matches Gen 1 R/B — defeating gives XP, not
    // catching). Clear the pending-XP edge so pumpBattle's just_finished
    // branch doesn't try to also pay out for this fight.
    xp_pending_ = false;
    battle_->exit();
    dirty_ = true;
}

// ── Path #3 Session 1 smoke-test commands ──────────────────────────────────
//
// `lora_send <text>` ships a raw byte payload over the air on LongFast US
// 907.125 MHz. Other Meshtastic devices on the same channel will RECEIVE
// it at the wire level — they won't decode it as a Meshtastic packet (we
// don't have the protocol layer yet), but they may log "rx packet of N
// bytes, decode failed" or similar, which is enough to prove the link.
//
// `lora_stats` prints counters: configs applied, TX attempted/ok/err,
// RX packets/bytes. RX should increment whenever ANY other Meshtastic
// device on LongFast US is transmitting nearby — useful for confirming
// the radio is actually listening to live mesh traffic.

void MatsuMonsterTerminal::cmdLoraSend(const char *args)
{
    if (!meshtastic_lora_is_up()) {
        println("(lora_send: radio not up — meshtastic_lora_begin failed)");
        return;
    }
    if (!args || !*args) {
        println("Usage: lora_send <text>");
        return;
    }
    size_t len = strlen(args);
    if (len > 64) {
        println("(lora_send: truncating to 64 bytes)");
        len = 64;
    }
    esp_err_t err = meshtastic_lora_send_raw((const uint8_t *)args, len);
    if (err == ESP_OK) {
        printf_line("lora_send: %u bytes OK on 907.125 MHz", (unsigned)len);
    } else {
        printf_line("lora_send: FAILED (esp_err=%d)", (int)err);
    }
}

void MatsuMonsterTerminal::cmdLoraStats()
{
    meshtastic_lora_stats_t s;
    meshtastic_lora_get_stats(&s);
    println("── LoRa stats ──");
    printf_line("  init_ok        %u", (unsigned)s.init_ok);
    printf_line("  configs        %u", (unsigned)s.configs_applied);
    printf_line("  tx attempted   %u", (unsigned)s.tx_attempted);
    printf_line("  tx ok          %u", (unsigned)s.tx_ok);
    printf_line("  tx err         %u", (unsigned)s.tx_err);
    printf_line("  rx packets     %u", (unsigned)s.rx_packets);
    printf_line("  rx bytes total %u", (unsigned)s.rx_bytes_total);
    printf_line("  relay sent     %u", (unsigned)meshtastic_proto_total_relayed());
    println("── Diagnostics ──");
    printf_line("  init_err=%d  ms=%u",  (int)s.last_init_err,
                (unsigned)s.last_init_ms);
    printf_line("  cfg_err=%d   ms=%u",  (int)s.last_config_err,
                (unsigned)s.last_config_ms);
    printf_line("  mode_err=%d  ms=%u",  (int)s.last_mode_err,
                (unsigned)s.last_mode_ms);
    printf_line("  tx_pre=%d tx=%d tx_post=%d  ms=%u len=%u",
                (int)s.last_tx_pre_mode_err,
                (int)s.last_tx_err,
                (int)s.last_tx_post_mode_err,
                (unsigned)s.last_tx_ms,
                (unsigned)s.last_tx_len);
}

void MatsuMonsterTerminal::cmdLoraReinit()
{
    println("Retrying lora bring-up...");
    esp_err_t err = meshtastic_lora_begin();
    if (err == ESP_OK) {
        println("lora_reinit: OK (radio now configured for LongFast US)");
    } else {
        printf_line("lora_reinit: FAILED (esp_err=%d)", (int)err);
        println("(check `lora_stats` for last_*_err to see which call failed)");
    }
}

void MatsuMonsterTerminal::cmdLoraProbe()
{
    println("Probing tanmatsu-lora primitives...");
    meshtastic_lora_probe_result_t r;
    meshtastic_lora_probe(&r);
    for (int i = 0; i < r.probe_count; ++i) {
        const char *verdict = (r.entries[i].result == 0) ? "OK" : "FAIL";
        printf_line("  %-18s %s (%d)",
                    r.entries[i].label, verdict,
                    (int)r.entries[i].result);
    }
    if (r.cur_frequency > 0) {
        println("── C6 current config ──");
        printf_line("  freq    %u Hz",  (unsigned)r.cur_frequency);
        printf_line("  SF      %u",     (unsigned)r.cur_sf);
        printf_line("  BW      %u kHz", (unsigned)r.cur_bw);
        printf_line("  CR      4/%u",   (unsigned)r.cur_cr);
    } else {
        println("(C6 didn't return a valid config — get_config failed)");
    }
}

// ── Path #3 Session 2a — Meshtastic protocol view ───────────────────────────
//
// `mesh_recent` shows the most recent parsed packets. Each row is one
// Meshtastic on-air packet broken down by 16-byte header fields. The
// payload bytes are still encrypted (decryption lands in Session 2b),
// but the plaintext header alone already tells us a lot: which other
// node we heard, the LongFast channel hash, hop counts, and whether
// our LoRa tuning is right.

static const char *portnum_label(int p)
{
    switch (p) {
        case MESHTASTIC_PORTNUM_TEXT_MESSAGE_APP: return "TextMessage";
        case MESHTASTIC_PORTNUM_POSITION_APP:     return "Position";
        case MESHTASTIC_PORTNUM_NODEINFO_APP:     return "NodeInfo";
        case MESHTASTIC_PORTNUM_ROUTING_APP:      return "Routing";
        case MESHTASTIC_PORTNUM_ADMIN_APP:        return "Admin";
        case MESHTASTIC_PORTNUM_TELEMETRY_APP:    return "Telemetry";
        case MESHTASTIC_PORTNUM_TRACEROUTE_APP:   return "Traceroute";
        case MESHTASTIC_PORTNUM_UNKNOWN_APP:      return "Unknown";
        default:                                  return NULL;
    }
}

void MatsuMonsterTerminal::cmdMeshRecent()
{
    meshtastic_recent_entry_t entries[MESHTASTIC_RECENT_DEPTH];
    size_t n = meshtastic_proto_recent(entries, MESHTASTIC_RECENT_DEPTH);
    printf_line("── mesh_recent (parsed %u total) ──",
                (unsigned)meshtastic_proto_total_parsed());
    if (n == 0) {
        println("(no packets yet — wait for T-Deck Plus to broadcast)");
        return;
    }
    uint32_t now_ms_v = (uint32_t)(esp_timer_get_time() / 1000ULL);
    for (size_t i = 0; i < n; i++) {
        const meshtastic_recent_entry_t &e = entries[i];
        uint32_t age_s = (now_ms_v - e.rx_ms) / 1000u;
        uint8_t  hl    = meshtastic_hdr_hop_limit(e.header.flags);
        size_t   plen  = e.raw_len > MESHTASTIC_HEADER_LEN
                             ? (size_t)e.raw_len - MESHTASTIC_HEADER_LEN : 0;

        // Portnum / decrypt summary, same logic as before.
        char portcol[24] = "—";
        if (e.decrypted) {
            if (e.portnum_guess >= 0) {
                const char *lbl = portnum_label(e.portnum_guess);
                if (lbl) {
                    snprintf(portcol, sizeof(portcol), "%s(%d)",
                             lbl, (int)e.portnum_guess);
                } else {
                    snprintf(portcol, sizeof(portcol), "Port%d",
                             (int)e.portnum_guess);
                }
            } else if (e.plain_sample_len > 0) {
                snprintf(portcol, sizeof(portcol), "?? (b0=%02x)",
                         e.plain_sample[0]);
            }
        }

        // Two-line-per-packet layout: header on row 1, decoded body on
        // row 2 (only when we have one). Keeps both readable without
        // running off the right edge of the terminal. NodeNum gets
        // resolved through the NodeDB to a short name when known.
        char who[16];
        meshtastic_format_node(e.header.from, who, sizeof(who));
        printf_line("[%us] %s  ch=%02x  hop=%u  plen=%u  %s",
                    (unsigned)age_s,
                    who,
                    e.header.channel,
                    hl,
                    (unsigned)plen,
                    portcol);
        if (e.body[0] != '\0') {
            printf_line("       %s", e.body);
        }
    }
}

void MatsuMonsterTerminal::cmdMeshMessages()
{
    meshtastic_recent_entry_t entries[MESHTASTIC_RECENT_DEPTH];
    size_t n = meshtastic_proto_recent(entries, MESHTASTIC_RECENT_DEPTH);
    println("── mesh_messages (text only) ──");
    uint32_t now_ms_v = (uint32_t)(esp_timer_get_time() / 1000ULL);
    size_t shown = 0;
    for (size_t i = 0; i < n; i++) {
        const meshtastic_recent_entry_t &e = entries[i];
        if (!e.decrypted)                                            continue;
        if (e.portnum_guess != MESHTASTIC_PORTNUM_TEXT_MESSAGE_APP)  continue;
        if (e.body[0] == '\0')                                       continue;
        uint32_t age_s = (now_ms_v - e.rx_ms) / 1000u;
        char who[16];
        meshtastic_format_node(e.header.from, who, sizeof(who));
        printf_line("[%us] %s: %s", (unsigned)age_s, who, e.body);
        shown++;
    }
    if (shown == 0) {
        println("(no text messages heard yet)");
    }
}

void MatsuMonsterTerminal::cmdMeshNodes()
{
    meshtastic_node_entry_t nodes[MESHTASTIC_NODEDB_CAP];
    size_t n = meshtastic_nodedb_snapshot(nodes, MESHTASTIC_NODEDB_CAP);
    printf_line("── mesh_nodes (%u known) ──", (unsigned)n);
    if (n == 0) {
        println("(no nodes seen yet)");
        return;
    }
    uint32_t now_ms_v = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t my_id    = meshtastic_proto_node_id();
    for (size_t i = 0; i < n; i++) {
        const meshtastic_node_entry_t &e = nodes[i];
        uint32_t age_s = (now_ms_v - e.last_seen_ms) / 1000u;
        const char *me  = (e.node_num == my_id) ? "* " : "  ";
        printf_line("%s!%08lx  %-7s  %-16s  age=%us",
                    me,
                    (unsigned long)e.node_num,
                    e.short_name[0] ? e.short_name : "(no-short)",
                    e.long_name[0]  ? e.long_name  : "(no-long)",
                    (unsigned)age_s);
    }
    println("(* = us)");
}

// ── Path #3 Session 2d — Meshtastic TX commands ─────────────────────────────

void MatsuMonsterTerminal::cmdMeshSend(const char *args)
{
    if (!meshtastic_lora_is_up()) {
        println("(mesh_send: radio not up)");
        return;
    }
    if (!args || !*args) {
        println("Usage: mesh_send <text>");
        return;
    }
    esp_err_t err = meshtastic_send_text(args);
    if (err == ESP_OK) {
        printf_line("mesh_send: \"%s\" sent as !%08lx",
                    args, (unsigned long)meshtastic_proto_node_id());
    } else {
        printf_line("mesh_send: FAILED (%s)", esp_err_to_name(err));
    }
}

void MatsuMonsterTerminal::cmdMeshAnnounce(const char *args)
{
    if (!meshtastic_lora_is_up()) {
        println("(mesh_announce: radio not up)");
        return;
    }
    // Optional override: `mesh_announce <long> <short>`. If args is
    // empty, the proto layer fills in the compile-time defaults.
    const char *long_name  = nullptr;
    const char *short_name = nullptr;
    // Simplest possible split — find the last space; everything before
    // is long_name, everything after is short_name. Works for the
    // single-token case (no space → no short override) too.
    char long_buf[32]  = {0};
    char short_buf[12] = {0};
    if (args && *args) {
        const char *sp = strrchr(args, ' ');
        if (sp && sp != args) {
            size_t llen = (size_t)(sp - args);
            if (llen >= sizeof(long_buf)) llen = sizeof(long_buf) - 1;
            memcpy(long_buf, args, llen);
            long_buf[llen] = '\0';
            strncpy(short_buf, sp + 1, sizeof(short_buf) - 1);
            long_name  = long_buf;
            short_name = short_buf;
        } else {
            strncpy(long_buf, args, sizeof(long_buf) - 1);
            long_name = long_buf;
        }
    }
    esp_err_t err = meshtastic_send_nodeinfo(long_name, short_name);
    if (err == ESP_OK) {
        printf_line("mesh_announce: NodeInfo sent as !%08lx",
                    (unsigned long)meshtastic_proto_node_id());
    } else {
        printf_line("mesh_announce: FAILED (%s)", esp_err_to_name(err));
    }
}

void MatsuMonsterTerminal::cmdDaycareBeacon()
{
    if (!daycare_) { println("(no daycare wired)"); return; }
    if (!daycare_->isActive()) { println("Daycare not active (no check-in yet)"); return; }

    const auto &state = daycare_->getState();
    printf_line("Beacon: node=%s trainer=%s party=%u",
                daycare_->getShortName(), daycare_->getGameName(),
                (unsigned)state.partyCount);
    for (uint8_t i = 0; i < state.partyCount && i < 6; i++) {
        const char *nick = state.pokemon[i].nickname[0]
                               ? state.pokemon[i].nickname
                               : speciesName(state.pokemon[i].speciesDex);
        printf_line("  #%u %s Lv%u", (unsigned)state.pokemon[i].speciesDex,
                    nick, (unsigned)(state.pokemon[i].savLevel + state.pokemon[i].totalLevelsGained));
    }

    meshtastic_lora_stats_t before;
    meshtastic_lora_get_stats(&before);

    daycare_->forceBeacon();

    meshtastic_lora_stats_t after;
    meshtastic_lora_get_stats(&after);
    if (after.tx_ok > before.tx_ok) {
        printf_line("TX OK (size=%u bytes on-air)", (unsigned)(after.tx_ok - before.tx_ok));
    } else if (after.tx_err > before.tx_err) {
        println("TX FAILED — check lora_stats");
    } else {
        println("TX status unclear — check lora_stats");
    }
}

void MatsuMonsterTerminal::cmdLoraTxTest(const char *args)
{
    if (!meshtastic_lora_is_up()) {
        println("(radio not up — try lora_reinit)");
        return;
    }
    int n = 0;
    if (!args || !*args || (n = atoi(args)) < 1) {
        println("Usage: lora_tx_test <size>");
        println("  Sends an N-byte raw packet (16-byte Meshtastic header");
        println("  + (N-16) dummy payload). N must be 16..256.");
        println("  Use to find the C6 TX size ceiling.");
        return;
    }
    if (n < 16)  n = 16;
    if (n > 256) n = 256;

    // Build a valid-looking Meshtastic packet so the C6 treats it the
    // same as a real TX. The header is 16 bytes; fill the rest with 0xAA.
    uint8_t pkt[256];
    memset(pkt, 0xAA, sizeof(pkt));
    // Minimal header: to=broadcast, from=our node, random id, flags
    uint32_t bcast = 0xFFFFFFFF;
    uint32_t from  = meshtastic_proto_node_id();
    uint32_t id    = (uint32_t)esp_random();
    memcpy(pkt + 0, &bcast, 4);
    memcpy(pkt + 4, &from,  4);
    memcpy(pkt + 8, &id,    4);
    pkt[12] = 0x63;  // flags: hop_limit=3, hop_start=3
    pkt[13] = 0x08;  // channel hash (LongFast)
    pkt[14] = 0;
    pkt[15] = 0;

    printf_line("TX test: %d bytes...", n);

    meshtastic_lora_stats_t before;
    meshtastic_lora_get_stats(&before);

    esp_err_t err = meshtastic_lora_send_raw((const uint8_t *)pkt, (size_t)n);

    meshtastic_lora_stats_t after;
    meshtastic_lora_get_stats(&after);

    if (err == ESP_OK) {
        printf_line("  OK (%d bytes sent)", n);
    } else {
        printf_line("  FAIL: %s (0x%x)", esp_err_to_name(err), (int)err);
        printf_line("  pre_mode=%d tx_err=%d post_mode=%d",
                    (int)after.last_tx_pre_mode_err,
                    (int)after.last_tx_err,
                    (int)after.last_tx_post_mode_err);
    }
}

void MatsuMonsterTerminal::cmdQuit()
{
    println("Returning to emulator...");
    wants_exit_ = true;
}

// ── Render ──────────────────────────────────────────────────────────────────

void MatsuMonsterTerminal::render()
{
    if (!fb_ || canvas_w_ <= 0 || canvas_h_ <= 0) return;
    if (!dirty_ && !input_only_dirty_) return;

    // Throttle ONLY the heavy full-redraw path; the fast input-line path is
    // small enough (one rect + two text calls + one blit) that letting it
    // run at the full pump cadence (~60 Hz) keeps typing and the blinking
    // cursor responsive. Throttling the input path was causing the cursor
    // to "stay there" mid-typing because keystrokes between throttle
    // windows had to wait for the next 33ms tick to redraw.
    uint32_t t = now_ms();
    if (dirty_ && (t - last_render_ms_ < 33)) return;
    last_render_ms_ = t;

    if (dirty_) {
        // Full redraw: scrollback / header / panel changed.
        pax_background(fb_, COLOR_BG);
        drawHeader();
        drawScrollback();
        drawSidePanel();
        drawInputLine();
    } else {
        // Fast path: only the input line + cursor changed (typing, blink).
        // Clear a bit more vertical real estate than strictly needed —
        // pax glyphs and the cursor block extend a few pixels past the
        // simple bbox, and partial residue looked like a "frozen cursor"
        // when the new cursor position drew over a stale pixel column.
        // Restricted to the LEFT region so it doesn't clobber the panel
        // (the panel's footer rows sit inside the strip's vertical range).
        int strip_y = canvas_h_ - INPUT_H - 8;
        int strip_w = canvas_w_ - PANEL_W;
        pax_draw_rect(fb_, COLOR_BG, 0, strip_y,
                      strip_w, canvas_h_ - strip_y);
        drawInputLine();
    }

    // canvas_w_ × canvas_h_ are the post-rotation user-coord dimensions
    // (e.g. 800×480 landscape). bsp_display_blit needs the panel's NATIVE
    // memory dimensions (e.g. 480 wide × 800 tall portrait), so swap.
    bsp_display_blit(0, 0, canvas_h_, canvas_w_, pax_buf_get_pixels(fb_));
    dirty_            = false;
    input_only_dirty_ = false;
}

void MatsuMonsterTerminal::drawHeader()
{
    char hdr[LINE_WIDTH];
    int  nbr = radio_ ? radio_->getNeighborCount() : 0;
    snprintf(hdr, sizeof(hdr), "MonsterMesh Terminal   nbrs:%d %s",
             nbr,
#ifdef CONFIG_MATSUMONSTER_MESH_STUB
             "[stub radio]"
#else
             "[serial radio]"
#endif
            );
    pax_draw_text(fb_, COLOR_HEADER, pax_font_sky_mono, FONT_PX,
                  MARGIN_X, HEADER_Y, hdr);

    // Separator under header — only spans the LEFT (scrollback) region;
    // the side panel draws its own internal separators.
    pax_draw_line(fb_, COLOR_DIM,
                  MARGIN_X, HEADER_H,
                  canvas_w_ - PANEL_W - 4, HEADER_H);
}

void MatsuMonsterTerminal::drawScrollback()
{
    int top    = HEADER_H + 4;
    int bottom = canvas_h_ - INPUT_H - 4;
    int height = bottom - top;
    int max_lines = height / LINE_PX;
    if (max_lines <= 0) return;

    // The scrollback ring's most recent valid line is at scroll_head_-1.
    // scroll_offset_ scrolls the view up (older lines come into view).
    int last_index = scroll_head_ - 1;
    int shown = (scroll_fill_ < max_lines) ? scroll_fill_ : max_lines;

    int newest_visible = last_index - scroll_offset_;
    int oldest_visible = newest_visible - shown + 1;

    int y = top;
    for (int i = oldest_visible; i <= newest_visible; ++i) {
        if (i < last_index - scroll_fill_ + 1) { y += LINE_PX; continue; }
        int idx = ((i % SCROLL_LINES) + SCROLL_LINES) % SCROLL_LINES;
        pax_draw_text(fb_, COLOR_TEXT, pax_font_sky_mono, FONT_PX,
                      MARGIN_X, y, scroll_[idx]);
        y += LINE_PX;
    }

    // Scroll-position hint inside the left region (panel owns the right).
    if (scroll_offset_ > 0) {
        char hint[16];
        snprintf(hint, sizeof(hint), "[-%d]", scroll_offset_);
        pax_draw_text(fb_, COLOR_DIM, pax_font_sky_mono, FONT_PX,
                      canvas_w_ - PANEL_W - 60, top, hint);
    }
}

void MatsuMonsterTerminal::drawInputLine()
{
    int y = canvas_h_ - INPUT_H;

    // Separator + prompt span the LEFT region only — the side panel
    // owns the area from (canvas_w_ - PANEL_W) onwards.
    pax_draw_line(fb_, COLOR_DIM,
                  MARGIN_X, y - 2,
                  canvas_w_ - PANEL_W - 4, y - 2);

    // Measure the prompt's actual rendered width so the input text follows
    // it exactly (pax_font_sky_mono is wider than my old hardcoded 7px/glyph
    // assumption, which left the cursor lagging behind the typed text).
    pax_vec2f prompt_size = pax_text_size(pax_font_sky_mono, FONT_PX, "> ");
    int input_x = MARGIN_X + (int)prompt_size.x;

    pax_draw_text(fb_, COLOR_PROMPT, pax_font_sky_mono, FONT_PX,
                  MARGIN_X, y, "> ");
    pax_draw_text(fb_, COLOR_INPUT, pax_font_sky_mono, FONT_PX,
                  input_x, y, input_buf_);

    if (cursor_on_) {
        // Place cursor at the actual end of the rendered input — measured,
        // not estimated. Fixes the "cursor stays held up before the last
        // few letters" symptom.
        pax_vec2f input_size = pax_text_size(pax_font_sky_mono, FONT_PX, input_buf_);
        int cx = input_x + (int)input_size.x;
        pax_draw_rect(fb_, COLOR_CURSOR, cx, y + 2, 6, FONT_PX);
    }
}

// ── Right-side context panel ────────────────────────────────────────────────
// Shows information that mirrors the current command/state: an active
// battle gets a moves panel (1-4 + PP/power, plus HP for both sides);
// otherwise we show a party summary + command cheatsheet so the user
// always has something useful in the otherwise-empty right column.

// Compact 3-letter type abbreviation (NORMAL → "Nor", ELECTRIC → "Ele").
// Falls back to "???" for out-of-range. Static lookup table — generated
// once from GEN1_TYPE_NAMES at first call so we don't rebuild on every
// render frame.
static const char *typeAbbr(uint8_t type)
{
    static char abbr[24][4] = {};
    static bool ready = false;
    if (!ready) {
        const size_t n = sizeof(GEN1_TYPE_NAMES) / sizeof(GEN1_TYPE_NAMES[0]);
        for (size_t i = 0; i < n && i < 24; ++i) {
            const char *s = GEN1_TYPE_NAMES[i];
            abbr[i][0] = s[0];
            abbr[i][1] = s[1] ? (char)(s[1] | 0x20) : '\0';   // lower
            abbr[i][2] = s[2] ? (char)(s[2] | 0x20) : '\0';
            abbr[i][3] = '\0';
        }
        ready = true;
    }
    if (type >= 24) return "???";
    return abbr[type];
}

void MatsuMonsterTerminal::drawSidePanel()
{
    int px     = canvas_w_ - PANEL_W;       // panel left edge
    int top    = HEADER_Y;
    int bottom = canvas_h_ - 4;
    int right  = canvas_w_ - 4;

    // Vertical separator between scrollback and panel
    pax_draw_line(fb_, COLOR_DIM, px, top, px, bottom);

    int x = px + 8;
    int y = top + 2;

    bool in_battle = battle_ && battle_->isActive() &&
                     battle_->phase() != MonsterMeshTextBattle::Phase::FINISHED;

    if (in_battle) drawBattlePanel(x, y, right);
    else           drawIdlePanel  (x, y, right);
}

void MatsuMonsterTerminal::drawBattlePanel(int x, int y, int right)
{
    // Section title
    pax_draw_text(fb_, COLOR_HEADER, pax_font_sky_mono, FONT_PX,
                  x, y, "── BATTLE ──");
    y += LINE_PX + 2;

    const auto &eng = battle_->engine();
    const auto &mp  = eng.party(0);
    const auto &fp  = eng.party(1);
    if (mp.count == 0 || fp.count == 0) return;
    const auto &me   = mp.mons[mp.active];
    const auto &foe  = fp.mons[fp.active];

    // Player line: nickname + level
    char buf[64];
    snprintf(buf, sizeof(buf), "You: %.10s L%u",
             me.nickname[0] ? me.nickname : "???", (unsigned)me.level);
    pax_draw_text(fb_, COLOR_TEXT, pax_font_sky_mono, FONT_PX, x, y, buf);
    y += LINE_PX;

    // HP line — colour-coded by ratio so a quick glance shows danger.
    uint32_t hp_color = COLOR_TEXT;
    if (me.maxHp > 0) {
        uint32_t pct = (uint32_t)me.hp * 100u / me.maxHp;
        if      (pct <= 25) hp_color = 0xFFFF5050;   // red
        else if (pct <= 50) hp_color = 0xFFFFCC40;   // amber
        else                hp_color = 0xFF60E060;   // green
    }
    snprintf(buf, sizeof(buf), "HP %u/%u", (unsigned)me.hp, (unsigned)me.maxHp);
    pax_draw_text(fb_, hp_color, pax_font_sky_mono, FONT_PX, x, y, buf);
    y += LINE_PX + 4;

    // Moves block — 1..4. Each move takes 2 lines: "n) Name" then
    // indented "Type P:pp PP:cur/max".
    for (int i = 0; i < 4; ++i) {
        if (me.moves[i] == 0) {
            snprintf(buf, sizeof(buf), "%d) ---", i + 1);
            pax_draw_text(fb_, COLOR_DIM, pax_font_sky_mono, FONT_PX,
                          x, y, buf);
            y += LINE_PX * 2;
            continue;
        }
        const Gen1MoveData *m = gen1Move(me.moves[i]);
        if (!m) {
            snprintf(buf, sizeof(buf), "%d) #%u", i + 1, (unsigned)me.moves[i]);
            pax_draw_text(fb_, COLOR_TEXT, pax_font_sky_mono, FONT_PX,
                          x, y, buf);
            y += LINE_PX * 2;
            continue;
        }
        // Greyed if out of PP — engine refuses to use it.
        uint32_t name_color = (me.pp[i] == 0) ? COLOR_DIM : COLOR_INPUT;
        snprintf(buf, sizeof(buf), "%d) %.14s", i + 1, m->name);
        pax_draw_text(fb_, name_color, pax_font_sky_mono, FONT_PX, x, y, buf);
        y += LINE_PX;
        if (m->power == 0) {
            snprintf(buf, sizeof(buf), "   %s status PP%u/%u",
                     typeAbbr(m->type), (unsigned)me.pp[i], (unsigned)m->pp);
        } else {
            snprintf(buf, sizeof(buf), "   %s P%u PP%u/%u",
                     typeAbbr(m->type), (unsigned)m->power,
                     (unsigned)me.pp[i], (unsigned)m->pp);
        }
        pax_draw_text(fb_, COLOR_DIM, pax_font_sky_mono, FONT_PX, x, y, buf);
        y += LINE_PX;
    }

    y += 4;

    // Hotkey hints — mirrors the line printed in the scrollback at battle
    // start, so the user can find them again without scrolling.
    pax_draw_text(fb_, COLOR_PROMPT, pax_font_sky_mono, FONT_PX,
                  x, y, "S=switch  F=flee");
    y += LINE_PX;
    pax_draw_text(fb_, COLOR_PROMPT, pax_font_sky_mono, FONT_PX,
                  x, y, "type 'catch' to net");
    y += LINE_PX;
    pax_draw_text(fb_, COLOR_PROMPT, pax_font_sky_mono, FONT_PX,
                  x, y, "ESC=forfeit");
    y += LINE_PX + 4;

    // Foe block
    pax_draw_line(fb_, COLOR_DIM, x, y, right - 8, y);
    y += 4;
    snprintf(buf, sizeof(buf), "Foe: %.10s L%u",
             foe.nickname[0] ? foe.nickname : speciesName(foe.species),
             (unsigned)foe.level);
    pax_draw_text(fb_, COLOR_TEXT, pax_font_sky_mono, FONT_PX, x, y, buf);
    y += LINE_PX;
    uint32_t foe_color = COLOR_TEXT;
    if (foe.maxHp > 0) {
        uint32_t pct = (uint32_t)foe.hp * 100u / foe.maxHp;
        if      (pct <= 25) foe_color = 0xFFFF5050;
        else if (pct <= 50) foe_color = 0xFFFFCC40;
        else                foe_color = 0xFF60E060;
    }
    snprintf(buf, sizeof(buf), "HP %u/%u", (unsigned)foe.hp, (unsigned)foe.maxHp);
    pax_draw_text(fb_, foe_color, pax_font_sky_mono, FONT_PX, x, y, buf);
}

void MatsuMonsterTerminal::drawIdlePanel(int x, int y, int right)
{
    char buf[64];

    // ── Party (live SRAM read; falls back to a hint line on no-party) ──
    pax_draw_text(fb_, COLOR_HEADER, pax_font_sky_mono, FONT_PX,
                  x, y, "── PARTY ──");
    y += LINE_PX + 2;

    Gen1Party party = {};
    uint8_t n = sram_ ? DaycareSavPatcher::buildGen1Party(sram_, &party) : 0;
    if (n == 0) {
        pax_draw_text(fb_, COLOR_DIM, pax_font_sky_mono, FONT_PX,
                      x, y, "(no Gen 1 save)");
        y += LINE_PX + 4;
    } else {
        for (uint8_t i = 0; i < n && i < 6; ++i) {
            const auto   &m   = party.mons[i];
            const char   *nick = (party.nicknames[i][0] != 0)
                                   ? (const char *)party.nicknames[i]
                                   : speciesName(m.species);
            snprintf(buf, sizeof(buf), "%u. %.10s L%u",
                     (unsigned)(i + 1), nick, (unsigned)m.level);
            pax_draw_text(fb_, COLOR_TEXT, pax_font_sky_mono, FONT_PX,
                          x, y, buf);
            y += LINE_PX;

            uint16_t hp    = ((uint16_t)m.hp[0]    << 8) | m.hp[1];
            uint16_t maxHp = ((uint16_t)m.maxHp[0] << 8) | m.maxHp[1];
            uint32_t hp_color = COLOR_DIM;
            if (maxHp > 0) {
                uint32_t pct = (uint32_t)hp * 100u / maxHp;
                if      (pct == 0)  hp_color = 0xFF808080;
                else if (pct <= 25) hp_color = 0xFFFF5050;
                else if (pct <= 50) hp_color = 0xFFFFCC40;
                else                hp_color = 0xFF60E060;
            }
            snprintf(buf, sizeof(buf), "   HP %u/%u", (unsigned)hp,
                     (unsigned)maxHp);
            pax_draw_text(fb_, hp_color, pax_font_sky_mono, FONT_PX,
                          x, y, buf);
            y += LINE_PX + 2;
        }
    }

    // ── Command cheatsheet ──
    y += 4;
    pax_draw_line(fb_, COLOR_DIM, x, y, right - 8, y);
    y += 4;
    pax_draw_text(fb_, COLOR_HEADER, pax_font_sky_mono, FONT_PX,
                  x, y, "── COMMANDS ──");
    y += LINE_PX + 2;

    static const char *kCmds[] = {
        "run        random fight",
        "fight <n>  mesh battle",
        "party      show team",
        "status     mon stats",
        "help       all commands",
        "quit       exit term",
    };
    for (size_t i = 0; i < sizeof(kCmds) / sizeof(kCmds[0]); ++i) {
        pax_draw_text(fb_, COLOR_TEXT, pax_font_sky_mono, FONT_PX,
                      x, y, kCmds[i]);
        y += LINE_PX;
    }
    y += 4;
    pax_draw_text(fb_, COLOR_PROMPT, pax_font_sky_mono, FONT_PX,
                  x, y, "Fn+T / ESC = exit");
}

// ── Helpers ─────────────────────────────────────────────────────────────────

const char *MatsuMonsterTerminal::speciesName(uint8_t dexNum)
{
    if (dexNum == 0 || dexNum > 151) return "???";
    return daycareSpeciesNames[dexNum];
}
