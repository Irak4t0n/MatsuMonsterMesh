# Porting Notes — MatsuMonsterMesh

This file tracks open work that needs hardware testing or design input
before it can be closed. None of these block the current `idf.py build`
or the emulator-only path.

## Hardware-side TODOs

### 1. Real radio transport

`SerialMeshtasticRadio` is still a logging stub —
[`components/meshtastic_radio/meshtastic_radio_serial.cpp`](components/meshtastic_radio/meshtastic_radio_serial.cpp)
has a block comment laying out the three SDIO/EPPP options. Per project
guidance we'll address this in a later pass once we can iterate on
hardware with the C6 already running Meshtastic.

### 2. Daycare ↔ radio callbacks

`PokemonDaycare::setSendDm` / `setBroadcast` / `setSendBeacon` aren't wired
yet. With the stub radio they'd just log; once the radio is real, register
lambdas (or thunks, since this is C ABI) that route to
`MeshtasticRadio::sendPacket`.

### 3. `Gen1Party` mapping from daycare/SRAM

`cmdFight` and `cmdRun` in the terminal use a placeholder party
(`count=1`, only `species[0]` populated). The full mapping (Gen 1 EV/DV
bytes from SRAM → `Gen1Pokemon` stat fields) needs to be written —
straightforward but tedious. The wire path through `MeshtasticRadio` is
exercised correctly even with the placeholder.

### 4. Auto check-in from emulator save

`daycare.checkIn(sram_iface, shortName, gameName)` is never called from
`main.c` yet — until it is, `getState().partyCount == 0` and `tick()` is
a no-op. A future iteration could hook this off a Pokémon-save-detection
routine in the running ROM.

### 5. SRAM bank validity

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

## Display blit race

Verified safe in code review (`blit_task` drains the `sem_frame_ready`
semaphore but skips the BSP call while in `MONSTER_STATE_TERMINAL`) —
should be confirmed on hardware that Fn+T entry/exit is flicker-free
across at least a few transitions.
