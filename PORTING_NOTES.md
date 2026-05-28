# Porting Notes — MatsuMonsterMesh

This file tracks open work that needs hardware testing or design input
before it can be closed. None of these block the current `idf.py build`
or the emulator-only path.

## Resolved

### ~~1. Real radio transport~~ ✓ (Session 4)

Live LoRa radio is working via `LoRaMeshtasticRadio` → tanmatsu-lora →
SDIO → C6. TX and RX verified on hardware.

### ~~2. Daycare ↔ radio callbacks~~ ✓ (Session 4)

`setSendDm` / `setBroadcast` / `setSendBeacon` are wired in
`monster_wiring.cpp` and route through the live LoRa radio.

### ~~3. Auto check-in from emulator save~~ ✓ (Session 4)

`daycare.checkIn()` is called on ROM load with the Meshtastic short
name. Party is parsed from SRAM automatically.

### ~~4. C6 LoRa TX timeout~~ ✓ (Session 7)

Renze (Nicolai Electronics) fixed the C6 tanmatsu-radio firmware to
calculate dynamic TX timeouts based on actual packet airtime instead of
the old hardcoded ~1s. Full 122-byte DaycareBeacon (all 6 pokemon +
ngPlusTier) transmits successfully at SF11/BW250 (LongFast).

Requires: tanmatsu-radio >= v2.1.0 (commits e17c642, a66ef5d),
tanmatsu-lora >= v0.1.1, esp-hosted-tanmatsu >= v2.12.3.

### ~~5. T-Deck ↔ Tanmatsu daycare interop~~ ✓ (Session 8)

Bidirectional daycare beacon exchange verified on hardware. Three bugs
fixed:

1. **Portnum varint**: `meshtastic_guess_portnum()` rejected 2-byte
   varints. PRIVATE_APP (portnum 256) encodes as `0x80 0x02`. Added
   multi-byte varint support.

2. **Encryption channel**: Tanmatsu sent daycare beacons unencrypted on
   ch=0x00. T-Deck uses MonsterMesh channel (PSK="MonsterMesh!2024",
   hash=0x25). Added MM key for TX encryption and 3-key RX decryption
   fallback (LongFast → MonsterMesh → plaintext).

3. **Beacon size**: Tanmatsu sent variable-length beacons (e.g. 36 bytes
   for 1 pokemon). T-Deck checks `payload_len >= sizeof(DaycareBeacon)`
   (122 bytes) and silently drops undersized beacons. Fixed by always
   sending the full 122-byte struct. Also added `ngPlusTier` field to
   match upstream `GoatsAndMonkeys/monster_mesh` struct layout.

## Hardware-side TODOs

### ~~1. `Gen1Party` mapping from daycare/SRAM~~ ✓ (Session 4–7)

`buildGen1Party()` does a byte-perfect memcpy of the 44-byte packed
`Gen1Pokemon` structs from SRAM (DVs, stat exp, moves, PP, levels, HP).
`initBattlePokeFromSave()` unpacks DVs, reads stat exp, and calculates
all stats using the correct Gen 1 formula. OT names and nicknames are
decoded from the Gen 1 character set. The placeholder Pikachu fallback
only fires when there's no Gen 1 save at all (e.g. Tetris).

### 2. SRAM bank validity

`gnuboy_sram_init()` points at `ram.sbank`, which is allocated by
`sram_load()`. If a ROM has no battery RAM (e.g. Tetris), `ram.sbank` may
be `NULL` or zero-size.

**Status (this revision):** A NULL/size guard is now in
[`components/emulator_sram_iface/gnuboy_sram.c`](components/emulator_sram_iface/gnuboy_sram.c) —
all eight accessors check `ram.sbank != NULL && mbc.ramsize > 0` and
degrade gracefully (`size()` returns 0, `data()` returns NULL, reads
return 0, writes are no-ops). `DaycareSavPatcher` was already
size-checking via the `IEmulatorSRAM*` overloads; both the raw-pointer
and the iface overloads now also explicitly NULL-guard their arguments,
and the iface overloads enforce a minimum size of
`SAV_CHECKSUM_OFFSET + 1` bytes (the highest offset the patcher writes).

Hardware test still needed: confirm a non-battery ROM (Tetris) boots
and the daycare cleanly stays inactive — no crash, no garbage party.

### ~~3. Display blit race~~ ✓ (Session 7)

Confirmed flicker-free on hardware. `blit_task` drains the
`sem_frame_ready` semaphore but skips the BSP call while in
`MONSTER_STATE_TERMINAL`.
