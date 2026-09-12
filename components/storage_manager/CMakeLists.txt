idf_component_register(
    SRCS "session_manager.c" "config_loader.c" "profile_manager.c" "plan_manager.c" "short_term_mem.c" "task_log.c"
    INCLUDE_DIRS "."
    REQUIRES sdmmc_driver espressif__cjson
)
