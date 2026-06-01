// SPDX-License-Identifier: GPL-3.0-or-later
// See MonsterMeshTextBattle.h for description.

#include "MonsterMeshTextBattle.h"
#include "monster_core_compat.h"
#include "MeshtasticRadio.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

// ── Logging glue from engine ────────────────────────────────────────────────

void MonsterMeshTextBattle::engineLogCb(const char *line, void *ctx)
{
    static_cast<MonsterMeshTextBattle *>(ctx)->appendLog(line);
}

void MonsterMeshTextBattle::appendLog(const char *line)
{
    if (!line) return;
    snprintf(log_[logHead_], sizeof(log_[logHead_]), "%s", line);
    logHead_ = (logHead_ + 1) % LOG_LINES;
    if (logFill_ < LOG_LINES) logFill_++;
    scrollPending_++;
    dirty_ = true;
    // Push immediately to whoever's listening (the terminal's scrollback).
    // Without this hook the caller has to poll visibleLogLines() — and that
    // path silently drops every intermediate line of a multi-line turn.
    if (ext_log_cb_) ext_log_cb_(line, ext_log_ctx_);
}

uint8_t MonsterMeshTextBattle::visibleLogLines(char out[][LOG_WIDTH + 1], uint8_t maxLines) const
{
    uint8_t shown = (logFill_ > scrollPending_) ? (uint8_t)(logFill_ - scrollPending_) : 0;
    if (shown > maxLines) shown = maxLines;
    for (uint8_t i = 0; i < shown; ++i) {
        int idx = (logHead_ + LOG_LINES - shown + i) % LOG_LINES;
        snprintf(out[i], LOG_WIDTH + 1, "%s", log_[idx]);
    }
    return shown;
}

// ── Networked initiator ─────────────────────────────────────────────────────

void MonsterMeshTextBattle::startNetworkedAsInitiator(uint32_t remoteId,
                                                     const Gen1Party &myParty)
{
    mode_     = Mode::NETWORKED;
    phase_    = Phase::WAIT_PARTY;
    remoteId_ = remoteId;
    session_  = (uint16_t)(mm_millis() & 0xFFFF);
    mySide_   = 0;   // initiator is always side 0
    cursor_   = 0; switchCursor_ = 0;
    pendingRemoteAction_ = false;
    logFill_ = logHead_ = 0; scrollPending_ = 0;

    uint32_t rngSeed = (uint32_t)(esp_random() ^ remoteId ^ session_);
    pendingRngSeed_ = rngSeed;
    myParty_ = myParty;
    memset(&remoteParty_, 0, sizeof(remoteParty_));
    partyChunksRecv_  = 0;
    partyChunksTotal_ = 0;

    sendStart(rngSeed, myParty);
    sendParty();
    lastSendMs_ = mm_millis();
    appendLog("Battle started!");
    appendLog("Exchanging parties...");
    lastRecvMs_ = mm_millis();
    dirty_ = true;
}

void MonsterMeshTextBattle::startNetworkedAsReceiver(uint32_t remoteId,
                                                     const Gen1Party &myParty,
                                                     uint32_t rngSeed)
{
    mode_     = Mode::NETWORKED;
    phase_    = Phase::WAIT_PARTY;
    remoteId_ = remoteId;
    mySide_   = 1;   // receiver is always side 1
    cursor_   = 0; switchCursor_ = 0;
    logFill_ = logHead_ = 0; scrollPending_ = 0;

    pendingRngSeed_ = rngSeed;
    myParty_ = myParty;
    memset(&remoteParty_, 0, sizeof(remoteParty_));
    partyChunksRecv_  = 0;
    partyChunksTotal_ = 0;

    sendParty();
    lastSendMs_ = mm_millis();
    appendLog("Battle started!");
    appendLog("Exchanging parties...");
    lastRecvMs_ = mm_millis();
    dirty_ = true;
}

void MonsterMeshTextBattle::startLocal(const Gen1Party &myParty,
                                       const Gen1Party &cpuParty,
                                       const char *intro)
{
    mode_         = Mode::LOCAL_ROGUELIKE;
    phase_        = Phase::WAIT_ACTION;
    remoteId_     = 0;
    mySide_       = 0;
    cursor_       = 0; switchCursor_ = 0;
    fleeAttempts_ = 0;
    logFill_ = logHead_ = 0; scrollPending_ = 0;

    uint32_t rngSeed = (uint32_t)(mm_millis() ^ esp_random());
    engine_.start(myParty, cpuParty, rngSeed);
    appendLog(intro ? intro : "A wild battle begins!");
    dirty_ = true;
}

void MonsterMeshTextBattle::exit()
{
    mode_ = Mode::OFF;
    phase_ = Phase::IDLE;
    dirty_ = true;
}

// ── Wire packet emit/receive ────────────────────────────────────────────────

void MonsterMeshTextBattle::sendStart(uint32_t rngSeed, const Gen1Party &myParty)
{
    if (!radio_) return;
    uint8_t buf[BATTLELINK_MAX_PKT];
    BattlePacket *pkt = (BattlePacket *)buf;
    memset(buf, 0, sizeof(buf));
    pkt->type = (uint8_t)PktType::TEXT_BATTLE_START;
    pkt->setSessionId(session_);
    pkt->seq  = 0;
    pkt->payload[0] = (rngSeed >> 24) & 0xFF;
    pkt->payload[1] = (rngSeed >> 16) & 0xFF;
    pkt->payload[2] = (rngSeed >> 8)  & 0xFF;
    pkt->payload[3] =  rngSeed        & 0xFF;
    pkt->payload[4] = 1;                  // gen
    pkt->payload[5] = myParty.count;
    // Party hash placeholder — real impl computes a quick fingerprint.
    for (int i = 0; i < 8; ++i) pkt->payload[6 + i] = myParty.species[i % 7];
    radio_->sendPacket(remoteId_, MM_CHANNEL, buf, BATTLELINK_HDR_SIZE + 14);
}

void MonsterMeshTextBattle::sendParty()
{
    if (!radio_) return;
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(&myParty_);
    size_t total = sizeof(Gen1Party);
    uint8_t nChunks = (uint8_t)((total + PARTY_CHUNK_DATA - 1) / PARTY_CHUNK_DATA);

    for (uint8_t i = 0; i < nChunks; ++i) {
        uint8_t buf[BATTLELINK_MAX_PKT];
        BattlePacket *pkt = (BattlePacket *)buf;
        memset(buf, 0, sizeof(buf));
        pkt->type = (uint8_t)PktType::TEXT_BATTLE_PARTY;
        pkt->setSessionId(session_);
        pkt->seq = i;
        pkt->payload[0] = i;        // partIdx
        pkt->payload[1] = nChunks;  // partTotal

        size_t offset = (size_t)i * PARTY_CHUNK_DATA;
        size_t chunkLen = (offset + PARTY_CHUNK_DATA <= total)
                              ? PARTY_CHUNK_DATA : (total - offset);
        memcpy(pkt->payload + 2, raw + offset, chunkLen);
        radio_->sendPacket(remoteId_, MM_CHANNEL, buf,
                           BATTLELINK_HDR_SIZE + 2 + (uint8_t)chunkLen);
    }
}

void MonsterMeshTextBattle::onPartyComplete()
{
    // Both engines must have identical side assignments:
    // side 0 = initiator's party, side 1 = receiver's party.
    if (mySide_ == 0)
        engine_.start(myParty_, remoteParty_, pendingRngSeed_);
    else
        engine_.start(remoteParty_, myParty_, pendingRngSeed_);

    appendLog("Opponent party received!");
    handleFaints();
    if (engine_.result() == Gen1BattleEngine::Result::ONGOING)
        phase_ = Phase::WAIT_ACTION;
    else
        phase_ = Phase::FINISHED;
    dirty_ = true;
}

void MonsterMeshTextBattle::sendAction(uint8_t actionType, uint8_t index)
{
    if (!radio_) return;
    uint8_t buf[BATTLELINK_MAX_PKT];
    BattlePacket *pkt = (BattlePacket *)buf;
    memset(buf, 0, sizeof(buf));
    pkt->type = (uint8_t)PktType::TEXT_BATTLE_ACTION;
    pkt->setSessionId(session_);
    pkt->seq  = engine_.turn() & 0xFF;
    uint16_t turn = engine_.turn();
    pkt->payload[0] = (turn >> 8) & 0xFF;
    pkt->payload[1] =  turn       & 0xFF;
    pkt->payload[2] = actionType;
    pkt->payload[3] = index;
    radio_->sendPacket(remoteId_, MM_CHANNEL, buf, BATTLELINK_HDR_SIZE + 4);
    lastSentAction_ = actionType;
    lastSentIndex_  = index;
    lastSentTurn_   = turn;
    lastSendMs_     = mm_millis();
}

void MonsterMeshTextBattle::sendForfeit()
{
    if (!radio_) return;
    uint8_t buf[BATTLELINK_HDR_SIZE];
    BattlePacket *pkt = (BattlePacket *)buf;
    pkt->type = (uint8_t)PktType::TEXT_BATTLE_FORFEIT;
    pkt->setSessionId(session_);
    pkt->seq = 0;
    radio_->sendPacket(remoteId_, MM_CHANNEL, buf, sizeof(buf));
}

void MonsterMeshTextBattle::sendHash()
{
    if (!radio_) return;
    uint8_t buf[BATTLELINK_MAX_PKT];
    BattlePacket *pkt = (BattlePacket *)buf;
    memset(buf, 0, sizeof(buf));
    pkt->type = (uint8_t)PktType::TEXT_BATTLE_HASH;
    pkt->setSessionId(session_);
    pkt->seq  = engine_.turn() & 0xFF;
    uint16_t turn = engine_.turn();
    pkt->payload[0] = (turn >> 8) & 0xFF;
    pkt->payload[1] =  turn       & 0xFF;
    engine_.hashState(pkt->payload + 2);
    radio_->sendPacket(remoteId_, MM_CHANNEL, buf, BATTLELINK_HDR_SIZE + 10);
}

bool MonsterMeshTextBattle::handlePacket(uint32_t fromId,
                                          const uint8_t *buf, size_t len)
{
    if (len < BATTLELINK_HDR_SIZE) return false;
    const BattlePacket *pkt = (const BattlePacket *)buf;
    PktType t = (PktType)pkt->type;

    if (t == PktType::TEXT_BATTLE_START) {
        if (mode_ != Mode::OFF) return true;  // already in a battle, ignore
        if (len < BATTLELINK_HDR_SIZE + 14) return true;
        uint32_t rngSeed = ((uint32_t)pkt->payload[0] << 24)
                         | ((uint32_t)pkt->payload[1] << 16)
                         | ((uint32_t)pkt->payload[2] <<  8)
                         |  (uint32_t)pkt->payload[3];
        // Auto-accept if we have a party supplier callback.
        if (party_cb_) {
            Gen1Party myParty;
            if (party_cb_(&myParty, party_ctx_)) {
                session_ = pkt->sessionId();
                startNetworkedAsReceiver(fromId, myParty, rngSeed);
                return true;
            }
        }
        // Fallback: record session so the terminal can call
        // startNetworkedAsReceiver() manually.
        session_  = pkt->sessionId();
        remoteId_ = fromId;
        return true;
    }
    if (mode_ == Mode::OFF) return false;
    if (pkt->sessionId() != session_) return false;

    switch (t) {
        case PktType::TEXT_BATTLE_PARTY: {
            if (phase_ != Phase::WAIT_PARTY) return true;
            if (len < BATTLELINK_HDR_SIZE + 2) return true;
            uint8_t idx   = pkt->payload[0];
            uint8_t total = pkt->payload[1];
            if (total == 0 || total > 8 || idx >= total) return true;
            partyChunksTotal_ = total;
            size_t dataLen = len - BATTLELINK_HDR_SIZE - 2;
            size_t offset  = (size_t)idx * PARTY_CHUNK_DATA;
            if (offset + dataLen > sizeof(Gen1Party)) return true;
            memcpy(reinterpret_cast<uint8_t *>(&remoteParty_) + offset,
                   pkt->payload + 2, dataLen);
            partyChunksRecv_ |= (1u << idx);
            lastRecvMs_ = mm_millis();
            // All chunks arrived?
            uint8_t mask = (uint8_t)((1u << total) - 1);
            if ((partyChunksRecv_ & mask) == mask)
                onPartyComplete();
            return true;
        }
        case PktType::TEXT_BATTLE_ACTION: {
            if (len < BATTLELINK_HDR_SIZE + 4) return true;
            uint16_t turn = ((uint16_t)pkt->payload[0] << 8) | pkt->payload[1];
            if (turn != engine_.turn()) return true;  // stale or future packet
            uint8_t act = pkt->payload[2];
            uint8_t idx = pkt->payload[3];
            engine_.submitAction(1 - mySide_, act, idx);
            pendingRemoteAction_ = true;
            lastRecvMs_ = mm_millis();
            return true;
        }
        case PktType::TEXT_BATTLE_FORFEIT:
            engine_.forfeit(1 - mySide_, engineLogCb, this);
            phase_ = Phase::FINISHED;
            return true;
        case PktType::TEXT_BATTLE_HASH: {
            if (len < BATTLELINK_HDR_SIZE + 10) return true;
            uint8_t mine[8]; engine_.hashState(mine);
            if (memcmp(mine, pkt->payload + 2, 8) != 0) {
                appendLog("Desync detected - match aborted.");
                engine_.forfeit(mySide_, engineLogCb, this);
                phase_ = Phase::FINISHED;
            }
            return true;
        }
        default: break;
    }
    return false;
}

// ── Turn resolution ─────────────────────────────────────────────────────────

void MonsterMeshTextBattle::resolveTurn()
{
    engine_.executeTurn(engineLogCb, this);
    handleFaints();

    // Damage emission: the engine applies HP changes silently (upstream
    // rendered HP bars on screen). Without those, every neutral non-crit hit
    // looks like nothing happened — even though the CPU did move and damage
    // was dealt. Print a per-turn HP line so the user can see turn outcomes.
    {
        const auto &pMe  = engine_.party(mySide_);
        const auto &pFoe = engine_.party(1 - mySide_);
        const auto &mMe  = pMe.mons[pMe.active];
        const auto &mFoe = pFoe.mons[pFoe.active];
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "HP: %.7s %u/%u  vs  %.7s %u/%u",
                 mMe.nickname[0] ? mMe.nickname : "you",
                 (unsigned)mMe.hp, (unsigned)mMe.maxHp,
                 mFoe.nickname[0] ? mFoe.nickname : "foe",
                 (unsigned)mFoe.hp, (unsigned)mFoe.maxHp);
        appendLog(buf);
    }

    if (engine_.result() != Gen1BattleEngine::Result::ONGOING) {
        // P1_WIN means side 0 won. If we're side 0, that's us.
        bool iWin = (engine_.result() == Gen1BattleEngine::Result::P1_WIN && mySide_ == 0)
                  || (engine_.result() == Gen1BattleEngine::Result::P2_WIN && mySide_ == 1);
        bool iLose = (engine_.result() == Gen1BattleEngine::Result::P1_WIN && mySide_ == 1)
                   || (engine_.result() == Gen1BattleEngine::Result::P2_WIN && mySide_ == 0);
        if (iWin)       appendLog("You won!");
        else if (iLose) appendLog("You blacked out...");
        else            appendLog("It's a draw.");
        phase_ = Phase::FINISHED;
        return;
    }
    pendingRemoteAction_ = false;
    if (mode_ == Mode::NETWORKED &&
        engine_.turn() % TEXT_BATTLE_HASH_INTERVAL == 0) {
        sendHash();
    }
    phase_ = Phase::WAIT_ACTION;
    cursor_ = 0;
}

void MonsterMeshTextBattle::handleFaints()
{
    for (int s = 0; s < 2; ++s) {
        if (engine_.party(s).mons[engine_.party(s).active].hp == 0) {
            engine_.autoReplaceIfFainted(s, engineLogCb, this);
        }
    }
}

// ── Tick — drive remote-action resolution + scroll throttle ─────────────────

void MonsterMeshTextBattle::tick(uint32_t nowMs)
{
    if (mode_ == Mode::OFF) return;

    // Reveal queued log lines slowly so the player can read.
    if (scrollPending_ && (nowMs - scrollMs_ >= SCROLL_INTERVAL_MS)) {
        scrollMs_ = nowMs;
        scrollPending_--;
        dirty_ = true;
    }

    if (mode_ == Mode::NETWORKED) {
        // Re-send party chunks during exchange.
        if (phase_ == Phase::WAIT_PARTY &&
            (nowMs - lastSendMs_ >= RESEND_INTERVAL_MS)) {
            sendParty();
            lastSendMs_ = nowMs;
        }
        // Re-broadcast our pending action periodically — LoRa is lossy.
        if (phase_ == Phase::WAIT_REMOTE && lastSentAction_ != 0xFF &&
            (nowMs - lastSendMs_ >= RESEND_INTERVAL_MS)) {
            sendAction(lastSentAction_, lastSentIndex_);
        }
        // Timeout: opponent hasn't sent anything in a while.
        if ((nowMs - lastRecvMs_) > REMOTE_TIMEOUT_MS &&
            phase_ != Phase::FINISHED) {
            appendLog("Opponent timed out.");
            if (phase_ == Phase::WAIT_PARTY) {
                // Engine not started yet, just abort.
                phase_ = Phase::FINISHED;
            } else {
                engine_.forfeit(1 - mySide_, engineLogCb, this);
                phase_ = Phase::FINISHED;
            }
        }
        // If both sides have submitted, resolve.
        if (phase_ == Phase::WAIT_REMOTE && pendingRemoteAction_) {
            resolveTurn();
        }
    } else if (mode_ == Mode::LOCAL_ROGUELIKE) {
        // CPU acts immediately after we submit.
        if (phase_ == Phase::WAIT_REMOTE) {
            uint8_t a, i;
            engine_.cpuPickAction(1, a, i);
            engine_.submitAction(1, a, i);
            resolveTurn();
        }
    }
}

// ── Input ───────────────────────────────────────────────────────────────────

void MonsterMeshTextBattle::handleKey(uint8_t c)
{
    if (mode_ == Mode::OFF) return;
    dirty_ = true;

    if (phase_ == Phase::FINISHED) {
        // Any key dismisses the result screen.
        exit();
        return;
    }

    if (c == 'f' || c == 'F') {
        if (mode_ == Mode::NETWORKED) {
            sendForfeit(); engine_.forfeit(mySide_, engineLogCb, this); phase_ = Phase::FINISHED;
        } else {
            ++fleeAttempts_;
            const auto &player = engine_.party(mySide_).mons[engine_.party(mySide_).active];
            const auto &enemy  = engine_.party(1 - mySide_).mons[engine_.party(1 - mySide_).active];
            uint32_t f = (uint32_t)player.spd * 32 / (enemy.spd + 1) + 30u * fleeAttempts_;
            bool escaped = (f >= 255) || ((uint8_t)(mm_millis() & 0xFF) < (uint8_t)f);
            if (escaped) { appendLog("Got away safely!"); phase_ = Phase::FINISHED; }
            else { appendLog("Can't escape!"); engine_.submitAction(mySide_, 2 /*FLEE_FAIL*/, 0); phase_ = Phase::WAIT_REMOTE; }
        }
        return;
    }
    if (c == 27 /* ESC */) {
        if (mode_ == Mode::NETWORKED) sendForfeit();
        engine_.forfeit(mySide_, engineLogCb, this);
        phase_ = Phase::FINISHED;
        return;
    }

    if (phase_ == Phase::WAIT_SWITCH) {
        const auto &p = engine_.party(mySide_);
        if (c == 'w' || c == 'W' || c == 0xB5 /* up */)
            switchCursor_ = (switchCursor_ + p.count - 1) % p.count;
        else if (c == 's' || c == 'S' || c == 0xB6 /* down */)
            switchCursor_ = (switchCursor_ + 1) % p.count;
        else if (c == '\n' || c == '\r' || c == ' ') {
            if (p.mons[switchCursor_].hp > 0 && switchCursor_ != p.active) {
                engine_.submitAction(mySide_, 1 /*SWITCH*/, switchCursor_);
                if (mode_ == Mode::NETWORKED) sendAction(1, switchCursor_);
                phase_ = Phase::WAIT_REMOTE;
            }
        } else if (c == 27) {
            phase_ = Phase::WAIT_ACTION;
        }
        return;
    }

    if (phase_ != Phase::WAIT_ACTION) return;

    if (c >= '1' && c <= '4') {
        uint8_t slot = c - '1';
        const auto &mon = engine_.party(mySide_).mons[engine_.party(mySide_).active];
        if (mon.moves[slot] == 0) { appendLog("No move there!");        return; }
        if (mon.pp[slot] == 0)    { appendLog("Out of PP for that move!"); return; }
        engine_.submitAction(mySide_, 0 /*USE_MOVE*/, slot);
        if (mode_ == Mode::NETWORKED) sendAction(0, slot);
        phase_ = Phase::WAIT_REMOTE;
    } else if (c == 's' || c == 'S') {
        switchCursor_ = engine_.party(mySide_).active;
        phase_ = Phase::WAIT_SWITCH;
    }
}
