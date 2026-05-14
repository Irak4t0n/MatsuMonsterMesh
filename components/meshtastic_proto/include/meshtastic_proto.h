// SPDX-License-Identifier: GPL-3.0-or-later
//
// meshtastic_proto — Meshtastic on-air protocol layer.
//
// Session 2a of the P4 Meshtastic port (Path #3). For now: parses the
// 16-byte plaintext header that prefixes every Meshtastic LoRa packet.
// Encryption (AES-256-CTR), channel hashing against the PSK, and
// protobuf decoding of the encrypted payload land in subsequent
// sessions.
//
// The on-air format we decode matches upstream Meshtastic's
// `PacketHeader` struct in firmware/src/mesh/RadioInterface.h:
//
//   typedef struct {
//       NodeNum to, from;   // 4 + 4 bytes, little-endian
//       PacketId id;        // 4 bytes, little-endian
//       uint8_t flags;      // 1 byte (sub-fields below)
//       uint8_t channel;    // 1 byte — 8-bit hash of channel name + PSK
//       uint8_t next_hop;   // 1 byte — truncated NodeNum of next-hop target
//       uint8_t relay_node; // 1 byte — truncated NodeNum of relay sender
//   } PacketHeader;         // 16 bytes total
//
// Flags byte layout (LSB → MSB):
//   bits 0-2: hop_limit  (0-7, decrements each relay)
//   bit  3:   want_ack
//   bit  4:   via_mqtt
//   bits 5-7: hop_start  (initial hop_limit set by originator)

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MESHTASTIC_HEADER_LEN     16
#define MESHTASTIC_BROADCAST_ADDR 0xFFFFFFFFu

// Canonical channel hash for the default Meshtastic LongFast channel
// (name "LongFast" XORed with the well-known default PSK `AQ==`).
// A received packet with this hash uses the default channel; our T-Deck
// Plus uses this one out of the box.
#define MESHTASTIC_LONGFAST_CHANNEL_HASH 0x08

// Common Data.portnum values (subset of upstream meshtastic.PortNum proto
// enum — see firmware/protobufs/portnums.proto). Surfaced so the terminal
// can label decrypted packets without dragging in the full nanopb stack
// (that happens in Session 2c).
#define MESHTASTIC_PORTNUM_UNKNOWN_APP        0
#define MESHTASTIC_PORTNUM_TEXT_MESSAGE_APP   1
#define MESHTASTIC_PORTNUM_POSITION_APP       3
#define MESHTASTIC_PORTNUM_NODEINFO_APP       4
#define MESHTASTIC_PORTNUM_ROUTING_APP        5
#define MESHTASTIC_PORTNUM_ADMIN_APP          6
#define MESHTASTIC_PORTNUM_TELEMETRY_APP     67
#define MESHTASTIC_PORTNUM_TRACEROUTE_APP    70

typedef struct {
    uint32_t to;
    uint32_t from;
    uint32_t id;
    uint8_t  flags;
    uint8_t  channel;
    uint8_t  next_hop;
    uint8_t  relay_node;
} meshtastic_header_t;

// Flags-byte accessors. Centralised so we don't sprinkle bit-shifts around
// the call sites and so the layout matches upstream's documented format
// exactly.
static inline uint8_t meshtastic_hdr_hop_limit(uint8_t flags) {
    return (uint8_t)(flags & 0x07);
}
static inline bool meshtastic_hdr_want_ack(uint8_t flags) {
    return (flags & 0x08) != 0;
}
static inline bool meshtastic_hdr_via_mqtt(uint8_t flags) {
    return (flags & 0x10) != 0;
}
static inline uint8_t meshtastic_hdr_hop_start(uint8_t flags) {
    return (uint8_t)((flags >> 5) & 0x07);
}

// Parse the first 16 bytes of `buf` into `out`. `buf_len` must be at
// least 16. Returns ESP_OK on success or ESP_ERR_INVALID_SIZE / _ARG.
// `payload` / `payload_len` are NOT stored in `out` — the caller already
// has the buffer; this struct holds just the parsed header fields.
esp_err_t meshtastic_parse_header(const uint8_t *buf, size_t buf_len,
                                  meshtastic_header_t *out);

// ── AES-128-CTR over the encrypted payload ─────────────────────────────
//
// Decrypts (or, identically for CTR-mode, encrypts) `len` bytes from
// `in` into `out` using:
//   * the well-known LongFast default 16-byte key (`defaultpsk` array in
//     upstream Meshtastic firmware src/mesh/Channels.h), and
//   * a 16-byte CTR nonce derived from this packet's `from` and `id`
//     fields, matching CryptoEngine::initNonce.
//
// `in` and `out` may alias. Returns ESP_OK on success.
//
// Caveat: only the default LongFast channel is supported here. Custom
// channels would need their own 32-byte key — Session 2c will surface
// that as a configurable parameter once we have a Channel concept.
esp_err_t meshtastic_decrypt_longfast(uint32_t from_node, uint32_t packet_id,
                                      const uint8_t *in, size_t len,
                                      uint8_t *out);

// Heuristic: looks at the first 2 bytes of a decrypted Meshtastic Data
// payload to guess whether decryption succeeded. A real Data message is
// protobuf-encoded and begins with the tag for field 1 (portnum), which
// is the byte 0x08 (field=1, wiretype=0 / varint). The byte after that
// is the portnum value itself (single byte for values 0-127). Returns
// the portnum on a likely-valid match, or -1 if the bytes don't look
// like a Data message start. Useful as a fast smoke test for the key
// before we wire in the full protobuf decoder.
int meshtastic_guess_portnum(const uint8_t *plain, size_t len);

// ── Minimal protobuf decode of the Data submessage ───────────────────
//
// After AES-CTR decrypt the payload is a `meshtastic.Data` protobuf
// (defined in upstream `protobufs/meshtastic/mesh.proto`):
//
//   message Data {
//       PortNum portnum = 1;         // varint
//       bytes   payload = 2;         // length-delimited
//       bool    want_response = 3;   // varint
//       fixed32 dest = 4;            // 32-bit fixed
//       fixed32 source = 5;
//       fixed32 request_id = 6;
//       fixed32 reply_id = 7;
//       fixed32 emoji = 8;
//       uint32  bitfield = 9;        // varint
//   }
//
// We only care about portnum + payload here; the rest get skipped with
// a generic skip-by-wiretype helper. Caller passes the decrypted plain
// buffer; `out_payload` is a pointer into that buffer (no copy).
typedef struct {
    int            portnum;       // -1 if no field 1 present
    const uint8_t *payload;       // NULL if no field 2 present
    size_t         payload_len;
} meshtastic_data_t;

bool meshtastic_decode_data(const uint8_t *plain, size_t len,
                            meshtastic_data_t *out);

// Decode the inner `User` protobuf carried in a NodeInfo payload:
//   message User {
//       string id = 1;             // e.g., "!f1bce6ec"
//       string long_name = 2;      // e.g., "Howard's T-Deck"
//       string short_name = 3;     // e.g., "HOWA"
//       bytes  macaddr = 4;
//       HardwareModel hw_model = 5;
//       bool   is_licensed = 6;
//       Role   role = 7;
//       bytes  public_key = 8;
//   }
//
// All `out_*` buffers receive a null-terminated string truncated to the
// supplied capacity. Pass NULL to skip a field.
bool meshtastic_decode_user(const uint8_t *buf, size_t len,
                            char *out_id,         size_t id_cap,
                            char *out_long_name,  size_t long_cap,
                            char *out_short_name, size_t short_cap);

// ── TX side ───────────────────────────────────────────────────────────
//
// Our node identity. Derived once from the P4's MAC address using the
// upstream Meshtastic convention (last 4 bytes of WiFi MAC interpreted
// as big-endian uint32_t). Stable across reboots; unique per device.
uint32_t meshtastic_proto_node_id(void);

// Send a broadcast text message on the default LongFast channel. The
// text must be NUL-terminated UTF-8; bytes after the first 200 are
// dropped to stay under SX1262's max payload. Returns ESP_OK if the
// packet was queued on the radio, or the underlying error otherwise.
// The receiving Meshtastic device will display this as a regular text
// message from our node — though without a prior NodeInfo announce the
// sender will appear as "Unknown" rather than by name.
esp_err_t meshtastic_send_text(const char *text);

// Send a broadcast NodeInfo announce on the default LongFast channel.
// `long_name` and `short_name` are NUL-terminated UTF-8 strings; pass
// NULL to use compile-time defaults ("HowBoyMatsu" / "HBM!"). The
// receiving Meshtastic device will add us to its node list with these
// names. Should be called once shortly after boot and periodically
// thereafter — Session 3 will turn this into an automatic timer.
esp_err_t meshtastic_send_nodeinfo(const char *long_name,
                                   const char *short_name);

// ── NodeDB (Session 3b) ────────────────────────────────────────────────
//
// A tiny in-memory directory of nodes we've heard. Each NodeInfo we
// receive (or send to ourselves) upserts an entry. The terminal uses
// these to render `HOWA: hi` instead of `!f1bce6ec: hi`.
//
// Capacity is small but more than enough for a typical home/neighbour
// mesh. When full, the least-recently-seen entry gets overwritten.

#define MESHTASTIC_NODEDB_CAP        32
#define MESHTASTIC_NODE_NAME_LONG    24
#define MESHTASTIC_NODE_NAME_SHORT   8

typedef struct {
    uint32_t node_num;
    uint32_t last_seen_ms;       // P4-clock ms of most recent packet
    char     long_name[MESHTASTIC_NODE_NAME_LONG];
    char     short_name[MESHTASTIC_NODE_NAME_SHORT];
    bool     valid;
} meshtastic_node_entry_t;

// Insert-or-update an entry. Either name may be NULL/empty — only the
// non-empty fields overwrite. Updates last_seen_ms on every call.
// Called automatically by the drainer whenever a NodeInfo is parsed;
// also called for our own node from meshtastic_proto_begin().
void meshtastic_nodedb_upsert(uint32_t node_num,
                              const char *long_name,
                              const char *short_name);

// Look up an entry by NodeNum. Returns true if found; copies name fields
// into the caller's buffers (truncated, NUL-terminated). Either out_*
// may be NULL.
bool meshtastic_nodedb_lookup(uint32_t node_num,
                              char *out_long,  size_t long_cap,
                              char *out_short, size_t short_cap);

// Format `node_num` for terminal display: short_name if known, else
// "!XXXXXXXX". `out` must be at least 10 bytes for the fallback. Always
// NUL-terminates.
void meshtastic_format_node(uint32_t node_num, char *out, size_t cap);

// Copy up to `max_out` known nodes into `out` (any order; you usually
// want to sort by last_seen_ms client-side). Returns count copied.
size_t meshtastic_nodedb_snapshot(meshtastic_node_entry_t *out, size_t max_out);

// ── Chat history (Session 5) ──────────────────────────────────────────
//
// Tiny ring buffer of recent TextMessages, both inbound and our own
// outbound. The drain task pushes inbound TextMessage(1) packets; the
// `meshtastic_send_text` path pushes outbound. The chat UI snapshots
// this buffer each render so the user sees a unified conversation log.

#define MESHTASTIC_CHAT_DEPTH 32
#define MESHTASTIC_CHAT_TEXT_MAX 120

typedef struct {
    uint32_t when_ms;
    uint32_t from_node;          // sender's NodeNum (us, if is_self)
    bool     is_self;            // true = we sent this
    uint8_t  text_len;
    char     text[MESHTASTIC_CHAT_TEXT_MAX];
} meshtastic_chat_entry_t;

// Copy up to max_out chat entries into `out`, newest first. Returns
// the number actually copied. Non-blocking.
size_t meshtastic_chat_snapshot(meshtastic_chat_entry_t *out, size_t max_out);

// Total messages pushed since boot (sent + received). Doesn't reset.
uint32_t meshtastic_chat_total(void);

// ── Ring buffer of recently-seen parsed packets ────────────────────────
//
// Background task pulls raw bytes off meshtastic_lora's queue, parses
// the header, and stores the header + a short payload sample in a small
// FIFO so the terminal's `mesh_recent` command can display it. Capacity
// is fixed at MESHTASTIC_RECENT_DEPTH; older entries get overwritten.

#define MESHTASTIC_RECENT_DEPTH         16
#define MESHTASTIC_RECENT_PAYLOAD_SAMPLE 16  // first N payload bytes kept

typedef struct {
    uint32_t            rx_ms;          // P4-clock ms when received
    uint8_t             raw_len;        // total packet length on the air
    meshtastic_header_t header;
    uint8_t             payload_sample_len;
    uint8_t             payload_sample[MESHTASTIC_RECENT_PAYLOAD_SAMPLE];
    // Session 2b additions — decryption attempt against the default
    // LongFast key. `decrypted` is set if AES-CTR ran (it always does in
    // the drain task), and `portnum_guess` is the byte-1 portnum value
    // *if* the first byte of plaintext is the expected protobuf tag
    // (0x08). A non-negative portnum_guess is strong evidence that the
    // key + nonce are right.
    bool                decrypted;
    int16_t             portnum_guess;  // -1 if not a likely Data message
    uint8_t             plain_sample_len;
    uint8_t             plain_sample[MESHTASTIC_RECENT_PAYLOAD_SAMPLE];
    // Session 2c: protobuf-decoded body for known portnums. Format:
    //   TextMessage(1) → the message text, truncated to fit
    //   NodeInfo(4)    → `long_name (SHORT)` extracted from User
    // Other portnums leave body[0] = '\0'.
    char                body[48];
} meshtastic_recent_entry_t;

// Start the background RX-drainer task. Safe to call once. Returns
// ESP_OK on success; logs failures with tag "mp".
esp_err_t meshtastic_proto_begin(void);

// Copy up to `max_out` of the most-recent parsed packets (newest first)
// into `out`. Returns the number actually copied. Non-blocking.
size_t meshtastic_proto_recent(meshtastic_recent_entry_t *out, size_t max_out);

// Total number of packets the drainer has parsed since begin(). Useful
// for the terminal's `lora_stats` summary.
uint32_t meshtastic_proto_total_parsed(void);

#ifdef __cplusplus
}
#endif
