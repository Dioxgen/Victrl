/*
 * UVC Frame Capture Driver — Generic UVC → SD card pipeline
 *
 * Integrated UVC host driver + frame capture + sdmmc_driver into a
 * simple API for:
 *   - Initializing USB / UVC / SD in one call
 *   - Running a continuous UVC stream
 *   - Capturing one frame at a time
 *   - Saving frames to SD card
 *
 * Device-specific settings (VID, PID, resolution, format) are
 * supplied by the caller at init time — this library is not tied
 * to any particular capture chip.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "usb/uvc_host.h"          /* for uvc_host_stream_config_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Public API ──────────────────────────────────────────────────── */

/**
 * @brief Initialize the capture subsystem.
 *
 * Initializes (in order):
 *   1. SD card via SDMMC (mount point /sdcard)
 *   2. USB Host Library + background event task
 *   3. UVC Host driver
 *   4. Opens the UVC stream described by @p stream_cfg
 *   5. Starts a continuous stream (pipeline stays warm)
 *   6. Discards 3 warm-up frames
 *
 * @param[in] stream_cfg  UVC stream configuration (VID, PID,
 *                        resolution, FPS, format, buffering, etc.).
 *                        The struct is copied internally; the caller
 *                        may free/reuse it after return.
 *
 * @return ESP_OK on success, otherwise error code.
 */
esp_err_t uvc_capture_init(const uvc_host_stream_config_t *stream_cfg);

/**
 * @brief Deinitialize the capture subsystem.
 *
 * Stops the UVC stream, closes it, uninstalls UVC driver and USB Host,
 * and unmounts the SD card.
 *
 * @return ESP_OK on success, otherwise error code.
 */
esp_err_t uvc_capture_deinit(void);

/**
 * @brief Length + hash fingerprint of one frame.
 *
 * For a genuinely static HDMI source the MS2109 MJPEG output is expected to be
 * byte-identical frame to frame, so an exact match means "nothing changed".
 * Always compare `len_bytes` too: a large delta is a change even if the encoder
 * turns out to add mild jitter to the entropy-coded stream.
 */
typedef struct {
    uint32_t len_bytes;
    uint32_t hash;      /* FNV-1a over the frame payload */
} uvc_frame_sig_t;

/**
 * @brief Capture one frame from the running UVC stream.
 *
 * Blocks until the next complete frame arrives.  For MJPEG streams,
 * leading garbage bytes before the JPEG SOI (0xFF 0xD8) are
 * automatically stripped before returning.
 *
 * @param[out] out_data   Pointer to receive the frame buffer
 *                        (caller must free()).  Allocated from SPIRAM.
 * @param[out] out_len    Receives the length of the captured data.
 * @param[in]  timeout_ms Maximum wait (ms).  0 = no limit.
 *
 * @return ESP_OK on success, otherwise error code.
 */
esp_err_t uvc_capture_one_frame(uint8_t **out_data, size_t *out_len,
                                uint32_t timeout_ms);

/**
 * @brief Same as uvc_capture_one_frame(), but also fingerprints the frame.
 *
 * The signature describes *exactly* the frame that was copied out, so it can be
 * compared later against uvc_capture_frame_sig() to decide whether the screen
 * changed. Taking it here rather than with a second call matters: a separate
 * call would consume a later frame, so the baseline would not be the image the
 * model actually saw.
 *
 * @param[out] out_sig    Optional; receives the signature. May be NULL.
 */
esp_err_t uvc_capture_one_frame_sig(uint8_t **out_data, size_t *out_len,
                                    uvc_frame_sig_t *out_sig, uint32_t timeout_ms);

/**
 * @brief Cheap signature of the next frame, without copying it out.
 *
 * Computes the fingerprint directly on the driver's frame buffer and hands the
 * frame straight back. Together with uvc_capture_one_frame_sig() this answers
 * "did the screen change since the frame I sent to the model?", which is what
 * makes it safe to reuse a screenshot and lets the agent tell the model
 * objectively whether its last action had any visible effect at all.
 *
 * @param[out] out_sig    Receives the signature.
 * @param[in]  timeout_ms Maximum wait (ms).  0 = no limit.
 *
 * @return ESP_OK on success, otherwise error code.
 */
esp_err_t uvc_capture_frame_sig(uvc_frame_sig_t *out_sig, uint32_t timeout_ms);

/**
 * @brief Save binary frame data to the SD card.
 *
 * The file is written under the mount point configured in the SDMMC
 * driver (default: /sdcard/).
 *
 * @param[in] filename  File name relative to SD card root
 *                      (e.g. "frame_001.jpg").
 * @param[in] data      Pointer to binary data.
 * @param[in] len       Number of bytes to write.
 *
 * @return ESP_OK on success, otherwise error code.
 */
esp_err_t uvc_capture_save_to_sd(const char *filename,
                                 const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
