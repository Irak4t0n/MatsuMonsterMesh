# Session 14 build & verify instructions for Claude Code

Read REVIEW_2026-07-15.md. Session 14 changes are already applied and committed locally (commit 81b1f59). Do the following:

1. Build with:
   `idf.py -B build/tanmatsu -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0 build`
2. Fix any compile errors in the Session 14 files only (DaycareTypes.h, PokemonDaycare.*, meshtastic_proto.c, mqtt_transport.c, monster_wiring.cpp, meshtastic_radio_lora.cpp). Commit any fixes.
3. When it builds clean, flash and monitor:
   `idf.py -p COM5 -B build/tanmatsu flash monitor` (confirm my COM port first).
4. Verify in the monitor log: beacon TX shows 123 bytes with rr=1 on boot, MQTT connects with the built-in credentials (no NVS config needed — the defaults are intentional), and no crash in mp_drain.
5. After I confirm the hardware test, push to origin main.
