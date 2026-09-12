idf_component_register(
    SRCS "button_driver.c"
    INCLUDE_DIRS "."
    REQUIRES driver agent_core log freertos
)
