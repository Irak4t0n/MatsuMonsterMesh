// SPDX-License-Identifier: GPL-3.0-or-later

#include "meshtastic_proto.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mbedtls/aes.h"

#include "meshtastic_lora.h"

static const char *TAG = "mp";

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// Read a little-endian uint32 from buf+off without alignment assumptions.
// Meshtastic's wire format is documented as little-endian everywhere.
static inline uint32_t read_u32_le(const uint8_t *buf, size_t off) {
    return ((uint32_t)buf[off + 0])       |
           ((uint32_t)buf[off + 1] <<  8) |
           ((uint32_t)buf[off + 2] << 16) |
           ((uint32_t)buf[off + 3] << 24);
}

// ─────────────────────────────────────────────────────────────────────
// AES-128-CTR for the default LongFast channel.
//
// Upstream Meshtastic stores the default channel's PSK as a single byte
// 0x01 in the protobuf (the well-known "AQ==" base64). That index gets
// expanded by `Channels::initDefaultChannel` (src/mesh/Channels.cpp) into
// the 16-byte `defaultpsk` array from `src/mesh/Channels.h`:
//
//   memcpy(k.bytes, defaultpsk, sizeof(defaultpsk));   // 16 bytes
//   k.bytes[15] += pskIndex - 1;                       // unchanged for idx=1
//
// CRITICAL: Meshtastic uses AES-128, not AES-256. The default PSK is
// 16 bytes, and `CryptoEngine` calls mbedtls with key_len=128. An earlier
// version of this file mistakenly used 32 bytes / AES-256, which kept
// the first 16 bytes correct but appended 16 stale bytes — producing
// garbage plaintext on every decrypt. Verified against upstream commit
// at github.com/meshtastic/firmware/blob/master/src/mesh/Channels.h.
static const uint8_t MESHTASTIC_LONGFAST_KEY[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

// Build the 16-byte CTR nonce per CryptoEngine::initNonce in upstream.
// nonce[0..7]  = packet_id as 64-bit LE (upper 32 bits zero on the wire)
// nonce[8..11] = from_node as 32-bit LE
// nonce[12..15] = extraNonce (zero for the default-channel path)
static void build_nonce(uint8_t out[16], uint32_t from_node, uint32_t packet_id)
{
    memset(out, 0, 16);
    out[0] = (uint8_t)(packet_id      );
    out[1] = (uint8_t)(packet_id >>  8);
    out[2] = (uint8_t)(packet_id >> 16);
    out[3] = (uint8_t)(packet_id >> 24);
    // bytes 4-7 = upper 32 bits of 64-bit packet_id, always zero on-air
    out[8]  = (uint8_t)(from_node      );
    out[9]  = (uint8_t)(from_node >>  8);
    out[10] = (uint8_t)(from_node >> 16);
    out[11] = (uint8_t)(from_node >> 24);
    // bytes 12-15 = extraNonce, zero
}

esp_err_t meshtastic_decrypt_longfast(uint32_t from_node, uint32_t packet_id,
                                      const uint8_t *in, size_t len,
                                      uint8_t *out)
{
    if (!in || !out)  return ESP_ERR_INVALID_ARG;
    if (len == 0)     return ESP_OK;

    uint8_t nonce[16];
    build_nonce(nonce, from_node, packet_id);

    uint8_t stream_block[16] = {0};
    size_t  nc_off = 0;

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    // Meshtastic uses AES-128. The default PSK is 16 bytes, and upstream
    // calls mbedtls with key_len=128. See note above the LONGFAST_KEY
    // constant.
    int rc = mbedtls_aes_setkey_enc(&ctx, MESHTASTIC_LONGFAST_KEY, 128);
    if (rc != 0) {
        mbedtls_aes_free(&ctx);
        return ESP_FAIL;
    }
    rc = mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce, stream_block, in, out);
    mbedtls_aes_free(&ctx);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

int meshtastic_guess_portnum(const uint8_t *plain, size_t len)
{
    // A real Data message's first byte is the protobuf tag for field 1
    // (portnum), which is `(1<<3) | 0` = 0x08. Field 1 is varint-encoded
    // PortNum. PortNums in active use are all < 128, so they fit in a
    // single varint byte. So the on-the-wire pattern for a Data message
    // starting with portnum=N (N<128) is exactly `08 NN ...`.
    if (!plain || len < 2)        return -1;
    if (plain[0] != 0x08)         return -1;
    if (plain[1] & 0x80)          return -1;  // multi-byte varint: portnum >= 128 — unlikely
    return (int)plain[1];
}

// ── Minimal protobuf reader ──────────────────────────────────────────
//
// Just enough to walk `Data` and `User` messages. No support for groups
// (deprecated proto2 wiretype 3/4) — Meshtastic doesn't use them.

// Read a varint at *pos, advance *pos. Returns -1 on truncation /
// >10-byte varint (we cap at 64-bit values so any encoded form is at
// most 10 bytes).
static int64_t pb_read_varint(const uint8_t *buf, size_t len, size_t *pos)
{
    uint64_t result = 0;
    int      shift  = 0;
    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        result |= ((uint64_t)(b & 0x7F)) << shift;
        if ((b & 0x80) == 0) return (int64_t)result;
        shift += 7;
        if (shift >= 64) return -1;
    }
    return -1;
}

// Skip a field whose tag has already been consumed, given its wiretype.
// Returns false on truncation / unknown wiretype.
static bool pb_skip_field(const uint8_t *buf, size_t len, size_t *pos, int wt)
{
    switch (wt) {
        case 0: { // varint
            int64_t v = pb_read_varint(buf, len, pos);
            return v >= 0;
        }
        case 1:   // 64-bit fixed
            if (*pos + 8 > len) return false;
            *pos += 8;
            return true;
        case 2: { // length-delimited
            int64_t l = pb_read_varint(buf, len, pos);
            if (l < 0 || (size_t)l > (len - *pos)) return false;
            *pos += (size_t)l;
            return true;
        }
        case 5:   // 32-bit fixed
            if (*pos + 4 > len) return false;
            *pos += 4;
            return true;
        default:
            return false;
    }
}

bool meshtastic_decode_data(const uint8_t *plain, size_t len,
                            meshtastic_data_t *out)
{
    if (!plain || !out) return false;
    out->portnum     = -1;
    out->payload     = NULL;
    out->payload_len = 0;

    size_t pos = 0;
    while (pos < len) {
        int64_t tag = pb_read_varint(plain, len, &pos);
        if (tag < 0) return false;
        int field = (int)(tag >> 3);
        int wt    = (int)(tag & 0x7);

        if (field == 1 && wt == 0) {                  // portnum (varint)
            int64_t v = pb_read_varint(plain, len, &pos);
            if (v < 0) return false;
            out->portnum = (int)v;
        } else if (field == 2 && wt == 2) {           // payload (bytes)
            int64_t l = pb_read_varint(plain, len, &pos);
            if (l < 0 || pos + (size_t)l > len) return false;
            out->payload     = plain + pos;
            out->payload_len = (size_t)l;
            pos += (size_t)l;
        } else {
            if (!pb_skip_field(plain, len, &pos, wt)) return false;
        }
    }
    return true;
}

// Read a string field — copy up to cap-1 bytes from buf+pos, NUL-term,
// advance pos by `field_len`. Truncation is silent. Returns false on
// truncation of the BUFFER (not the string).
static bool pb_copy_string(const uint8_t *buf, size_t len, size_t *pos,
                           size_t field_len, char *out, size_t cap)
{
    if (*pos + field_len > len) return false;
    if (out && cap > 0) {
        size_t copy = field_len < (cap - 1) ? field_len : (cap - 1);
        memcpy(out, buf + *pos, copy);
        out[copy] = '\0';
    }
    *pos += field_len;
    return true;
}

// ── Chat history (Session 5) ──────────────────────────────────────────

static meshtastic_chat_entry_t s_chat[MESHTASTIC_CHAT_DEPTH];
static size_t                  s_chat_head  = 0;     // next write
static size_t                  s_chat_count = 0;
static uint32_t                s_chat_total = 0;
static SemaphoreHandle_t       s_chat_mtx   = NULL;

// Session 3c: notify-on-new-message hook for the in-emulator overlay.
// Defined later via meshtastic_proto_set_chat_notify_cb(); fired from
// chat_push for deduped, non-self messages only. Declared up here so
// chat_push (below) can reference it before the setter implementation
// further down the file.
static meshtastic_chat_notify_cb_t s_chat_notify_cb = NULL;

static void chat_init(void)
{
    if (s_chat_mtx == NULL) {
        s_chat_mtx = xSemaphoreCreateMutex();
        memset(s_chat, 0, sizeof(s_chat));
    }
}

// Recent-packet-id dedup ring. Meshtastic devices relay broadcasts, so
// the same logical message arrives 2+ times — once direct, once per
// relay — with the same `id` but different `relay_node`. Without this
// filter, the chat UI prints every message twice.
//
// 32 entries gives enough headroom that fast back-and-forth text doesn't
// evict a packet id before its relay copy lands. Linear scan is fine at
// this size.
#define CHAT_DEDUP_DEPTH 32
static uint32_t s_chat_dedup[CHAT_DEDUP_DEPTH];
static int      s_chat_dedup_pos = 0;

// Must be called with s_chat_mtx held. Returns true if `pkt_id` has
// been seen recently. Otherwise records it.
static bool chat_dedup_check_locked(uint32_t pkt_id)
{
    if (pkt_id == 0) return false;  // unknown id — always allow
    for (int i = 0; i < CHAT_DEDUP_DEPTH; i++) {
        if (s_chat_dedup[i] == pkt_id) return true;
    }
    s_chat_dedup[s_chat_dedup_pos] = pkt_id;
    s_chat_dedup_pos = (s_chat_dedup_pos + 1) % CHAT_DEDUP_DEPTH;
    return false;
}

// ── Relay dedup (Session 3d) ─────────────────────────────────────────
//
// Separate from chat dedup — tracks ALL packet IDs (not just TextMessage)
// to prevent relay loops in flood routing. Every received packet's ID is
// recorded here; if a duplicate arrives later, we skip relaying it.
// 64 entries covers several minutes of typical LongFast traffic.
//
// NOT mutex-protected — only the drain task reads/writes this.
#define RELAY_DEDUP_DEPTH 64
static uint32_t s_relay_dedup[RELAY_DEDUP_DEPTH];
static int      s_relay_dedup_pos = 0;
static uint32_t s_relay_sent      = 0;

// Returns true if `pkt_id` has been seen recently. Otherwise records it.
static bool relay_dedup_check(uint32_t pkt_id)
{
    if (pkt_id == 0) return false;
    for (int i = 0; i < RELAY_DEDUP_DEPTH; i++) {
        if (s_relay_dedup[i] == pkt_id) return true;
    }
    s_relay_dedup[s_relay_dedup_pos] = pkt_id;
    s_relay_dedup_pos = (s_relay_dedup_pos + 1) % RELAY_DEDUP_DEPTH;
    return false;
}

// Push a text message into the chat ring. Skips if `pkt_id` was seen
// recently (relay copy of the same packet). Truncates oversized text.
// Safe to call from any task.
static void chat_push(uint32_t from, uint32_t pkt_id, bool is_self,
                      const uint8_t *text, size_t len)
{
    if (!s_chat_mtx) chat_init();
    if (!s_chat_mtx) return;
    xSemaphoreTake(s_chat_mtx, portMAX_DELAY);

    if (chat_dedup_check_locked(pkt_id)) {
        // Same packet_id we already recorded — quietly drop the
        // duplicate. The mesh_recent ring still saw it, so debug
        // tooling can still inspect relay behaviour.
        xSemaphoreGive(s_chat_mtx);
        return;
    }

    meshtastic_chat_entry_t *e = &s_chat[s_chat_head];
    s_chat_head = (s_chat_head + 1) % MESHTASTIC_CHAT_DEPTH;
    if (s_chat_count < MESHTASTIC_CHAT_DEPTH) s_chat_count++;
    s_chat_total++;

    memset(e, 0, sizeof(*e));
    e->when_ms   = now_ms();
    e->from_node = from;
    e->is_self   = is_self;
    size_t copy = len < (MESHTASTIC_CHAT_TEXT_MAX - 1)
                      ? len : (MESHTASTIC_CHAT_TEXT_MAX - 1);
    // Filter out control chars so a malformed packet can't paint
    // garbage on the screen later.
    size_t out = 0;
    for (size_t i = 0; i < copy; i++) {
        uint8_t c = text[i];
        if (c >= 0x20 && c != 0x7F) e->text[out++] = (char)c;
    }
    e->text[out]  = '\0';
    e->text_len   = (uint8_t)out;

    // Capture what we need for the notification callback while still
    // holding the lock (cheap copy), then invoke the callback OUTSIDE
    // the lock so handlers can call back into the proto layer without
    // deadlocking.
    bool        fire_cb = (!is_self) && (s_chat_notify_cb != NULL) && (out > 0);
    char        cb_text[MESHTASTIC_CHAT_TEXT_MAX];
    uint32_t    cb_from = from;
    if (fire_cb) {
        memcpy(cb_text, e->text, out + 1);
    }
    xSemaphoreGive(s_chat_mtx);

    if (fire_cb) {
        char who[16];
        meshtastic_format_node(cb_from, who, sizeof(who));
        s_chat_notify_cb(cb_from, who, cb_text);
    }
}

size_t meshtastic_chat_snapshot(meshtastic_chat_entry_t *out, size_t max_out)
{
    if (!out || max_out == 0 || !s_chat_mtx) return 0;
    xSemaphoreTake(s_chat_mtx, portMAX_DELAY);
    size_t copy = s_chat_count < max_out ? s_chat_count : max_out;
    // Newest first — same layout as recent_entries.
    for (size_t i = 0; i < copy; i++) {
        size_t idx = (s_chat_head + MESHTASTIC_CHAT_DEPTH - 1 - i) % MESHTASTIC_CHAT_DEPTH;
        out[i] = s_chat[idx];
    }
    xSemaphoreGive(s_chat_mtx);
    return copy;
}

uint32_t meshtastic_chat_total(void)
{
    if (!s_chat_mtx) return 0;
    xSemaphoreTake(s_chat_mtx, portMAX_DELAY);
    uint32_t v = s_chat_total;
    xSemaphoreGive(s_chat_mtx);
    return v;
}

// Setter for the s_chat_notify_cb hook declared at the top of the file.
void meshtastic_proto_set_chat_notify_cb(meshtastic_chat_notify_cb_t cb)
{
    s_chat_notify_cb = cb;
}

// Session 4: PRIVATE_APP receive callback for battle/daycare traffic.
static meshtastic_private_cb_t s_private_cb = NULL;

void meshtastic_proto_set_private_cb(meshtastic_private_cb_t cb)
{
    s_private_cb = cb;
}

// ── NodeDB (Session 3b) ───────────────────────────────────────────────

static meshtastic_node_entry_t s_nodedb[MESHTASTIC_NODEDB_CAP];
static SemaphoreHandle_t       s_nodedb_mtx = NULL;

static void nodedb_init(void)
{
    if (s_nodedb_mtx == NULL) {
        s_nodedb_mtx = xSemaphoreCreateMutex();
        memset(s_nodedb, 0, sizeof(s_nodedb));
    }
}

void meshtastic_nodedb_upsert(uint32_t node_num,
                              const char *long_name,
                              const char *short_name)
{
    if (!s_nodedb_mtx) nodedb_init();
    if (!s_nodedb_mtx) return;  // alloc failed earlier
    xSemaphoreTake(s_nodedb_mtx, portMAX_DELAY);

    // Locate existing entry or the LRU slot to overwrite.
    int      hit  = -1;
    int      lru  = 0;
    uint32_t oldest_seen = UINT32_MAX;
    for (int i = 0; i < MESHTASTIC_NODEDB_CAP; i++) {
        if (s_nodedb[i].valid && s_nodedb[i].node_num == node_num) {
            hit = i;
            break;
        }
        if (!s_nodedb[i].valid) {
            // Empty slot — use it if no exact hit. Sentinel "oldest" so
            // we prefer empty slots over evicting a real entry.
            if (oldest_seen != 0) {
                lru = i;
                oldest_seen = 0;
            }
        } else if (s_nodedb[i].last_seen_ms < oldest_seen) {
            oldest_seen = s_nodedb[i].last_seen_ms;
            lru = i;
        }
    }
    int slot = (hit >= 0) ? hit : lru;
    meshtastic_node_entry_t *e = &s_nodedb[slot];
    if (!e->valid || e->node_num != node_num) {
        memset(e, 0, sizeof(*e));
        e->node_num = node_num;
        e->valid    = true;
    }
    if (long_name  && long_name[0])  strlcpy(e->long_name,  long_name,  sizeof(e->long_name));
    if (short_name && short_name[0]) strlcpy(e->short_name, short_name, sizeof(e->short_name));
    e->last_seen_ms = now_ms();

    xSemaphoreGive(s_nodedb_mtx);
}

bool meshtastic_nodedb_lookup(uint32_t node_num,
                              char *out_long,  size_t long_cap,
                              char *out_short, size_t short_cap)
{
    if (out_long  && long_cap  > 0) out_long[0]  = '\0';
    if (out_short && short_cap > 0) out_short[0] = '\0';
    if (!s_nodedb_mtx) return false;
    xSemaphoreTake(s_nodedb_mtx, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < MESHTASTIC_NODEDB_CAP; i++) {
        if (s_nodedb[i].valid && s_nodedb[i].node_num == node_num) {
            if (out_long  && long_cap  > 0) strlcpy(out_long,  s_nodedb[i].long_name,  long_cap);
            if (out_short && short_cap > 0) strlcpy(out_short, s_nodedb[i].short_name, short_cap);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_nodedb_mtx);
    return found;
}

void meshtastic_format_node(uint32_t node_num, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    char shortn[MESHTASTIC_NODE_NAME_SHORT];
    if (meshtastic_nodedb_lookup(node_num, NULL, 0, shortn, sizeof(shortn))
        && shortn[0] != '\0') {
        strlcpy(out, shortn, cap);
    } else {
        snprintf(out, cap, "!%08lx", (unsigned long)node_num);
    }
}

size_t meshtastic_nodedb_snapshot(meshtastic_node_entry_t *out, size_t max_out)
{
    if (!out || max_out == 0 || !s_nodedb_mtx) return 0;
    xSemaphoreTake(s_nodedb_mtx, portMAX_DELAY);
    size_t copied = 0;
    for (int i = 0; i < MESHTASTIC_NODEDB_CAP && copied < max_out; i++) {
        if (s_nodedb[i].valid) {
            out[copied++] = s_nodedb[i];
        }
    }
    xSemaphoreGive(s_nodedb_mtx);
    return copied;
}

// ── TX side ─────────────────────────────────────────────────────────
//
// Compile-time defaults for our NodeInfo broadcast. These show up in
// every other Meshtastic device's node list. Keep `short` to ≤4 chars
// (upstream truncates to 4 anyway).
#define MT_DEFAULT_LONG_NAME  "HowBoyMatsu"
#define MT_DEFAULT_SHORT_NAME "HBM!"

// Cached node ID derived once from the P4's WiFi MAC.
static uint32_t s_node_id = 0;

uint32_t meshtastic_proto_node_id(void)
{
    if (s_node_id == 0) {
        uint8_t mac[6] = {0};
        // ESP_MAC_WIFI_STA is the canonical base MAC. esp_read_mac() returns
        // ESP_OK on success; on failure we fall back to a recognizable
        // pattern so we don't silently transmit as node !00000000.
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            // Upstream Meshtastic convention: last 4 MAC bytes packed
            // big-endian as a uint32_t. See firmware/src/mesh/NodeDB.cpp
            // `generateNodeNum()`.
            s_node_id = ((uint32_t)mac[2] << 24) |
                        ((uint32_t)mac[3] << 16) |
                        ((uint32_t)mac[4] <<  8) |
                        ((uint32_t)mac[5]      );
        } else {
            ESP_LOGW(TAG, "esp_read_mac failed — using fallback node id");
            s_node_id = 0x484F574Du;  // "HOWM"
        }
        ESP_LOGI(TAG, "node id = !%08lx", (unsigned long)s_node_id);
    }
    return s_node_id;
}

// Append a protobuf varint at *pos. Buffer must have room. Caller is
// responsible for never overflowing; we cap at 5-byte values (uint32).
static void pb_write_varint(uint8_t *buf, size_t *pos, uint32_t v)
{
    while (v >= 0x80) {
        buf[(*pos)++] = (uint8_t)((v & 0x7F) | 0x80);
        v >>= 7;
    }
    buf[(*pos)++] = (uint8_t)v;
}

// Append a tagged length-delimited string field. `field` is the protobuf
// field number; we always use wiretype 2 (length-delimited). NULL `str`
// is a no-op so callers can pass optional fields uniformly.
static void pb_write_string(uint8_t *buf, size_t *pos, int field, const char *str)
{
    if (!str) return;
    size_t len = strlen(str);
    uint32_t tag = ((uint32_t)field << 3) | 2;
    pb_write_varint(buf, pos, tag);
    pb_write_varint(buf, pos, (uint32_t)len);
    memcpy(buf + *pos, str, len);
    *pos += len;
}

// Common send path: takes a built-and-not-yet-encrypted Data protobuf,
// encrypts it with the LongFast key + the per-packet nonce, prepends
// the 16-byte plaintext header, and hands the result to the radio.
//
// `to`     = NodeNum to address (use 0xFFFFFFFF for broadcast).
// `pkt_id` = the 32-bit packet ID for this packet. Callers generate it
//            upstream so they can also feed it to chat-history dedup.
static esp_err_t send_data_frame(uint32_t to, uint32_t pkt_id,
                                 uint8_t *data_pb, size_t data_len)
{
    if (data_len == 0 || data_len + MESHTASTIC_HEADER_LEN > ML_RAW_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t from = meshtastic_proto_node_id();

    // Encrypt the Data bytes in place (CTR is symmetric — same primitive
    // we already use for RX-side decrypt).
    uint8_t encrypted[ML_RAW_MAX_LEN];
    esp_err_t err = meshtastic_decrypt_longfast(from, pkt_id,
                                                data_pb, data_len,
                                                encrypted);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "encrypt failed: %s", esp_err_to_name(err));
        return err;
    }

    // Build the on-air packet: 16-byte plaintext header followed by the
    // encrypted Data payload.
    uint8_t pkt[ML_RAW_MAX_LEN];
    pkt[0]  = (uint8_t)(to      );
    pkt[1]  = (uint8_t)(to  >>  8);
    pkt[2]  = (uint8_t)(to  >> 16);
    pkt[3]  = (uint8_t)(to  >> 24);
    pkt[4]  = (uint8_t)(from     );
    pkt[5]  = (uint8_t)(from >>  8);
    pkt[6]  = (uint8_t)(from >> 16);
    pkt[7]  = (uint8_t)(from >> 24);
    pkt[8]  = (uint8_t)(pkt_id      );
    pkt[9]  = (uint8_t)(pkt_id >>  8);
    pkt[10] = (uint8_t)(pkt_id >> 16);
    pkt[11] = (uint8_t)(pkt_id >> 24);
    // flags: hop_limit=3 (lo 3 bits), hop_start=3 (hi 3 bits), no ack/mqtt
    //        = 0b011_0_0_011 = 0x63
    pkt[12] = 0x63;
    pkt[13] = MESHTASTIC_LONGFAST_CHANNEL_HASH;
    pkt[14] = 0;  // next_hop — unused for broadcast
    pkt[15] = 0;  // relay_node — we are the originator
    memcpy(pkt + MESHTASTIC_HEADER_LEN, encrypted, data_len);

    size_t total = MESHTASTIC_HEADER_LEN + data_len;
    ESP_LOGI(TAG, "tx to=!%08lx from=!%08lx id=%08lx total=%u",
             (unsigned long)to, (unsigned long)from, (unsigned long)pkt_id,
             (unsigned)total);
    return meshtastic_lora_send_raw(pkt, total);
}

esp_err_t meshtastic_send_text(const char *text)
{
    if (!text)            return ESP_ERR_INVALID_ARG;
    size_t text_len = strlen(text);
    if (text_len == 0)    return ESP_ERR_INVALID_ARG;
    if (text_len > 200)   text_len = 200;

    // Generate the packet ID up here so we can feed it to chat_push
    // (for dedup) AND to send_data_frame (for the nonce + header).
    // High bit set: upstream convention; keeps it from looking like a
    // sentinel / zero value.
    uint32_t pkt_id = esp_random() | 0x80000000u;

    // Session 5: record our outbound in the chat history before we
    // actually send. The SX1262 doesn't loopback to its own RX, so this
    // is the only way our own messages show up in the chat view. The
    // packet_id we pass also seeds the dedup ring so the relay copy of
    // our own message gets suppressed when the drain task receives it.
    chat_push(meshtastic_proto_node_id(), pkt_id, true,
              (const uint8_t *)text, text_len);

    // Build the Data protobuf:
    //   field 1 (portnum, varint) = TEXT_MESSAGE_APP
    //   field 2 (payload, bytes)  = <text bytes>
    uint8_t data_pb[256];
    size_t  pos = 0;
    pb_write_varint(data_pb, &pos, (1 << 3) | 0);     // tag field=1 wt=varint
    pb_write_varint(data_pb, &pos, MESHTASTIC_PORTNUM_TEXT_MESSAGE_APP);
    pb_write_varint(data_pb, &pos, (2 << 3) | 2);     // tag field=2 wt=length-delim
    pb_write_varint(data_pb, &pos, (uint32_t)text_len);
    memcpy(data_pb + pos, text, text_len);
    pos += text_len;

    return send_data_frame(MESHTASTIC_BROADCAST_ADDR, pkt_id, data_pb, pos);
}

esp_err_t meshtastic_send_private(uint32_t dest,
                                   const uint8_t *payload, size_t len)
{
    if (!payload || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > 200)            len = 200;

    uint32_t pkt_id = esp_random() | 0x80000000u;

    // Build the Data protobuf:
    //   field 1 (portnum, varint) = PRIVATE_APP (256)
    //   field 2 (payload, bytes)  = <caller's payload>
    uint8_t data_pb[256];
    size_t  pos = 0;
    pb_write_varint(data_pb, &pos, (1 << 3) | 0);     // tag field=1 wt=varint
    pb_write_varint(data_pb, &pos, MESHTASTIC_PORTNUM_PRIVATE_APP);
    pb_write_varint(data_pb, &pos, (2 << 3) | 2);     // tag field=2 wt=length-delim
    pb_write_varint(data_pb, &pos, (uint32_t)len);
    memcpy(data_pb + pos, payload, len);
    pos += len;

    return send_data_frame(dest, pkt_id, data_pb, pos);
}

esp_err_t meshtastic_send_nodeinfo(const char *long_name, const char *short_name)
{
    if (!long_name)  long_name  = MT_DEFAULT_LONG_NAME;
    if (!short_name) short_name = MT_DEFAULT_SHORT_NAME;

    // Build the inner User protobuf:
    //   field 1 (id, string)         = "!XXXXXXXX"
    //   field 2 (long_name, string)  = long_name
    //   field 3 (short_name, string) = short_name
    // hw_model (field 5) is technically required but Meshtastic clients
    // accept its absence gracefully and just display "Unknown" for the
    // hardware type. Skip it until we pick a HardwareModel enum value.
    char id_str[12];
    snprintf(id_str, sizeof(id_str), "!%08lx",
             (unsigned long)meshtastic_proto_node_id());

    uint8_t user_pb[128];
    size_t  user_pos = 0;
    pb_write_string(user_pb, &user_pos, 1, id_str);
    pb_write_string(user_pb, &user_pos, 2, long_name);
    pb_write_string(user_pb, &user_pos, 3, short_name);

    // Wrap in Data: portnum=NODEINFO_APP, payload=<user_pb>, want_response=true.
    //
    // Field 3 (want_response) is the critical bit — upstream
    // NodeInfoModule::handleReceivedProtobuf only auto-replies with its
    // own NodeInfo when that flag is set. Without it, other devices
    // stay quiet until their own scheduled periodic re-announce (which
    // upstream defaults to ~3 hours), so our NodeDB ends up empty
    // for a very long time even though they hear us fine. Setting
    // want_response=true gets us a NodeInfo reply within seconds.
    uint8_t data_pb[256];
    size_t  data_pos = 0;
    pb_write_varint(data_pb, &data_pos, (1 << 3) | 0);   // portnum tag
    pb_write_varint(data_pb, &data_pos, MESHTASTIC_PORTNUM_NODEINFO_APP);
    pb_write_varint(data_pb, &data_pos, (2 << 3) | 2);   // payload tag (length-delim)
    pb_write_varint(data_pb, &data_pos, (uint32_t)user_pos);
    memcpy(data_pb + data_pos, user_pb, user_pos);
    data_pos += user_pos;
    pb_write_varint(data_pb, &data_pos, (3 << 3) | 0);   // want_response tag (varint)
    pb_write_varint(data_pb, &data_pos, 1);              // = true

    // NodeInfo doesn't need to dedup against chat (it's not a
    // TextMessage), but we still need a fresh packet_id for the
    // nonce + header. Same MSB convention as send_text.
    uint32_t pkt_id = esp_random() | 0x80000000u;
    return send_data_frame(MESHTASTIC_BROADCAST_ADDR, pkt_id, data_pb, data_pos);
}

// ─────────────────────────────────────────────────────────────────────

bool meshtastic_decode_user(const uint8_t *buf, size_t len,
                            char *out_id,         size_t id_cap,
                            char *out_long_name,  size_t long_cap,
                            char *out_short_name, size_t short_cap)
{
    if (out_id         && id_cap    > 0) out_id[0]         = '\0';
    if (out_long_name  && long_cap  > 0) out_long_name[0]  = '\0';
    if (out_short_name && short_cap > 0) out_short_name[0] = '\0';
    if (!buf) return false;

    size_t pos = 0;
    while (pos < len) {
        int64_t tag = pb_read_varint(buf, len, &pos);
        if (tag < 0) return false;
        int field = (int)(tag >> 3);
        int wt    = (int)(tag & 0x7);

        if (field >= 1 && field <= 3 && wt == 2) {
            int64_t l = pb_read_varint(buf, len, &pos);
            if (l < 0) return false;
            char  *dst = NULL;
            size_t cap = 0;
            switch (field) {
                case 1: dst = out_id;         cap = id_cap;    break;
                case 2: dst = out_long_name;  cap = long_cap;  break;
                case 3: dst = out_short_name; cap = short_cap; break;
            }
            if (!pb_copy_string(buf, len, &pos, (size_t)l, dst, cap)) return false;
        } else {
            if (!pb_skip_field(buf, len, &pos, wt)) return false;
        }
    }
    return true;
}
// ─────────────────────────────────────────────────────────────────────

esp_err_t meshtastic_parse_header(const uint8_t *buf, size_t buf_len,
                                  meshtastic_header_t *out)
{
    if (!buf || !out)                return ESP_ERR_INVALID_ARG;
    if (buf_len < MESHTASTIC_HEADER_LEN) return ESP_ERR_INVALID_SIZE;

    out->to         = read_u32_le(buf, 0);
    out->from       = read_u32_le(buf, 4);
    out->id         = read_u32_le(buf, 8);
    out->flags      = buf[12];
    out->channel    = buf[13];
    out->next_hop   = buf[14];
    out->relay_node = buf[15];
    return ESP_OK;
}

// ── Recent-packets ring buffer ────────────────────────────────────────

static meshtastic_recent_entry_t s_ring[MESHTASTIC_RECENT_DEPTH];
static size_t                    s_ring_head    = 0;       // next write slot
static size_t                    s_ring_count   = 0;       // 0..DEPTH
static uint32_t                  s_total_parsed = 0;
static SemaphoreHandle_t         s_ring_mtx     = NULL;
static TaskHandle_t              s_drain_task   = NULL;
static bool                      s_started      = false;

static void push_entry(const meshtastic_recent_entry_t *e)
{
    if (!s_ring_mtx) return;
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    s_ring[s_ring_head] = *e;
    s_ring_head = (s_ring_head + 1) % MESHTASTIC_RECENT_DEPTH;
    if (s_ring_count < MESHTASTIC_RECENT_DEPTH) s_ring_count++;
    s_total_parsed++;
    xSemaphoreGive(s_ring_mtx);
}

static void drain_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "drain task started");
    uint8_t raw[ML_RAW_MAX_LEN];
    for (;;) {
        size_t raw_len = 0;
        // Polling pop — meshtastic_lora_pop_raw is non-blocking. Sleep
        // 100 ms between attempts so we don't burn CPU when idle. A
        // future iteration can switch to a blocking queue handle if the
        // raw-bytes producer side grows to expose one, but at the
        // current packet rate (single-digit per minute on LongFast) the
        // 100 ms poll cost is negligible.
        if (meshtastic_lora_pop_raw(raw, sizeof(raw), &raw_len)) {
            meshtastic_recent_entry_t entry;
            entry.rx_ms   = now_ms();
            entry.raw_len = (uint8_t)(raw_len > 255 ? 255 : raw_len);

            esp_err_t perr = meshtastic_parse_header(raw, raw_len, &entry.header);
            if (perr != ESP_OK) {
                // Sub-16-byte packets shouldn't actually reach us (SX1262
                // CRC + Meshtastic-side min length both filter them), but
                // log defensively and drop.
                ESP_LOGW(TAG, "parse failed (%s), %u raw bytes",
                         esp_err_to_name(perr), (unsigned)raw_len);
                continue;
            }

            // Session 3d: record this packet ID in the relay dedup ring
            // EARLY — before any processing — so a second copy of the
            // same packet (arriving via a different relay path) is
            // suppressed even if we haven't finished processing the first.
            bool relay_already_seen = relay_dedup_check(entry.header.id);

            size_t payload_off = MESHTASTIC_HEADER_LEN;
            size_t payload_len = (raw_len > payload_off) ? (raw_len - payload_off) : 0;
            size_t sample      = payload_len < MESHTASTIC_RECENT_PAYLOAD_SAMPLE
                                     ? payload_len
                                     : MESHTASTIC_RECENT_PAYLOAD_SAMPLE;
            entry.payload_sample_len = (uint8_t)sample;
            if (sample > 0) {
                memcpy(entry.payload_sample, raw + payload_off, sample);
            }

            // Session 2b: attempt AES-128-CTR decrypt against the default
            // LongFast key + per-packet nonce. CTR mode never fails on
            // wrong key — you just get garbage — so we use the byte-1
            // protobuf-tag heuristic (`meshtastic_guess_portnum`) to
            // decide whether the result is plausibly real plaintext.
            entry.decrypted        = false;
            entry.portnum_guess    = -1;
            entry.plain_sample_len = 0;
            entry.body[0]          = '\0';
            if (payload_len > 0) {
                uint8_t plain[ML_RAW_MAX_LEN];
                esp_err_t derr = meshtastic_decrypt_longfast(
                    entry.header.from, entry.header.id,
                    raw + payload_off, payload_len, plain);
                if (derr == ESP_OK) {
                    entry.decrypted     = true;
                    entry.portnum_guess = (int16_t)meshtastic_guess_portnum(plain, payload_len);
                    size_t ps = payload_len < MESHTASTIC_RECENT_PAYLOAD_SAMPLE
                                    ? payload_len
                                    : MESHTASTIC_RECENT_PAYLOAD_SAMPLE;
                    entry.plain_sample_len = (uint8_t)ps;
                    memcpy(entry.plain_sample, plain, ps);

                    // Session 2c: pull a printable body out of the Data
                    // submessage when we recognize the portnum.
                    meshtastic_data_t d;
                    if (meshtastic_decode_data(plain, payload_len, &d) &&
                        d.payload != NULL) {
                        if (d.portnum == MESHTASTIC_PORTNUM_TEXT_MESSAGE_APP) {
                            size_t copy = d.payload_len < sizeof(entry.body) - 1
                                              ? d.payload_len
                                              : sizeof(entry.body) - 1;
                            memcpy(entry.body, d.payload, copy);
                            entry.body[copy] = '\0';
                            // Strip control bytes so we never paint
                            // garbage on the terminal. The first invalid
                            // byte terminates the displayed string.
                            for (size_t k = 0; k < copy; k++) {
                                if (entry.body[k] < 0x20 || entry.body[k] == 0x7F) {
                                    entry.body[k] = '\0';
                                    break;
                                }
                            }
                            // Session 5: also push into chat history so
                            // the chat UI shows incoming text messages.
                            // Pass the packet ID so the dedup ring can
                            // suppress relay copies (same packet ID,
                            // different relay_node).
                            chat_push(entry.header.from, entry.header.id,
                                      false, d.payload, d.payload_len);
                        } else if (d.portnum == MESHTASTIC_PORTNUM_NODEINFO_APP) {
                            char longn[MESHTASTIC_NODE_NAME_LONG]  = {0};
                            char shortn[MESHTASTIC_NODE_NAME_SHORT] = {0};
                            meshtastic_decode_user(d.payload, d.payload_len,
                                                   NULL, 0,
                                                   longn, sizeof(longn),
                                                   shortn, sizeof(shortn));
                            // Session 3b: remember this node's names so
                            // future TextMessage rows can render
                            // `HOWA: hi` instead of `!f1bce6ec: hi`.
                            // We key by `from` (the originator) not
                            // `relay_node` — the latter changes per hop.
                            meshtastic_nodedb_upsert(entry.header.from,
                                                     longn, shortn);
                            // Some NodeInfos have only a long name, or
                            // only a short, etc. Format defensively.
                            if (longn[0] && shortn[0]) {
                                snprintf(entry.body, sizeof(entry.body),
                                         "%s (%s)", longn, shortn);
                            } else if (longn[0]) {
                                snprintf(entry.body, sizeof(entry.body),
                                         "%s", longn);
                            } else if (shortn[0]) {
                                snprintf(entry.body, sizeof(entry.body),
                                         "(%s)", shortn);
                            }
                        } else if (d.portnum == MESHTASTIC_PORTNUM_PRIVATE_APP) {
                            // Session 4: forward PRIVATE_APP payloads to
                            // the battle/daycare radio layer. Gate on
                            // relay_already_seen (dedup) and from != self
                            // (avoid processing our own reflected packets).
                            if (!relay_already_seen &&
                                entry.header.from != meshtastic_proto_node_id() &&
                                s_private_cb && d.payload_len > 0) {
                                s_private_cb(entry.header.from,
                                             d.payload, d.payload_len);
                            }
                            snprintf(entry.body, sizeof(entry.body),
                                     "PRIVATE(%u)", (unsigned)d.payload_len);
                        }
                    }
                }
            }

            ESP_LOGI(TAG,
                     "rx from=!%08lx to=%08lx id=%08lx ch=0x%02x flags=0x%02x "
                     "hop=%u/%u plen=%u portnum=%d body=\"%s\"",
                     (unsigned long)entry.header.from,
                     (unsigned long)entry.header.to,
                     (unsigned long)entry.header.id,
                     entry.header.channel,
                     entry.header.flags,
                     meshtastic_hdr_hop_limit(entry.header.flags),
                     meshtastic_hdr_hop_start(entry.header.flags),
                     (unsigned)payload_len,
                     (int)entry.portnum_guess,
                     entry.body);

            push_entry(&entry);

            // ── Session 3d: flood relay ──────────────────────────────
            //
            // If this packet is from another node, hasn't been seen
            // before, and still has hops remaining, re-broadcast it so
            // nodes beyond our direct RF reach can receive it. This is
            // the standard Meshtastic flood-routing behaviour.
            //
            // The raw on-air bytes are retransmitted as-is — the
            // encrypted payload is NOT re-encrypted. Only two header
            // fields change:
            //   flags byte 12: hop_limit decremented by 1
            //   byte 15:       relay_node set to our low node-ID byte
            //
            // A random delay (100-500 ms) before TX reduces collision
            // probability when multiple relayers hear the same packet.
            {
                uint8_t hop_limit = meshtastic_hdr_hop_limit(entry.header.flags);
                // Skip relay for packets that exceed the C6's TX-complete
                // timeout at SF11/BW250 (~80 bytes on-air). Trying to relay
                // them just wastes airtime and logs NACK errors.
                if (!relay_already_seen &&
                    entry.header.from != meshtastic_proto_node_id() &&
                    hop_limit > 0 &&
                    raw_len <= 80) {

                    uint32_t delay_ms = 100 + (esp_random() % 400);
                    vTaskDelay(pdMS_TO_TICKS(delay_ms));

                    uint8_t relay_pkt[ML_RAW_MAX_LEN];
                    memcpy(relay_pkt, raw, raw_len);

                    uint8_t new_hop = hop_limit - 1;
                    relay_pkt[12] = (entry.header.flags & 0xF8) | (new_hop & 0x07);
                    relay_pkt[15] = (uint8_t)(meshtastic_proto_node_id() & 0xFF);

                    esp_err_t relay_err = meshtastic_lora_send_raw(relay_pkt, raw_len);
                    if (relay_err == ESP_OK) {
                        s_relay_sent++;
                        ESP_LOGI(TAG,
                                 "relay from=!%08lx id=%08lx hop=%u->%u delay=%ums",
                                 (unsigned long)entry.header.from,
                                 (unsigned long)entry.header.id,
                                 (unsigned)hop_limit, (unsigned)new_hop,
                                 (unsigned)delay_ms);
                    } else {
                        ESP_LOGW(TAG, "relay send failed: %s",
                                 esp_err_to_name(relay_err));
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ── Auto-announce (Session 3a) ───────────────────────────────────────
//
// Broadcast our NodeInfo on a "decaying" schedule so the network
// catches us quickly after boot without spamming the channel
// forever:
//   t=5s        first announce (radio settle)
//   t=35s       second  — covers cases where the 1st collided / was
//               missed on the receiver's RX window
//   t=2min      third
//   then every 5 min indefinitely
// Each announce has want_response=true (see send_nodeinfo above), so
// receivers that don't have us yet will reply with their NodeInfo and
// our NodeDB populates within seconds.

#define MT_ANNOUNCE_PERIOD_MS  300000   // 300 s = 5 min steady-state

static TaskHandle_t s_announce_task = NULL;

static void announcer_task(void *arg)
{
    (void)arg;
    static const uint32_t schedule_ms[] = { 5000, 30000, 90000 };
    for (size_t i = 0; i < sizeof(schedule_ms) / sizeof(schedule_ms[0]); i++) {
        vTaskDelay(pdMS_TO_TICKS(schedule_ms[i]));
        ESP_LOGI(TAG, "auto-announce NodeInfo (initial #%u)", (unsigned)(i + 1));
        esp_err_t err = meshtastic_send_nodeinfo(NULL, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "auto-announce failed: %s", esp_err_to_name(err));
        }
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MT_ANNOUNCE_PERIOD_MS));
        ESP_LOGI(TAG, "auto-announce NodeInfo (periodic)");
        esp_err_t err = meshtastic_send_nodeinfo(NULL, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "auto-announce failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t meshtastic_proto_begin(void)
{
    if (s_started) return ESP_OK;

    s_ring_mtx = xSemaphoreCreateMutex();
    if (!s_ring_mtx) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memset(s_ring, 0, sizeof(s_ring));
    s_ring_head    = 0;
    s_ring_count   = 0;
    s_total_parsed = 0;

    nodedb_init();
    chat_init();

    // Self-register in the NodeDB so terminal commands can render "us"
    // by the default name even before the announcer task fires.
    meshtastic_nodedb_upsert(meshtastic_proto_node_id(),
                             MT_DEFAULT_LONG_NAME, MT_DEFAULT_SHORT_NAME);

    if (xTaskCreate(drain_task, "mp_drain", 4 * 1024, NULL, 5, &s_drain_task) != pdPASS) {
        ESP_LOGE(TAG, "drain task alloc failed");
        vSemaphoreDelete(s_ring_mtx);
        s_ring_mtx = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(announcer_task, "mp_ann", 4 * 1024, NULL, 4,
                    &s_announce_task) != pdPASS) {
        // Non-fatal — drainer is up, RX still works, the user just won't
        // appear in T-Deck node lists until manual `mesh_announce`.
        ESP_LOGW(TAG, "announcer task alloc failed — NodeInfo broadcast disabled");
    }

    s_started = true;
    ESP_LOGI(TAG, "meshtastic_proto up (ring=%d, nodedb=%d, period=%ds)",
             MESHTASTIC_RECENT_DEPTH, MESHTASTIC_NODEDB_CAP,
             MT_ANNOUNCE_PERIOD_MS / 1000);
    return ESP_OK;
}

size_t meshtastic_proto_recent(meshtastic_recent_entry_t *out, size_t max_out)
{
    if (!out || !s_ring_mtx || max_out == 0) return 0;
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    size_t copy = s_ring_count < max_out ? s_ring_count : max_out;
    // Newest first. s_ring_head points one past the most recent entry
    // (or wraps around when the ring is full). So the newest valid slot
    // is (head - 1) mod DEPTH; we walk backward from there.
    for (size_t i = 0; i < copy; i++) {
        size_t idx = (s_ring_head + MESHTASTIC_RECENT_DEPTH - 1 - i) % MESHTASTIC_RECENT_DEPTH;
        out[i] = s_ring[idx];
    }
    xSemaphoreGive(s_ring_mtx);
    return copy;
}

uint32_t meshtastic_proto_total_parsed(void)
{
    if (!s_ring_mtx) return 0;
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    uint32_t v = s_total_parsed;
    xSemaphoreGive(s_ring_mtx);
    return v;
}

uint32_t meshtastic_proto_total_relayed(void)
{
    // Single-writer (drain task), 32-bit atomic read on ESP32 — no mutex.
    return s_relay_sent;
}
