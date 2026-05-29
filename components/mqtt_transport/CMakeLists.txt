# mqtt_transport — WiFi + MQTT bridge for Meshtastic MonsterMesh.
# Connects to an MQTT broker and bridges Meshtastic ServiceEnvelope
# packets alongside the LoRa radio path.

idf_component_register(
    SRCS
        "src/mqtt_transport.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        meshtastic_lora       # push_raw for RX injection
        meshtastic_proto      # node_id, header constants
        mqtt                  # esp_mqtt_client
        esp_wifi              # WiFi remote API
        esp_netif             # TCP/IP stack
        esp_event             # event loop
        mbedtls               # esp_crt_bundle for TLS verification
        freertos
        log
        esp_common
        esp_hw_support        # esp_read_mac
)
