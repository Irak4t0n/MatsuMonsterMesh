# matsumonster_ui — fullscreen terminal that drives the MonsterMesh
# subsystem. Renders into the same pax_buf_t / display HowBoyMatsu's main.c
# already uses; reads input from the same bsp_input event queue.

idf_component_register(
    SRCS
        "MatsuMonsterTerminal.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        monster_core
        meshtastic_radio
        emulator_sram_iface
        badge-bsp                  # bsp/display.h, bsp/input.h
        robotman2412__pax-gfx      # pax_gfx.h, pax_text.h, pax_fonts.h
        esp_timer
        log
)
