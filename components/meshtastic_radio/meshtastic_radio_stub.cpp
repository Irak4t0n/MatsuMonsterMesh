#include "meshtastic_radio_stub.h"
#include "esp_log.h"
#include <inttypes.h>

static const char *TAG = "MMRadio";

bool StubMeshtasticRadio::sendPacket(uint32_t dest, uint8_t channel,
                                     const uint8_t* payload, size_t len)
{
    (void)payload;
    ESP_LOGI(TAG,
             "stub send: dest=0x%08" PRIx32 " ch=%u len=%u",
             (uint32_t)dest, (unsigned)channel, (unsigned)len);
    return true;
}

int StubMeshtasticRadio::pollPackets(MeshPacketSimple* out, int max_count)
{
    (void)out;
    (void)max_count;
    return 0;
}

int StubMeshtasticRadio::getNeighborCount()
{
    return 0;
}

bool StubMeshtasticRadio::getNodeList(MeshNodeInfo* nodes, int max_nodes, int* count)
{
    (void)nodes;
    (void)max_nodes;
    if (count) *count = 0;
    return true;
}
