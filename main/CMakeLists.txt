idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        agent_core
        button_driver
        cloud_client
        display_task
        hid_device
        sdmmc_driver
        storage_manager
        web_server
        wifi_manager
        uvc_capture_card_driver
        nvs_flash
        freertos
)
