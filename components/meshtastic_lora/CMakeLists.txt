# meshtastic_lora — thin P4-side wrapper over the tanmatsu-lora component
# (which fronts the C6's SX1262 over SDIO-RPC). Session 1 = raw packet I/O
# only; the Meshtastic protocol layer (encryption, routing, NodeDB) gets
# built on top in later sessions.

idf_component_register(
    SRCS
        "src/meshtastic_lora.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        nicolaielectronics__tanmatsu-lora   # lora_init/set_config/send/recv
        nicolaielectronics__esp-hosted-tanmatsu  # esp_hosted_custom.h + esp_hosted_connect_to_slave
        badgeteam__badge-bsp                # bsp/power.h — power-cycle the C6
        freertos                            # xQueue, xTaskCreate
        log
        esp_common
)
