// LoRaMeshtasticRadio — MeshtasticRadio backed by the real LoRa stack.
//
// Session 4: bridges the MeshtasticRadio abstract interface (used by
// MonsterMeshTextBattle, PokemonDaycare, etc.) to the meshtastic_proto /
// meshtastic_lora layers that already handle C6 bring-up, AES-128-CTR
// encryption, protobuf framing, and on-air TX/RX.
//
// Battle/daycare payloads are sent as Meshtastic Data messages with
// portnum = PRIVATE_APP (256). Standard Meshtastic clients silently
// ignore this portnum. Received PRIVATE_APP packets are delivered via
// a callback from the drain task into an internal FreeRTOS queue.

#pragma once

#include "MeshtasticRadio.h"

class LoRaMeshtasticRadio : public MeshtasticRadio {
public:
    LoRaMeshtasticRadio();
    ~LoRaMeshtasticRadio() override;

    // Create the RX queue and register the PRIVATE_APP callback with the
    // proto layer. Safe to call before or after meshtastic_proto_begin();
    // packets simply won't arrive until the drain task is running.
    bool begin();

    bool sendPacket(uint32_t dest, uint8_t channel,
                    const uint8_t* payload, size_t len) override;
    int  pollPackets(MeshPacketSimple* out, int max_count) override;
    int  getNeighborCount() override;
    bool getNodeList(MeshNodeInfo* nodes, int max_nodes, int* count) override;
};
