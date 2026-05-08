# monster_core — game logic ported from MonsterMesh (GPL-3.0).
#
# Headless C++ component: daycare, Gen 1 battle engine, LORD save, BattleLink
# wire codec. Mesh I/O is reached through MeshtasticRadio (a forward-declared
# interface in components/meshtastic_radio); cart-RAM access goes through
# IEmulatorSRAM (components/emulator_sram_iface).
#
# Headers-only files (BattlePacket, DaycareTypes/Data/Achievements/SavPatcher,
# Gen1Species, PokemonData, ISerialLink, MeshtasticTransport, all showdown_*)
# don't appear in SRCS — they're consumed via INCLUDE_DIRS.

idf_component_register(
    SRCS
        "PokemonDaycare.cpp"
        "DaycareEventGen.cpp"
        "Gen1BattleEngine.cpp"
        "MonsterMeshTextBattle.cpp"
        "LordGyms.cpp"
        "LordLogic.cpp"
        "LordSave.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        meshtastic_radio
        emulator_sram_iface
        esp_timer
        esp_hw_support       # esp_random()
)

# The upstream MonsterMesh code uses `memset(&s, 0, sizeof(s))` on a few C++
# structs that have default member initializers (e.g. Gen1BattleEngine's
# BattlePoke / BattleParty). GCC 14+ flags those with -Werror=class-memaccess.
# Behaviour is well-defined here (members are POD-like), so suppress the
# warning for this component instead of rewriting upstream code.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=class-memaccess)

