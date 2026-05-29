This project "MatsuMonsterMesh" combines two things:

1. **HowBoyMatsu** — A GameBoy Color emulator for the Tanmatsu handheld console (ESP32-P4 + ESP32-C6), created by Irak4t0n to showcase what the Tanmatsu can do.

2. **MonsterMesh** — A Meshtastic-based Pokemon Gen 1 mesh battle/daycare system, ported from the upstream [GoatsAndMonkeys/monster_mesh](https://github.com/GoatsAndMonkeys/monster_mesh) T-Deck project.

Players use LoRa radio to exchange daycare beacons and battle over the mesh — no internet required. The emulator's live SRAM is the source of truth for party data.

## Current State (Session 12)

- GBC emulation at ~60 FPS with save states, rewind, fast forward
- Bidirectional daycare interop with upstream MonsterMesh on T-Deck verified on hardware
- LoRa TX/RX on MonsterMesh channel (AES-128-CTR, PSK="MonsterMesh!2024", ch=0x25)
- Configurable multi-channel system: up to 8 Meshtastic channels (LongFast + MonsterMesh pre-populated), persisted to NVS
- Channel-aware RX decryption: iterates all registered channel keys (replaces hardcoded fallback)
- Channel-aware TX: encrypts with active channel's PSK and hash
- Terminal commands: ch_list, ch_add, ch_del, ch_set, ch_reset, clear for channel management
- Chat UI side panel showing channel list, active channel info, node count, MQTT status
- Fn+1..8 hotkeys in chat UI to switch active TX channel
- MQTT transport: WiFi auto-connect, TLS to EMQX broker, channel-based routing (MonsterMesh=MQTT, others=LoRa)
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

## Rules

- **Always update README.md, DEVLOG.md, and CLAUDE.md with each push to GitHub.**
