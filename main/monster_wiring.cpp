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
#include "DaycareSavPatcher.h"   // gen1CharToAscii + SAV_* constants

static const char *TAG = "MMWire";

// audio_mute / blit_task_handle live in main.c — declare them so we can
// pause emulator audio + display while the terminal owns the screen.
extern "C" volatile int  audio_mute;
extern "C" TaskHandle_t  blit_task_handle;

// I2S channel pause/resume helpers defined in main.c — needed so the audio
// hardware stops cleanly on terminal entry instead of looping the last
// DMA-loaded sample.
extern "C" void mm_audio_pause(void);
extern "C" void mm_audio_resume(void);

// Checkpoint helper: writes ram.sbank to /sdcard/saves/<rom>.sav. Called on
// terminal entry so any in-game Save done since the last Backspace is
// persisted before a reboot / badgelink relaunch can lose it.
extern "C" void mm_sram_persist(void);

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

extern "C" void monster_auto_checkin(const char *shortName)
{
    // Direct port of upstream MonsterMeshModule::daycareAutoCheckIn() —
    // see _refs_monster_mesh/.../MonsterMeshModule.cpp:2399.
    if (!s_inited || !s_sram_ok) {
        ESP_LOGW(TAG, "auto-checkin: wiring not ready");
        return;
    }
    const uint8_t *sram = iemu_sram_data(&s_sram);
    size_t         sram_sz = iemu_sram_size(&s_sram);
    // Need at least 0x2598 + 7 bytes to read the trainer name, plus the
    // full SAV layout for daycare.checkIn() to read the party safely.
    if (!sram || sram_sz < (size_t)SAV_CHECKSUM_OFFSET + 1) {
        ESP_LOGW(TAG, "auto-checkin: SRAM unavailable or too small (size=%u)",
                 (unsigned)sram_sz);
        return;
    }

    // Decode trainer name from Gen 1 player-name offset (0x2598, 7 bytes,
    // Gen 1 charset, 0x50 terminator).
    char gameName[8] = {};
    for (int i = 0; i < 7; ++i) {
        uint8_t c = sram[0x2598 + i];
        if (c == 0x50) break;
        gameName[i] = gen1CharToAscii(c);
    }

    const char *sn = shortName ? shortName : "MM01";
    s_daycare.checkIn(sram, sn, gameName);

    const auto &state = s_daycare.getState();
    ESP_LOGI(TAG, "auto-checkin: trainer='%s' shortName='%s' party=%u",
             gameName, sn, (unsigned)state.partyCount);
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
    // Checkpoint any in-game Save the player did since the last Backspace —
    // otherwise a reboot / app-relaunch from inside the terminal loses it.
    mm_sram_persist();
    s_state = MONSTER_STATE_TERMINAL;
    audio_mute = 1;
    mm_audio_pause();   // stop the I2S DMA so it doesn't loop the last sample
    if (s_terminal) {
        s_terminal->clearExitFlag();    // ignore stale exit from previous session
        s_terminal->prepareForReentry();// bail out of stale mid-battle state
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
        mm_audio_resume();   // I2S DMA back online before unmuting
        audio_mute = 0;
        ESP_LOGI(TAG, "state -> EMULATOR");
        return true;
    }
    return false;
}
