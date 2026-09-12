# sdmmc_driver — SDMMC SD 卡驱动

## 简介

基于 ESP-IDF SDMMC 外设的 SD 卡驱动，提供 FatFS 文件系统挂载和基础文件操作。

## API

| 函数 | 说明 |
|------|------|
| `sdmmc_driver_init(config, &card)` | 初始化 SDMMC + 挂载 FatFS |
| `sdmmc_driver_deinit(mount, card)` | 卸载文件系统 |
| `sdmmc_driver_write_file(path, data)` | 写入文本文件 |
| `sdmmc_driver_write_binary_file(path, data, len)` | 写入二进制文件 |
| `sdmmc_driver_read_file(path, buf, size)` | 读取文件 |
| `sdmmc_driver_get_default_config(&cfg)` | 获取默认引脚和配置 |

## 配置

Kconfig 可配项：引脚、总线宽度、频率、挂载点、LDO 电源控制。

## 注意事项

- ESP32-P4 有两个 SDMMC 槽位。SD 卡使用 **Slot 0**，C6 WiFi 使用 **Slot 1**
- 与 ESP-Hosted 共存时使用 `WORKAROUND_HOSTED_SDMMC`（跳过 SDMMC 控制器初始化，由 ESP-Hosted 管理）
- 挂载点默认 `/sdcard`
- 支持片上 LDO 供电（ESP32-P4 DEV-KIT）
