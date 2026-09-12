# uvc_capture_card_driver — UVC 采集卡驱动

## 简介

USB Host UVC 驱动，专门适配 MS2109 HDMI 转 USB 采集卡。提供连续 MJPEG 流捕获。

## API

| 函数 | 说明 |
|------|------|
| `uvc_capture_init(stream_cfg)` | 初始化 SD 卡 + USB Host + UVC 流 |
| `uvc_capture_one_frame(&data, &len, timeout)` | 阻塞等待一帧 MJPEG |
| `uvc_capture_deinit()` | 反初始化 |
| `uvc_capture_save_to_sd(name, data, len)` | 保存帧到 SD 卡 |

## MJPEG 处理

- 自动去除 JPEG SOI (0xFF 0xD8) 前的垃圾字节
- 帧数据从 SPIRAM 分配（`MALLOC_CAP_SPIRAM`）
- 持续流保持 MS2109 HDMI 接收器锁定

## 设备配置

MS2109: VID=0x534D, PID=0x2109

## 注意事项

- USB Host 配置 `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=3000`（必须，MS2109 描述符很大）
- USB 缓冲区偏向 IN 传输（`CONFIG_USB_HOST_HW_BUFFER_BIAS_IN=y`）
- SD 卡如果已挂载（`/sdcard` 目录存在），内部 SD 初始化自动跳过
- 帧缓冲区大小 1MB（1080p MJPEG 可能超过 500KB）
