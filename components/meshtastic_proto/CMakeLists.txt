# meshtastic_proto — pure Meshtastic on-air protocol code. Header parsing,
# channel hashing, AES-CTR encryption, protobuf encode/decode of MeshPacket.
# Sits between the raw-bytes meshtastic_lora component (radio I/O) and the
# high-level meshtastic_radio component (used by the daycare/battle code).

idf_component_register(
    SRCS
        "src/meshtastic_proto.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        meshtastic_lora     # pop_raw + ML_RAW_MAX_LEN + send_raw
        freertos            # xTaskCreate, queues, semaphores
        log
        esp_common
        esp_hw_support      # esp_read_mac for node id derivation
        esp_system          # esp_random() for fresh packet ids
        esp_timer           # esp_timer_get_time for rx timestamps
        mbedtls             # AES-128-CTR for Meshtastic payload crypt
)
