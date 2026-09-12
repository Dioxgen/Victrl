# wifi_manager — WiFi 管理 (C6 协处理)

## 简介

通过 ESP-Hosted SDIO 协议与 ESP32-C6 协处理器通信，使用标准 esp_wifi API 透明控制 C6 上的 WiFi 硬件。

## 架构

```
ESP32-P4                    ESP32-C6
┌──────────┐   SDIO   ┌──────────────┐
│ WiFi STA │◄────────►│ ESP-Hosted   │
│ (lwIP)   │          │ Slave FW     │
│          │          │ + WiFi Radio │
└──────────┘          └──────────────┘
```

`esp_wifi_remote` 组件将 P4 侧的 `esp_wifi_*()` 调用透明转发到 C6 执行。

## 初始化流程

1. `esp_netif_init()` + `esp_event_loop_create_default()` + `esp_netif_create_default_wifi_sta()`
2. `esp_hosted_init()` — SDIO 传输初始化（内部自动调用）
3. `esp_wifi_init()` — 实际调用 `esp_wifi_remote_init()`
4. 注册 WiFi/IP 事件回调
5. `esp_wifi_set_mode(WIFI_MODE_STA)` → `esp_wifi_set_config()` → `esp_wifi_start()`
6. 等待连接或超时（30s）

## 注意事项

- ESP32-P4 没有内置 WiFi，完全依赖 C6
- C6 必须预先烧录 ESP-Hosted Slave 固件
- SDIO 使用 Slot 1，SD 卡使用 Slot 0，两者不冲突
- 连接失败有 5 次自动重试
- NTP 时间同步在 WiFi 连接成功后执行
