// SPDX-License-Identifier: GPL-3.0-or-later
//
// monster_wiring.cpp — C ABI implementation of the Step 6 wiring layer.
// See monster_wiring.h for rationale.

#include "monster_wiring.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "MeshtasticRadio.h"
#include "meshtastic_radio_stub.h"
#include "meshtastic_radio_serial.h"
#include "PokemonDaycare.h"
#include "MonsterMeshTextBattle.h"
#include "MatsuMonsterTerminal.h"
#include "emulator_sram_iface.h"
#include "gnuboy_sram.h"

static const char *TAG = "MMWire";

// audio_mute / blit_task_handle live in main.c — declare them so we can
// pause emulator audio + display while the terminal owns the screen.
extern "C" volatile int  audio_mute;
extern "C" TaskHandle_t  blit_task_handle;

// ── Static storage for the C++ subsystem ────────────────────────────────────

#ifdef CONFIG_MATSUMONSTER_MESH_STUB
static StubMeshtasticRadio    s_radio;
#else
static SerialMeshtasticRadio  s_radio;
#endif

static PokemonDaycare         s_daycare;
static MonsterMeshTextBattle  s_battle(&s_radio);
static IEmulatorSRAM          s_sram = {};

// MatsuMonsterTerminal is held by pointer so we can construct it after we
// know the canvas dims — same pattern main.c uses for several globals.
static MatsuMonsterTerminal  *s_terminal = nullptr;

static monster_state_t        s_state    = MONSTER_STATE_EMULATOR;
static bool                   s_inited   = false;
static bool                   s_sram_ok  = false;

static inline uint32_t now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ─────────────────────────────────────────────────────────────────────────────

extern "C" void monster_init(pax_buf_t *fb, int canvas_w, int canvas_h,
                             QueueHandle_t input_q)
{
    if (s_inited) return;

    s_daycare.init();   // zero-init state, magic, partyCount=0 → tick() is a no-op until checkIn

    static uint8_t terminal_storage[sizeof(MatsuMonsterTerminal)] alignas(MatsuMonsterTerminal);
    s_terminal = new (terminal_storage)
        MatsuMonsterTerminal(&s_daycare, &s_battle, &s_radio, &s_sram);
    s_terminal->setRenderTarget(fb, canvas_w, canvas_h);
    s_terminal->setInputQueue(input_q);
    s_terminal->begin();

    s_inited = true;
    ESP_LOGI(TAG,
             "monster_wiring init done (radio=%s)",
#ifdef CONFIG_MATSUMONSTER_MESH_STUB
             "Stub"
#else
             "Serial"
#endif
            );
}

extern "C" void monster_init_sram(void)
{
    // Caller (main.c) is required to call us only AFTER gnuboy's loader has
    // allocated ram.sbank. gnuboy_sram_init points the iface fields at the
    // live emulator state — see components/emulator_sram_iface/gnuboy_sram.c.
    gnuboy_sram_init(&s_sram);
    s_sram_ok = true;
    ESP_LOGI(TAG, "IEmulatorSRAM bound (size=%u)",
             (unsigned)iemu_sram_size(&s_sram));
}

extern "C" void monster_daycare_tick(void)
{
    if (!s_inited) return;
    static uint32_t last_tick_ms = 0;
    uint32_t t = now_ms();
    if (t - last_tick_ms < 10000) return;   // 10s cadence (matches Step 6 spec)
    last_tick_ms = t;
    s_daycare.tick(t);
}

extern "C" monster_state_t monster_get_state(void)
{
    return s_state;
}

extern "C" void monster_enter_terminal(void)
{
    if (!s_inited || s_state == MONSTER_STATE_TERMINAL) return;
    s_state = MONSTER_STATE_TERMINAL;
    audio_mute = 1;
    if (s_terminal) {
        s_terminal->clearExitFlag();   // ignore stale exit from previous session
        // Re-print the banner so the user knows they're in the terminal.
        s_terminal->println("");
        s_terminal->println("=== Entered terminal (Fn+T or ESC to exit) ===");
    }
    ESP_LOGI(TAG, "state -> TERMINAL");
}

extern "C" bool monster_terminal_pump(void)
{
    if (!s_inited || !s_terminal) return false;
    if (s_state != MONSTER_STATE_TERMINAL) return false;

    s_terminal->handleInput();
    s_terminal->render();

    if (s_terminal->wantsToExit()) {
        s_terminal->clearExitFlag();
        s_state    = MONSTER_STATE_EMULATOR;
        audio_mute = 0;
        ESP_LOGI(TAG, "state -> EMULATOR");
        return true;
    }
    return false;
}
