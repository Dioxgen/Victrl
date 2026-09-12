idf_component_register(
    SRCS "jpeg_utils.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_jpeg
        log
        # Used directly by jpeg_utils.c:
        esp_timer   # esp_timer_get_time() for the per-stage JPEG breakdown
        freertos    # the mutex guarding the cached codec engines
)
