# MatsuMonsterMesh

**MonsterMesh ported onto HowBoyMatsu, running on the Tanmatsu handheld.**

A portable Game Boy Color emulator + Pokémon Gen 1 mesh network, no
internet required. Battle, trade, and run a daycare with whoever else is
on LoRa within range — your save file is the source of truth, the radio
carries the rest.

> **Status:** active development — LoRa radio is live (RX working,
> TX for small packets working, large-packet TX under investigation),
> Meshtastic protocol stack and chat UI are functional, daycare system
> is wired to the real radio. See [PORTING_NOTES.md](PORTING_NOTES.md)
> for the open work list.

---

## What it is

The Tanmatsu is an ESP32-P4 + ESP32-C6 handheld with a QWERTY keyboard,
MIPI-DSI display, SD card, and a LoRa radio that runs Meshtastic firmware.
This project takes two existing pieces of work and fuses them:

- **HowBoyMatsu** — Emulator created by myself (Irak4t0n) for the
  Tanmatsu/Konsool, itself derived from
  [gnuboy](https://github.com/rofl0r/gnuboy)
- **MonsterMesh** — a Meshtastic module that turns a Game Boy into a mesh
  battle/trade/daycare device, originally built for the LilyGo T-Deck —
  upstream at [GoatsAndMonkeys/monster_mesh](https://github.com/GoatsAndMonkeys/monster_mesh)

The MonsterMesh game logic was ported headlessly into a set of ESP-IDF
components, decoupled from the Meshtastic firmware (which it can no
longer be a direct module of, since the C6 is a separate chip). Mesh I/O
goes through a `MeshtasticRadio` interface; cart RAM goes through an
`IEmulatorSRAM` interface. The emulator stays untouched.

You hit **Fn+T** during emulation to drop into the MonsterMesh terminal,
where commands like `party`, `status`, `fight <name>`, and `run` drive
the daycare and battle systems against other Tanmatsu users on the mesh.

## Hardware

This builds for the **Tanmatsu** handheld (a.k.a. Konsool):

- **SoC**: ESP32-P4 (RISC-V, dual-core), with PSRAM enabled at 200 MHz.
  PSRAM size depends on board revision — see Nicolai Electronics' product
  page for definitive specs.
- **Radio**: ESP32-C6 coprocessor, expected to be running Meshtastic
  firmware (flashed via the Tanmatsu recovery tool). Communicates with
  the P4 over a 4-bit SDIO bus — see [PORTING_NOTES.md](PORTING_NOTES.md)
  for the radio transport status.
- **Display**: ST7701-driven MIPI-DSI LCD, 480×800 native portrait;
  HowBoyMatsu (and this firmware) rotate 90° CW to render in 800×480
  landscape.
- **Input**: full QWERTY keyboard + D-pad + function keys (Fn modifier
  used for app-level combos like Fn+T)
- **Storage**: microSD slot (4-bit SDIO) for ROMs and save files
- **Flash**: 16 MB on-board (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`)

See [tanmatsu_hardware.h](managed_components/badgeteam__badge-bsp/targets/tanmatsu/tanmatsu_hardware.h)
in the badge-bsp managed component for the full pin map.

## Features

### Emulator (inherited from HowBoyMatsu)
- ~60 FPS Game Boy / Game Boy Color emulation, dual-core pipeline
- Stretch / aspect-fit / 3× integer scale modes (F3)
- 10 save state slots per ROM (F4), with rewind (F5) and 5×/8× fast forward (F6)
- SRAM autosave + RTC for Pokémon Gold/Silver/Crystal
- Default and WASD button layouts (F2)

### MonsterMesh layer (this port)
- Headless Gen 1 battle engine with deterministic dual-side execution
  over a `MeshtasticRadio` abstraction
- Daycare: party check-in/out, hourly events, friendship/rivalry,
  achievements, XP write-back to the running ROM's SRAM — wired to
  the live LoRa radio with auto-beacon on ROM load
- LORD save / Kanto gym roster data (compiled in, gameplay loop pending)
- BattleLink wire protocol over Meshtastic PRIVATE_APP portnum
- Fullscreen terminal UI rendered on top of the emulator (Fn+T to toggle)

### Meshtastic integration
- **LoRa radio**: live on US 907.125 MHz LongFast via the C6
  coprocessor's tanmatsu-radio firmware — RX confirmed working,
  TX works for small packets (NodeInfo, text), large packets
  (daycare beacons) under investigation
- **Protocol stack**: 16-byte Meshtastic header parsing, NodeDB,
  channel decryption (default key), TEXT_MESSAGE_APP + PRIVATE_APP
  portnum routing
- **Chat UI** (Alt+M): full Meshtastic chat view with compose bar,
  message history, and in-emulator notification overlay
- **Daycare ↔ mesh**: beacon TX/RX, broadcast text, DM callbacks
  all wired through the real radio — cross-device compatible with
  upstream MonsterMesh on T-Deck

### Launcher integration
Designed to install as a regular app on the Tanmatsu launcher via
[badgelink](https://github.com/badgeteam/esp32-component-badgelink).
The Makefile has `make install` and `make run` targets for this — see
[Building](#building) below.

## Building

You need ESP-IDF v5.5.1 or newer. The repo follows the badgeteam
multi-board layout, so the recommended build path is the top-level
Makefile:

```bash
make DEVICE=tanmatsu          # build into build/tanmatsu/
make install DEVICE=tanmatsu  # install via badgelink (USB)
make run DEVICE=tanmatsu      # launch the app on the device
```

Equivalent direct `idf.py` invocation if you don't want to use the
Makefile:

```bash
idf.py -B build/tanmatsu \
       -DDEVICE=tanmatsu \
       -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" \
       -DSDKCONFIG=sdkconfig_tanmatsu \
       -DIDF_TARGET=esp32p4 \
       -DFAT=0 \
       build
```

The build target uses partition table [`partition_tables/16M.csv`](partition_tables/16M.csv)
(2 MB app partition, 8 MB AppFS, 4 MB FAT). At v0.1.0 the firmware is
**~1.0 MB (1013 KB)** — about half the app partition is free.

Other supported `DEVICE=` targets (inherited from the badgeteam template,
not currently exercised by this port): `konsool`, `esp32-p4-function-ev-board`,
`mch2022`, `kami`, `hackerhotel-2024`. Per-device configs live in
[`sdkconfigs/`](sdkconfigs/).

## Flashing

Direct ESP-IDF flash (when developing — overwrites the launcher):

```bash
idf.py -p COM5 -B build/tanmatsu flash monitor
```

Replace `COM5` with the serial port of your Tanmatsu (`/dev/ttyACM0` on
Linux/macOS).

Production-style install via the launcher (keeps the launcher in place,
installs MatsuMonsterMesh as a launcher app):

```bash
make badgelink   # one-time: clones the badgelink helper tools
make install     # builds + uploads the .bin into AppFS
make run         # tells the launcher to start it
```

## Project layout

| Path | What it is |
|---|---|
| [`main/`](main/) | App entry. `main.c` keeps the unmodified HowBoyMatsu emulator loop; `monster_wiring.cpp` is a small C-ABI bridge that lets `main.c` reach the C++ MonsterMesh subsystems and drives the `EMULATOR ↔ TERMINAL` state machine. |
| [`components/gnuboy/`](components/gnuboy/) | The gnuboy emulator core, **untouched** — any new emulator-facing functionality is added via wrappers around it, never inside it. |
| [`components/emulator_sram_iface/`](components/emulator_sram_iface/) | `IEmulatorSRAM` — generic interface to a Game Boy cart's battery RAM. The `gnuboy_sram` adapter points it at gnuboy's live `ram.sbank`. Future emulators implement the same interface to plug in. |
| [`components/meshtastic_radio/`](components/meshtastic_radio/) | `MeshtasticRadio` interface plus `LoRaMeshtasticRadio` (live LoRa via the C6) and `StubMeshtasticRadio` (logs sends, returns no packets). The live impl routes PRIVATE_APP packets through the Meshtastic protocol stack. |
| [`components/monster_core/`](components/monster_core/) | Headless game logic ported from upstream MonsterMesh: Gen 1 battle engine, daycare, LORD save / gym data, BattleLink wire codec. All rendering and Meshtastic-firmware coupling has been stripped — mesh I/O goes through `MeshtasticRadio`, cart RAM through `IEmulatorSRAM`. |
| [`components/matsumonster_ui/`](components/matsumonster_ui/) | `MatsuMonsterTerminal` — fullscreen text terminal that drives the MonsterMesh subsystems via PAX graphics + the existing BSP input queue. Activated with **Fn+T**, exits with **Fn+T** or ESC. |
| [`partition_tables/`](partition_tables/) | Partition layout CSVs. `16M.csv` is the one used for Tanmatsu builds. |
| [`sdkconfigs/`](sdkconfigs/) | Per-device default sdkconfig fragments. Tanmatsu builds layer `general` + `tanmatsu`. |
| [`badgelink/`](https://github.com/badgeteam/esp32-component-badgelink) | Cloned by `make badgelink` (not committed). Provides the host-side tools that talk to the launcher's USB protocol for `make install` / `make run`. |
| [`Makefile`](Makefile) | Wrapper around `idf.py` plus `install` / `run` / `prepare` targets. |
| [`metadata.json`](metadata.json) | App metadata consumed by the launcher (icons, name, author, AppFS slug). |
| [`PORTING_NOTES.md`](PORTING_NOTES.md) | Open hardware-test items and design follow-ups. |

`build/`, `managed_components/`, `sdkconfig`, `sdkconfig_*`, and
`dependencies.lock` are gitignored — they're regenerated on the first
build by ESP-IDF's component manager and the `idf.py reconfigure` step.
The `_refs_monster_mesh/` directory (a local clone of the upstream
MonsterMesh repo for cross-reference) is also gitignored.

## Status

**Active development.** The port is running on real hardware with LoRa
radio active. The emulator, terminal, chat UI, and daycare system are
all functional. Cross-device mesh communication with upstream
MonsterMesh (T-Deck Plus) is partially working.

Things that work today:

- GBC emulation at ~60 FPS, all emulator features (save states, rewind,
  fast forward, ROM selector)
- Fn+T terminal with `party`, `status`, `fight`, `daycare_beacon`,
  `mesh_recent`, `lora_stats`, and more
- Alt+M Meshtastic chat UI with compose bar and message history
- In-emulator notification overlay for incoming mesh messages
- LoRa RX: receives Meshtastic packets (NodeInfo, text, PRIVATE_APP)
- LoRa TX: sends small packets (text messages, NodeInfo) successfully
- Daycare auto-check-in on ROM load with real Meshtastic short name
- Daycare beacon RX: recognises neighbours running upstream MonsterMesh
- Packet dispatch: PRIVATE_APP packets routed to daycare (beacons) or
  battle engine by type byte + size disambiguation

Things that don't work yet:

- **LoRa TX for large packets**: daycare beacons (~100+ bytes on-air)
  fail at the C6's tanmatsu-radio firmware level — small packets go
  through fine, suggesting a payload size limit in the SDIO TX path.
  This is the main blocker for full daycare interop.
- The `Gen1Party` mapping out of SRAM into the battle engine uses a
  placeholder party (correct species, default everything else)

## Credits

- **HowBoyMatsu** — Emulator created by myself (Irak4t0n) for the
  Tanmatsu/Konsool.
- **MonsterMesh** by [GoatsAndMonkeys](https://github.com/GoatsAndMonkeys/monster_mesh) —
  the Pokémon Gen 1 battle / daycare / mesh-protocol code, ported into
  `components/monster_core/`. GPL-3.0.
- **gnuboy** — Game Boy emulator core, preserved from HowBoyMatsu.
  Originally by Laguna, maintained as a fork at
  [rofl0r/gnuboy](https://github.com/rofl0r/gnuboy). GPL-2.0+.
- **Tanmatsu hardware** by [Nicolai Electronics](https://github.com/Nicolai-Electronics) —
  the device this runs on.
- **PAX Graphics** by [robotman2412](https://github.com/robotman2412/pax-graphics) —
  the 2D framebuffer library used for everything but the GBC raster.
- **badge-bsp** + **badgelink** + **tanmatsu-coprocessor** by
  [Badge.Team](https://github.com/badgeteam) — board support and
  launcher protocol.
- **Meshtastic** ([meshtastic.org](https://meshtastic.org)) — the mesh
  network the C6 is expected to be running.
- **Pokémon Showdown** for Gen 1/2 battle data tables consumed by
  `components/monster_core/`.
- **Pokémon** is © Nintendo / Game Freak / Creatures. This project does
  not ship any Game Boy ROMs or original Pokémon assets — bring your own
  legally-dumped cartridge.

## License

GPL-3.0-or-later, inherited from upstream MonsterMesh. See [LICENSE](LICENSE).

The gnuboy component is GPL-2.0-or-later (compatible). HowBoyMatsu's
original codebase was MIT, which is also GPL-3.0-compatible.
