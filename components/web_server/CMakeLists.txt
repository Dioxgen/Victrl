idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_server
        log
        agent_core
        storage_manager
        wifi_manager
        # Used directly by the SD file API and /api/status:
        esp_timer       # esp_timer_get_time() (uptime, time-since-sync)
        fatfs           # esp_vfs_fat_info(), esp_vfs_fat.h
        heap            # heap_caps_get_free_size() / largest block
        freertos        # vTaskDelay() in the reboot handler
        # /api/snapshot pulls a live frame and serves the last image the model
        # was shown:
        uvc_capture_card_driver
        cloud_client
)
