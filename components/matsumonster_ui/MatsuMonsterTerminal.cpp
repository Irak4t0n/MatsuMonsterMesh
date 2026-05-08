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
#include <string.h>
#include <stdarg.h>

// BSP headers are plain C with no `extern "C"` guards of their own — wrap
// here so bsp_display_blit() etc. link to their C definitions. PAX headers
// (below) are C++-aware and must NOT be wrapped (they pull in <initializer_list>).
extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/display.h"
#include "bsp/input.h"
}
#include "pax_fonts.h"
#include "pax_text.h"

#include "PokemonDaycare.h"
#include "MonsterMeshTextBattle.h"
#include "MeshtasticRadio.h"
#include "emulator_sram_iface.h"
#include "DaycareData.h"     // daycareSpeciesNames[]
#include "PokemonData.h"     // Gen1Party

// ── Layout constants (logical px) ───────────────────────────────────────────
static constexpr int   FONT_PX      = 12;     // pax font size in px
static constexpr int   LINE_PX      = 14;     // line stride
static constexpr int   MARGIN_X     = 6;
static constexpr int   HEADER_Y     = 4;
static constexpr int   HEADER_H     = 18;
static constexpr int   INPUT_H      = 18;

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

// ─────────────────────────────────────────────────────────────────────────────

MatsuMonsterTerminal::MatsuMonsterTerminal(PokemonDaycare        *daycare,
                                           MonsterMeshTextBattle *battle,
                                           MeshtasticRadio       *radio,
                                           IEmulatorSRAM         *sram)
  : daycare_(daycare), battle_(battle), radio_(radio), sram_(sram)
{}

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

    // Cursor blink — toggle every 500ms; no event needed, just dirty the frame.
    uint32_t t = now_ms();
    if (t - blink_ms_ >= 500) {
        blink_ms_  = t;
        cursor_on_ = !cursor_on_;
        dirty_     = true;
    }
}

void MatsuMonsterTerminal::onKeyboard(char ascii, uint32_t modifiers)
{
    // Fn+T toggles back to emulator (matches the combo Step 6 uses to enter).
    if ((modifiers & BSP_INPUT_MODIFIER_FUNCTION) &&
        (ascii == 't' || ascii == 'T')) {
        wants_exit_ = true;
        return;
    }

    if (ascii == '\n' || ascii == '\r') { submitInput(); return; }
    if (ascii == '\b' || ascii == 127)  { backspaceInput(); return; }
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
        dirty_ = true;
    }
}

void MatsuMonsterTerminal::backspaceInput()
{
    if (input_len_ > 0) {
        input_buf_[--input_len_] = '\0';
        dirty_ = true;
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
    println("  quit          — back to emulator");
}

void MatsuMonsterTerminal::cmdParty()
{
    if (!daycare_) { println("(no daycare wired)"); return; }
    const auto &state = daycare_->getState();
    if (!daycare_->isActive() || state.partyCount == 0) {
        println("Daycare empty. Check in from the emulator first.");
        return;
    }
    printf_line("Party (%u):", (unsigned)state.partyCount);
    for (uint8_t i = 0; i < state.partyCount; ++i) {
        const auto &p = state.pokemon[i];
        const char *nick = (p.nickname[0] != '\0') ? p.nickname : speciesName(p.speciesDex);
        uint8_t lvl = p.savLevel + (uint8_t)p.totalLevelsGained;
        printf_line("  %u. %-10s L%-3u  (%s)",
                    (unsigned)(i + 1), nick, (unsigned)lvl,
                    speciesName(p.speciesDex));
    }
}

void MatsuMonsterTerminal::cmdStatus()
{
    if (!daycare_)  { println("(no daycare wired)"); return; }
    if (!radio_)    { println("(no radio wired)");   return; }

    const auto &state = daycare_->getState();
    printf_line("Daycare:    %s", daycare_->isActive() ? "active" : "idle");
    printf_line("Party:      %u", (unsigned)state.partyCount);
    printf_line("Neighbors:  %d (radio reports %d)",
                (int)daycare_->getNeighborCount(),
                radio_->getNeighborCount());
    printf_line("Achievements: 0x%016llx",
                (unsigned long long)state.achievementFlags);

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

    // Build a placeholder Gen1Party from the daycare state. A real
    // implementation will translate state.pokemon[] -> Gen1Pokemon stats
    // (mapping exp curve / DVs / moves). For now we pass an empty default
    // party so the engine can boot — the wire protocol is the part we
    // actually care about exercising in stub mode.
    Gen1Party myParty = {};
    myParty.count = 1;
    myParty.species[0] = daycare_->getState().pokemon[0].speciesDex;

    battle_->startNetworkedAsInitiator(match->nodeId, myParty);
    printf_line("Challenging %s (0x%08x)...", match->shortName, (unsigned)match->nodeId);
}

void MatsuMonsterTerminal::cmdRun()
{
    if (!battle_) { println("(battle subsystem not wired)"); return; }
    Gen1Party myParty  = {};   // TODO: derive from daycare state / SRAM
    Gen1Party cpuParty = {};
    myParty.count  = 1;
    cpuParty.count = 1;
    if (daycare_ && daycare_->getState().partyCount > 0) {
        myParty.species[0] = daycare_->getState().pokemon[0].speciesDex;
    } else {
        myParty.species[0] = 25;   // Pikachu default
    }
    cpuParty.species[0] = 19;       // Rattata
    battle_->startLocal(myParty, cpuParty);
    println("Roguelike battle started. (See terminal log for turns.)");
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
    if (!dirty_) return;

    pax_background(fb_, COLOR_BG);
    drawHeader();
    drawScrollback();
    drawInputLine();

    bsp_display_blit(0, 0, canvas_w_, canvas_h_, pax_buf_get_pixels(fb_));
    dirty_ = false;
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

    // Separator under header
    pax_draw_line(fb_, COLOR_DIM,
                  MARGIN_X, HEADER_H,
                  canvas_w_ - MARGIN_X, HEADER_H);
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

    // Scroll-position hint in the right margin if user has scrolled up.
    if (scroll_offset_ > 0) {
        char hint[16];
        snprintf(hint, sizeof(hint), "[-%d]", scroll_offset_);
        pax_draw_text(fb_, COLOR_DIM, pax_font_sky_mono, FONT_PX,
                      canvas_w_ - 60, top, hint);
    }
}

void MatsuMonsterTerminal::drawInputLine()
{
    int y = canvas_h_ - INPUT_H;

    pax_draw_line(fb_, COLOR_DIM,
                  MARGIN_X, y - 2,
                  canvas_w_ - MARGIN_X, y - 2);

    pax_draw_text(fb_, COLOR_PROMPT, pax_font_sky_mono, FONT_PX,
                  MARGIN_X, y, "> ");
    pax_draw_text(fb_, COLOR_INPUT, pax_font_sky_mono, FONT_PX,
                  MARGIN_X + 16, y, input_buf_);

    if (cursor_on_) {
        // Approximate monospace advance: pax_font_sky_mono at size 12 is ~7px wide.
        int cx = MARGIN_X + 16 + input_len_ * 7;
        pax_draw_rect(fb_, COLOR_CURSOR, cx, y + 2, 6, FONT_PX);
    }
}

// ── Helpers ─────────────────────────────────────────────────────────────────

const char *MatsuMonsterTerminal::speciesName(uint8_t dexNum)
{
    if (dexNum == 0 || dexNum > 151) return "???";
    return daycareSpeciesNames[dexNum];
}
