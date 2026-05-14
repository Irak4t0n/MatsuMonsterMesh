// SPDX-License-Identifier: GPL-3.0-or-later
//
// meshtastic_lora — thin wrapper around `nicolaielectronics/tanmatsu-lora`
// that handles bring-up, raw packet I/O, and per-packet counters.
//
// Session 1 of the P4-side Meshtastic port (Path #3) — at this stage we are
// only proving that the LoRa link works at the wire level (raw bytes go
// out, raw bytes come in). The Meshtastic wire format, encryption, and
// routing layers get built on top in later sessions.
//
// All radio I/O goes through the ESP32-C6 coprocessor over SDIO-RPC; we
// never touch GPIOs / SPI / IRQ pins directly. The C6's `tanmatsu-radio`
// firmware exposes the SX1262 via the tanmatsu-lora API.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Preset modem params for Meshtastic's LongFast on US 915. Region + slot
// numbers come from upstream meshtastic/firmware src/mesh/RadioInterface.cpp:
//   freq = freqStart + (bw / 2000) + slot * (bw / 1000)
//        = 902.0 MHz + 0.125 MHz + slot * 0.250 MHz
//
// Slot 19 = 906.875 MHz — what the user's T-Deck Plus actually uses.
// Slot 20 = 907.125 MHz — the upstream-documented LongFast default we
//   originally tuned to. Upstream picks the slot from a hash of the
//   channel name, so different devices on the same logical "LongFast"
//   channel can end up on different physical slots depending on which
//   channel name they have configured. The hash for the empty
//   channel-name string maps to slot 19 (a/k/a what most T-Deck Plus
//   units ship with), so that's the right default for our use case.
//
// If you ever need to talk to a device on a different slot, change this
// constant and rebuild — and revisit the slot-from-channel-name hash
// once Session 2c lands the proper Channel concept.
#define ML_LONGFAST_US_FREQ_HZ    906875000u   // slot 19
#define ML_LONGFAST_SF            11
#define ML_LONGFAST_BW_KHZ        250
#define ML_LONGFAST_CR            5     // 4/5
#define ML_LONGFAST_PREAMBLE      16
#define ML_LONGFAST_SYNC_WORD     0x2B  // Meshtastic "private" sync byte
// 22 dBm is the US-915 region max but some SX1262 module + C6 firmware
// combos reject it via sx126x_set_tx_params; the launcher's own default is
// 14 dBm. Drop to 14 as a known-good first so we can prove the link works
// at all, then revisit raising it once everything else is verified.
#define ML_LONGFAST_TX_POWER_DBM  14

// Max raw-packet length the tanmatsu-lora driver supports (lora_protocol
// lora_packet_t.data is uint8_t[256]).
#define ML_RAW_MAX_LEN            256

// Minimum raw-packet length the SX1262 framing at coding-rate 4/5 will
// accept. The C6 firmware NACKs a PACKET_TX shorter than this with the
// log line "Packet length N is too short for coding rate 8/5, minimum is
// 8 bytes". Real Meshtastic packets are well above this (the 16-byte
// plaintext header alone clears it), so this is a smoke-test guard.
#define ML_RAW_MIN_LEN            8

// Bring the radio up. Initialises tanmatsu-lora, pushes our LongFast US
// modem config, and parks the chip in continuous-RX. Returns ESP_OK on
// success; logs failures with tag "ml". Safe to call repeatedly — second
// and subsequent calls become reconfigure-and-resume-RX no-ops.
esp_err_t meshtastic_lora_begin(void);

// Returns true once begin() has succeeded at least once.
bool meshtastic_lora_is_up(void);

// Send a raw byte payload over the air. Caller's buffer must be ≤
// ML_RAW_MAX_LEN bytes. Blocks for up to ~2 s while the SDIO RPC ack
// returns. Returns ESP_OK on success.
//
// NB: in Session 1 this sends RAW bytes, not Meshtastic-framed packets —
// other Meshtastic devices on the same channel will receive the bytes
// but won't decode them as a Meshtastic packet. Useful for verifying
// the radio link only.
esp_err_t meshtastic_lora_send_raw(const uint8_t *data, size_t len);

// Pop one raw-packet from the receive queue if available. Non-blocking.
// On success, *out_len is set to the packet length and up to max_len
// bytes are copied into out_buf. Returns true if a packet was popped,
// false if the queue is empty.
bool meshtastic_lora_pop_raw(uint8_t *out_buf, size_t max_len,
                             size_t *out_len);

// Cumulative counters since begin() — handy for the terminal's `lora_stats`
// command to verify the radio is actually doing anything.
typedef struct {
    uint32_t init_ok;          // 1 once lora_init() succeeded
    uint32_t configs_applied;  // # of successful lora_set_config calls
    uint32_t tx_attempted;     // # of sendPacket calls
    uint32_t tx_ok;            // # that returned ESP_OK
    uint32_t tx_err;           // # that failed (non-ESP_OK return)
    uint32_t rx_packets;       // # received from the C6's queue
    uint32_t rx_bytes_total;   // sum of received-packet lengths
    // Diagnostic fields — added so we can troubleshoot bring-up without
    // P4 serial visibility. Each `last_*_err` holds the most recent
    // esp_err_t return code (0 = success); the `*_ms` timestamps record
    // P4-clock milliseconds at the moment each call returned. Cross-ref
    // against the C6's `I (xxx)` clock manually (the C6 logs use the C6's
    // own boot clock which is close to but not identical to the P4's).
    int32_t  last_init_err;
    int32_t  last_config_err;
    int32_t  last_mode_err;
    uint32_t last_init_ms;
    uint32_t last_config_ms;
    uint32_t last_mode_ms;
    // TX path diagnostics. last_tx_err is the most recent return code from
    // `lora_send_packet` itself; last_tx_pre_mode_err / last_tx_post_mode_err
    // are the `lora_set_mode` calls we make before/after the send to put
    // the chip in STANDBY for TX and back into RX afterwards. If
    // last_tx_pre_mode_err is non-zero, we never even attempted the send.
    int32_t  last_tx_err;
    int32_t  last_tx_pre_mode_err;
    int32_t  last_tx_post_mode_err;
    uint32_t last_tx_ms;
    uint16_t last_tx_len;          // raw on-air byte count of last TX attempt
} meshtastic_lora_stats_t;

void meshtastic_lora_get_stats(meshtastic_lora_stats_t *out);

// Diagnostic probe — calls each tanmatsu-lora primitive (get_status,
// get_config, set_mode RX, set_mode STANDBY) individually so the caller
// can see which one returns ESP_FAIL / NACK. Each entry pairs an
// esp_err_t return with a short human label. probe_count tells the
// caller how many entries were populated (max MESHTASTIC_LORA_PROBE_MAX).
#define MESHTASTIC_LORA_PROBE_MAX 6
typedef struct {
    const char *label;
    int32_t     result;
} meshtastic_lora_probe_entry_t;

typedef struct {
    int                          probe_count;
    meshtastic_lora_probe_entry_t entries[MESHTASTIC_LORA_PROBE_MAX];
    // Snapshot of whatever lora_get_config returned (zero-init if it
    // didn't succeed). Helps verify the C6 thinks it's at the same
    // freq/SF/BW we asked for.
    uint32_t  cur_frequency;
    uint8_t   cur_sf;
    uint16_t  cur_bw;
    uint8_t   cur_cr;
} meshtastic_lora_probe_result_t;

void meshtastic_lora_probe(meshtastic_lora_probe_result_t *out);

#ifdef __cplusplus
}
#endif
