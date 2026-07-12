idf_component_register(
    SRCS "../../esp32-csi-node/main/main.c"
    INCLUDE_DIRS
        "."
        "../../esp32-csi-node/main"
    REQUIRES
        esp_event
        esp_driver_gpio
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        cjson
        nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP32_INTRUDER_BOARD_VARIANT=\"ESP32-S3-WROOM-1U\"
    ESP32_INTRUDER_HARDWARE_PROFILE=\"s3-enhanced\"
    ESP32_INTRUDER_S3_WROOM_1U=1
    ESP32_INTRUDER_CSI_QUEUE_LEN=512
    ESP32_INTRUDER_MAX_IDLE_RATE_HZ=160
    ESP32_INTRUDER_MAX_BOOST_RATE_HZ=400
    ESP32_INTRUDER_DEFAULT_BOOST_RATE_HZ=120
    ESP32_INTRUDER_CSI_AGGREGATION_STACK=6144
    ESP32_INTRUDER_PI_TELEMETRY_STACK=6144
)
