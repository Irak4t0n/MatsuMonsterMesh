// SPDX-License-Identifier: GPL-3.0-or-later
//
// mqtt_transport.c — WiFi + MQTT bridge for Meshtastic MonsterMesh.

#include "mqtt_transport.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "meshtastic_lora.h"
#include "meshtastic_proto.h"

static const char *TAG = "mqtt";

// ── Configuration ────────────────────────────────────────────────────────

// MQTT broker — from the user's Meshtastic MQTT config screenshots.
#define MQTT_BROKER_URI   "mqtts://sf17b671.ala.us-east-1.emqxsl.com:8883"
#define MQTT_USERNAME     "ash"
#define MQTT_PASSWORD     "large4meowth"

// Meshtastic MQTT topic structure: <root>/2/e/<channel>/<nodeId>
// root="kanto", channel="MonsterMesh"
#define MQTT_ROOT_TOPIC   "kanto"
#define MQTT_CHANNEL_NAME "MonsterMesh"

// Subscribe topic: all encrypted traffic on this root.
// We subscribe broadly so we don't miss beacons if the peer's channel name
// differs slightly (e.g. "MonsterMesh" vs "MonsterMesh Center").  LongFast
// traffic won't appear here because peers keep MQTT uplink disabled on that
// channel.  The drain task tries every registered PSK anyway, so decryption
// is channel-agnostic.
#define MQTT_SUB_TOPIC    MQTT_ROOT_TOPIC "/2/e/#"

// ── State ────────────────────────────────────────────────────────────────

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool                     s_connected   = false;
static bool                     s_wifi_up     = false;
static EventGroupHandle_t       s_wifi_events = NULL;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Publish topic buffer (built once after we know our node ID)
static char s_pub_topic[80];

// ── Protobuf helpers (hand-rolled, minimal) ──────────────────────────────

static size_t pb_varint_size(uint32_t val)
{
    size_t n = 1;
    while (val > 0x7F) { n++; val >>= 7; }
    return n;
}

static void pb_put_varint(uint8_t *buf, size_t *pos, uint32_t val)
{
    while (val > 0x7F) {
        buf[(*pos)++] = (uint8_t)(val & 0x7F) | 0x80;
        val >>= 7;
    }
    buf[(*pos)++] = (uint8_t)val;
}

static void pb_put_fixed32(uint8_t *buf, size_t *pos, uint32_t val)
{
    buf[(*pos)++] = (uint8_t)(val);
    buf[(*pos)++] = (uint8_t)(val >> 8);
    buf[(*pos)++] = (uint8_t)(val >> 16);
    buf[(*pos)++] = (uint8_t)(val >> 24);
}

static void pb_put_bytes(uint8_t *buf, size_t *pos,
                          uint32_t field, const uint8_t *data, size_t len)
{
    pb_put_varint(buf, pos, (field << 3) | 2);  // wiretype 2 = length-delimited
    pb_put_varint(buf, pos, (uint32_t)len);
    memcpy(buf + *pos, data, len);
    *pos += len;
}

static void pb_put_string(uint8_t *buf, size_t *pos,
                            uint32_t field, const char *str)
{
    pb_put_bytes(buf, pos, field, (const uint8_t *)str, strlen(str));
}

static void pb_put_fixed32_field(uint8_t *buf, size_t *pos,
                                   uint32_t field, uint32_t val)
{
    pb_put_varint(buf, pos, (field << 3) | 5);  // wiretype 5 = 32-bit
    pb_put_fixed32(buf, pos, val);
}

static void pb_put_varint_field(uint8_t *buf, size_t *pos,
                                  uint32_t field, uint32_t val)
{
    pb_put_varint(buf, pos, (field << 3) | 0);  // wiretype 0 = varint
    pb_put_varint(buf, pos, val);
}

// Read a varint from buf[*pos], advancing *pos. Returns 0 on error.
static uint32_t pb_read_varint(const uint8_t *buf, size_t len, size_t *pos)
{
    uint32_t val = 0;
    int shift = 0;
    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        val |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return val;
        shift += 7;
        if (shift >= 35) break;  // overflow guard
    }
    return val;
}

static uint32_t pb_read_fixed32(const uint8_t *buf, size_t len, size_t *pos)
{
    if (*pos + 4 > len) return 0;
    uint32_t val = ((uint32_t)buf[*pos]) |
                   ((uint32_t)buf[*pos + 1] << 8) |
                   ((uint32_t)buf[*pos + 2] << 16) |
                   ((uint32_t)buf[*pos + 3] << 24);
    *pos += 4;
    return val;
}

// Skip a protobuf field by wiretype. Returns false on error.
static bool pb_skip(const uint8_t *buf, size_t len, size_t *pos, uint32_t wt)
{
    switch (wt) {
        case 0: pb_read_varint(buf, len, pos); return *pos <= len;
        case 1: *pos += 8; return *pos <= len;
        case 2: { uint32_t n = pb_read_varint(buf, len, pos); *pos += n; return *pos <= len; }
        case 5: *pos += 4; return *pos <= len;
        default: return false;
    }
}

// ── ServiceEnvelope encoder ──────────────────────────────────────────────

// Encode a MeshPacket protobuf into `out`, return encoded length.
// Field numbers match standard Meshtastic firmware MQTT encoding:
//   1: from (fixed32)
//   2: to   (fixed32)
//   3: channel (varint)  — channel HASH (e.g. 0x25 for MonsterMesh)
//   5: encrypted (bytes) — standard firmware uses field 5
//   6: id (fixed32)
//   9: hop_limit (varint)
//  14: via_mqtt (bool/varint)
//  15: hop_start (varint)
static size_t encode_mesh_packet(uint8_t *out, size_t cap,
                                   uint32_t from, uint32_t to,
                                   uint32_t pkt_id, uint8_t channel_hash,
                                   const uint8_t *encrypted, size_t enc_len)
{
    size_t pos = 0;
    (void)cap;  // caller ensures buffer is large enough

    // Match standard Meshtastic firmware encoding (field 5 for encrypted,
    // channel = hash value, not index).
    pb_put_fixed32_field(out, &pos, 1, from);            // from
    pb_put_fixed32_field(out, &pos, 2, to);               // to
    pb_put_varint_field(out, &pos, 3, channel_hash);       // channel HASH (not index)
    pb_put_bytes(out, &pos, 5, encrypted, enc_len);       // encrypted (field 5!)
    pb_put_fixed32_field(out, &pos, 6, pkt_id);           // id
    pb_put_varint_field(out, &pos, 9, 3);                 // hop_limit
    pb_put_varint_field(out, &pos, 14, 1);                // via_mqtt = true
    pb_put_varint_field(out, &pos, 15, 3);                // hop_start

    return pos;
}

// Encode a ServiceEnvelope wrapping a MeshPacket.
// ServiceEnvelope:
//   1: packet (MeshPacket, submessage)
//   2: channel_id (string)
//   3: gateway_id (string)
static size_t encode_service_envelope(uint8_t *out, size_t cap,
                                        uint32_t from, uint32_t to,
                                        uint32_t pkt_id, uint8_t channel_hash,
                                        const uint8_t *encrypted, size_t enc_len,
                                        const char *channel_id,
                                        const char *gateway_id)
{
    // Encode MeshPacket into a temp buffer first to get its size
    uint8_t mp_buf[512];
    size_t mp_len = encode_mesh_packet(mp_buf, sizeof(mp_buf),
                                        from, to, pkt_id, channel_hash,
                                        encrypted, enc_len);

    size_t pos = 0;
    // field 1 = MeshPacket (submessage, wiretype 2)
    pb_put_bytes(out, &pos, 1, mp_buf, mp_len);
    // field 2 = channel_id (string)
    pb_put_string(out, &pos, 2, channel_id);
    // field 3 = gateway_id (string)
    pb_put_string(out, &pos, 3, gateway_id);

    return pos;
}

// ── ServiceEnvelope decoder ──────────────────────────────────────────────

typedef struct {
    uint32_t from;
    uint32_t to;
    uint32_t id;
    uint32_t channel;
    uint32_t hop_limit;
    uint32_t hop_start;
    bool     via_mqtt;
    const uint8_t *encrypted;
    size_t   encrypted_len;
} decoded_mesh_packet_t;

// Decode a MeshPacket protobuf. Returns true on success.
static bool decode_mesh_packet(const uint8_t *buf, size_t len,
                                 decoded_mesh_packet_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t pos = 0;
    while (pos < len) {
        uint32_t tag = pb_read_varint(buf, len, &pos);
        uint32_t field = tag >> 3;
        uint32_t wt    = tag & 0x07;

        switch (field) {
            case 1:  // from (fixed32)
                if (wt != 5) { if (!pb_skip(buf, len, &pos, wt)) return false; break; }
                out->from = pb_read_fixed32(buf, len, &pos);
                break;
            case 2:  // to (fixed32)
                if (wt != 5) { if (!pb_skip(buf, len, &pos, wt)) return false; break; }
                out->to = pb_read_fixed32(buf, len, &pos);
                break;
            case 3:  // channel (varint)
                if (wt != 0) { if (!pb_skip(buf, len, &pos, wt)) return false; break; }
                out->channel = pb_read_varint(buf, len, &pos);
                break;
            case 5:  // encrypted (bytes) — legacy field 5 (some firmware)
            case 8:  // encrypted (bytes) — standard upstream field 8
                if (wt != 2) { if (!pb_skip(buf, len, &pos, wt)) return false; break; }
                out->encrypted_len = pb_read_varint(buf, len, &pos);
                if (pos + out->encrypted_len > len) return false;
                out->encrypted = buf + pos;
                pos += out->encrypted_len;
                break;
            case 6:  // id (fixed32)
                if (wt != 5) { if (!pb_skip(buf, len, &pos, wt)) return false; break; }
                out->id = pb_read_fixed32(buf, len, &pos);
                break;
            case 9:  // hop_limit (varint)
                if (wt != 0) { if (!pb_skip(buf, len, &pos, wt)) return false; break; }
                out->hop_limit = pb_read_varint(buf, len, &pos);
                break;
            case 14: // via_mqtt (varint/bool)
                if (wt != 0) { if (!pb_skip(buf, len, &pos, wt)) return false; break; }
                out->via_mqtt = pb_read_varint(buf, len, &pos) != 0;
                break;
            case 15: // hop_start (varint)
                if (wt != 0) { if (!pb_skip(buf, len, &pos, wt)) return false; break; }
                out->hop_start = pb_read_varint(buf, len, &pos);
                break;
            default:
                if (!pb_skip(buf, len, &pos, wt)) return false;
                break;
        }
    }
    return out->encrypted != NULL && out->encrypted_len > 0;
}

// Decode a ServiceEnvelope, extract MeshPacket, reconstruct on-air format,
// and push into the LoRa raw queue for the drain task to process.
static void handle_mqtt_message(const uint8_t *data, size_t data_len)
{
    // Hex dump first 64 bytes for protocol comparison
    {
        char hex[64 * 3 + 1];
        size_t dump_len = data_len < 64 ? data_len : 64;
        for (size_t i = 0; i < dump_len; i++)
            snprintf(hex + i * 3, 4, "%02x ", data[i]);
        hex[dump_len * 3] = '\0';
        ESP_LOGI(TAG, "mqtt rx envelope[%u]: %s", (unsigned)data_len, hex);
    }

    // Parse ServiceEnvelope: field 1 = MeshPacket (bytes/submessage)
    size_t pos = 0;
    const uint8_t *mp_buf = NULL;
    size_t mp_len = 0;

    while (pos < data_len) {
        uint32_t tag = pb_read_varint(data, data_len, &pos);
        uint32_t field = tag >> 3;
        uint32_t wt    = tag & 0x07;

        if (field == 1 && wt == 2) {
            mp_len = pb_read_varint(data, data_len, &pos);
            if (pos + mp_len > data_len) return;
            mp_buf = data + pos;
            pos += mp_len;
        } else {
            if (!pb_skip(data, data_len, &pos, wt)) return;
        }
    }

    if (!mp_buf || mp_len == 0) {
        ESP_LOGW(TAG, "ServiceEnvelope missing MeshPacket");
        return;
    }

    decoded_mesh_packet_t mp;
    if (!decode_mesh_packet(mp_buf, mp_len, &mp)) {
        ESP_LOGW(TAG, "failed to decode MeshPacket");
        return;
    }

    // Skip our own packets (we sent them, no need to process)
    if (mp.from == meshtastic_proto_node_id()) {
        ESP_LOGD(TAG, "mqtt rx skip own packet id=%08lx", (unsigned long)mp.id);
        return;
    }

    ESP_LOGI(TAG, "mqtt rx from=!%08lx to=%08lx id=%08lx ch=0x%02x enc=%u",
             (unsigned long)mp.from, (unsigned long)mp.to,
             (unsigned long)mp.id, (unsigned)mp.channel,
             (unsigned)mp.encrypted_len);

    // Reconstruct 16-byte on-air header + encrypted payload
    size_t total = MESHTASTIC_HEADER_LEN + mp.encrypted_len;
    if (total > ML_RAW_MAX_LEN) {
        ESP_LOGW(TAG, "mqtt packet too large: %u", (unsigned)total);
        return;
    }

    uint8_t raw[ML_RAW_MAX_LEN];
    // to (LE)
    raw[0] = (uint8_t)(mp.to);
    raw[1] = (uint8_t)(mp.to >> 8);
    raw[2] = (uint8_t)(mp.to >> 16);
    raw[3] = (uint8_t)(mp.to >> 24);
    // from (LE)
    raw[4] = (uint8_t)(mp.from);
    raw[5] = (uint8_t)(mp.from >> 8);
    raw[6] = (uint8_t)(mp.from >> 16);
    raw[7] = (uint8_t)(mp.from >> 24);
    // id (LE)
    raw[8]  = (uint8_t)(mp.id);
    raw[9]  = (uint8_t)(mp.id >> 8);
    raw[10] = (uint8_t)(mp.id >> 16);
    raw[11] = (uint8_t)(mp.id >> 24);
    // flags: hop_limit=0 so drain task won't relay MQTT packets onto LoRa.
    // via_mqtt bit set to mark origin.
    uint8_t flags = 0x10;  // hop_limit=0, via_mqtt=1, hop_start=0
    raw[12] = flags;
    raw[13] = (uint8_t)mp.channel;  // channel hash from MeshPacket
    raw[14] = 0;     // next_hop
    raw[15] = 0;     // relay_node
    // Encrypted payload
    memcpy(raw + MESHTASTIC_HEADER_LEN, mp.encrypted, mp.encrypted_len);

    // Push into the LoRa raw queue — the drain task will decrypt,
    // dedup, and dispatch exactly like a LoRa packet.
    bool ok = meshtastic_lora_push_raw(raw, total);
    ESP_LOGI(TAG, "mqtt rx injected %u bytes into raw queue: %s",
             (unsigned)total, ok ? "ok" : "FAIL");
}

// ── WiFi event handlers ──────────────────────────────────────────────────

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started, connecting...");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
                s_wifi_up = false;
                esp_wifi_connect();
                break;
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_up = true;
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
    }
}

// ── MQTT event handler ───────────────────────────────────────────────────

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            s_connected = true;
            // Subscribe to MonsterMesh channel (QoS 1, matching standard Meshtastic)
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_SUB_TOPIC, 1);
            ESP_LOGI(TAG, "subscribed to %s", MQTT_SUB_TOPIC);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            s_connected = false;
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA: topic_len=%d data_len=%d",
                     event->topic_len, event->data_len);
            if (event->topic && event->topic_len > 0) {
                ESP_LOGI(TAG, "  topic: %.*s", event->topic_len, event->topic);
                // Skip PKI-encrypted topics (we can't decrypt those)
                if (memmem(event->topic, event->topic_len, "/PKI/", 5)) {
                    ESP_LOGD(TAG, "  skipping PKI topic");
                    break;
                }
            }
            if (event->data && event->data_len > 0) {
                handle_mqtt_message((const uint8_t *)event->data,
                                     (size_t)event->data_len);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "MQTT error type=%d",
                     event->error_handle ? event->error_handle->error_type : -1);
            break;

        default:
            break;
    }
}

// ── Background init task ─────────────────────────────────────────────────

static void mqtt_init_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "mqtt_init_task started");

    // 1. Initialize TCP/IP and event loop (safe to call multiple times)
    esp_err_t err = esp_netif_init();
    ESP_LOGI(TAG, "esp_netif_init: %s", esp_err_to_name(err));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        goto done;
    }
    err = esp_event_loop_create_default();
    ESP_LOGI(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        goto done;
    }

    // 2. Create default STA interface
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    ESP_LOGI(TAG, "esp_netif_create_default_wifi_sta: %p", sta);
    if (!sta) {
        goto done;
    }

    // 3. Init WiFi (via esp-hosted remote to C6)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    ESP_LOGI(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        goto done;
    }

    // Register event handlers
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    // 4. Set mode and start — credentials come from NVS (launcher-configured)
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "esp_wifi_set_mode(STA): %s", esp_err_to_name(err));
    err = esp_wifi_start();
    ESP_LOGI(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        goto done;
    }

    // 5. Wait for WiFi connection (up to 30s)
    ESP_LOGI(TAG, "waiting for WiFi connection (30s timeout)...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(30000));
    ESP_LOGI(TAG, "WiFi wait result: bits=0x%lx", (unsigned long)bits);
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi connection timed out — MQTT disabled");
        goto done;
    }

    // 6. Build publish topic
    uint32_t node_id = meshtastic_proto_node_id();
    snprintf(s_pub_topic, sizeof(s_pub_topic),
             MQTT_ROOT_TOPIC "/2/e/" MQTT_CHANNEL_NAME "/!%08lx",
             (unsigned long)node_id);
    ESP_LOGI(TAG, "publish topic: %s", s_pub_topic);

    // 7. Start MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        goto done;
    }
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, NULL);
    err = esp_mqtt_client_start(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "MQTT client started, connecting to %s", MQTT_BROKER_URI);
    }

done:
    vTaskDelete(NULL);
}

// ── Public API ───────────────────────────────────────────────────────────

esp_err_t mqtt_transport_init(void)
{
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return ESP_ERR_NO_MEM;

    // Run WiFi + MQTT init in a background task so it doesn't block
    // the emulator from starting. WiFi association can take 5-10s.
    if (xTaskCreate(mqtt_init_task, "mqtt_init", 6 * 1024, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create mqtt_init task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "MQTT transport init started (background)");
    return ESP_OK;
}

bool mqtt_transport_is_connected(void)
{
    return s_connected;
}

bool mqtt_transport_wifi_up(void)
{
    return s_wifi_up;
}

esp_err_t mqtt_transport_publish(uint32_t to, uint32_t from,
                                   uint32_t pkt_id, uint8_t channel_hash,
                                   const uint8_t *encrypted, size_t enc_len)
{
    if (!s_connected || !s_mqtt_client) return ESP_ERR_INVALID_STATE;
    if (!encrypted || enc_len == 0) return ESP_ERR_INVALID_ARG;

    // Build the gateway_id string: "!XXXXXXXX"
    char gw_id[12];
    snprintf(gw_id, sizeof(gw_id), "!%08lx", (unsigned long)from);

    // Encode ServiceEnvelope — pass channel_hash directly (standard firmware
    // puts the hash in the MeshPacket channel field, not the index).
    uint8_t envelope[768];
    size_t env_len = encode_service_envelope(envelope, sizeof(envelope),
                                              from, to, pkt_id, channel_hash,
                                              encrypted, enc_len,
                                              MQTT_CHANNEL_NAME, gw_id);

    // Hex dump first 64 bytes of ServiceEnvelope for protocol debugging
    {
        char hex[64 * 3 + 1];
        size_t dump_len = env_len < 64 ? env_len : 64;
        for (size_t i = 0; i < dump_len; i++)
            snprintf(hex + i * 3, 4, "%02x ", envelope[i]);
        hex[dump_len * 3] = '\0';
        ESP_LOGI(TAG, "mqtt tx envelope[%u]: %s", (unsigned)env_len, hex);
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_pub_topic,
                                          (const char *)envelope, (int)env_len,
                                          1, 0);  // QoS 1, no retain
    if (msg_id < 0) {
        ESP_LOGW(TAG, "mqtt publish failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "mqtt tx to=!%08lx id=%08lx ch=0x%02x topic=%s",
             (unsigned long)to, (unsigned long)pkt_id, (unsigned)channel_hash, s_pub_topic);
    return ESP_OK;
}
