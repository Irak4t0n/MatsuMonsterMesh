This project "MatsuMonsterMesh" combines two things:

1. **HowBoyMatsu** — A GameBoy Color emulator for the Tanmatsu handheld console (ESP32-P4 + ESP32-C6), created by Irak4t0n to showcase what the Tanmatsu can do.

2. **MonsterMesh** — A Meshtastic-based Pokemon Gen 1 mesh battle/daycare system, ported from the upstream [GoatsAndMonkeys/monster_mesh](https://github.com/GoatsAndMonkeys/monster_mesh) T-Deck project.

Players use LoRa radio to exchange daycare beacons and battle over the mesh — no internet required. The emulator's live SRAM is the source of truth for party data.

## Current State (Session 15)

Session 15 (Jul 15-16 2026) — MQTT fix, PvP, visual battle station:
- MQTT broker updated to `mqtts://mqtt.cableclub.net:8883` (upstream Default.h)
- `hb` (hollaback) terminal command added
- `pvp <name>` command: server-auth PvP protocol (0x66-0x6C) for T-Deck interop
- Gen1BattleEngine synced with upstream (Gen 3 mechanics, full move effects,
  Substitute/Bide/Mimic/Transform, matching hashState for PvP)
- Gen 3 data files: showdown_gen3_basestats/moves/typechart.h
- Gen 3 sprite data: Gen3Front565.h (88x88), Gen3Back565.h (80x80)
- **Visual battle station**: full-screen Gen 1 staircase layout with 2x scaled
  sprites, white background, HP bars, pokeball-bordered text box, WASD+K/L input
- Species handling: all readers normalize to dex numbers, engine skips internalToDex
- Catch system: C key during wild battles, Gen 1 SRAM + Gen 2 WRAM support
- Known upstream gaps: Gauntlet/mmg (0x70-0x76), Dungeon (0x80-0x86)

## Previous State (Session 14b)

Session 14b (Jul 15 2026) — battle UX fixes for Gen 2 / Crystal:
- DaycareBeacon is now 123 bytes: added `requestResponse` (hollaback) after
  `ngPlusTier`, matching current upstream; RX on both sides tolerates 122
- `broadcastBeacon` fills `ngPlusTier` from `lordCurrentNgPlusTier()` (was
  always 0 on the wire) and sets `requestResponse=1` on boot/manual beacons;
  incoming rr=1 beacons get a rr=0 reply beacon
- AES-256 channels fixed: `aes_ctr_crypt` picks 128/256-bit keys from psk_len
  (was hardcoded 128, truncating 32-byte PSKs); ch_add rejects non-0/16/32 PSKs
- Channel registry (`s_channels`/`s_tx_channel`) now mutex-guarded against
  terminal-vs-drain-task races; TX snapshots the channel under the lock
- MQTT credentials read from NVS (`mm_mqtt`: broker/user/pass) with source
  fallback — ROTATE the old leaked EMQX password
- Beacon interval 30s debug → 5 min; drain_task stack 4K → 6K
- RX hex-dump/per-mon logging gated behind MM_WIRE_VERBOSE_RX (default 0)
- Wild encounter foe panel now shows species name (was "WILD")
- Gen 2 move fallback: if all 4 moves are Gen 2-only (>165), Struggle assigned
- WAIT_SWITCH phase now renders a visible party list with cursor in the
  battle panel (was invisible, eating all input)
- XP payout: uses engine party data (not daycare), shares among participants,
  excludes fainted; explicit participation bitmask tracks switches
- Gen 2 WRAM XP patching: writes EXP + level directly to live WRAM for
  Crystal/Gold/Silver (Gen 1 still patches SRAM as before)
- Gen 2 growth rate table (dex 152-251) for expForLevel/levelForExp
- Party panel shows XP current/next-level (e.g. `XP 400/525`)

## Previous State (Session 13b)

- GBC emulation at ~60 FPS with save states, rewind, fast forward
- Bidirectional daycare interop with upstream MonsterMesh on T-Deck verified on hardware
- LoRa TX/RX on MonsterMesh channel (AES-128-CTR, PSK="MonsterMesh!2024", ch=0x25)
- Configurable multi-channel system: up to 8 Meshtastic channels (LongFast + MonsterMesh pre-populated), persisted to NVS
- Channel-aware RX decryption: iterates all registered channel keys (replaces hardcoded fallback)
- Channel-aware TX: encrypts with active channel's PSK and hash
- Terminal commands: ch_list, ch_add, ch_del, ch_set, ch_reset, clear for channel management
- Short command aliases: `st`, `f`, `q`, `ls`, `lr`, `li`, `lp`, `lt`, `mr`, `mm`, `ms`, `ma`, `mn`, `db`, `mq`, `cl`, `ca`, `cd`, `cs`, `cr`
- Chat UI side panel showing channel list, active channel info, node count, MQTT status
- Fn+1..8 hotkeys in chat UI to switch active TX channel
- MQTT transport: WiFi auto-connect, TLS to EMQX broker, channel-based routing (MonsterMesh=MQTT only, no LoRa fallback)
- MQTT RX subscription broadened to `kanto/2/e/#` to catch beacons regardless of channel name
- MQTT RX: PKI topics skipped, channel hash taken from MeshPacket (not hardcoded)
- MQTT TX callback returns bool; MonsterMesh channel drops packet if MQTT unavailable (no LoRa leak)
- MQTT ServiceEnvelope encoding with correct Meshtastic proto field numbers (verified against official mesh.proto)
- MQTT RX: ServiceEnvelope decoding, hop_limit=0 injection to prevent LoRa relay of MQTT packets
- mqtt_status terminal command for on-device MQTT/WiFi diagnostics
- Channel presets: `ch_add <name> default` uses standard Meshtastic PSK
- Unread message badge in emulator overlay (top-right `[N] Fn+M`)
- Chat message wrapping: long messages wrap to multiple visual lines
- Full 122-byte DaycareBeacon TX matching upstream struct (including ngPlusTier field)
- Meshtastic chat UI (Fn+M) and terminal (Fn+T) both functional
- Fn+M from terminal jumps directly to chat; Fn+T from chat jumps to terminal
- NodeDB persisted to NVS across reboots; also populated from DaycareBeacon short names
- Daycare auto-check-in from live emulator SRAM on ROM load
- Gen 2 (Crystal/Gold/Silver) support: live WRAM party reading, daycare check-in, battle with move filtering
- Terminal + chat input lines scroll horizontally when text overflows the left panel
- Terminal fully pax-free: header, input line, side panel, battle panel all use FastText
- FastText `fast_hline()` / `fast_rect()` primitives for separators and cursors
- Fight command: local mirror match vs CPU copy of neighbor's party (was networked)
- Battle intro text: "A trainer battle begins!" for fight/gym/e4, "A wild battle begins!" for run
- All terminal strings use ASCII only (no Unicode em dashes — glyph cache is ASCII 32-127)
- **Legend of Charizard (LORD)**: full gym + Elite Four + Champion campaign
  - 8 Kanto gyms (4 grunts + leader each), linear badge-gated progression
  - Elite Four + Champion (Lorelei, Bruno, Agatha, Lance, Blue) — requires all 8 badges
  - NG+ tiers (1-5): defeating Champion unlocks harder gym/E4 levels + coverage moves
  - 8 badge-gated wild encounter routes (Viridian Forest through Cerulean Cave)
  - `run` command uses route-based encounters matching badge count
  - `gym`, `e4`, `lord` terminal commands for full campaign gameplay
  - LORD save persisted to /monstermesh/lord.dat (badges, progress, NG+ tier, run stats)

## Rules

- **Always update README.md, DEVLOG.md, and CLAUDE.md with each push to GitHub.**
