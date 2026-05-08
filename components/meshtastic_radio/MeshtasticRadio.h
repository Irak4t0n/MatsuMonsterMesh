// MeshtasticRadio — abstract interface for sending / receiving Meshtastic
// packets from MatsuMonsterMesh game code. Concrete implementations:
//   - SerialMeshtasticRadio  (talks to the ESP32-C6 radio over UART)
//   - StubMeshtasticRadio    (logs sends, no real I/O)
//
// Step 3 ports monster_core to depend only on this interface; Steps 4-6 wire
// up the concrete implementations.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus

struct MeshPacketSimple {
    uint32_t from;
    uint8_t  channel;
    uint8_t  payload[256];
    size_t   payload_len;
};

struct MeshNodeInfo {
    uint32_t node_id;
    char     short_name[5];
    char     long_name[40];
    int8_t   snr;
};

class MeshtasticRadio {
public:
    virtual ~MeshtasticRadio() = default;

    // Send a packet to a specific node id (0xFFFFFFFF == broadcast) on the
    // given Meshtastic channel index. Returns false if the radio is not ready
    // or the packet was dropped.
    virtual bool sendPacket(uint32_t dest, uint8_t channel,
                            const uint8_t* payload, size_t len) = 0;

    // Drain up to max_count received packets into out[]. Returns the number
    // of packets actually written. Non-blocking.
    virtual int  pollPackets(MeshPacketSimple* out, int max_count) = 0;

    // Approximate count of currently-known mesh neighbours.
    virtual int  getNeighborCount() = 0;

    // Fill out a snapshot of the node DB. Returns false on error; on success
    // *count holds the number of entries written (<= max_nodes).
    virtual bool getNodeList(MeshNodeInfo* nodes, int max_nodes, int* count) = 0;
};

#endif // __cplusplus
