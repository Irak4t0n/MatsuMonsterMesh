// SerialMeshtasticRadio — intended to talk to the ESP32-C6 radio coprocessor.
//
// CURRENT STATUS (Step 4): stub bodies only — see meshtastic_radio_serial.cpp
// for the block comment explaining why uart_driver_install is NOT called.
// All methods log a warning and behave like StubMeshtasticRadio.
//
// The class still exists (and the Kconfig switch still picks it) so future
// iterations can fill in the real transport without changing the call sites
// in main.c or monster_core.

#pragma once

#include "MeshtasticRadio.h"

class SerialMeshtasticRadio : public MeshtasticRadio {
public:
    SerialMeshtasticRadio();
    ~SerialMeshtasticRadio() override;

    // Lifecycle. Currently a no-op that logs the open questions; eventual
    // real impl will install the SDIO/UART driver and start a receive task.
    bool begin();
    void end();

    bool sendPacket(uint32_t dest, uint8_t channel,
                    const uint8_t* payload, size_t len) override;
    int  pollPackets(MeshPacketSimple* out, int max_count) override;
    int  getNeighborCount() override;
    bool getNodeList(MeshNodeInfo* nodes, int max_nodes, int* count) override;

private:
    bool initialised_ = false;
};
