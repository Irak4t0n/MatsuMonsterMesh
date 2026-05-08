idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()


idf_component_register(
	SRCS
		"main.c"
		"menu.c"
		"rom_selector.c"
		"monster_wiring.cpp"
	PRIV_REQUIRES
                esp_mm
		esp_lcd
		fatfs
		nvs_flash
		bt
		badge-bsp
		tanmatsu-wifi
        gnuboy
        # MatsuMonsterMesh additions — Step 6 will wire these into main.c.
        # Listed here so they compile during build even before they're called.
        emulator_sram_iface
        meshtastic_radio
        monster_core
        matsumonster_ui
		${extra_requires}
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
