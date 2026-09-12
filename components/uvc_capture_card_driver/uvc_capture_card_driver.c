/*
 * UVC Frame Capture Driver — Implementation
 *
 * Generic pipeline: USB Host → UVC stream → frame buffer → caller.
 * Device settings are provided at init time.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "sdmmc_driver.h"
#include "uvc_capture_card_driver.h"

static const char *TAG = "uvc_capture";

/* ─── Internal state ─────────────────────────────────────────────── */

static SemaphoreHandle_t s_frame_sem;
static uvc_host_frame_t *volatile s_latest_frame;
static uvc_host_stream_hdl_t s_uvc_stream;
static sdmmc_card_t *s_sd_card;
static bool s_initialized;
static bool s_streaming;

/* ─── Forward declarations ───────────────────────────────────────── */

static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx);
static void stream_event_callback(const uvc_host_stream_event_data_t *event, void *user_ctx);
static void usb_lib_task(void *arg);

/* ─── SD card init / deinit ──────────────────────────────────────── */

static esp_err_t sd_card_init(void)
{
    /* Skip if already mounted (e.g. by main.c for early config load) */
    struct stat st;
    if (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode)) {
        ESP_LOGI(TAG, "SD card already mounted, skipping init");
        return ESP_OK;
    }

    sdmmc_driver_config_t sd_cfg;
    sdmmc_driver_get_default_config(&sd_cfg);

    ESP_RETURN_ON_ERROR(
        sdmmc_driver_init(&sd_cfg, &s_sd_card),
        TAG, "SD card init failed");
    return ESP_OK;
}

static esp_err_t sd_card_deinit(void)
{
    if (s_sd_card == NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        sdmmc_driver_deinit("/sdcard", s_sd_card),
        TAG, "SD card deinit failed");
    s_sd_card = NULL;
    return ESP_OK;
}

/* ─── USB Host init ──────────────────────────────────────────────── */

#define USB_LIB_TASK_PRIO   (15)
#define USB_LIB_TASK_STACK  (4096)

static esp_err_t usb_init(void)
{
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_RETURN_ON_ERROR(
        usb_host_install(&host_cfg),
        TAG, "USB Host install failed");

    BaseType_t ret = xTaskCreatePinnedToCore(
        usb_lib_task, "usb_lib",
        USB_LIB_TASK_STACK, NULL,
        USB_LIB_TASK_PRIO, NULL, tskNO_AFFINITY);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create USB lib task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: All devices freed");
        }
    }
}

/* ─── UVC driver init ────────────────────────────────────────────── */

static esp_err_t uvc_init(void)
{
    const uvc_host_driver_config_t uvc_cfg = {
        .driver_task_stack_size = 4 * 1024,
        .driver_task_priority   = USB_LIB_TASK_PRIO + 1,
        .xCoreID                = tskNO_AFFINITY,
        .create_background_task = true,
    };
    ESP_RETURN_ON_ERROR(
        uvc_host_install(&uvc_cfg),
        TAG, "UVC driver install failed");
    return ESP_OK;
}

/* ─── UVC stream ─────────────────────────────────────────────────── */

static uvc_host_stream_config_t s_stream_cfg;

static esp_err_t uvc_stream_open(void)
{
    ESP_LOGI(TAG, "Opening UVC stream 0x%04X:0x%04X %" PRIu32 "x%" PRIu32 "@%.1fFPS fmt=%d...",
             s_stream_cfg.usb.vid, s_stream_cfg.usb.pid,
             (uint32_t)s_stream_cfg.vs_format.h_res,
             (uint32_t)s_stream_cfg.vs_format.v_res,
             (double)s_stream_cfg.vs_format.fps,
             s_stream_cfg.vs_format.format);

    ESP_RETURN_ON_ERROR(
        uvc_host_stream_open(&s_stream_cfg, pdMS_TO_TICKS(5000), &s_uvc_stream),
        TAG, "Failed to open UVC stream");
    ESP_LOGI(TAG, "UVC stream opened");
    return ESP_OK;
}

/* ─── Frame callback ─────────────────────────────────────────────── */

static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    (void)user_ctx;

    /* Keep at most one frame buffered so the ISOC driver's empty pool
     * never runs dry.  If the app hasn't consumed the previous frame yet,
     * return it now and replace it with the new one. */
    uvc_host_frame_t *old = s_latest_frame;
    s_latest_frame = (uvc_host_frame_t *)frame; /* cast away const — caller owns frame */
    if (old) {
        uvc_host_frame_return(s_uvc_stream, old);
    } else {
        /* First frame after a gap — signal the consumer. */
        xSemaphoreGive(s_frame_sem);
    }
    return false; /* false: we retain the frame; return is done above for the old one */
}

/* ─── Stream event callback ──────────────────────────────────────── */

static void stream_event_callback(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "USB transfer error: 0x%x", event->transfer_error.error);
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "MS2109 device disconnected");
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "Frame overflow — increase frame_size in config");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGW(TAG, "Frame underflow — increase frame buffer count or speed up processing");
        break;
    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════ */

esp_err_t uvc_capture_init(const uvc_host_stream_config_t *stream_cfg)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    if (stream_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 0. Copy & canonize stream config */
    s_stream_cfg = *stream_cfg;
    s_stream_cfg.event_cb = stream_event_callback;
    s_stream_cfg.frame_cb = frame_callback;
    s_stream_cfg.user_ctx = NULL; /* callbacks use globals */

    /* 1. SD card */
    ESP_LOGI(TAG, "--- Initializing SD card ---");
    ESP_RETURN_ON_ERROR(sd_card_init(), TAG, "SD init failed");

    /* 2. Frame sync */
    s_frame_sem = xSemaphoreCreateBinary();
    if (s_frame_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create frame semaphore");
        return ESP_ERR_NO_MEM;
    }
    s_latest_frame = NULL;

    /* 3. USB Host */
    ESP_LOGI(TAG, "--- Initializing USB Host ---");
    ESP_RETURN_ON_ERROR(usb_init(), TAG, "USB init failed");

    /* Give USB host time to settle before installing UVC */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 4. UVC driver */
    ESP_LOGI(TAG, "--- Initializing UVC driver ---");
    ESP_RETURN_ON_ERROR(uvc_init(), TAG, "UVC init failed");

    /* 5. Open UVC stream (device must be plugged in) */
    ESP_LOGI(TAG, "--- Opening UVC stream ---");
    esp_err_t err = uvc_stream_open();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Is the UVC device connected?");
        return err;
    }

    /* 6. Start streaming — keep it running continuously so the
     *    HDMI receiver / encoder pipeline stays locked and warm. */
    ESP_LOGI(TAG, "--- Starting continuous stream ---");
    ESP_RETURN_ON_ERROR(
        uvc_host_stream_start(s_uvc_stream),
        TAG, "Failed to start continuous stream");

    s_initialized = true;
    s_streaming  = true;

    /* Discard the first few frames while the pipeline stabilises */
    ESP_LOGI(TAG, "Warming up pipeline (discarding initial frames)...");
    for (int i = 0; i < 3; i++) {
        if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
            /* The semaphore can be signalled with no frame buffered if a
             * callback raced the consumer; returning NULL would fault. */
            uvc_host_frame_t *stale = s_latest_frame;
            s_latest_frame = NULL;
            if (stale) {
                uvc_host_frame_return(s_uvc_stream, stale);
            }
        }
    }
    ESP_LOGI(TAG, "Pipeline warm-up complete");
    return ESP_OK;
}

esp_err_t uvc_capture_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing capture subsystem...");

    if (s_uvc_stream != NULL) {
        if (s_streaming) {
            uvc_host_stream_stop(s_uvc_stream);
            s_streaming = false;
        }
        uvc_host_stream_close(s_uvc_stream);
        s_uvc_stream = NULL;
    }

    uvc_host_uninstall();
    usb_host_uninstall();

    if (s_frame_sem != NULL) {
        vSemaphoreDelete(s_frame_sem);
        s_frame_sem = NULL;
    }

    sd_card_deinit();

    s_initialized = false;
    ESP_LOGI(TAG, "Capture subsystem deinitialized");
    return ESP_OK;
}

/* ── Frame helpers ───────────────────────────────────────────────── */

/* Wait for the next frame and take ownership of it. */
static esp_err_t take_frame(uvc_host_frame_t **out_frame, uint32_t timeout_ms)
{
    if (!s_initialized || !s_streaming) return ESP_ERR_INVALID_STATE;

    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    ESP_LOGD(TAG, "Waiting for frame (timeout=%" PRIu32 " ms)...", timeout_ms);

    if (xSemaphoreTake(s_frame_sem, ticks) != pdTRUE) {
        ESP_LOGE(TAG, "Frame receive timed out");
        return ESP_ERR_TIMEOUT;
    }

    uvc_host_frame_t *frame = s_latest_frame;
    s_latest_frame = NULL; /* we own it now */
    if (frame == NULL) {
        /* Possible if the frame callback raced with the consumer. */
        ESP_LOGE(TAG, "Frame semaphore signalled but no frame buffered");
        return ESP_ERR_INVALID_STATE;
    }
    *out_frame = frame;
    return ESP_OK;
}

/* For MJPEG, scan forward past garbage bytes before the SOI (MS2109 quirk). */
static const uint8_t *skip_to_soi(const uvc_host_frame_t *frame, size_t *io_len)
{
    const uint8_t *src = frame->data;
    size_t src_len = *io_len;

    if (frame->vs_format.format == UVC_VS_FORMAT_MJPEG && src_len > 2) {
        size_t soi_off = 0;
        while (soi_off < src_len - 1) {
            if (src[soi_off] == 0xFF && src[soi_off + 1] == 0xD8) break;
            soi_off++;
        }
        if (soi_off > 0 && soi_off < src_len - 1) {
            ESP_LOGW(TAG, "Stripping %u garbage bytes before MJPEG SOI",
                     (unsigned)soi_off);
            src += soi_off;
            src_len -= soi_off;
        }
    }
    *io_len = src_len;
    return src;
}

static uint32_t fnv1a32(const uint8_t *p, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

esp_err_t uvc_capture_one_frame_sig(uint8_t **out_data, size_t *out_len,
                                    uvc_frame_sig_t *out_sig, uint32_t timeout_ms)
{
    if (out_data == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uvc_host_frame_t *frame = NULL;
    esp_err_t err = take_frame(&frame, timeout_ms);
    if (err != ESP_OK) return err;

    ESP_LOGD(TAG, "Frame received: %u bytes, format=%d, %ux%u",
             (unsigned)frame->data_len,
             frame->vs_format.format,
             frame->vs_format.h_res,
             frame->vs_format.v_res);

    size_t src_len = frame->data_len;
    const uint8_t *src = skip_to_soi(frame, &src_len);

    /* Fingerprint the very bytes we are about to copy. */
    if (out_sig) {
        out_sig->len_bytes = (uint32_t)src_len;
        out_sig->hash = fnv1a32(src, src_len);
    }

    *out_len  = src_len;
    *out_data = heap_caps_malloc(src_len, MALLOC_CAP_SPIRAM);
    if (*out_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate output buffer (%u bytes)",
                 (unsigned)src_len);
        uvc_host_frame_return(s_uvc_stream, frame);
        return ESP_ERR_NO_MEM;
    }
    memcpy(*out_data, src, src_len);

    /* Return frame to driver — stream keeps running */
    ESP_ERROR_CHECK(uvc_host_frame_return(s_uvc_stream, frame));

    return ESP_OK;
}

esp_err_t uvc_capture_one_frame(uint8_t **out_data, size_t *out_len, uint32_t timeout_ms)
{
    return uvc_capture_one_frame_sig(out_data, out_len, NULL, timeout_ms);
}

esp_err_t uvc_capture_frame_sig(uvc_frame_sig_t *out_sig, uint32_t timeout_ms)
{
    if (out_sig == NULL) return ESP_ERR_INVALID_ARG;

    uvc_host_frame_t *frame = NULL;
    esp_err_t err = take_frame(&frame, timeout_ms);
    if (err != ESP_OK) return err;

    size_t len = frame->data_len;
    const uint8_t *src = skip_to_soi(frame, &len);

    out_sig->len_bytes = (uint32_t)len;
    out_sig->hash = fnv1a32(src, len);

    /* Nothing is copied: the frame goes straight back to the driver. */
    ESP_ERROR_CHECK(uvc_host_frame_return(s_uvc_stream, frame));
    return ESP_OK;
}

esp_err_t uvc_capture_save_to_sd(const char *filename, const uint8_t *data, size_t len)
{
    if (filename == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build full path: /sdcard/<filename> */
    char full_path[128];
    int written = snprintf(full_path, sizeof(full_path), "/sdcard/%s", filename);
    if (written < 0 || (size_t)written >= sizeof(full_path)) {
        ESP_LOGE(TAG, "Filename too long: %s", filename);
        return ESP_ERR_INVALID_ARG;
    }

    return sdmmc_driver_write_binary_file(full_path, data, len);
}
