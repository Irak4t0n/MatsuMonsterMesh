# MatsuMonsterMesh

**MonsterMesh ported onto HowBoyMatsu, running on the Tanmatsu handheld.**

A portable Game Boy Color emulator + Pokémon Gen 1 mesh network, no
internet required. Battle, trade, and run a daycare with whoever else is
on LoRa within range — your save file is the source of truth, the radio
carries the rest.

> **Status:** v0.1.0 — initial port complete and `idf.py build` is green,
> but hardware testing is still pending. Things are likely to break.
> See [PORTING_NOTES.md](PORTING_NOTES.md) for the open work list.

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
  achievements, XP write-back to the running ROM's SRAM
- LORD save / Kanto gym roster data (compiled in, gameplay loop pending)
- BattleLink wire protocol over Meshtastic channel 1
- Fullscreen terminal UI rendered on top of the emulator (Fn+T to toggle)

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
| [`components/meshtastic_radio/`](components/meshtastic_radio/) | `MeshtasticRadio` interface plus two impls: `StubMeshtasticRadio` (logs sends, returns no packets) and `SerialMeshtasticRadio` (still a stub — see [PORTING_NOTES.md](PORTING_NOTES.md)). `CONFIG_MATSUMONSTER_MESH_STUB` picks between them. |
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

**Active development.** The port compiles, the state machine wiring is
in place, and the stub radio path is exercised end-to-end in code review.
Nothing in this repo has been flashed to a Tanmatsu yet — the open
hardware-validation items are tracked in
[PORTING_NOTES.md](PORTING_NOTES.md), in roughly the order you'd want
to test them.

Things that work today:

- `idf.py build` for `DEVICE=tanmatsu` (binary ~1.0 MB / 1013 KB)
- The original GBC emulator path is preserved bit-for-bit
- Fn+T toggles into the terminal; `party`, `status`, `fight`, `run`,
  `quit`, and `help` commands are wired
- The stub radio logs every send via `ESP_LOGI(MMRadio, …)`

Things that don't work yet:

- The C6 serial transport is a stub (the Tanmatsu's P4↔C6 bus is SDIO,
  not UART; the implementation path is documented but unwritten)
- Daycare check-in is not auto-triggered from a loaded save — the
  terminal will show an empty party until `daycare.checkIn(...)` is
  called from somewhere
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
