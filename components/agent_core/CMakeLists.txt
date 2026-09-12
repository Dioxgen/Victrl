idf_component_register(
    SRCS "agent_core.c" "action_executor.c"
    INCLUDE_DIRS "."
    REQUIRES cloud_client hid_device uvc_capture_card_driver storage_manager freertos esp_timer
)
