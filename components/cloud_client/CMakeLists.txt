idf_component_register(
    SRCS "base64.c" "system_prompts.c" "json_parser.c" "cloud_client.c"
    INCLUDE_DIRS "."
    REQUIRES
        mbedtls
        esp_http_client
        esp-tls
        sdmmc_driver
        espressif__cjson
        jpeg_utils
        storage_manager
        # Used directly by cloud_client.c:
        esp_timer   # esp_timer_get_time() for the per-phase latency breakdown
)
