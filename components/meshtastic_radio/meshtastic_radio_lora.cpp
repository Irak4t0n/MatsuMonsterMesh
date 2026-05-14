// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session 4: LoRa-backed MeshtasticRadio for battle/daycare traffic.
// See meshtastic_radio_lora.h for overview.

#include "meshtastic_radio_lora.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

extern "C" {
#include "meshtastic_proto.h"
}

static const char *TAG = "MMRadio";

// ── Internal RX queue ────────────────────────────────────────────────────────
//
// The drain task fires the PRIVATE_APP callback on its own thread; we
// stash each packet in a FreeRTOS queue so pollPackets() can drain them
// non-blocking from the game loop. Only one LoRaMeshtasticRadio instance
// is expected — the queue is file-static.

#define LORA_RADIO_RX_DEPTH 16

struct RxEntry {
    uint32_t from;
    uint16_t len;
    uint8_t  data[237];   // Meshtastic decoded-payload max
};

static QueueHandle_t s_rx_queue = nullptr;

static void private_rx_cb(uint32_t from_node,
                           const uint8_t *payload, size_t len)
{
    if (!s_rx_queue || !payload || len == 0) return;
    RxEntry e;
    e.from = from_node;
    e.len  = (len > sizeof(e.data)) ? sizeof(e.data) : (uint16_t)len;
    memcpy(e.data, payload, e.len);
    if (xQueueSend(s_rx_queue, &e, 0) != pdTRUE) {
        // Full — drop oldest so the newest packet isn't lost.
        RxEntry drop;
        xQueueReceive(s_rx_queue, &drop, 0);
        xQueueSend(s_rx_queue, &e, 0);
        ESP_LOGW(TAG, "rx queue full, dropped oldest");
    }
}

// ── LoRaMeshtasticRadio implementation ───────────────────────────────────────

LoRaMeshtasticRadio::LoRaMeshtasticRadio()  = default;
LoRaMeshtasticRadio::~LoRaMeshtasticRadio() = default;

bool LoRaMeshtasticRadio::begin()
{
    if (!s_rx_queue) {
        s_rx_queue = xQueueCreate(LORA_RADIO_RX_DEPTH, sizeof(RxEntry));
        if (!s_rx_queue) {
            ESP_LOGE(TAG, "LoRa RX queue alloc failed");
            return false;
        }
    }
    meshtastic_proto_set_private_cb(private_rx_cb);
    ESP_LOGI(TAG, "LoRaMeshtasticRadio up (portnum=%d, queue=%d)",
             MESHTASTIC_PORTNUM_PRIVATE_APP, LORA_RADIO_RX_DEPTH);
    return true;
}

bool LoRaMeshtasticRadio::sendPacket(uint32_t dest, uint8_t channel,
                                      const uint8_t *payload, size_t len)
{
    (void)channel;   // always LongFast default channel
    esp_err_t err = meshtastic_send_private(dest, payload, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sendPacket failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

int LoRaMeshtasticRadio::pollPackets(MeshPacketSimple *out, int max_count)
{
    if (!s_rx_queue || !out || max_count <= 0) return 0;
    int count = 0;
    RxEntry e;
    while (count < max_count &&
           xQueueReceive(s_rx_queue, &e, 0) == pdTRUE) {
        out[count].from        = e.from;
        out[count].channel     = 0;
        out[count].payload_len = e.len;
        memcpy(out[count].payload, e.data, e.len);
        count++;
    }
    return count;
}

int LoRaMeshtasticRadio::getNeighborCount()
{
    meshtastic_node_entry_t nodes[MESHTASTIC_NODEDB_CAP];
    return (int)meshtastic_nodedb_snapshot(nodes, MESHTASTIC_NODEDB_CAP);
}

bool LoRaMeshtasticRadio::getNodeList(MeshNodeInfo *nodes,
                                       int max_nodes, int *count)
{
    if (!nodes || max_nodes <= 0) {
        if (count) *count = 0;
        return false;
    }
    meshtastic_node_entry_t raw[MESHTASTIC_NODEDB_CAP];
    size_t n = meshtastic_nodedb_snapshot(raw, MESHTASTIC_NODEDB_CAP);
    int out = 0;
    for (size_t i = 0; i < n && out < max_nodes; i++) {
        nodes[out].node_id = raw[i].node_num;
        strlcpy(nodes[out].short_name, raw[i].short_name,
                sizeof(nodes[out].short_name));
        strlcpy(nodes[out].long_name, raw[i].long_name,
                sizeof(nodes[out].long_name));
        nodes[out].snr = 0;   // no per-packet SNR from our LoRa layer
        out++;
    }
    if (count) *count = out;
    return true;
}
