idf_component_register(
    SRCS "NV3007_driver.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_driver_spi esp_driver_gpio log freertos)
