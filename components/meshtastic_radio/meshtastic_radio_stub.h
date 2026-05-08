// StubMeshtasticRadio — no-op MeshtasticRadio for bring-up and testing.
//
// Every send is logged with tag "MMRadio"; receive/list APIs return empty.
// Has zero hardware dependencies, so it always compiles and links.

#pragma once

#include "MeshtasticRadio.h"

class StubMeshtasticRadio : public MeshtasticRadio {
public:
    StubMeshtasticRadio() = default;
    ~StubMeshtasticRadio() override = default;

    bool sendPacket(uint32_t dest, uint8_t channel,
                    const uint8_t* payload, size_t len) override;
    int  pollPackets(MeshPacketSimple* out, int max_count) override;
    int  getNeighborCount() override;
    bool getNodeList(MeshNodeInfo* nodes, int max_nodes, int* count) override;
};
