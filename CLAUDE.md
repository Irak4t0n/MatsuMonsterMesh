This project "MatsuMonsterMesh" combines two things:

1. **HowBoyMatsu** — A GameBoy Color emulator for the Tanmatsu handheld console (ESP32-P4 + ESP32-C6), created by Irak4t0n to showcase what the Tanmatsu can do.

2. **MonsterMesh** — A Meshtastic-based Pokemon Gen 1 mesh battle/daycare system, ported from the upstream [GoatsAndMonkeys/monster_mesh](https://github.com/GoatsAndMonkeys/monster_mesh) T-Deck project.

Players use LoRa radio to exchange daycare beacons and battle over the mesh — no internet required. The emulator's live SRAM is the source of truth for party data.

## Current State (Session 10)

- GBC emulation at ~60 FPS with save states, rewind, fast forward
- Bidirectional daycare interop with upstream MonsterMesh on T-Deck verified on hardware
- LoRa TX/RX on MonsterMesh channel (AES-128-CTR, PSK="MonsterMesh!2024", ch=0x25)
- Multi-key RX decryption: LongFast → MonsterMesh → plaintext fallback
- Full 122-byte DaycareBeacon TX matching upstream struct (including ngPlusTier field)
- Meshtastic chat UI (Alt+M) and terminal (Fn+T) both functional
- Daycare auto-check-in from live emulator SRAM on ROM load
- Gen 2 (Crystal/Gold/Silver) support: live WRAM party reading, daycare check-in, battle with move filtering
- Terminal scrollback: direct-to-framebuffer glyph cache bypasses pax CW rotation (~640ms → <5ms per frame)
- Bottom-anchored scrollback layout: text fills up to the input line with no gap

## Rules

- **Always update README.md, DEVLOG.md, and CLAUDE.md with each push to GitHub.**
