/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <string.h> // For memcpy

#include "esp_log.h"

#include "uvc_stream.h" // For uvc_host_stream_pause()
#include "uvc_types_priv.h"
#include "uvc_check_priv.h"
#include "uvc_frame_priv.h"
#include "uvc_critical_priv.h"

static const char *TAG = "uvc-isoc";

/**
 * @brief Callback function for handling Isochronous USB transfers from a UVC camera.
 *
 * This function processes isochronous transfer packets, which may contain video frame data. The following key points
 * are handled in the transfer:
 *
 * - **Start of Frame (SoF)**: Detected by a change in Frame ID, which toggles between 0 and 1.
 * - **End of Frame (EoF)**: Signaled in the packet header.
 * - **Transfer Characteristics**:
 *   - **No CRC**: Data corruption is possible.
 *   - **No ACK**: Packets can be missed.
 *   - **Packet Header**: Each packet includes a header used to detect errors, missed packets, and other issues.
 *
 * The callback performs the following tasks:
 * 1. Checks the status of each isochronous packet and handles various USB transfer statuses (e.g., completed,
 *    error, device disconnected).
 * 2. Parses packet headers to detect the start of new frames, handles errors, and manages frame buffers.
 * 3. Aggregates valid data into a frame buffer, ensuring no buffer overflow occurs.
 * 4. Signals the end of a frame and invokes user-defined callbacks if necessary.
 *
 * @param[in] transfer Pointer to the completed USB transfer structure.
 */
void isoc_transfer_callback(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    uvc_stream_t *uvc_stream = (uvc_stream_t *)transfer->context;

    // USB_TRANSFER_STATUS_NO_DEVICE is set in transfer->status.
    // Other error codes are saved in status of each ISOC packet descriptor
    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_ERROR_CHECK(uvc_host_stream_pause(uvc_stream)); // This should never fail
    }

    if (!UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        return; // If the streaming was turned off, we don't have to do anything
    }

    const uint8_t *payload = transfer->data_buffer;
    for (int i = 0; i < transfer->num_isoc_packets; i++) {
        usb_isoc_packet_desc_t *isoc_desc = &transfer->isoc_packet_desc[i];

        // Check USB status
        switch (isoc_desc->status) {
        case USB_TRANSFER_STATUS_COMPLETED:
            break;
        case USB_TRANSFER_STATUS_NO_DEVICE:
        case USB_TRANSFER_STATUS_CANCELED:
            ESP_ERROR_CHECK(uvc_host_stream_pause(uvc_stream)); // This should never fail
            return; // No need to process the rest
        case USB_TRANSFER_STATUS_ERROR:
        case USB_TRANSFER_STATUS_OVERFLOW:
        case USB_TRANSFER_STATUS_STALL:
            ESP_LOGW(TAG, "usb err %d", isoc_desc->status);
            uvc_stream->single_thread.skip_current_frame = true;
            goto next_isoc_packet; // Data corrupted

        case USB_TRANSFER_STATUS_TIMED_OUT:
        case USB_TRANSFER_STATUS_SKIPPED:
            goto next_isoc_packet; // Skipped and timed out ISOC transfers are not an issue
        default:
            assert(false);
        }

        // Check for start of new frame
        const uvc_payload_header_t *payload_header = (const uvc_payload_header_t *)payload;
        if (!uvc_frame_payload_header_validate(payload_header, isoc_desc->actual_num_bytes)) {
            ESP_LOGD(TAG, "invalid UVC payload header, %02x, %02x, len:%d", payload[0], payload[1], isoc_desc->actual_num_bytes);
            uvc_stream->single_thread.skip_current_frame = true;
            goto next_isoc_packet;
        }

        // Derive payload data pointer/length once and reuse below
        const uint8_t *payload_data = payload + payload_header->bHeaderLength;
        size_t payload_data_len = isoc_desc->actual_num_bytes - payload_header->bHeaderLength;

        if (payload_data_len == 0) {
            // This is a zero-length packet, skip it
            ESP_LOGD(TAG, "zero-length packet, skipping");
            goto next_isoc_packet;
        }

        // Check for error flag
        if (payload_header->bmHeaderInfo.error) {
            ESP_LOGW(TAG, "frame error");
            uvc_stream->single_thread.skip_current_frame = true;
        }

        const bool start_of_frame = (uvc_stream->single_thread.current_frame_id != payload_header->bmHeaderInfo.frame_id);
        if (start_of_frame) {
            // We detected start of new frame. Update Frame ID and start fetching this frame
            uvc_stream->single_thread.current_frame_id   = payload_header->bmHeaderInfo.frame_id;
            uvc_stream->single_thread.skip_current_frame = payload_header->bmHeaderInfo.error;

            // Get free frame buffer for this new frame (or handle missed EoF first)
            UVC_ENTER_CRITICAL();
            const bool need_new_frame = (uvc_stream->dynamic.streaming && !uvc_stream->dynamic.current_frame);
            if (need_new_frame) {
                UVC_EXIT_CRITICAL();

                // MS2109 quirk: no EoF bit, so the first ISOC packet of a new
                // frame may contain trailing garbage from the previous frame
                // before the JPEG SOI (FF D8).  Scan forward and strip it.
                if (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG) {
                    size_t scan_end = (payload_data_len > 1) ? (payload_data_len - 1) : 0;
                    size_t soi_off = 0;
                    while (soi_off < scan_end) {
                        if (payload_data[soi_off] == JPEG_MARKER &&
                            payload_data[soi_off + 1] == JPEG_SOI) {
                            break;
                        }
                        soi_off++;
                    }
                    if (soi_off > 0) {
                        ESP_LOGW(TAG, "stripped %u garbage bytes before MJPEG SOI",
                                 (unsigned)soi_off);
                        payload_data += soi_off;
                        payload_data_len -= soi_off;
                    }
                }

                uvc_stream->dynamic.current_frame = uvc_frame_get_empty(uvc_stream);
                if (uvc_stream->dynamic.current_frame == NULL) {
                    // There is no free frame buffer now, skipping this frame
                    uvc_stream->single_thread.skip_current_frame = true;

                    // Inform the user about the underflow
                    uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
                    if (stream_cb) {
                        const uvc_host_stream_event_data_t event = {
                            .type = UVC_HOST_FRAME_BUFFER_UNDERFLOW,
                        };
                        stream_cb(&event, uvc_stream->constant.cb_arg);
                    }
                    goto next_isoc_packet;
                }
            } else {
                // We received SoF but current_frame is not NULL: We missed EoF.
                // Some devices (e.g. MacroSilicon MS2109) never set the EoF bit
                // in MJPEG payload headers. For MJPEG, complete the frame anyway.
                uvc_host_frame_t *old_frame = uvc_stream->dynamic.current_frame;
                uvc_stream->dynamic.current_frame = NULL;
                const bool old_skip = uvc_stream->single_thread.skip_current_frame;
                const bool is_mjpeg = (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG);
                uvc_host_frame_callback_t fb_cb = uvc_stream->constant.frame_cb;
                void *cb_arg = uvc_stream->constant.cb_arg;
                UVC_EXIT_CRITICAL();

                if (is_mjpeg && fb_cb && old_frame && !old_skip) {
                    bool returned = fb_cb(old_frame, cb_arg);
                    if (returned) {
                        uvc_host_frame_return((uvc_host_stream_hdl_t)uvc_stream, old_frame);
                    }
                } else if (old_frame) {
                    if (!is_mjpeg) {
                        ESP_EARLY_LOGW(TAG, "missed EoF");
                    }
                    uvc_frame_reset(old_frame);
                    uvc_host_frame_return((uvc_host_stream_hdl_t)uvc_stream, old_frame);
                }

                // Acquire a new empty frame buffer for the new frame that just started
                uvc_stream->dynamic.current_frame = uvc_frame_get_empty(uvc_stream);
                if (uvc_stream->dynamic.current_frame == NULL) {
                    uvc_stream->single_thread.skip_current_frame = true;
                } else {
                    uvc_stream->single_thread.skip_current_frame = false;
                }

                // Strip garbage bytes before SOI (same MS2109 quirk)
                if (is_mjpeg) {
                    size_t scan_end = (payload_data_len > 1) ? (payload_data_len - 1) : 0;
                    size_t soi_off = 0;
                    while (soi_off < scan_end) {
                        if (payload_data[soi_off] == JPEG_MARKER &&
                            payload_data[soi_off + 1] == JPEG_SOI) {
                            break;
                        }
                        soi_off++;
                    }
                    if (soi_off > 0) {
                        ESP_LOGW(TAG, "stripped %u garbage bytes before MJPEG SOI",
                                 (unsigned)soi_off);
                        payload_data += soi_off;
                        payload_data_len -= soi_off;
                    }
                }
            }
        }

        // Add received data to frame buffer
        if (!uvc_stream->single_thread.skip_current_frame) {
            uvc_host_frame_t *current_frame = UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame);

            esp_err_t ret = uvc_frame_add_data(current_frame, payload_data, payload_data_len);
            if (ret != ESP_OK) {
                // Frame buffer overflow, skip this frame
                uvc_stream->single_thread.skip_current_frame = true;

                // Inform the user about the overflow
                uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
                if (stream_cb) {
                    const uvc_host_stream_event_data_t event = {
                        .type = UVC_HOST_FRAME_BUFFER_OVERFLOW,
                    };
                    stream_cb(&event, uvc_stream->constant.cb_arg);
                }
                goto next_isoc_packet;
            }
        }

        // End of Frame. Pass the frame to user
        if (payload_header->bmHeaderInfo.end_of_frame) {
            bool return_frame = true; // In case streaming is stopped ATM, we must return the frame

            // Check if the user did not stop the stream in the meantime
            UVC_ENTER_CRITICAL();
            uvc_host_frame_t *this_frame = uvc_stream->dynamic.current_frame;
            uvc_stream->dynamic.current_frame = NULL; // Stop writing more data to this frame

            // Determine if we should invoke the frame callback:
            // Only invoke the callback if streaming is active, a frame callback exists,
            // and we have a valid frame to pass to the user.
            const bool invoke_fb_callback = (uvc_stream->dynamic.streaming && uvc_stream->constant.frame_cb && this_frame && !uvc_stream->single_thread.skip_current_frame);
            UVC_EXIT_CRITICAL();

            if (invoke_fb_callback) {
                return_frame = uvc_stream->constant.frame_cb(this_frame, uvc_stream->constant.cb_arg);
            }
            if (return_frame) {
                // The user has processed the frame in his callback, return it back to empty queue
                uvc_host_frame_return(uvc_stream, this_frame);
            }
        }
next_isoc_packet:
        payload += isoc_desc->num_bytes;
        continue;
    }

    if (UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        usb_host_transfer_submit(transfer); // Restart the transfer
    }
}
