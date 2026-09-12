/*
 * SDMMC Driver - Reusable SD card library implementation
 *
 * Core SD card operations extracted and generalized from the ESP-IDF
 * SDMMC example for reuse across projects.
 */

#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdmmc_driver.h"

#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

/* On IDF v6.0+, SDMMC host controller can only be initialized once.
 * When ESP-Hosted uses SDIO (Slot 1), SD card (Slot 0) must skip init. */
#if defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
#define WORKAROUND_HOSTED_SDMMC 1
static esp_err_t sdmmc_host_init_dummy(void) { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_dummy(void) { return ESP_OK; }
#else
#define WORKAROUND_HOSTED_SDMMC 0
#endif

#include "sdkconfig.h"

static const char *TAG = "sdmmc_driver";

/* ─── Default configuration ─────────────────────────────────────────── */

void sdmmc_driver_get_default_config(sdmmc_driver_config_t *config)
{
    config->clk_pin    = CONFIG_SDMMC_DRV_CLK_PIN;
    config->cmd_pin    = CONFIG_SDMMC_DRV_CMD_PIN;
    config->d0_pin     = CONFIG_SDMMC_DRV_D0_PIN;
    config->bus_width  = CONFIG_SDMMC_DRV_BUS_WIDTH;
    config->d1_pin     = CONFIG_SDMMC_DRV_D1_PIN;
    config->d2_pin     = CONFIG_SDMMC_DRV_D2_PIN;
    config->d3_pin     = CONFIG_SDMMC_DRV_D3_PIN;
    config->format_if_mount_failed = CONFIG_SDMMC_DRV_FORMAT_IF_MOUNT_FAILED;
    config->mount_point          = CONFIG_SDMMC_DRV_MOUNT_POINT;
    config->max_files            = CONFIG_SDMMC_DRV_MAX_FILES;
    config->allocation_unit_size = CONFIG_SDMMC_DRV_ALLOC_UNIT_SIZE;
    config->max_freq_khz         = CONFIG_SDMMC_DRV_MAX_FREQ_KHZ;
    config->pwr_ctrl_ldo_enable  = CONFIG_SDMMC_DRV_LDO_ENABLE;
    config->pwr_ctrl_ldo_chan_id = CONFIG_SDMMC_DRV_LDO_CHAN_ID;
}

/* ─── SD card init / deinit ─────────────────────────────────────────── */

esp_err_t sdmmc_driver_init(const sdmmc_driver_config_t *config, sdmmc_card_t **out_card)
{
    esp_err_t ret;

    if (config == NULL || out_card == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = config->format_if_mount_failed,
        .max_files = config->max_files,
        .allocation_unit_size = config->allocation_unit_size,
    };

    ESP_LOGI(TAG, "Initializing SD card via SDMMC peripheral");
    ESP_LOGI(TAG, "Pins: CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d (bus_width=%d)",
             config->clk_pin, config->cmd_pin, config->d0_pin,
             config->d1_pin, config->d2_pin, config->d3_pin,
             config->bus_width);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;  /* SD card on slot 0, WiFi C6 on slot 1 */
    host.max_freq_khz = config->max_freq_khz;

#if WORKAROUND_HOSTED_SDMMC
    /* ESP-Hosted already initialized the SDMMC controller; skip re-init */
    host.init = &sdmmc_host_init_dummy;
    host.deinit = &sdmmc_host_deinit_dummy;
#endif

    // If the board has an on-chip LDO power supply for the SD slot,
    // initialize it before the SDMMC host.
    if (config->pwr_ctrl_ldo_enable) {
        sd_pwr_ctrl_ldo_config_t ldo_config = {
            .ldo_chan_id = config->pwr_ctrl_ldo_chan_id,
        };
        sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;

        ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create on-chip LDO power control driver");
            return ret;
        }
        host.pwr_ctrl_handle = pwr_ctrl_handle;
    }

    // This initializes the slot without card detect (CD) and write protect
    // (WP) signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    // Set bus width to use:
    slot_config.width = config->bus_width;

    // On chips where the GPIOs used for SD card can be configured, set them
    // in the slot_config structure:
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = config->clk_pin;
    slot_config.cmd = config->cmd_pin;
    slot_config.d0 = config->d0_pin;
    if (config->bus_width == 4) {
        slot_config.d1 = config->d1_pin;
        slot_config.d2 = config->d2_pin;
        slot_config.d3 = config->d3_pin;
    }
#endif  // CONFIG_SOC_SDMMC_USE_GPIO_MATRIX

    // Enable internal pullups on enabled pins. The internal pullups
    // are insufficient however, please make sure 10k external pullups
    // are connected on the bus.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(config->mount_point, &host, &slot_config,
                                  &mount_config, out_card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "Enable CONFIG_SDMMC_DRV_FORMAT_IF_MOUNT_FAILED to auto-format.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card (%s). "
                     "Check pull-up resistors on signal lines.", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "Filesystem mounted successfully");
    sdmmc_card_print_info(stdout, *out_card);

    return ESP_OK;
}

esp_err_t sdmmc_driver_deinit(const char *mount_point, sdmmc_card_t *card)
{
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(mount_point, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card (%s)", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

/* ─── File operations ───────────────────────────────────────────────── */

esp_err_t sdmmc_driver_write_file(const char *path, const char *data)
{
    if (path == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Writing file: %s", path);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", path);
        return ESP_FAIL;
    }
    fprintf(f, "%s", data);
    /* fclose() flushes VFS buffers to SD card — its return value
       reflects the actual hardware write result. */
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "Failed to close file (write error): %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "File written (%d bytes)", (int)strlen(data));
    return ESP_OK;
}

esp_err_t sdmmc_driver_read_file(const char *path, char *buffer, size_t buf_size)
{
    if (path == NULL || buffer == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Reading file: %s", path);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", path);
        return ESP_FAIL;
    }

    if (fgets(buffer, buf_size, f) == NULL) {
        ESP_LOGE(TAG, "Failed to read from file: %s", path);
        fclose(f);
        return ESP_FAIL;
    }
    fclose(f);

    /* Strip trailing newline */
    char *pos = strchr(buffer, '\n');
    if (pos) {
        *pos = '\0';
    }
    ESP_LOGI(TAG, "Read from file: '%s'", buffer);
    return ESP_OK;
}

esp_err_t sdmmc_driver_write_binary_file(const char *path, const uint8_t *data, size_t len)
{
    if (path == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Writing binary file: %s (%u bytes)", path, (unsigned)len);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for binary writing: %s", path);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    if (written != len) {
        ESP_LOGE(TAG, "Failed to write all bytes (%u/%u)", (unsigned)written, (unsigned)len);
        fclose(f);
        return ESP_FAIL;
    }

    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "Failed to close file (write error): %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Binary file written successfully");
    return ESP_OK;
}

/* ─── Speed benchmark ──────────────────────────────────────────────── */

esp_err_t sdmmc_driver_speed_test(const char *mount_point,
                                  const char *file_path,
                                  size_t file_size_kb,
                                  size_t chunk_size)
{
    if (mount_point == NULL || file_path == NULL ||
        file_size_kb == 0 || chunk_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t total_bytes = file_size_kb * 1024;
    if (chunk_size > total_bytes) {
        chunk_size = total_bytes;
    }

    /* Allocate write buffer */
    uint8_t *buf = malloc(chunk_size);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u-byte buffer", (unsigned)chunk_size);
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0xA5, chunk_size);

    /* ── WRITE ──────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Speed test: writing %u KB (%u B chunks)...",
             (unsigned)file_size_kb, (unsigned)chunk_size);

    FILE *f = fopen(file_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to create speed-test file");
        free(buf);
        return ESP_FAIL;
    }

    int64_t t_start = esp_timer_get_time();
    size_t remaining = total_bytes;
    while (remaining > 0) {
        size_t n = (remaining < chunk_size) ? remaining : chunk_size;
        size_t written = fwrite(buf, 1, n, f);
        if (written != n) {
            ESP_LOGE(TAG, "Write failed at offset %u",
                     (unsigned)(total_bytes - remaining));
            fclose(f);
            free(buf);
            return ESP_FAIL;
        }
        remaining -= n;
    }
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "Failed to close file after write");
        free(buf);
        return ESP_FAIL;
    }
    int64_t t_write = esp_timer_get_time() - t_start;
    float write_speed = (float)total_bytes / (float)t_write * 1000.0f;

    ESP_LOGI(TAG, "Write: %u KB in %lld ms  ->  %.1f KB/s",
             (unsigned)file_size_kb,
             (long long)(t_write / 1000), write_speed);

    /* ── READ ───────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Speed test: reading back %u KB...", (unsigned)file_size_kb);

    f = fopen(file_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open speed-test file for reading");
        free(buf);
        return ESP_FAIL;
    }

    t_start = esp_timer_get_time();
    while (fread(buf, 1, chunk_size, f) > 0) {
        /* data discarded */
    }
    fclose(f);
    int64_t t_read = esp_timer_get_time() - t_start;
    float read_speed = (float)total_bytes / (float)t_read * 1000.0f;

    ESP_LOGI(TAG, "Read:  %u KB in %lld ms  ->  %.1f KB/s",
             (unsigned)file_size_kb,
             (long long)(t_read / 1000), read_speed);

    /* Clean up test file */
    unlink(file_path);
    free(buf);

    ESP_LOGI(TAG, "Speed test complete");
    return ESP_OK;
}
