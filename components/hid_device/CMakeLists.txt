idf_component_register(
    SRCS "hid_device.c" "hid_reports.c" "hid_keymap.c" "hid_test.c" "mouse_test.c"
    INCLUDE_DIRS "."
    REQUIRES esp_tinyusb driver log freertos
)
