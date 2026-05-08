# MeshtasticRadio — abstract interface plus two concrete impls.
#
# Both StubMeshtasticRadio and SerialMeshtasticRadio always compile.
# CONFIG_MATSUMONSTER_MESH_STUB (defined in this component's Kconfig) selects
# which one main.c instantiates at runtime — see Step 6 wiring.
#
# `driver` is REQUIRED for the (currently-stubbed) uart APIs the serial
# transport will eventually use; `log` for ESP_LOGI/W.

idf_component_register(
    SRCS
        "meshtastic_radio_stub.cpp"
        "meshtastic_radio_serial.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver       # uart_driver_install (referenced in serial impl, currently #if 0'd)
        log
)
