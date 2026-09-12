# uvc_capture_card_driver — UVC Capture Card Driver

> **English** | [中文](README_CN.md)

## Overview

**Provenance, because it is easy to lose:** 18 of the 20 `.c`/`.h` files here are Espressif's own UVC host code (`uvc_host`, `uvc_stream`, `uvc_bulk`, `uvc_isoc`, `uvc_control`, `uvc_frame`, the descriptor parsers, the private headers and `usb_types_uvc.h`), under Apache-2.0 with `SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD` in each file — those headers are retained deliberately and must stay. Only `uvc_capture_card_driver.c` and its header are this project's own wrapper, and they are the two files with no Espressif header.

A USB Host UVC capture driver that strings the USB Host, the UVC stack and `sdmmc_driver` into one pipeline: a single call initializes the SD card, the USB Host and the UVC driver, then keeps one UVC stream permanently open while the caller pulls frames one at a time with a blocking call. VID/PID, resolution, frame rate, format and buffering are all supplied by the caller through `uvc_host_stream_config_t`; the component is not tied to any particular capture chip. The current project uses it with an MS2109 (`0x534D:0x2109`).

## API

| Function | Description |
|------|------|
| `uvc_capture_init(stream_cfg)` | Initializes the SD card, the USB Host plus its event task, and the UVC driver in order, then opens and starts the stream and discards 3 warm-up frames; returns an error if the device is not attached |
| `uvc_capture_deinit()` | Stops and closes the stream, uninstalls the UVC driver and the USB Host, deletes the frame semaphore, and unmounts the SD card if this component mounted it |
| `uvc_capture_one_frame(&data, &len, timeout_ms)` | Blocks for the next frame and copies the data to SPIRAM; the caller must `free()` it. `timeout_ms` is in milliseconds, 0 means no limit |
| `uvc_capture_one_frame_sig(&data, &len, &sig, timeout_ms)` | Same, and also returns the `uvc_frame_sig_t` of that frame (byte count + FNV-1a hash) |
| `uvc_capture_frame_sig(&sig, timeout_ms)` | Computes the fingerprint of the next frame only, copies nothing, and hands the frame straight back to the driver |
| `uvc_capture_save_to_sd(name, data, len)` | Writes a binary file as `/sdcard/<name>`; the full path is limited to 128 bytes |

`uvc_capture_init()` overrides `event_cb`, `frame_cb` and `user_ctx` in the supplied configuration and uses every other field as-is. The component also exposes the complete UVC Host API from `include/usb/uvc_host.h` (`uvc_host_install()`, `uvc_host_stream_open()`, `uvc_host_stream_start()`, `uvc_host_stream_format_select()`, `uvc_host_get_frame_list()`, `uvc_host_desc_print()`, and so on).

## Frame pulling model

The internal frame callback retains only the newest frame: when a new frame arrives the previous one is returned to the driver immediately, and the semaphore is given only when no unconsumed frame is buffered, so the frame pool cannot run dry just because the application consumes frames slowly. Every `uvc_capture_*` call on the application side is a blocking read; `uvc_capture_one_frame*()` adds one copy for the caller, while `uvc_capture_frame_sig()` copies nothing.

The transfer path is chosen automatically from the transfer type of the streaming endpoint, ISOC or BULK. Both MJPEG compatibility measures — stripping the garbage bytes before the SOI (`0xFF 0xD8`) and completing a frame that carries no EoF bit when the next SoF arrives — exist only in the ISOC path; the BULK path instead requires that the first packet of every frame starts with the SOI.

## Kconfig

`menuconfig` → **USB HOST UVC**:

| Symbol | Default | Description |
|------|--------|------|
| `CONFIG_UVC_PRINTF_CONFIGURATION_DESCRIPTOR` | n | Print the UVC configuration descriptor |
| `CONFIG_UVC_INTERVAL_ARRAY_SIZE` | 3 | Size of the discrete frame interval array in `uvc_host_frame_info_t` |
| `CONFIG_UVC_CHECK_PAYLOAD_HEADER_EOH` | y | Validate the EOH bit of the payload header |

## Notes

- The component defines no USB Host related Kconfig options. `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=3000` and `CONFIG_USB_HOST_HW_BUFFER_BIAS_IN=y` in the project root `sdkconfig.defaults` are consumed by the ESP-IDF USB Host library, not by this component.
- The USB PHY is configured by the USB Host library itself (`skip_phy_setup = false` in `usb_init()`); the component performs no external PHY or hub setup.
- Frame buffer size and count are up to the caller: a `frame_size` of 0 falls back to the negotiated `dwMaxVideoFrameSize`, and a `frame_heap_caps` of 0 falls back to `MALLOC_CAP_DEFAULT`. The current project configures 6 frame buffers of 1 MB (`MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA`) and 4 URBs in `main/main.c`, with the resolution taken from `capture_width` / `capture_height` in `/sdcard/config.json`.
- If the SD card is already mounted by someone else (the `/sdcard` directory exists), `uvc_capture_init()` skips SD initialization, and `uvc_capture_deinit()` then leaves it mounted.
- Devices without the EoF bit are only rescued by "complete the previous frame at the next SoF" in the ISOC + MJPEG combination; the BULK path has no such completion and, per the source comment, pays for its EoF detection by discarding frames whose last packet is a full MPS packet.
- Frame overflow and underflow are only reported as log lines (`Frame overflow` / `Frame underflow`) from the stream event callback; an overflowing frame is discarded.
- A capture timeout returns `ESP_ERR_TIMEOUT`; an uninitialized driver or a stream that is not running returns `ESP_ERR_INVALID_STATE`.
- Dependencies: `sdmmc_driver` (the `REQUIRES` entry in CMakeLists) and `usb` ^1.0.0 (`idf_component.yml`).
