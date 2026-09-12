# uvc_capture_card_driver — UVC 采集卡驱动

> **中文** | [English](README.md)

## 简介

**关于来源，因为这一点很容易丢失：** 这里 20 个 `.c`/`.h` 中有 18 个是乐鑫自己的 UVC host 代码（`uvc_host`、`uvc_stream`、`uvc_bulk`、`uvc_isoc`、`uvc_control`、`uvc_frame`、描述符解析、各个私有头文件以及 `usb_types_uvc.h`），以 Apache-2.0 发布，每个文件都带 `SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD`——这些版权头是刻意保留的，必须留着。只有 `uvc_capture_card_driver.c` 与其头文件是本项目自己的封装，也就是仅有的两个不含乐鑫版权头的文件。

USB Host UVC 采集驱动，把 USB Host、UVC 协议栈与 `sdmmc_driver` 串成一条流水线：一次调用完成 SD 卡、USB Host、UVC 驱动的初始化，随后常开一路 UVC 流，调用方以阻塞方式逐帧取图。VID/PID、分辨率、帧率、格式与缓冲区全部由调用方通过 `uvc_host_stream_config_t` 传入，组件本身不绑定任何特定采集芯片；当前项目用它接 MS2109（`0x534D:0x2109`）。

## API

| 函数 | 说明 |
|------|------|
| `uvc_capture_init(stream_cfg)` | 依次初始化 SD 卡、USB Host 与事件任务、UVC 驱动，打开并启动流，丢弃 3 帧预热；设备未接入时返回错误 |
| `uvc_capture_deinit()` | 停流、关流、卸载 UVC 驱动与 USB Host、删除帧信号量，并卸载本组件自己挂载的 SD 卡 |
| `uvc_capture_one_frame(&data, &len, timeout_ms)` | 阻塞等待下一帧，帧数据拷贝到 SPIRAM，调用方负责 `free()`；`timeout_ms` 单位为毫秒，0 表示不限时 |
| `uvc_capture_one_frame_sig(&data, &len, &sig, timeout_ms)` | 同上，并返回这一帧的 `uvc_frame_sig_t`（字节数 + FNV-1a 哈希） |
| `uvc_capture_frame_sig(&sig, timeout_ms)` | 只计算下一帧的指纹，不拷贝数据，帧立即交还驱动 |
| `uvc_capture_save_to_sd(name, data, len)` | 以 `/sdcard/<name>` 写入二进制文件，完整路径上限 128 字节 |

`uvc_capture_init()` 会覆盖传入配置中的 `event_cb`、`frame_cb` 与 `user_ctx`，其余字段原样使用。组件同时公开 `include/usb/uvc_host.h` 的完整 UVC Host API（`uvc_host_install()`、`uvc_host_stream_open()`、`uvc_host_stream_start()`、`uvc_host_stream_format_select()`、`uvc_host_get_frame_list()`、`uvc_host_desc_print()` 等）。

## 取帧模型

内部帧回调只保留最新一帧：新帧到来时上一帧立即交还驱动，仅当当前没有未取走的帧时才释放一次信号量，因此帧缓冲不会因为应用取帧慢而被耗空。应用侧的 `uvc_capture_*` 系列都是阻塞读取；`uvc_capture_one_frame*()` 会再拷贝一份给调用方，`uvc_capture_frame_sig()` 不拷贝。

传输路径按流式端点的传输类型自动选择 ISOC 或 BULK。MJPEG 的两处兼容处理——裁掉 SOI（`0xFF 0xD8`）之前的垃圾字节、在下一个 SoF 到来时补全没有 EoF 位的帧——都只存在于 ISOC 路径；BULK 路径反过来要求每帧首包必须以 SOI 开头。

## Kconfig

`menuconfig` → **USB HOST UVC**：

| 符号 | 默认值 | 说明 |
|------|--------|------|
| `CONFIG_UVC_PRINTF_CONFIGURATION_DESCRIPTOR` | n | 打印 UVC 配置描述符 |
| `CONFIG_UVC_INTERVAL_ARRAY_SIZE` | 3 | `uvc_host_frame_info_t` 中离散帧间隔数组的大小 |
| `CONFIG_UVC_CHECK_PAYLOAD_HEADER_EOH` | y | 校验载荷头的 EOH 位 |

## 注意事项

- 组件不定义任何 USB Host 相关 Kconfig。项目根目录 `sdkconfig.defaults` 中的 `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=3000` 与 `CONFIG_USB_HOST_HW_BUFFER_BIAS_IN=y` 由 ESP-IDF 的 USB Host 库消费，不是本组件的选项。
- USB PHY 由 USB Host 库自行配置（`usb_init()` 中 `skip_phy_setup = false`），组件不做外部 PHY 或 Hub 的设置。
- 帧缓冲区大小与数量由调用方决定：`frame_size` 为 0 时回退到协商得到的 `dwMaxVideoFrameSize`，`frame_heap_caps` 为 0 时回退到 `MALLOC_CAP_DEFAULT`。当前项目在 `main/main.c` 中配置 6 个 1 MB 帧缓冲（`MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA`）与 4 个 URB，分辨率取自 `/sdcard/config.json` 的 `capture_width` / `capture_height`。
- 若 SD 卡已由外部挂载（`/sdcard` 目录存在），`uvc_capture_init()` 会跳过 SD 初始化，此时 `uvc_capture_deinit()` 也不会卸载它。
- 缺 EoF 位的设备只在 ISOC + MJPEG 组合下靠“下一个 SoF 补全上一帧”挽救；BULK 路径没有这层补全，源码注释说明它以丢弃末包为满 MPS 的帧为代价来判断 EoF。
- 帧溢出与欠溢出只通过流事件回调打印日志（`Frame overflow` / `Frame underflow`），溢出时该帧被丢弃。
- 取帧超时返回 `ESP_ERR_TIMEOUT`；未初始化或流未启动时返回 `ESP_ERR_INVALID_STATE`。
- 依赖 `sdmmc_driver`（CMakeLists 中的 `REQUIRES`）与 `usb` ^1.0.0（`idf_component.yml`）。
