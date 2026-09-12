/*
 * SDMMC Driver - Reusable SD card library
 *
 * Extracted from ESP-IDF SDMMC example. Provides a simplified API for
 * initializing, mounting, reading, and writing SD cards via SDMMC interface.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration structure for SDMMC driver initialization
 */
typedef struct {
    int clk_pin;                /**< CLK GPIO number */
    int cmd_pin;                /**< CMD GPIO number */
    int d0_pin;                 /**< D0 GPIO number */
    int d1_pin;                 /**< D1 GPIO number (set to -1 if not used) */
    int d2_pin;                 /**< D2 GPIO number (set to -1 if not used) */
    int d3_pin;                 /**< D3 GPIO number (set to -1 if not used) */
    int bus_width;              /**< Bus width: 1 or 4 */
    bool format_if_mount_failed; /**< Format the card if mount fails */
    const char *mount_point;    /**< Mount point path (e.g., "/sdcard") */
    int max_files;              /**< Max number of open files */
    int allocation_unit_size;   /**< Allocation unit size in bytes */
    int max_freq_khz;           /**< Max SDMMC frequency in kHz (0 for default 20MHz) */
    bool pwr_ctrl_ldo_enable;   /**< Internal LDO for SD slot (ESP32-P4-WIFI6-DEV-KIT only) */
    int  pwr_ctrl_ldo_chan_id;  /**< LDO channel (default 4, DEV-KIT only) */
} sdmmc_driver_config_t;

/**
 * @brief Initialize SD card and mount FAT filesystem
 *
 * @param config  Pointer to driver configuration
 * @param out_card  Output pointer to the SD card handle
 * @return ESP_OK on success, otherwise error code
 */
esp_err_t sdmmc_driver_init(const sdmmc_driver_config_t *config, sdmmc_card_t **out_card);

/**
 * @brief Unmount filesystem and release SD card resources
 *
 * @param mount_point  Mount point to unmount
 * @param card  SD card handle from sdmmc_driver_init()
 * @return ESP_OK on success, otherwise error code
 */
esp_err_t sdmmc_driver_deinit(const char *mount_point, sdmmc_card_t *card);

/**
 * @brief Write data to a file on the SD card
 *
 * @param path  Full path to the file (e.g., "/sdcard/test.txt")
 * @param data  Null-terminated string data to write
 * @return ESP_OK on success, otherwise error code
 */
esp_err_t sdmmc_driver_write_file(const char *path, const char *data);

/**
 * @brief Write binary data to a file on the SD card
 *
 * @param path  Full path to the file (e.g., "/sdcard/frame.jpg")
 * @param data  Pointer to binary data buffer
 * @param len   Number of bytes to write
 * @return ESP_OK on success, otherwise error code
 */
esp_err_t sdmmc_driver_write_binary_file(const char *path, const uint8_t *data, size_t len);

/**
 * @brief Read data from a file on the SD card
 *
 * @param path  Full path to the file
 * @param buffer  Output buffer for the file contents
 * @param buf_size  Size of the output buffer
 * @return ESP_OK on success, otherwise error code
 */
esp_err_t sdmmc_driver_read_file(const char *path, char *buffer, size_t buf_size);

/**
 * @brief Get default configuration
 *
 * Fills the config struct with default pin assignments and settings
 * suitable for the detected chip target.
 *
 * @param config  Pointer to config struct to fill
 */
void sdmmc_driver_get_default_config(sdmmc_driver_config_t *config);

/**
 * @brief Run a continuous write/read speed benchmark
 *
 * Writes a large file in chunks, then reads it back, measuring elapsed
 * time to compute throughput in KB/s.
 *
 * @param mount_point   SD card mount point (e.g., "/sdcard")
 * @param file_path     Path to the test file (e.g., "/sdcard/speedtest.bin")
 * @param file_size_kb  Size of the test file in kilobytes
 * @param chunk_size    Size of each read/write chunk in bytes
 * @return ESP_OK on success, otherwise error code
 */
esp_err_t sdmmc_driver_speed_test(const char *mount_point,
                                  const char *file_path,
                                  size_t file_size_kb,
                                  size_t chunk_size);

#ifdef __cplusplus
}
#endif
