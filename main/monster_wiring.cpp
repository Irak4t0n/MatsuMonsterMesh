// SPDX-License-Identifier: GPL-3.0-or-later
//
// monster_wiring.cpp — C ABI implementation of the Step 6 wiring layer.
// See monster_wiring.h for rationale.

#include "monster_wiring.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "MeshtasticRadio.h"
#include "meshtastic_radio_lora.h"
#include "meshtastic_lora.h"     // Session 1 of Path #3: raw LoRa bring-up
#include "meshtastic_proto.h"    // Session 2a: parse 16-byte plaintext header
#include "PokemonDaycare.h"
#include "MonsterMeshTextBattle.h"
#include "MatsuMonsterTerminal.h"
#include "MeshtasticChatView.h"   // Path #3 Session 5: full Meshtastic UI
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

static LoRaMeshtasticRadio     s_radio;

static PokemonDaycare         s_daycare;
static MonsterMeshTextBattle  s_battle(&s_radio);
static IEmulatorSRAM          s_sram = {};

// MatsuMonsterTerminal is held by pointer so we can construct it after we
// know the canvas dims — same pattern main.c uses for several globals.
static MatsuMonsterTerminal  *s_terminal = nullptr;
// Path #3 Session 5: same storage trick for the chat view.
static MeshtasticChatView    *s_chat     = nullptr;

static monster_state_t        s_state    = MONSTER_STATE_EMULATOR;
static bool                   s_inited   = false;
static bool                   s_sram_ok  = false;

static inline uint32_t now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ── Daycare callback stubs ────────────────────────────────────────────────────
//
// These are wired to s_daycare.setSendBeacon / setBroadcast / setSendDm in
// monster_init(). Each callback has a `void *ctx` that we ignore (the
// static objects are file-scoped anyway).

static void daycare_send_beacon_cb(const DaycareBeacon &beacon, void *ctx)
{
    (void)ctx;
    DaycareBeacon b = beacon;
    b.nodeId = meshtastic_proto_node_id();
    size_t len = offsetof(DaycareBeacon, pokemon)
               + b.partyCount * sizeof(b.pokemon[0]);
    s_radio.sendPacket(0xFFFFFFFF, 0, (const uint8_t *)&b, len);
}

static void daycare_broadcast_cb(const char *msg, void *ctx)
{
    (void)ctx;
    // Achievement / flavour text → Meshtastic default text channel.
    meshtastic_send_text(msg);
}

static void daycare_send_dm_cb(uint32_t destNodeId, const char *msg, void *ctx)
{
    (void)ctx;
    // The upstream DM path sends to a specific node's text channel. We
    // don't have directed text messaging, so broadcast it instead — the
    // daycare DMs are fun flavour text that everyone can enjoy.
    (void)destNodeId;
    meshtastic_send_text(msg);
}

// ─────────────────────────────────────────────────────────────────────────────

extern "C" void monster_init(pax_buf_t *fb, int canvas_w, int canvas_h,
                             QueueHandle_t input_q)
{
    if (s_inited) return;

    // Session 1 of the P4 Meshtastic port: bring the LoRa radio up. The
    // upper layers (battle / daycare / terminal) still see the stub for
    // now — the LoRa wrapper just makes sure the C6 is on LongFast US
    // 907.125 MHz and parked in RX, so we can already test wire-level
    // traffic with `lora_send` / `lora_stats` from the terminal.
    esp_err_t lora_err = meshtastic_lora_begin();
    if (lora_err != ESP_OK) {
        ESP_LOGW(TAG, "meshtastic_lora_begin failed (%s) — radio inert",
                 esp_err_to_name(lora_err));
    } else {
        // Session 2a: spin up the Meshtastic protocol layer. The drainer
        // task pulls raw bytes off meshtastic_lora's queue and parses the
        // 16-byte header into a ring buffer that the terminal's
        // `mesh_recent` command surfaces.
        esp_err_t mp_err = meshtastic_proto_begin();
        if (mp_err != ESP_OK) {
            ESP_LOGW(TAG, "meshtastic_proto_begin failed (%s)",
                     esp_err_to_name(mp_err));
        }
    }

    // Session 4: wire the battle/daycare radio to the live LoRa stack.
    // Creates the PRIVATE_APP RX queue and registers the drain-task
    // callback. Degrades gracefully if the LoRa layer didn't come up
    // (sendPacket returns false, pollPackets returns 0).
    s_radio.begin();

    s_daycare.init();   // zero-init state, magic, partyCount=0 → tick() is a no-op until checkIn
    s_daycare.setSendBeacon(daycare_send_beacon_cb, nullptr);
    s_daycare.setBroadcast(daycare_broadcast_cb, nullptr);
    s_daycare.setSendDm(daycare_send_dm_cb, nullptr);

    static uint8_t terminal_storage[sizeof(MatsuMonsterTerminal)] alignas(MatsuMonsterTerminal);
    s_terminal = new (terminal_storage)
        MatsuMonsterTerminal(&s_daycare, &s_battle, &s_radio, &s_sram);
    s_terminal->setRenderTarget(fb, canvas_w, canvas_h);
    s_terminal->setInputQueue(input_q);
    s_terminal->begin();

    // Path #3 Session 5: full chat UI. Same placement-new trick as the
    // terminal so we never call C++ static-init on a class that depends
    // on canvas dims known only after BSP init.
    static uint8_t chat_storage[sizeof(MeshtasticChatView)] alignas(MeshtasticChatView);
    s_chat = new (chat_storage) MeshtasticChatView();
    s_chat->setRenderTarget(fb, canvas_w, canvas_h);
    s_chat->setInputQueue(input_q);

    s_inited = true;
    ESP_LOGI(TAG, "monster_wiring init done (radio=LoRa)");
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

    // Try the real Meshtastic short name from the NodeDB first; fall back
    // to the caller-supplied name (typically "MM01" until the radio is up).
    char realShort[MESHTASTIC_NODE_NAME_SHORT] = {};
    const char *sn = shortName ? shortName : "MM01";
    if (meshtastic_nodedb_lookup(meshtastic_proto_node_id(),
                                 NULL, 0, realShort, sizeof(realShort))
        && realShort[0] != '\0') {
        sn = realShort;
    }
    s_daycare.checkIn(sram, sn, gameName);
    s_daycare.forceBeacon();

    const auto &state = s_daycare.getState();
    ESP_LOGI(TAG, "auto-checkin: trainer='%s' shortName='%s' party=%u",
             gameName, sn, (unsigned)state.partyCount);
}

extern "C" void monster_daycare_tick(void)
{
    if (!s_inited) return;

    // ── Drain incoming PRIVATE_APP packets and route to battle / daycare ──
    // This runs every frame so we don't queue-starve during fast exchanges.
    // DaycareBeacon and TEXT_BATTLE_START share type byte 0x60 (upstream
    // MonsterMesh compat). We disambiguate by size: beacons are large
    // (>= sizeof(DaycareBeacon)), battle-start packets are small (~18 B).
    MeshPacketSimple pkts[4];
    int n = s_radio.pollPackets(pkts, 4);
    if (n > 0) {
        ESP_LOGI(TAG, "PRIVATE_APP: got %d pkt(s)", n);
    }
    for (int i = 0; i < n; i++) {
        if (pkts[i].payload_len < 1) continue;
        uint8_t type = pkts[i].payload[0];
        ESP_LOGI(TAG, "  pkt[%d] from=!%08lx type=0x%02x len=%u",
                 i, (unsigned long)pkts[i].from, type,
                 (unsigned)pkts[i].payload_len);
        // Full beacon (0x60) — sent by T-Deck Plus / upstream MonsterMesh.
        if (type == DAYCARE_BEACON_TYPE_FULL &&
            pkts[i].payload_len >= offsetof(DaycareBeacon, pokemon)) {
            DaycareBeacon beacon = {};
            size_t copy = pkts[i].payload_len < sizeof(DaycareBeacon)
                              ? pkts[i].payload_len : sizeof(DaycareBeacon);
            memcpy(&beacon, pkts[i].payload, copy);
            s_daycare.handleBeacon(beacon);
        // Compact beacon (0x61) — sent by other Tanmatsu devices.
        } else if (type == DAYCARE_BEACON_TYPE_COMPACT &&
                   pkts[i].payload_len >= offsetof(DaycareBeaconCompact, pokemon)) {
            DaycareBeaconCompact compact = {};
            size_t copy = pkts[i].payload_len < sizeof(DaycareBeaconCompact)
                              ? pkts[i].payload_len : sizeof(DaycareBeaconCompact);
            memcpy(&compact, pkts[i].payload, copy);
            DaycareBeacon beacon = {};
            daycareBeaconFromCompact(beacon, compact);
            s_daycare.handleBeacon(beacon);
        } else {
            // Everything else → battle engine (it checks PktType internally).
            s_battle.handlePacket(pkts[i].from,
                                  pkts[i].payload, pkts[i].payload_len);
        }
    }

    // ── Rate-limited daycare tick (beacons, events, mood, etc.) ───────────
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

// ── Path #3 Session 5: chat-mode plumbing ────────────────────────────

extern "C" void monster_enter_chat(void)
{
    if (!s_inited || s_state == MONSTER_STATE_CHAT) return;
    // Same SRAM-checkpoint discipline as monster_enter_terminal — the
    // chat session can take arbitrary time and we don't want a power
    // loss mid-session to drop in-emulator progress.
    mm_sram_persist();

    // From EMULATOR: stop audio. From TERMINAL: audio is already muted.
    if (s_state == MONSTER_STATE_EMULATOR) {
        audio_mute = 1;
        mm_audio_pause();
    }
    s_state = MONSTER_STATE_CHAT;
    if (s_chat) {
        s_chat->clearExitFlag();
        s_chat->clearTerminalJump();
        s_chat->begin();
    }
    ESP_LOGI(TAG, "state -> CHAT");
}

extern "C" bool monster_chat_pump(void)
{
    if (!s_inited || !s_chat) return false;
    if (s_state != MONSTER_STATE_CHAT) return false;

    s_chat->handleInput();
    s_chat->render();

    // Fn+T from inside the chat: jump straight to the terminal without
    // bouncing through emulator state. Same audio-mute regime carries
    // over since both states keep the emulator paused.
    if (s_chat->wantsTerminalJump()) {
        s_chat->clearTerminalJump();
        s_state = MONSTER_STATE_TERMINAL;
        if (s_terminal) {
            s_terminal->clearExitFlag();
            s_terminal->prepareForReentry();
            s_terminal->println("");
            s_terminal->println("=== Switched from chat (Fn+T) ===");
        }
        ESP_LOGI(TAG, "state -> TERMINAL (from CHAT)");
        return true;
    }

    if (s_chat->wantsToExit()) {
        s_chat->clearExitFlag();
        s_state    = MONSTER_STATE_EMULATOR;
        mm_audio_resume();
        audio_mute = 0;
        ESP_LOGI(TAG, "state -> EMULATOR (from CHAT)");
        return true;
    }
    return false;
}
