# sdmmc_driver — SDMMC SD 卡驱动

> **中文** | [English](README.md)

## 简介

基于 ESP-IDF SDMMC 外设的 SD 卡驱动，负责挂载 FAT 文件系统并提供基础文件读写。组件只封装一层薄接口：一次 `sdmmc_driver_init()` 完成主机初始化、挂载与卡片信息打印，其余函数直接在挂载点上调用标准 C 文件接口。当前项目把它跑在 ESP32-P4 的 **Slot 0** 上，Slot 1 留给 ESP32-C6 WiFi 模块的 ESP-Hosted SDIO 接口。

## 参数

默认值由 Kconfig（menuconfig → **SDMMC Driver**）提供，全部字段经 `sdmmc_driver_get_default_config()` 填入 `sdmmc_driver_config_t`，也可由调用方自行覆盖。

| 参数 | Kconfig 符号 | 默认值 |
|------|--------------|--------|
| CLK 引脚 | `CONFIG_SDMMC_DRV_CLK_PIN` | 43 |
| CMD 引脚 | `CONFIG_SDMMC_DRV_CMD_PIN` | 44 |
| D0 引脚 | `CONFIG_SDMMC_DRV_D0_PIN` | 39 |
| D1 引脚 | `CONFIG_SDMMC_DRV_D1_PIN` | 40（1 位模式下忽略） |
| D2 引脚 | `CONFIG_SDMMC_DRV_D2_PIN` | 41（1 位模式下忽略） |
| D3 引脚 | `CONFIG_SDMMC_DRV_D3_PIN` | 42（1 位模式下忽略） |
| 总线宽度 | `CONFIG_SDMMC_DRV_BUS_WIDTH` | 4（可为 1） |
| 最大时钟频率 | `CONFIG_SDMMC_DRV_MAX_FREQ_KHZ` | 40000（40 MHz 高速模式） |
| 挂载失败时自动格式化 | `CONFIG_SDMMC_DRV_FORMAT_IF_MOUNT_FAILED` | y |
| 挂载点 | `CONFIG_SDMMC_DRV_MOUNT_POINT` | `/sdcard` |
| 最大同时打开文件数 | `CONFIG_SDMMC_DRV_MAX_FILES` | 5 |
| FAT 分配单元大小 | `CONFIG_SDMMC_DRV_ALLOC_UNIT_SIZE` | 16384 字节 |
| 片上 LDO 供电 | `CONFIG_SDMMC_DRV_LDO_ENABLE` | y |
| LDO 通道号 | `CONFIG_SDMMC_DRV_LDO_CHAN_ID` | 4（仅 `LDO_ENABLE` 为 y 时可配） |

## API

| 函数 | 说明 |
|------|------|
| `sdmmc_driver_init(config, &card)` | 初始化 SDMMC 主机（Slot 0）并挂载 FAT 文件系统，成功后打印卡片信息 |
| `sdmmc_driver_deinit(mount_point, card)` | 卸载文件系统并释放卡片资源 |
| `sdmmc_driver_write_file(path, data)` | 以文本模式写入字符串；`fclose()` 的返回值即真实写卡结果 |
| `sdmmc_driver_write_binary_file(path, data, len)` | 以二进制模式写入 `len` 字节，逐字节校验写入量 |
| `sdmmc_driver_read_file(path, buffer, buf_size)` | 读取文件 |
| `sdmmc_driver_get_default_config(&cfg)` | 用 Kconfig 值填充配置结构体 |
| `sdmmc_driver_speed_test(mount_point, file_path, file_size_kb, chunk_size)` | 按 `chunk_size` 分块写入 `file_size_kb` 后回读，打印 KB/s 吞吐，结束时删除测试文件 |

入参为空指针（或 `len`、`buf_size` 为 0）时返回 `ESP_ERR_INVALID_ARG`；文件打开、读写或 `fclose()` 失败时返回 `ESP_FAIL`；速度测试的缓冲区分配失败返回 `ESP_ERR_NO_MEM`。

## 注意事项

- **这是一块板子专用的配置，而不是通用驱动。** 默认引脚（CLK=43 / CMD=44 / D0–D3=39–42）、`CONFIG_SDMMC_DRV_LDO_ENABLE=y` 与 LDO 通道号 4 都来自 ESP32-P4-WIFI6-DEV-KIT，与 ESP-IDF 给 ESP32-P4 的 GPIO 矩阵默认值一致。换板子必须改这两项：没有片上 LDO 的板子把 `CONFIG_SDMMC_DRV_LDO_ENABLE` 设为 n，否则 `sd_pwr_ctrl_new_on_chip_ldo()` 会失败并让 `sdmmc_driver_init()` 直接返回错误。
- 片上 LDO 相关代码由 `SOC_SDMMC_IO_POWER_EXTERNAL` 保护，只有支持该能力的芯片（ESP32-P4 为 y）才编译进去；LDO 会在 SDMMC 主机初始化之前建立。
- ESP32-P4 有两个 SDMMC 槽位：SD 卡固定使用 **Slot 0**，ESP32-C6 WiFi 模块使用 **Slot 1**（SDIO，4 位总线）。槽位号在代码中写死，不可配置。
- 挂载点、最大文件数、分配单元大小、是否自动格式化均取自配置；`CONFIG_SDMMC_DRV_MOUNT_POINT` 的默认值是 `/sdcard`，本项目的其他组件（如 `uvc_capture_card_driver`）也按 `/sdcard/` 前缀拼接自己的路径。
- 同时打开 `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE`（ESP-Hosted 走 SDIO）且 IDF ≥ 6.0 时，组件会把 `host.init` / `host.deinit` 换成空实现，因为 IDF v6.0+ 的 SDMMC 主机控制器只能初始化一次，控制器已由 ESP-Hosted 初始化。当前项目的 `sdkconfig` 正是这个组合。
- 不接线卡检测（CD）与写保护（WP）信号。组件打开内部上拉（`SDMMC_SLOT_FLAG_INTERNAL_PULLUP`），但代码注释明确说明内部上拉不够，信号线上仍应接 10 kΩ 外部上拉。
- 未使用 GPIO 矩阵的芯片上，引脚由 IOMUX 固定，`clk_pin` / `cmd_pin` / `d0_pin` / `d1_pin` / `d2_pin` / `d3_pin` 字段不生效。
- `sdmmc_driver_read_file()` 基于 `fgets()`：只读取第一行，最多 `buf_size - 1` 字节并截断，且不回传实际长度；返回的字符串会去掉结尾换行符。读整块数据请直接用标准 C 文件接口。
- `CONFIG_SDMMC_DRV_MAX_FREQ_KHZ` 默认 40 MHz；若信号完整性变差，Kconfig 帮助文本建议降到 20000。
- 组件依赖 `fatfs`、`driver`、`esp_timer`（见 `CMakeLists.txt`）。
