// SPDX-License-Identifier: GPL-3.0-or-later
//
// MonsterMeshTextBattle — turn-based Gen 1 battle engine wrapper + LoRa wire
// glue. Headless port for MatsuMonsterMesh: rendering is the terminal's
// responsibility (see MatsuMonsterTerminal in Step 5). This class owns the
// engine state, the scrolling log buffer, and the BattleLink packet codec.
//
// Two modes:
//   - NETWORKED: both peers run Gen1BattleEngine with the same RNG seed and
//     exchange only TEXT_BATTLE_ACTION packets. State never crosses the wire.
//   - LOCAL:     single-player vs CPU (used by the roguelike crawler).
//
// Mesh send/receive go through a MeshtasticRadio* (see components/meshtastic_radio).

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "Gen1BattleEngine.h"
#include "BattlePacket.h"

class MeshtasticRadio;   // components/meshtastic_radio/MeshtasticRadio.h

class MonsterMeshTextBattle {
public:
    static constexpr uint8_t LOG_LINES = 6;
    static constexpr uint8_t LOG_WIDTH = 40;     // chars per line

    // Meshtastic channel index used for MonsterMesh traffic (originally
    // documented as "MonsterMesh Center" in MeshtasticTransport.h). Step 4's
    // SerialMeshtasticRadio is responsible for honouring this.
    static constexpr uint8_t MM_CHANNEL = 1;
    static constexpr uint32_t MM_BROADCAST_ID = 0xFFFFFFFFu;

    enum class Mode  : uint8_t { OFF, NETWORKED, LOCAL_ROGUELIKE };
    enum class Phase : uint8_t {
        IDLE,           // not in a battle
        WAIT_ACTION,    // waiting for player input
        WAIT_REMOTE,    // submitted; waiting for opponent's action
        WAIT_SWITCH,    // showing switch menu
        ANIMATING,      // turn just resolved, scrolling messages
        FINISHED,       // result_ != ONGOING; press any key to exit
    };

    explicit MonsterMeshTextBattle(MeshtasticRadio *radio)
      : radio_(radio) {}

    // ── Lifecycle ───────────────────────────────────────────────────────────
    // Networked initiator. `myParty` is our current save's party; `remoteId`
    // is the peer node. We pick the seed and broadcast START.
    void startNetworkedAsInitiator(uint32_t remoteId, const Gen1Party &myParty);

    // Networked receiver. Called by handlePacket() on TEXT_BATTLE_START.
    void startNetworkedAsReceiver(uint32_t remoteId, const Gen1Party &myParty,
                                  uint32_t rngSeed);

    // Local roguelike battle. CPU runs side 1.
    void startLocal(const Gen1Party &myParty, const Gen1Party &cpuParty);

    void exit();

    bool isActive() const { return mode_ != Mode::OFF; }
    Mode mode()     const { return mode_; }
    Phase phase()   const { return phase_; }

    // ── Input ───────────────────────────────────────────────────────────────
    void handleKey(uint8_t c);

    // ── Wire ────────────────────────────────────────────────────────────────
    // Returns true if it consumed the packet (caller should not process further).
    bool handlePacket(uint32_t fromId, const uint8_t *buf, size_t len);

    // ── Pump ────────────────────────────────────────────────────────────────
    void tick(uint32_t nowMs);

    // ── State accessors for the terminal/UI ─────────────────────────────────
    // The terminal pulls these out and draws them with the BSP renderer.
    const Gen1BattleEngine &engine() const { return engine_; }
    uint16_t turn() const { return engine_.turn(); }
    uint8_t  cursor() const { return cursor_; }
    uint8_t  switchCursor() const { return switchCursor_; }
    uint32_t remoteId() const { return remoteId_; }

    // Scrolling log: returns count of lines currently revealed (after the
    // scroll-throttle delay) and copies them oldest-first into out[].
    uint8_t  visibleLogLines(char out[][LOG_WIDTH + 1], uint8_t maxLines) const;

    // Was UI state dirtied since last poll? Terminal calls clearDirty() after
    // it redraws.
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

private:
    MeshtasticRadio    *radio_ = nullptr;
    Gen1BattleEngine    engine_;

    Mode  mode_  = Mode::OFF;
    Phase phase_ = Phase::IDLE;

    uint32_t remoteId_ = 0;
    uint16_t session_  = 0;
    uint8_t  cursor_   = 0;          // selected move/party slot
    uint8_t  switchCursor_ = 0;
    bool     pendingRemoteAction_ = false;
    uint8_t  lastSentAction_ = 0xFF; // for retry
    uint8_t  lastSentIndex_  = 0;
    uint16_t lastSentTurn_   = 0;
    uint32_t lastSendMs_     = 0;
    uint8_t  fleeAttempts_   = 0;

    // Scrolling text log — circular buffer.
    char    log_[LOG_LINES][LOG_WIDTH + 1] = {};
    uint8_t logHead_ = 0;        // next write slot
    uint8_t logFill_ = 0;        // number of valid lines
    uint32_t scrollMs_ = 0;      // millis at which we can show the next line
    uint8_t  scrollPending_ = 0; // lines queued but not yet shown

    bool dirty_ = true;

    // Timeouts
    uint32_t lastRecvMs_ = 0;
    static constexpr uint32_t REMOTE_TIMEOUT_MS = 60000;   // 60s w/o packet → forfeit
    static constexpr uint32_t SCROLL_INTERVAL_MS = 600;    // text reveal cadence
    static constexpr uint32_t RESEND_INTERVAL_MS = 4000;   // re-broadcast our action

    // Local→engine helpers
    void appendLog(const char *line);
    static void engineLogCb(const char *line, void *ctx);

    void sendStart(uint32_t rngSeed, const Gen1Party &myParty);
    void sendAction(uint8_t actionType, uint8_t index);
    void sendForfeit();
    void sendHash();

    // After both sides submitted, run executeTurn and prep next phase.
    void resolveTurn();

    // Auto-replace a fainted active mon at the start of each input phase.
    void handleFaints();
};
