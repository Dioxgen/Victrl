# sdmmc_driver — SDMMC SD card driver

> **English** | [中文](README_CN.md)

## Overview

An SD card driver built on the ESP-IDF SDMMC peripheral; it mounts a FAT filesystem and provides basic file read/write. The component is a thin wrapper: a single `sdmmc_driver_init()` performs host initialization, mounting and card info printing, and the remaining functions call the standard C file API on the mount point. This project runs it on **Slot 0** of the ESP32-P4 and leaves Slot 1 to the ESP-Hosted SDIO interface of the ESP32-C6 WiFi module.

## Parameters

The defaults come from Kconfig (menuconfig → **SDMMC Driver**). Every field is filled into `sdmmc_driver_config_t` by `sdmmc_driver_get_default_config()` and can be overridden by the caller.

| Parameter | Kconfig symbol | Default |
|-----------|----------------|---------|
| CLK pin | `CONFIG_SDMMC_DRV_CLK_PIN` | 43 |
| CMD pin | `CONFIG_SDMMC_DRV_CMD_PIN` | 44 |
| D0 pin | `CONFIG_SDMMC_DRV_D0_PIN` | 39 |
| D1 pin | `CONFIG_SDMMC_DRV_D1_PIN` | 40 (ignored in 1-bit mode) |
| D2 pin | `CONFIG_SDMMC_DRV_D2_PIN` | 41 (ignored in 1-bit mode) |
| D3 pin | `CONFIG_SDMMC_DRV_D3_PIN` | 42 (ignored in 1-bit mode) |
| Bus width | `CONFIG_SDMMC_DRV_BUS_WIDTH` | 4 (1 is allowed) |
| Max clock frequency | `CONFIG_SDMMC_DRV_MAX_FREQ_KHZ` | 40000 (40 MHz high-speed) |
| Format on mount failure | `CONFIG_SDMMC_DRV_FORMAT_IF_MOUNT_FAILED` | y |
| Mount point | `CONFIG_SDMMC_DRV_MOUNT_POINT` | `/sdcard` |
| Max open files | `CONFIG_SDMMC_DRV_MAX_FILES` | 5 |
| FAT allocation unit size | `CONFIG_SDMMC_DRV_ALLOC_UNIT_SIZE` | 16384 bytes |
| On-chip LDO power | `CONFIG_SDMMC_DRV_LDO_ENABLE` | y |
| LDO channel ID | `CONFIG_SDMMC_DRV_LDO_CHAN_ID` | 4 (configurable only while `LDO_ENABLE` is y) |

## API

| Function | Description |
|----------|-------------|
| `sdmmc_driver_init(config, &card)` | Initialize the SDMMC host (Slot 0) and mount the FAT filesystem; prints card info on success |
| `sdmmc_driver_deinit(mount_point, card)` | Unmount the filesystem and release the card |
| `sdmmc_driver_write_file(path, data)` | Write a string in text mode; the return value of `fclose()` is the real card write result |
| `sdmmc_driver_write_binary_file(path, data, len)` | Write `len` bytes in binary mode, verifying the byte count |
| `sdmmc_driver_read_file(path, buffer, buf_size)` | Read a file |
| `sdmmc_driver_get_default_config(&cfg)` | Fill the config struct with the Kconfig values |
| `sdmmc_driver_speed_test(mount_point, file_path, file_size_kb, chunk_size)` | Write `file_size_kb` in `chunk_size` blocks, read it back, print KB/s throughput and delete the test file |

A null pointer (or `len` / `buf_size` equal to 0) returns `ESP_ERR_INVALID_ARG`; a failed open, read/write or `fclose()` returns `ESP_FAIL`; a failed buffer allocation in the speed test returns `ESP_ERR_NO_MEM`.

## Notes

- **This is a board-specific configuration, not a generic driver.** The default pins (CLK=43 / CMD=44 / D0–D3=39–42), `CONFIG_SDMMC_DRV_LDO_ENABLE=y` and LDO channel 4 all come from the ESP32-P4-WIFI6-DEV-KIT, and match the GPIO-matrix defaults ESP-IDF provides for the ESP32-P4. On another board you must change two things: set `CONFIG_SDMMC_DRV_LDO_ENABLE` to n when the board has no on-chip LDO, otherwise `sd_pwr_ctrl_new_on_chip_ldo()` fails and `sdmmc_driver_init()` returns that error directly.
- The on-chip LDO code is guarded by `SOC_SDMMC_IO_POWER_EXTERNAL` and is compiled in only on chips that support it (y for the ESP32-P4); the LDO is set up before the SDMMC host is initialized.
- The ESP32-P4 has two SDMMC slots: the SD card is fixed to **Slot 0**, the ESP32-C6 WiFi module uses **Slot 1** (SDIO, 4-bit bus). The slot number is hard-coded and not configurable.
- Mount point, max open files, allocation unit size and format-on-failure all come from the config; `CONFIG_SDMMC_DRV_MOUNT_POINT` defaults to `/sdcard`, and other components in this project (such as `uvc_capture_card_driver`) build their paths as `"/sdcard/..."` too.
- When `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE` is enabled (ESP-Hosted over SDIO) and IDF ≥ 6.0, the component replaces `host.init` / `host.deinit` with no-ops, because an IDF v6.0+ SDMMC host controller can only be initialized once and ESP-Hosted has already done it. This project's `sdkconfig` is exactly that combination.
- The card-detect (CD) and write-protect (WP) signals are not wired up. The component enables internal pull-ups (`SDMMC_SLOT_FLAG_INTERNAL_PULLUP`), but the code comments state plainly that these are not sufficient: 10 kΩ external pull-ups are still required on the bus.
- On chips without a GPIO matrix the pins are fixed by IOMUX, and the `clk_pin` / `cmd_pin` / `d0_pin` / `d1_pin` / `d2_pin` / `d3_pin` fields have no effect.
- `sdmmc_driver_read_file()` is built on `fgets()`: it reads only the first line, truncated to at most `buf_size - 1` bytes, and does not report the actual length; a trailing newline is stripped from the returned string. Use the standard C file API to read whole buffers.
- `CONFIG_SDMMC_DRV_MAX_FREQ_KHZ` defaults to 40 MHz; the Kconfig help text suggests dropping to 20000 if signal integrity becomes marginal.
- The component requires `fatfs`, `driver` and `esp_timer` (see `CMakeLists.txt`).
