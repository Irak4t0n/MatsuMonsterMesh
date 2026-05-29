// SPDX-License-Identifier: GPL-3.0-or-later
//
// mqtt_transport — WiFi + MQTT bridge for Meshtastic MonsterMesh.
//
// Connects to a Meshtastic MQTT broker (e.g., EMQX) and subscribes to the
// MonsterMesh channel topic. Incoming MQTT packets are decoded from the
// Meshtastic ServiceEnvelope protobuf and injected into the existing LoRa
// drain task via meshtastic_lora_push_raw(). Outgoing packets are published
// as ServiceEnvelope protobufs in parallel with the LoRa TX path.
//
// WiFi credentials come from the Tanmatsu launcher's NVS storage — no
// hardcoded SSID/password required.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize WiFi (via esp-hosted remote to C6) and connect to the MQTT
// broker. Runs asynchronously — WiFi association and MQTT connect happen
// in background tasks. LoRa continues to work independently.
//
// Call once from monster_init() after meshtastic_lora_begin(). Safe to
// call even if WiFi isn't configured (gracefully logs and returns).
esp_err_t mqtt_transport_init(void);

// Returns true once the MQTT client is connected and subscribed.
bool mqtt_transport_is_connected(void);

// Returns true once WiFi has obtained an IP address.
bool mqtt_transport_wifi_up(void);

// Publish a Meshtastic packet to the MQTT broker as a ServiceEnvelope.
// Called from the meshtastic_proto MQTT TX hook. The encrypted bytes
// are wrapped in a MeshPacket → ServiceEnvelope protobuf and published
// to the configured topic. Non-blocking; returns ESP_OK on success.
esp_err_t mqtt_transport_publish(uint32_t to, uint32_t from,
                                  uint32_t pkt_id, uint8_t channel_hash,
                                  const uint8_t *encrypted, size_t enc_len);

#ifdef __cplusplus
}
#endif
