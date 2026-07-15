// SPDX-License-Identifier: GPL-3.0-or-later
//
// MatsuMonsterTerminal - fullscreen text terminal.
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

// BSP headers are plain C with no `extern "C"` guards of their own - wrap
// here so bsp_display_blit() etc. link to their C definitions. PAX headers
// (below) are C++-aware and must NOT be wrapped (they pull in <initializer_list>).
extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"   // wild-encounter species + level jitter
#include "bsp/display.h"
#include "bsp/input.h"

// Defined in main/main.c - Alt+M from the terminal forwards here so the
// user can swap to the Meshtastic UI app without first leaving the
// terminal. main.c's reboot helper sets the AppFS bootsel and resets.
bool restart_to_meshtastic(void);
}
static inline bool mm_restart_to_meshtastic(void) {
    return restart_to_meshtastic();
}
#include "pax_fonts.h"
#include "pax_text.h"

#include "FastText.h"
#include "PokemonDaycare.h"
#include "MonsterMeshTextBattle.h"
#include "MeshtasticRadio.h"
#include "meshtastic_lora.h"     // Path #3 smoke-test commands
#include "meshtastic_proto.h"    // Path #3 Session 2a parsed-header view
#include "emulator_sram_iface.h"
#include "gnuboy_sram.h"        // gnuboy_wram_bank1()
#include "DaycareData.h"        // daycareSpeciesNames[]
#include "DaycareSavPatcher.h"  // buildGen1Party, readPartyFromWRAM
#include "PokemonData.h"        // Gen1Party
#include "mqtt_transport.h"     // mqtt_transport_is_connected()
#include "LordGyms.h"          // LORD gym rosters
#include "LordE4.h"            // LORD Elite Four + Champion
#include "LordRoutes.h"        // LORD route-based wild encounters
#include "LordLogic.h"         // LORD progression logic + NG+

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

// Forward decl - defined later in the file, used by cmdFight/cmdRun.
static void fill_placeholder_mon(Gen1Pokemon &m,
                                 uint8_t species, uint8_t level,
                                 uint8_t move_id, uint8_t move_pp);

// ── Wild encounter pool ────────────────────────────────────────────────────
// Common Gen 1 wilds from the Viridian / Pallet / Route 1 / Viridian Forest
// area - direct port of upstream MonsterMesh's AREA_POOL_0
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
// or Slam (80) - that's how Paras one-shots a same-level lead. Cap is
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

    // STAB moves first - same min as upstream so STAB still beats filler.
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
        battle_->setPartySupplier(
            [](Gen1Party *out, void *ctx) -> bool {
                auto *self = static_cast<MatsuMonsterTerminal *>(ctx);
                if (!self->loadSavParty()) return false;
                *out = self->sav_party_;
                return true;
            },
            this);
    }
}

void MatsuMonsterTerminal::setRenderTarget(pax_buf_t *fb, int canvas_w, int canvas_h)
{
    fb_       = fb;
    canvas_w_ = canvas_w;
    canvas_h_ = canvas_h;
    fast_text_init(fb);
}

void MatsuMonsterTerminal::setInputQueue(QueueHandle_t q)
{
    input_q_ = q;
}

void MatsuMonsterTerminal::prepareForReentry()
{
    // Cancel any battle the user left mid-fight before exiting via Fn+T.
    // Without this, battle_->isActive() stays true on re-entry and the
    // engine intercepts every keystroke at the `>` prompt - typing letters
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
    panel_dirty_       = true;   // panel party may have changed
    refreshPanelParty();
}

void MatsuMonsterTerminal::begin()
{
    scroll_head_ = scroll_fill_ = scroll_offset_ = 0;
    input_len_   = 0;
    input_buf_[0] = '\0';
    wants_exit_  = false;
    dirty_       = true;
    panel_dirty_ = true;
    refreshPanelParty();

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

    // Cursor blink - toggle every 500ms. Only the input line needs to
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
    // ctor - no polling needed here.
    if (battle_->isActive()) {
        battle_->tick(now_ms());
        if (battle_->dirty()) {
            panel_dirty_ = true;  // HP/moves changed - redraw battle panel
            battle_->clearDirty();
        }
        // Track switches for XP participation
        uint8_t cur = battle_->engine().party(battle_->mySide()).active;
        if (cur != battle_prev_active_) {
            battle_participated_ |= (1 << cur);
            battle_prev_active_ = cur;
        }
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
        panel_dirty_ = true;   // switch from battle panel → idle panel
        refreshPanelParty();
    }

    // XP payout: when a battle that was ours-to-win just ended in P1_WIN,
    // write the gained XP into the live SRAM (so it carries back into the
    // emulator save) AND tell the daycare so its session counters
    // (totalXpGained / totalLevelsGained) reflect battle wins too.
    bool just_finished = battle_was_active_ && !battle_->isActive();
    if (just_finished && xp_pending_) {
        xp_pending_ = false;
        bool won = battle_->engine().result() == Gen1BattleEngine::Result::P1_WIN;
        if (won && sram_) {
            const auto &eng = battle_->engine();
            const auto &myParty = eng.party(battle_->mySide());
            uint32_t xp_total = (uint32_t)last_foe_level_ * 8u;

            // Detect Gen 2: patchPokemon uses Gen 1 SRAM offsets which
            // would corrupt a Crystal/Gold/Silver save.
            const char *romName = gnuboy_rom_name();
            bool gen2 = romName && (strstr(romName, "CRYSTAL") ||
                                    strstr(romName, "GOLD") ||
                                    strstr(romName, "SILVER"));

            // Use the explicit participation bitmask (set at battle start
            // and on every switch-in). Exclude fainted Pokemon (hp == 0).
            uint8_t participated = battle_participated_;
            for (uint8_t i = 0; i < myParty.count; ++i) {
                if ((participated & (1 << i)) && myParty.mons[i].hp == 0)
                    participated &= ~(1 << i);
            }
            uint8_t n_parts = 0;
            for (uint8_t i = 0; i < myParty.count; ++i)
                if (participated & (1 << i)) n_parts++;
            if (n_parts == 0) { n_parts = 1; participated = (1 << myParty.active); }
            uint32_t xp_each = xp_total / n_parts;
            if (xp_each == 0) xp_each = 1;

            uint8_t *buf = iemu_sram_data(sram_);
            bool can_patch_g1 = !gen2 && buf &&
                             iemu_sram_size(sram_) >= DaycareSavPatcher::SAV_MIN_BYTES;
            uint8_t *wram = gen2 ? gnuboy_wram_bank1() : nullptr;

            for (uint8_t i = 0; i < myParty.count; ++i) {
                if (!(participated & (1 << i))) continue;
                const auto &m = myParty.mons[i];
                const char *name = m.nickname[0] ? m.nickname
                                 : (m.species >= 1 && m.species <= 151)
                                       ? speciesName(m.species) : "???";

                if (can_patch_g1 && m.species >= 1 && m.species <= 151) {
                    // Gen 1: patch SRAM directly
                    uint8_t nl = DaycareSavPatcher::patchPokemon(
                                     buf, i, m.species, xp_each);
                    printf_line("%s gained %u XP!", name, (unsigned)xp_each);
                    if (nl > m.level)
                        printf_line("%s reached L%u!", name, (unsigned)nl);
                    if (daycare_) daycare_->recordBattleWin(i, xp_each, nl);
                } else if (gen2 && wram) {
                    // Gen 2: patch WRAM directly (emulator is paused)
                    uint8_t nl = DaycareSavPatcher::patchGen2WRAM(
                                     wram, i, m.species, xp_each);
                    printf_line("%s gained %u XP!", name, (unsigned)xp_each);
                    if (nl > m.level)
                        printf_line("%s reached L%u!", name, (unsigned)nl);
                    if (daycare_) daycare_->recordBattleWin(i, xp_each, nl);
                } else {
                    printf_line("%s gained %u XP!", name, (unsigned)xp_each);
                    if (daycare_) daycare_->recordBattleWin(i, xp_each, 0);
                }
            }
            if (can_patch_g1) {
                DaycareSavPatcher::fixChecksum(buf);
                iemu_sram_mark_dirty(sram_);
            }
        }

        // LORD progression on win.
        if (won) {
            if (battle_type_ == BattleType::GYM) {
                lord_save_.gymProgress[battle_gym_idx_] = battle_trainer_ + 1;
                if (battle_trainer_ + 1 >= LORD_GYM_TRAINERS) {
                    lordOnGymCleared(lord_save_, battle_gym_idx_);
                    const LordGym *g = lordGym(battle_gym_idx_);
                    printf_line("*** You earned the %s Badge! ***",
                                g ? g->badgeName : "???");
                } else {
                    const LordGym *g = lordGym(battle_gym_idx_);
                    printf_line("Trainer defeated! (%u/%u in %s Gym)",
                                (unsigned)(battle_trainer_ + 1),
                                (unsigned)LORD_GYM_TRAINERS,
                                g ? g->city : "?");
                }
                lordSaveAndReport();
            } else if (battle_type_ == BattleType::E4) {
                lord_save_.e4Progress = battle_gym_idx_ + 1;
                const LordE4Member *m = lordE4Member(battle_gym_idx_);
                printf_line("*** %s %s defeated! ***",
                            m ? m->title : "???", m ? m->name : "???");

                if (battle_gym_idx_ + 1 >= LORD_E4_COUNT) {
                    lordOnE4Cleared(lord_save_);
                    println("*** CHAMPION DEFEATED! League cleared! ***");
                    if (lord_save_.ngPlusTier < 5) {
                        lord_save_.ngPlusTier++;
                        lordSetCurrentNgPlusTier(lord_save_.ngPlusTier);
                        printf_line("NG+%u unlocked! Gyms and E4 are stronger now.",
                                    (unsigned)lord_save_.ngPlusTier);
                    }
                } else {
                    uint8_t next = battle_gym_idx_ + 1;
                    const LordE4Member *nm = lordE4Member(next);
                    if (nm) printf_line("Next: %s %s. Type `e4` to continue.",
                                        nm->title, nm->name);
                }
                lordSaveAndReport();
            }
        } else if (battle_type_ == BattleType::E4) {
            // Lost to E4 - reset progress back to Lorelei.
            lord_save_.e4Progress = 0;
            println("Blacked out... E4 progress reset to Lorelei.");
            lordSaveAndReport();
        }
        battle_type_ = BattleType::WILD;  // reset for next battle
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
    // Fn+M jumps to the chat view.
    if ((modifiers & BSP_INPUT_MODIFIER_FUNCTION) &&
        (ascii == 'm' || ascii == 'M')) {
        wants_chat_ = true;
        return;
    }
    // Alt+M → reboot into the Tanmatsu Meshtastic UI app. Same shortcut
    // main.c registers for the emulator state - duplicated here so the
    // user can swap apps without first leaving the terminal.
    if ((modifiers & BSP_INPUT_MODIFIER_ALT) &&
        (ascii == 'm' || ascii == 'M')) {
        if (mm_restart_to_meshtastic()) return;   // unreachable on success
        println("(Alt+M: Meshtastic UI app not installed.)");
        println("Install Nicolai-Electronics/tanmatsu-meshtastic-ui");
        println("via BadgeLink first.");
        return;
    }

    // Battle just ended (Phase::FINISHED) - any keystroke dismisses the
    // result screen. Do that here AND fall through so the same keystroke
    // also reaches the terminal-input path. Otherwise the user loses the
    // first character of the command they're trying to type next.
    if (battle_ && battle_->isActive() &&
        battle_->phase() == MonsterMeshTextBattle::Phase::FINISHED) {
        battle_->exit();
        dirty_ = true;
        panel_dirty_ = true;
        refreshPanelParty();
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
            panel_dirty_ = true;  // battle panel HP/moves may have changed
            return;
        }
        // Fall through: keystroke goes to the terminal input buffer.
    }

    if (ascii == '\n' || ascii == '\r') { submitInput(); return; }
    // NOTE: backspace is intentionally NOT handled here. The Tanmatsu BSP
    // fires *both* a NAVIGATION event (key=BACKSPACE) and a KEYBOARD event
    // (ascii='\b') for a single physical press. submitInput / wants_exit_
    // are idempotent so Enter/ESC double-firing is invisible, but
    // backspaceInput is destructive - handling both paths deletes two
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
        // Only the bottom input line needs to redraw - keep the scrollback
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
    // Commands may start/end battles or change party state - refresh panel.
    panel_dirty_ = true;
    refreshPanelParty();
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
    // Game commands
    if      (starts_with(cmd, "party",  &args))  cmdParty();
    else if (starts_with(cmd, "status", &args) || starts_with(cmd, "st", &args))  cmdStatus();
    else if (starts_with(cmd, "fight",  &args) || starts_with(cmd, "f",  &args))  cmdFight(args);
    else if (starts_with(cmd, "run",    &args))  cmdRun();
    else if (starts_with(cmd, "catch",  &args))  cmdCatch();
    else if (starts_with(cmd, "gym",   &args))  cmdGym(args);
    else if (starts_with(cmd, "e4",    &args))  cmdE4();
    else if (starts_with(cmd, "lord",  &args))  cmdLord();
    // LoRa commands (short: ls, lr, li, lp, lt)
    else if (starts_with(cmd, "lora_send",  &args) || starts_with(cmd, "ls", &args)) cmdLoraSend(args);
    else if (starts_with(cmd, "lora_stats", &args) || starts_with(cmd, "lr", &args)) cmdLoraStats();
    else if (starts_with(cmd, "lora_reinit", &args) || starts_with(cmd, "li", &args)) cmdLoraReinit();
    else if (starts_with(cmd, "lora_probe", &args) || starts_with(cmd, "lp", &args)) cmdLoraProbe();
    else if (starts_with(cmd, "lora_tx_test", &args) || starts_with(cmd, "lt", &args)) cmdLoraTxTest(args);
    // Mesh commands (short: mr, mm, ms, ma, mn)
    else if (starts_with(cmd, "mesh_recent", &args) || starts_with(cmd, "mr", &args)) cmdMeshRecent();
    else if (starts_with(cmd, "mesh_messages", &args) || starts_with(cmd, "mm", &args)) cmdMeshMessages();
    else if (starts_with(cmd, "mesh_send", &args) || starts_with(cmd, "ms", &args)) cmdMeshSend(args);
    else if (starts_with(cmd, "mesh_announce", &args) || starts_with(cmd, "ma", &args)) cmdMeshAnnounce(args);
    else if (starts_with(cmd, "mesh_nodes", &args) || starts_with(cmd, "mn", &args)) cmdMeshNodes();
    // Daycare / MQTT (short: db, mq)
    else if (starts_with(cmd, "daycare_beacon", &args) || starts_with(cmd, "db", &args)) cmdDaycareBeacon();
    else if (starts_with(cmd, "mqtt_status", &args) || starts_with(cmd, "mq", &args)) cmdMqttStatus();
    // Channel commands (short: cl, ca, cd, cs, cr)
    else if (starts_with(cmd, "ch_list", &args) || starts_with(cmd, "cl", &args))    cmdChList();
    else if (starts_with(cmd, "ch_add",  &args) || starts_with(cmd, "ca", &args))    cmdChAdd(args);
    else if (starts_with(cmd, "ch_del",  &args) || starts_with(cmd, "cd", &args))    cmdChDel(args);
    else if (starts_with(cmd, "ch_set",  &args) || starts_with(cmd, "cs", &args))    cmdChSet(args);
    else if (starts_with(cmd, "ch_reset", &args) || starts_with(cmd, "cr", &args))   cmdChReset();
    // Misc
    else if (starts_with(cmd, "clear",  &args))  cmdClear();
    else if (starts_with(cmd, "quit",   &args) || starts_with(cmd, "q", &args))  cmdQuit();
    else if (starts_with(cmd, "exit",   &args))  cmdQuit();
    else if (starts_with(cmd, "help",   &args))  cmdHelp();
    else if (starts_with(cmd, "?",      &args))  cmdHelp();
    else                                         printf_line("Unknown command: %s", cmd);
}

void MatsuMonsterTerminal::cmdHelp()
{
    println("Commands:");
    println(" -- Game --");
    println("  party       - list daycare party");
    println("  st          - daycare status + neighbors");
    println("  f <name>    - mirror match vs neighbor");
    println("  run         - wild encounter (badge route)");
    println("  catch       - catch current wild Pokemon");
    println("  gym [N]     - list/challenge gym N");
    println("  e4          - challenge Elite Four");
    println("  lord        - LORD save summary");
    println(" -- LoRa --");
    println("  ls <text>   - send raw probe on LongFast");
    println("  lr          - LoRa init/TX/RX counters");
    println("  li          - retry LoRa bring-up");
    println("  lp          - exercise LoRa primitives");
    println("  lt <N>      - send N-byte test packet");
    println(" -- Mesh --");
    println("  mr          - recent Meshtastic packets");
    println("  mm          - text messages only");
    println("  ms <text>   - broadcast text on LongFast");
    println("  ma          - broadcast NodeInfo");
    println("  mn          - list known nodes");
    println("  db          - force daycare beacon TX");
    println("  mq          - MQTT/WiFi status");
    println(" -- Channels --");
    println("  cl          - list channels");
    println("  ca <n> <k>  - add channel (name + PSK)");
    println("  cd <N>      - remove channel N");
    println("  cs <N>      - set active TX channel");
    println("  cr          - reset to defaults");
    println(" --");
    println("  clear       - clear scrollback");
    println("  q           - back to emulator");
}

void MatsuMonsterTerminal::cmdParty()
{
    const uint8_t *wram = gnuboy_wram_bank1();
    if (!wram) {
        println("(emulator WRAM not available - load a ROM first)");
        return;
    }

    // Detect Gen 2 from ROM name
    const char *romName = gnuboy_rom_name();
    bool gen2 = romName && (strstr(romName, "CRYSTAL") ||
                            strstr(romName, "GOLD") ||
                            strstr(romName, "SILVER"));
    ESP_LOGI("party", "ROM: \"%s\" gen2=%d", romName ? romName : "(null)", gen2);

    DaycarePartyInfo party[6];
    uint8_t count = gen2
        ? DaycareSavPatcher::readPartyFromWRAM_Gen2(wram, party)
        : DaycareSavPatcher::readPartyFromWRAM(wram, party);

    if (count == 0) {
        println("Party empty (no Pokemon data found).");
        return;
    }

    printf_line("Party (%u):", (unsigned)count);
    for (uint8_t i = 0; i < count; ++i) {
        const auto &p = party[i];
        const char *nick = (p.nickname[0] != '\0')
                               ? p.nickname
                               : speciesName(p.dexNum);
        printf_line("  %u. %-10s L%u",
                    (unsigned)(i + 1), nick, (unsigned)p.level);
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
    uint8_t nbrCnt = daycare_->getNeighborCount();
    printf_line("Neighbors:  %d (mesh nodes: %d)",
                (int)nbrCnt, radio_->getNeighborCount());
    const auto *nbrs = daycare_->getNeighbors();
    for (uint8_t i = 0; i < nbrCnt; ++i) {
        const char *lead = (nbrs[i].partyCount > 0 && nbrs[i].party[0].species > 0)
                               ? speciesName(nbrs[i].party[0].species) : "?";
        printf_line("  [%s] %s - %s lv%u (%u mon)",
                    nbrs[i].shortName,
                    nbrs[i].gameName[0] ? nbrs[i].gameName : "(no name)",
                    lead,
                    (unsigned)(nbrs[i].partyCount > 0 ? nbrs[i].party[0].level : 0),
                    (unsigned)nbrs[i].partyCount);
    }
    printf_line("Achievements: 0x%016llx",
                (unsigned long long)state.achievementFlags);

    // Per-Pokémon session counters - updated by cmdRun's XP payout +
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
    if (!battle_ || !daycare_) {
        println("(battle subsystem not wired)");
        return;
    }
    if (!*args) {
        println("Usage: fight <neighbor short name>");
        return;
    }

    if (daycare_->getNeighborCount() == 0) {
        println("No mesh neighbors yet - wait for beacons.");
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
        println("Try `status` to see who's nearby.");
        return;
    }

    if (match->partyCount == 0) {
        printf_line("%s has no party data yet.", match->shortName);
        return;
    }

    // Player party from the live save.
    Gen1Party myParty;
    if (!loadSavParty()) {
        println("(No live save - load a Pokemon ROM first.)");
        return;
    }
    myParty = sav_party_;

    // Build opponent party from the neighbor's beacon data.
    Gen1Party cpuParty = {};
    uint8_t n = match->partyCount > 6 ? 6 : match->partyCount;
    cpuParty.count = n;
    for (uint8_t i = 0; i < n; ++i) {
        const auto &src = match->party[i];
        if (src.species == 0) { cpuParty.count = i; break; }
        // If beacon carried no moves (all zero), give Tackle so the CPU
        // can actually attack (older beacons or beacons before check-in).
        uint8_t moves[4];
        memcpy(moves, src.moves, 4);
        if (moves[0] == 0 && moves[1] == 0 && moves[2] == 0 && moves[3] == 0) {
            pick_moves_for_species(src.species, src.level, moves);
        }
        build_wild_mon(cpuParty.mons[i], src.species, src.level, moves);
        cpuParty.species[i] = src.species;
        const char *nick = src.nickname[0] ? src.nickname : speciesName(src.species);
        snprintf((char *)cpuParty.nicknames[i], 11, "%s", nick);
    }

    if (cpuParty.count == 0) {
        printf_line("%s has no valid Pokemon.", match->shortName);
        return;
    }

    const char *trainerName = match->gameName[0] ? match->gameName : match->shortName;
    printf_line("=== Mirror Match vs %s ===", trainerName);
    printf_line("They have %u Pokemon, led by %s (L%u).",
                (unsigned)cpuParty.count,
                speciesName(cpuParty.species[0]),
                (unsigned)cpuParty.mons[0].level);

    battle_type_         = BattleType::WILD;  // no LORD progression
    last_battle_line_[0] = '\0';
    last_foe_species_    = cpuParty.species[0];
    last_foe_level_      = cpuParty.mons[0].level;
    xp_pending_          = true;
    battle_participated_ = (1 << 0);  // lead participates
    battle_prev_active_  = 0;
    battle_->startLocal(myParty, cpuParty, "A trainer battle begins!");
    println("1-4 = move, S = switch, F = flee, ESC = forfeit.");
}

// Re-read the live Gen1Party from the bound IEmulatorSRAM each call.
// Originally cached on first use (mirroring upstream's pattern), but that
// meant `run`/`fight` could fire with stale data after the player saved
// in-game. The read is cheap (~800 bytes from PSRAM) so there's no
// reason to cache. Returns true if at least one Pokémon was read.
bool MatsuMonsterTerminal::loadSavParty()
{
    // Prefer WRAM (live state) over SRAM (last save) so battles use the
    // player's current in-game party, even if they haven't saved recently.
    const uint8_t *wram = gnuboy_wram_bank1();
    if (wram) {
        const char *romName = gnuboy_rom_name();
        bool gen2 = romName && (strstr(romName, "CRYSTAL") ||
                                strstr(romName, "GOLD") ||
                                strstr(romName, "SILVER"));
        uint8_t n = gen2
            ? DaycareSavPatcher::buildGen1PartyFromWRAM_Gen2(wram, &sav_party_)
            : DaycareSavPatcher::buildGen1PartyFromWRAM(wram, &sav_party_);
        sav_party_ready_ = (n > 0);
        if (n > 0) return true;
    }
    // Fallback to SRAM if WRAM isn't available
    if (!sram_) return false;
    uint8_t n = DaycareSavPatcher::buildGen1Party(sram_, &sav_party_);
    sav_party_ready_ = (n > 0);
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
        println("(No live save detected - using placeholder Pikachu.)");
    }

    // CPU opponent: pick from LORD route based on badge count (falls back
    // to the hardcoded kWildPool if LORD isn't loaded or route fails).
    Gen1Party cpuParty = {};

    ensureLordLoaded();
    uint8_t bc = 0;
    for (uint8_t i = 0; i < 8; ++i) if (lordHasBadge(lord_save_, i)) ++bc;
    uint8_t routeIdx = bc < 8 ? bc : 7;
    const LordRoute *route = lordRoute(routeIdx);

    bool route_ok = lordPickWildEncounter(routeIdx, cpuParty);
    if (route_ok) {
        if (route) printf_line("[%s]", route->name);
    } else {
        // Fallback: original hardcoded pool.
        cpuParty.count = 1;
        uint8_t cpuLevel = 3;
        if (real_party && myParty.mons[0].level > 0) {
            int target = (int)myParty.mons[0].level - 3;
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
    }

    uint8_t cpuSpecies = cpuParty.species[0];
    uint8_t cpuLevel   = cpuParty.mons[0].level;

    // Player placeholder fallback (no live save): give it a real nickname
    // so the engine's "[POKEMON] used X!" template doesn't render blank.
    if (!real_party) {
        snprintf((char *)myParty.nicknames[0], 11, "%s", speciesName(25));
    }

    battle_type_         = BattleType::WILD;
    last_battle_line_[0] = '\0';
    last_foe_species_    = cpuSpecies;
    last_foe_level_      = cpuLevel;
    xp_pending_          = true;
    battle_participated_ = (1 << 0);
    battle_prev_active_  = 0;
    battle_->startLocal(myParty, cpuParty);

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
        println("No wild Pokemon to catch - try `run` first.");
        return;
    }
    if (battle_->mode() != MonsterMeshTextBattle::Mode::LOCAL_ROGUELIKE) {
        println("Can't catch a trainer's Pokemon!");
        return;
    }
    if (!sram_) {
        println("(catch: no SRAM bound - can't write to save)");
        return;
    }

    // ── Catch chance ──────────────────────────────────────────────────
    // Hand-tuned for the roguelike pacing - Gen 1's actual formula uses
    // ball type, status, and a divisor that's brutal at full HP. Ours:
    //   base 30% + 1% per missing HP%, +25% if foe is statused, capped 95%.
    // Quick reference at L5 vs full-HP foe → 30%; at 1HP → 95%.
    uint8_t foeSide = 1 - battle_->mySide();
    const auto &foe = battle_->engine().party(foeSide).mons[
                          battle_->engine().party(foeSide).active];
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
    // BattlePoke doesn't track DVs after init - re-use the same default
    // build_wild_mon writes (0x88/0x88, the upstream wild-encounter pair).
    cm.dvs[0] = 0x88;
    cm.dvs[1] = 0x88;
    snprintf(cm.nickname, sizeof(cm.nickname), "%s",
             foe.nickname[0] ? foe.nickname : speciesName(foe.species));

    // ── Write the record + checksum ───────────────────────────────────
    int dest = DaycareSavPatcher::addCaughtMon(sram_, cm);
    if (dest == 0) {
        printf_line("Gotcha! %s was added to your party.", foeName);
    } else if (dest >= 1 && dest <= 12) {
        printf_line("Gotcha! %s was sent to PC Box %d.", foeName, dest);
    } else if (dest == -1) {
        println("Caught it... but party AND all PC boxes are full!");
        return;   // leave battle running so the player can flee/forfeit
    } else {
        println("(catch: SRAM write failed)");
        return;
    }

    uint8_t *buf = iemu_sram_data(sram_);
    if (buf) DaycareSavPatcher::fixChecksum(buf);

    // No XP for caught mons (matches Gen 1 R/B - defeating gives XP, not
    // catching). Clear the pending-XP edge so pumpBattle's just_finished
    // branch doesn't try to also pay out for this fight.
    xp_pending_ = false;
    battle_->exit();
    dirty_ = true;
}

// ── LORD commands ───────────────────────────────────────────────────────────

void MatsuMonsterTerminal::ensureLordLoaded()
{
    if (!lord_loaded_) {
        lordLoad(lord_save_);
        lordSetCurrentNgPlusTier(lord_save_.ngPlusTier);
        lord_loaded_ = true;
    }
}

void MatsuMonsterTerminal::lordSaveAndReport()
{
    if (lordSave(lord_save_)) {
        // silent success
    } else {
        println("(warning: failed to save LORD progress)");
    }
}

void MatsuMonsterTerminal::cmdLord()
{
    ensureLordLoaded();
    printf_line("=== Legend of Charizard ===");

    // Badge display
    uint8_t bc = 0;
    for (uint8_t i = 0; i < 8; ++i) if (lordHasBadge(lord_save_, i)) ++bc;
    printf_line("Badges: %u/8", (unsigned)bc);

    static const char *kBadgeNames[8] = {
        "Boulder", "Cascade", "Thunder", "Rainbow",
        "Soul", "Marsh", "Volcano", "Earth"
    };
    char badges_str[64] = {};
    int pos = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        if (lordHasBadge(lord_save_, i)) {
            if (pos > 0) pos += snprintf(badges_str + pos, sizeof(badges_str) - pos, ", ");
            pos += snprintf(badges_str + pos, sizeof(badges_str) - pos, "%s", kBadgeNames[i]);
        }
    }
    if (pos > 0) printf_line("  %s", badges_str);

    if (lord_save_.leagueCleared)
        printf_line("League: CLEARED (NG+%u)", (unsigned)lord_save_.ngPlusTier);
    else
        println("League: not yet cleared");

    printf_line("Runs: %lu total, best %u waves",
                (unsigned long)lord_save_.totalRuns,
                (unsigned)lord_save_.bestRunWaves);
}

void MatsuMonsterTerminal::cmdGym(const char *args)
{
    if (!battle_) { println("(battle subsystem not wired)"); return; }
    ensureLordLoaded();

    // No argument: list gyms.
    if (!args || !*args) {
        static const char *kCities[8] = {
            "Pewter", "Cerulean", "Vermilion", "Celadon",
            "Fuchsia", "Saffron", "Cinnabar", "Viridian"
        };
        println("=== Kanto Gyms ===");
        for (uint8_t i = 0; i < 8; ++i) {
            const LordGym *g = lordGym(i);
            bool unlocked = lordGymUnlocked(lord_save_, i);
            bool cleared  = lordHasBadge(lord_save_, i);
            const char *status = cleared ? "CLEARED" :
                                 unlocked ? "unlocked" : "locked";
            printf_line("  %u. %s (%s) - %s",
                        (unsigned)(i + 1), kCities[i],
                        g ? g->leaderName : "?", status);
        }
        println("Use: gym <N> to challenge (1-8)");
        return;
    }

    int gymNum = atoi(args);
    if (gymNum < 1 || gymNum > 8) {
        println("Usage: gym <1-8>");
        return;
    }
    uint8_t gymIdx = (uint8_t)(gymNum - 1);

    if (!lordGymUnlocked(lord_save_, gymIdx)) {
        println("That gym is locked - clear the previous gym first.");
        return;
    }

    const LordGym *g = lordGym(gymIdx);
    if (!g) { println("(bad gym data)"); return; }

    // Determine next trainer to fight.
    uint8_t trainerIdx = lord_save_.gymProgress[gymIdx];
    if (trainerIdx >= LORD_GYM_TRAINERS) {
        printf_line("%s Gym already cleared! You have the %s Badge.",
                    g->city, g->badgeName);
        return;
    }

    const LordGymTrainer &tr = g->trainers[trainerIdx];

    // Build player party.
    Gen1Party myParty;
    if (!loadSavParty()) {
        println("(No live save - load a Pokemon ROM first.)");
        return;
    }
    myParty = sav_party_;

    // Build gym trainer party.
    Gen1Party cpuParty;
    if (!lordBuildGymParty(gymIdx, trainerIdx, cpuParty)) {
        println("(failed to build gym party)");
        return;
    }

    if (trainerIdx == LORD_GYM_LEADER_INDEX) {
        printf_line("=== %s Gym Leader %s! ===", g->city, g->leaderName);
    } else {
        printf_line("=== %s Gym: %s ===", g->city, tr.name);
    }
    printf_line("Trainer has %u Pokemon.", (unsigned)cpuParty.count);

    battle_type_    = BattleType::GYM;
    battle_gym_idx_ = gymIdx;
    battle_trainer_ = trainerIdx;
    last_foe_species_ = cpuParty.mons[0].species;
    last_foe_level_   = cpuParty.mons[0].level;
    xp_pending_       = true;
    battle_participated_ = (1 << 0);
    battle_prev_active_  = 0;
    last_battle_line_[0] = '\0';
    battle_->startLocal(myParty, cpuParty, "A trainer battle begins!");
    println("1-4 = move, S = switch, F = flee, ESC = forfeit.");
}

void MatsuMonsterTerminal::cmdE4()
{
    if (!battle_) { println("(battle subsystem not wired)"); return; }
    ensureLordLoaded();

    // Require all 8 badges.
    uint8_t bc = 0;
    for (uint8_t i = 0; i < 8; ++i) if (lordHasBadge(lord_save_, i)) ++bc;
    if (bc < 8) {
        printf_line("You need all 8 badges to challenge the Elite Four. (%u/8)", bc);
        return;
    }

    // Determine next E4 member.
    uint8_t idx = lord_save_.e4Progress;
    if (idx >= LORD_E4_COUNT) idx = 0;  // restart from Lorelei

    const LordE4Member *m = lordE4Member(idx);
    if (!m) { println("(bad E4 data)"); return; }

    // Build player party.
    Gen1Party myParty;
    if (!loadSavParty()) {
        println("(No live save - load a Pokemon ROM first.)");
        return;
    }
    myParty = sav_party_;

    // Build E4 party.
    Gen1Party cpuParty;
    if (!lordBuildE4Party(idx, cpuParty)) {
        println("(failed to build E4 party)");
        return;
    }

    printf_line("=== %s %s (%s type) ===", m->title, m->name, m->typeFlavor);
    printf_line("They have %u Pokemon (L%u-%u).",
                (unsigned)cpuParty.count,
                (unsigned)cpuParty.mons[0].level,
                (unsigned)cpuParty.mons[cpuParty.count - 1].level);

    battle_type_    = BattleType::E4;
    battle_gym_idx_ = idx;
    battle_trainer_ = 0;
    last_foe_species_ = cpuParty.mons[0].species;
    last_foe_level_   = cpuParty.mons[0].level;
    xp_pending_       = true;
    battle_participated_ = (1 << 0);
    battle_prev_active_  = 0;
    last_battle_line_[0] = '\0';
    battle_->startLocal(myParty, cpuParty, "A trainer battle begins!");
    println("1-4 = move, S = switch, F = flee, ESC = forfeit.");
}

// ── Path #3 Session 1 smoke-test commands ──────────────────────────────────
//
// `lora_send <text>` ships a raw byte payload over the air on LongFast US
// 907.125 MHz. Other Meshtastic devices on the same channel will RECEIVE
// it at the wire level - they won't decode it as a Meshtastic packet (we
// don't have the protocol layer yet), but they may log "rx packet of N
// bytes, decode failed" or similar, which is enough to prove the link.
//
// `lora_stats` prints counters: configs applied, TX attempted/ok/err,
// RX packets/bytes. RX should increment whenever ANY other Meshtastic
// device on LongFast US is transmitting nearby - useful for confirming
// the radio is actually listening to live mesh traffic.

void MatsuMonsterTerminal::cmdLoraSend(const char *args)
{
    if (!meshtastic_lora_is_up()) {
        println("(lora_send: radio not up - meshtastic_lora_begin failed)");
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
    println("-- LoRa stats --");
    printf_line("  init_ok        %u", (unsigned)s.init_ok);
    printf_line("  configs        %u", (unsigned)s.configs_applied);
    printf_line("  tx attempted   %u", (unsigned)s.tx_attempted);
    printf_line("  tx ok          %u", (unsigned)s.tx_ok);
    printf_line("  tx err         %u", (unsigned)s.tx_err);
    printf_line("  rx packets     %u", (unsigned)s.rx_packets);
    printf_line("  rx bytes total %u", (unsigned)s.rx_bytes_total);
    printf_line("  relay sent     %u", (unsigned)meshtastic_proto_total_relayed());
    println("-- Diagnostics --");
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
        println("-- C6 current config --");
        printf_line("  freq    %u Hz",  (unsigned)r.cur_frequency);
        printf_line("  SF      %u",     (unsigned)r.cur_sf);
        printf_line("  BW      %u kHz", (unsigned)r.cur_bw);
        printf_line("  CR      4/%u",   (unsigned)r.cur_cr);
    } else {
        println("(C6 didn't return a valid config - get_config failed)");
    }
}

// ── Path #3 Session 2a - Meshtastic protocol view ───────────────────────────
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
    printf_line("-- mesh_recent (parsed %u total) --",
                (unsigned)meshtastic_proto_total_parsed());
    if (n == 0) {
        println("(no packets yet - wait for T-Deck Plus to broadcast)");
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
        char portcol[24] = "-";
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
    println("-- mesh_messages (text only) --");
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
    printf_line("-- mesh_nodes (%u known) --", (unsigned)n);
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

// ── Path #3 Session 2d - Meshtastic TX commands ─────────────────────────────

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
    // Simplest possible split - find the last space; everything before
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
        println("TX FAILED - check lora_stats");
    } else {
        println("TX status unclear - check lora_stats");
    }
}

void MatsuMonsterTerminal::cmdLoraTxTest(const char *args)
{
    if (!meshtastic_lora_is_up()) {
        println("(radio not up - try lora_reinit)");
        return;
    }
    int n = 0;
    if (!args || !*args || (n = atoi(args)) < 1) {
        println("Usage: lora_tx_test <size>");
        println("  Sends an N-byte raw packet (16-byte Meshtastic header");
        println("  + (N-16) dummy payload). N must be 16..256.");
        println("  Use to test TX at various payload sizes.");
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

void MatsuMonsterTerminal::cmdMqttStatus()
{
    bool wifi = mqtt_transport_wifi_up();
    bool mqtt = mqtt_transport_is_connected();
    println("-- MQTT status --");
    printf_line("  WiFi: %s", wifi ? "connected" : "not connected");
    printf_line("  MQTT: %s", mqtt ? "CONNECTED" : "not connected");
    ESP_LOGI("mqtt_cmd", "mqtt_status: wifi=%d mqtt=%d", wifi, mqtt);
}

void MatsuMonsterTerminal::cmdChList()
{
    println("-- Channels --");
    uint8_t active = meshtastic_channel_get_tx();
    for (int i = 0; i < MESHTASTIC_MAX_CHANNELS; i++) {
        const meshtastic_channel_t *ch = meshtastic_channel_get(i);
        if (!ch) continue;
        printf_line("  %s%d: %-16s hash=0x%02X psk=%dB",
                    (i == active) ? ">" : " ",
                    i + 1, ch->name, ch->hash, ch->psk_len);
    }
    printf_line("  Active TX: %d", active + 1);
}

// Parse hex string into bytes. Returns number of bytes parsed.
static int hex_to_bytes(const char *hex, uint8_t *out, int max_out)
{
    int n = 0;
    while (*hex && n < max_out) {
        // Skip spaces
        while (*hex == ' ') hex++;
        if (!*hex) break;

        uint8_t hi = 0, lo = 0;
        char c = *hex++;
        if (c >= '0' && c <= '9') hi = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') hi = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') hi = (uint8_t)(c - 'A' + 10);
        else return -1;

        c = *hex++;
        if (c >= '0' && c <= '9') lo = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') lo = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') lo = (uint8_t)(c - 'A' + 10);
        else return -1;

        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

// Standard Meshtastic default key (used by all preset channels: LongFast,
// LongSlow, MediumFast, MediumSlow, ShortFast, ShortSlow, etc.)
static const uint8_t MESHTASTIC_DEFAULT_KEY[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

void MatsuMonsterTerminal::cmdChAdd(const char *args)
{
    if (!args || !*args) {
        println("Usage: ch_add <name> <hex-psk>");
        println("       ch_add <name> default");
        println("  'default' uses the standard Meshtastic PSK");
        println("  Example: ch_add LongSlow default");
        println("  Example: ch_add MyChannel d4f1bb3a20290759f0bcffabcf4e6901");
        return;
    }
    // Split into name and PSK at first space after name
    char name[MESHTASTIC_CHANNEL_NAME_MAX] = {};
    const char *p = args;
    int ni = 0;
    while (*p && *p != ' ' && ni < (int)sizeof(name) - 1) {
        name[ni++] = *p++;
    }
    name[ni] = '\0';
    while (*p == ' ') p++;

    uint8_t psk[32] = {};
    uint8_t psk_len = 0;
    if (*p) {
        if (strncmp(p, "default", 7) == 0) {
            memcpy(psk, MESHTASTIC_DEFAULT_KEY, 16);
            psk_len = 16;
        } else {
            int n = hex_to_bytes(p, psk, 32);
            if (n != 16 && n != 32) {
                println("PSK must be 16 or 32 bytes, or 'default'");
                return;
            }
            psk_len = (uint8_t)n;
        }
    }

    int idx = meshtastic_channel_add(name, psk, psk_len);
    if (idx < 0) {
        println("Channel table full (max 8)");
    } else {
        const meshtastic_channel_t *ch = meshtastic_channel_get(idx);
        printf_line("Added channel %d: %s (hash=0x%02X)",
                    idx + 1, ch->name, ch->hash);
    }
}

void MatsuMonsterTerminal::cmdChDel(const char *args)
{
    if (!args || !*args) {
        println("Usage: ch_del <N>  (1-8)");
        return;
    }
    int idx = atoi(args) - 1;
    if (idx < 0 || idx >= MESHTASTIC_MAX_CHANNELS) {
        println("Invalid channel number (1-8)");
        return;
    }
    if (idx == 0) {
        println("Cannot remove channel 1 (LongFast)");
        return;
    }
    esp_err_t err = meshtastic_channel_remove((uint8_t)idx);
    if (err == ESP_OK) {
        printf_line("Removed channel %d", idx + 1);
    } else {
        printf_line("Channel %d not found", idx + 1);
    }
}

void MatsuMonsterTerminal::cmdChSet(const char *args)
{
    if (!args || !*args) {
        println("Usage: ch_set <N>  (1-8)");
        return;
    }
    int idx = atoi(args) - 1;
    if (idx < 0 || idx >= MESHTASTIC_MAX_CHANNELS) {
        println("Invalid channel number (1-8)");
        return;
    }
    const meshtastic_channel_t *ch = meshtastic_channel_get((uint8_t)idx);
    if (!ch) {
        printf_line("Channel %d is empty", idx + 1);
        return;
    }
    meshtastic_channel_set_tx((uint8_t)idx);
    printf_line("Active TX channel: %d (%s)", idx + 1, ch->name);
}

void MatsuMonsterTerminal::cmdChReset()
{
    meshtastic_channel_reset();
    println("Channels reset to defaults (LongFast + MonsterMesh)");
    cmdChList();
}

void MatsuMonsterTerminal::cmdClear()
{
    memset(scroll_, 0, sizeof(scroll_));
    scroll_head_ = 0;
    scroll_fill_ = 0;
    scroll_offset_ = 0;
    dirty_ = true;
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
    if (!dirty_ && !panel_dirty_ && !input_only_dirty_) return;

    bool need_heavy = dirty_ || panel_dirty_;
    last_render_ms_ = now_ms();

    int panel_x = canvas_w_ - PANEL_W;

    if (need_heavy) {
        // Clear the left region by zeroing the raw framebuffer directly.
        // CW rotation maps logical cols 0..panel_x-1 to physical rows
        // 0..panel_x-1 (contiguous), so a single memset does the job.
        uint16_t *raw = (uint16_t *)pax_buf_get_pixels(fb_);
        memset(raw, 0, panel_x * 480 * sizeof(uint16_t));

        drawHeader();
        drawScrollbackFast();
        drawInputLine();

        if (panel_dirty_) {
            // Clear panel region: physical rows panel_x..canvas_w_-1
            memset(raw + panel_x * 480, 0, PANEL_W * 480 * sizeof(uint16_t));
            drawSidePanel();
        }
        bsp_display_blit(0, 0, canvas_h_, canvas_w_, pax_buf_get_pixels(fb_));
    } else {
        int strip_y = canvas_h_ - INPUT_H - 8;
        pax_draw_rect(fb_, COLOR_BG, 0, strip_y,
                      panel_x, canvas_h_ - strip_y);
        drawInputLine();
        bsp_display_blit(0, 0, canvas_h_, canvas_w_, pax_buf_get_pixels(fb_));
    }

    dirty_            = false;
    panel_dirty_      = false;
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
    int panel_x = canvas_w_ - PANEL_W;
    fast_text_blit(fb_, MARGIN_X, HEADER_Y, hdr, COLOR_HEADER, panel_x);
    fast_hline(fb_, MARGIN_X, panel_x - 4, HEADER_H, COLOR_DIM);
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

    // Bottom-anchor: newest line sits just above the input separator.
    int y = bottom - shown * LINE_PX;
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

void MatsuMonsterTerminal::drawScrollbackFast()
{
    if (!fast_text_glyph_w()) { drawScrollback(); return; }

    int top    = HEADER_H + 4;
    int bottom = canvas_h_ - INPUT_H - 4;
    int height = bottom - top;
    int max_lines = height / LINE_PX;
    if (max_lines <= 0) return;

    int panel_x = canvas_w_ - PANEL_W;
    int last_index = scroll_head_ - 1;
    int shown = (scroll_fill_ < max_lines) ? scroll_fill_ : max_lines;
    int newest_visible = last_index - scroll_offset_;
    int oldest_visible = newest_visible - shown + 1;

    // Bottom-anchor: newest line sits just above the input separator.
    int y = bottom - shown * LINE_PX;
    for (int i = oldest_visible; i <= newest_visible; ++i) {
        if (i < last_index - scroll_fill_ + 1) { y += LINE_PX; continue; }
        int idx = ((i % SCROLL_LINES) + SCROLL_LINES) % SCROLL_LINES;
        fast_text_blit(fb_,MARGIN_X, y, scroll_[idx], COLOR_TEXT, panel_x);
        y += LINE_PX;
    }

    if (scroll_offset_ > 0) {
        char hint[16];
        snprintf(hint, sizeof(hint), "[-%d]", scroll_offset_);
        fast_text_blit(fb_,panel_x - 60, top, hint, COLOR_DIM, panel_x);
    }
}

void MatsuMonsterTerminal::drawInputLine()
{
    int y = canvas_h_ - INPUT_H;
    int panel_x = canvas_w_ - PANEL_W;
    int gw = fast_text_glyph_w();

    fast_hline(fb_, MARGIN_X, panel_x - 4, y - 2, COLOR_DIM);

    int input_x = MARGIN_X + gw * 2;  // "> " = 2 glyph widths
    fast_text_blit(fb_, MARGIN_X, y, "> ", COLOR_PROMPT, panel_x);

    // Scroll the visible window so the cursor stays within the left panel
    int avail_px = panel_x - input_x - gw;
    int vis_chars = (gw > 0) ? (avail_px / gw) : 0;
    if (vis_chars < 1) vis_chars = 1;

    const char *vis = input_buf_;
    int vis_len = input_len_;
    if (vis_len > vis_chars) {
        int skip = vis_len - vis_chars;
        vis     += skip;
        vis_len  = vis_chars;
    }

    fast_text_blit(fb_, input_x, y, vis, COLOR_INPUT, panel_x);

    if (cursor_on_) {
        int cx = input_x + vis_len * gw;
        if (cx < panel_x)
            fast_rect(fb_, cx, y + 2, 6, FONT_PX, COLOR_CURSOR);
    }
}

// ── Right-side context panel ────────────────────────────────────────────────
// Shows information that mirrors the current command/state: an active
// battle gets a moves panel (1-4 + PP/power, plus HP for both sides);
// otherwise we show a party summary + command cheatsheet so the user
// always has something useful in the otherwise-empty right column.

void MatsuMonsterTerminal::drawSidePanel()
{
    int px     = canvas_w_ - PANEL_W;       // panel left edge
    int top    = HEADER_Y;
    int bottom = canvas_h_ - 4;
    int right  = canvas_w_ - 4;

    // Vertical separator - draw as a 1px-wide column directly
    uint16_t *raw = (uint16_t *)pax_buf_get_pixels(fb_);
    uint16_t dim565 = (uint16_t)(((0x70>>3)<<11)|((0x70>>2)<<5)|(0x70>>3));
    int base = px * 480 + (480 - 1 - top);
    for (int ly = top; ly <= bottom; ly++)
        raw[base - (ly - top)] = dim565;

    int x = px + 8;
    int y = top + 2;

    bool in_battle = battle_ && battle_->isActive() &&
                     battle_->phase() != MonsterMeshTextBattle::Phase::FINISHED;

    if (in_battle) drawBattlePanel(x, y, right);
    else           drawIdlePanel  (x, y, right);
}

void MatsuMonsterTerminal::drawBattlePanel(int x, int y, int right)
{
    fast_text_blit(fb_, x, y, "-- BATTLE --", COLOR_HEADER, right);
    y += LINE_PX + 2;

    if (battle_->phase() == MonsterMeshTextBattle::Phase::WAIT_PARTY) {
        fast_text_blit(fb_, x, y, "Exchanging parties...", COLOR_DIM, right);
        return;
    }
    const auto &eng = battle_->engine();
    uint8_t side = battle_->mySide();
    const auto &mp  = eng.party(side);
    const auto &fp  = eng.party(1 - side);
    if (mp.count == 0 || fp.count == 0) return;
    const auto &me   = mp.mons[mp.active];
    const auto &foe  = fp.mons[fp.active];

    char buf[64];

    // ── WAIT_SWITCH: show party list with cursor ──
    if (battle_->phase() == MonsterMeshTextBattle::Phase::WAIT_SWITCH) {
        fast_text_blit(fb_, x, y, "-- SWITCH TO --", COLOR_HEADER, right);
        y += LINE_PX + 2;
        uint8_t cur = battle_->switchCursor();
        for (uint8_t i = 0; i < mp.count; ++i) {
            const auto &m = mp.mons[i];
            const char *mark = (i == cur) ? "> " : "  ";
            const char *tag  = (i == mp.active) ? " [out]" : "";
            uint32_t col = COLOR_TEXT;
            if (m.hp == 0) col = COLOR_DIM;
            else if (i == cur) col = COLOR_INPUT;
            snprintf(buf, sizeof(buf), "%s%.8s L%u%s",
                     mark,
                     m.nickname[0] ? m.nickname : speciesName(m.species),
                     (unsigned)m.level, tag);
            fast_text_blit(fb_, x, y, buf, col, right);
            y += LINE_PX;
            uint32_t hpc = COLOR_DIM;
            if (m.hp > 0 && m.maxHp > 0) {
                uint32_t pct = (uint32_t)m.hp * 100u / m.maxHp;
                if      (pct <= 25) hpc = 0xFFFF5050;
                else if (pct <= 50) hpc = 0xFFFFCC40;
                else                hpc = 0xFF60E060;
            }
            snprintf(buf, sizeof(buf), "  HP %u/%u",
                     (unsigned)m.hp, (unsigned)m.maxHp);
            fast_text_blit(fb_, x, y, buf, hpc, right);
            y += LINE_PX;
        }
        y += 4;
        fast_text_blit(fb_, x, y, "W/S=move  Enter=ok", COLOR_PROMPT, right);
        y += LINE_PX;
        fast_text_blit(fb_, x, y, "ESC=cancel", COLOR_PROMPT, right);
        y += LINE_PX + 4;
    } else {
        // ── Normal move selection panel ──
        snprintf(buf, sizeof(buf), "You: %.10s L%u",
                 me.nickname[0] ? me.nickname : "???", (unsigned)me.level);
        fast_text_blit(fb_, x, y, buf, COLOR_TEXT, right);
        y += LINE_PX;

        uint32_t hp_color = COLOR_TEXT;
        if (me.maxHp > 0) {
            uint32_t pct = (uint32_t)me.hp * 100u / me.maxHp;
            if      (pct <= 25) hp_color = 0xFFFF5050;
            else if (pct <= 50) hp_color = 0xFFFFCC40;
            else                hp_color = 0xFF60E060;
        }
        snprintf(buf, sizeof(buf), "HP %u/%u", (unsigned)me.hp, (unsigned)me.maxHp);
        fast_text_blit(fb_, x, y, buf, hp_color, right);
        y += LINE_PX + 4;

        for (int i = 0; i < 4; ++i) {
            if (me.moves[i] == 0) {
                snprintf(buf, sizeof(buf), "%d) ---", i + 1);
                fast_text_blit(fb_, x, y, buf, COLOR_DIM, right);
                y += LINE_PX * 2;
                continue;
            }
            const Gen1MoveData *m = gen1Move(me.moves[i]);
            if (!m) {
                snprintf(buf, sizeof(buf), "%d) #%u", i + 1, (unsigned)me.moves[i]);
                fast_text_blit(fb_, x, y, buf, COLOR_TEXT, right);
                y += LINE_PX * 2;
                continue;
            }
            uint32_t name_color = (me.pp[i] == 0) ? COLOR_DIM : COLOR_INPUT;
            snprintf(buf, sizeof(buf), "%d) %.14s", i + 1, m->name);
            fast_text_blit(fb_, x, y, buf, name_color, right);
            y += LINE_PX;
            snprintf(buf, sizeof(buf), "   PP %u/%u",
                     (unsigned)me.pp[i], (unsigned)m->pp);
            fast_text_blit(fb_, x, y, buf, COLOR_DIM, right);
            y += LINE_PX;
        }

        y += 4;
        fast_text_blit(fb_, x, y, "S=switch  F=flee", COLOR_PROMPT, right);
        y += LINE_PX;
        fast_text_blit(fb_, x, y, "type 'catch' to net", COLOR_PROMPT, right);
        y += LINE_PX;
        fast_text_blit(fb_, x, y, "ESC=forfeit", COLOR_PROMPT, right);
        y += LINE_PX + 4;
    }

    fast_hline(fb_, x, right - 8, y, COLOR_DIM);
    y += 4;
    snprintf(buf, sizeof(buf), "Foe: %.10s L%u",
             foe.nickname[0] ? foe.nickname : speciesName(foe.species),
             (unsigned)foe.level);
    fast_text_blit(fb_, x, y, buf, COLOR_TEXT, right);
    y += LINE_PX;
    uint32_t foe_color = COLOR_TEXT;
    if (foe.maxHp > 0) {
        uint32_t pct = (uint32_t)foe.hp * 100u / foe.maxHp;
        if      (pct <= 25) foe_color = 0xFFFF5050;
        else if (pct <= 50) foe_color = 0xFFFFCC40;
        else                foe_color = 0xFF60E060;
    }
    snprintf(buf, sizeof(buf), "HP %u/%u", (unsigned)foe.hp, (unsigned)foe.maxHp);
    fast_text_blit(fb_, x, y, buf, foe_color, right);
}

void MatsuMonsterTerminal::refreshPanelParty()
{
    const uint8_t *wram = gnuboy_wram_bank1();
    const char *romName = gnuboy_rom_name();
    bool gen2 = romName && (strstr(romName, "CRYSTAL") ||
                            strstr(romName, "GOLD") ||
                            strstr(romName, "SILVER"));
    if (gen2 && wram)
        cached_panel_party_count_ = DaycareSavPatcher::buildGen1PartyFromWRAM_Gen2(wram, &cached_panel_party_);
    else if (wram)
        cached_panel_party_count_ = DaycareSavPatcher::buildGen1PartyFromWRAM(wram, &cached_panel_party_);
    else if (sram_)
        cached_panel_party_count_ = DaycareSavPatcher::buildGen1Party(sram_, &cached_panel_party_);
    else
        cached_panel_party_count_ = 0;
}

void MatsuMonsterTerminal::drawIdlePanel(int x, int y, int right)
{
    char buf[64];

    fast_text_blit(fb_, x, y, "-- PARTY --", COLOR_HEADER, right);
    y += LINE_PX + 2;

    const Gen1Party &party = cached_panel_party_;
    uint8_t n = cached_panel_party_count_;
    if (n == 0) {
        fast_text_blit(fb_, x, y, "(no save data)", COLOR_DIM, right);
        y += LINE_PX + 4;
    } else {
        for (uint8_t i = 0; i < n && i < 6; ++i) {
            const auto   &m   = party.mons[i];
            const char   *nick = (party.nicknames[i][0] != 0)
                                   ? (const char *)party.nicknames[i]
                                   : speciesName(m.species);
            snprintf(buf, sizeof(buf), "%u. %.10s L%u",
                     (unsigned)(i + 1), nick, (unsigned)m.level);
            fast_text_blit(fb_, x, y, buf, COLOR_TEXT, right);
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
            uint32_t exp = ((uint32_t)m.exp[0] << 16) |
                           ((uint32_t)m.exp[1] << 8) | m.exp[2];
            uint8_t lvl = m.level ? m.level : m.boxLevel;
            uint32_t nextExp = (m.species >= 1 && m.species <= 251 && lvl < 100)
                                   ? expForLevel(m.species, lvl + 1) : 0;
            if (nextExp > 0)
                snprintf(buf, sizeof(buf), "   HP %u/%u  XP %lu/%lu",
                         (unsigned)hp, (unsigned)maxHp,
                         (unsigned long)exp, (unsigned long)nextExp);
            else
                snprintf(buf, sizeof(buf), "   HP %u/%u  XP %lu",
                         (unsigned)hp, (unsigned)maxHp, (unsigned long)exp);
            fast_text_blit(fb_, x, y, buf, hp_color, right);
            y += LINE_PX + 2;
        }
    }

    y += 4;
    fast_hline(fb_, x, right - 8, y, COLOR_DIM);
    y += 4;
    fast_text_blit(fb_, x, y, "-- COMMANDS --", COLOR_HEADER, right);
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
        fast_text_blit(fb_, x, y, kCmds[i], COLOR_TEXT, right);
        y += LINE_PX;
    }
    y += 4;
    fast_text_blit(fb_, x, y, "Fn+T / ESC = exit", COLOR_PROMPT, right);
}

// ── Helpers ─────────────────────────────────────────────────────────────────

const char *MatsuMonsterTerminal::speciesName(uint8_t dexNum)
{
    if (dexNum == 0 || dexNum > 151) return "???";
    return daycareSpeciesNames[dexNum];
}
