idf_component_register(
    SRCS "display_task.c"
    INCLUDE_DIRS "."
    REQUIRES NV3007_driver agent_core storage_manager wifi_manager log freertos
)
