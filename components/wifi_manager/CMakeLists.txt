idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        log
        # Used directly by the SNTP code:
        esp_timer   # esp_timer_get_time() for "time since last sync"
        lwip        # esp_sntp.h
)
