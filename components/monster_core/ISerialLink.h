#pragma once
// Reference-only interface kept for the future link-cable-over-LoRa work
// (PokemonLinkProxy / MonsterMeshBattleShim). Not wired up in this port.
#include <stdint.h>

class ISerialLink {
public:
    virtual ~ISerialLink() = default;
    virtual void onSerialTx(uint8_t byte) = 0;
    virtual bool onSerialRx(uint8_t &out) = 0;
};
