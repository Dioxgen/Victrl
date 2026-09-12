# NV3007_driver — NV3007 SPI LCD 驱动

> **中文** | [English](README.md)

## 简介

面向 ESP32-P4 的 NV3007 SPI LCD 驱动，面板分辨率 142×428；坐标系由 `USE_HORIZONTIAL` 在编译期决定，当前配置为横屏，即 `LCD_W=428`、`LCD_H=142`。总线使用 `esp_driver_spi` 的 `SPI2_HOST`，80 MHz、SPI 模式 0（CPOL=0、CPHA=0）、半双工，CS 由传输的 pre/post 回调用软件控制，DC/RST/BLK 是普通 GPIO。驱动提供填充、点、线、矩形、圆、三角形、ASCII 字符/数字/位图绘制以及睡眠唤醒接口；字库是编译进 `lcd_font.h` 的 6×12、8×16、12×24 ASCII 点阵，没有中文字库。

## 方向配置

```c
#define USE_HORIZONTIAL  2   /* 0:竖屏  1:竖屏180°  2:横屏  3:横屏 */
#define LANDSCAPE_ALT    1   /* 0或1: 横屏下的两个方向，不对就换 */
```

这两个宏定义在 `include/NV3007_driver.h`，没有 `#ifndef` 保护，只能改头文件；编译期用 `-D` 覆盖会触发宏重定义，实际生效的仍是头文件里的值。分辨率、偏移量作用的坐标轴以及 `NV3007_Init()` 写入的 MADCTL（0x36）取值都由它们决定：竖屏 `LCD_W=142`、`LCD_H=428`，横屏 `LCD_W=428`、`LCD_H=142`；当前配置（`2`/`1`）为横屏，`0x36=0xA0`。运行期不能改方向，必须改宏后重新编译。

## 引脚（默认）

| 信号 | 宏 | 引脚 |
|------|-----|------|
| MOSI | `NV3007_PIN_MOSI` | GPIO20 |
| CLK | `NV3007_PIN_CLK` | GPIO21 |
| CS | `NV3007_PIN_CS` | GPIO22 |
| DC | `NV3007_PIN_DC` | GPIO4 |
| RST | `NV3007_PIN_RST` | GPIO5 |
| BLK | `NV3007_PIN_BLK` | GPIO6 |

引脚宏都有 `#ifndef` 保护，可以用编译期 `-D` 覆盖，不必改头文件。

| 总线参数 | 宏 / 位置 | 值 |
|----------|-----------|-----|
| SPI 主机 | `NV3007_SPI_HOST` | `SPI2_HOST` |
| 时钟频率 | `NV3007_SPI_CLOCK_HZ` | 80 MHz |
| SPI 模式 | `spi_device_interface_config_t.mode` | 0（CPOL=0、CPHA=0） |
| 设备选项 | `spi_device_interface_config_t` | `SPI_DEVICE_HALFDUPLEX`、`spics_io_num=-1`、`miso_io_num=-1`、`queue_size=4` |
| DMA | `spi_bus_initialize()` | `SPI_DMA_CH_AUTO`，`max_transfer_sz=NV3007_MAX_BUF_SIZE` |
| 面板偏移 | `LCD_X_OFFSET` / `LCD_Y_OFFSET` | `0x0C` / `0x00`（横屏时 `0x0C` 作用于行方向） |
| 缓冲区 | `NV3007_BUF_SIZE` / `NV3007_MAX_BUF_SIZE` | `LCD_W*10*2` / `LCD_W*LCD_H*2` |

## API

| 函数 | 说明 |
|------|------|
| `NV3007_Init(void)` | 初始化 SPI 总线与 GPIO、硬件复位、下发完整寄存器序列并开显示 |
| `NV3007_Deinit(void)` | 进入睡眠、关背光、`spi_bus_free()` 释放总线 |
| `NV3007_WriteReg(uint8_t reg)` | 写命令（DC 拉低） |
| `NV3007_WriteByte(uint8_t dat)` | 写 1 字节数据 |
| `NV3007_WriteHalfWord(uint16_t dat)` | 写 2 字节数据，高字节先发 |
| `NV3007_WriteData(const uint8_t *dat, int len)` | 写 `len` 字节数据 |
| `NV3007_SetWindow(xs, ys, xe, ye)` | 设置显示窗口（0x2A/0x2B）并进入显存写（0x2C） |
| `NV3007_Fill(xs, ys, xe, ye, color)` | 区域填充，坐标裁剪到屏内 |
| `NV3007_FastFill(color)` | 全屏填充 |
| `NV3007_DrawPoint(x, y, color)` | 单点 |
| `NV3007_DrawLine(xs, ys, xe, ye, color)` | 直线，水平/垂直线自动走 `FillHLine`/`Fill` 快速路径 |
| `NV3007_FillHLine(x, y, w, color)` | 宽 `w` 的水平线，一条窗口命令加分块传输 |
| `NV3007_DrawRectangle(xs, ys, xe, ye, color)` | 空心矩形 |
| `NV3007_DrawFillRectangle(xs, ys, xe, ye, color)` | 实心矩形，内部就是 `NV3007_Fill()` |
| `NV3007_DrawCircle(xc, yc, r, color, mode)` | 圆，`mode` 非 0 为实心 |
| `NV3007_DrawTriangle(x, y, xs, ys, xe, ye, color)` | 空心三角形 |
| `NV3007_ShowChar(x, y, num, fc, bc, sizey, mode)` | 单字符，`sizey` 只能取 12/16/24 |
| `NV3007_ShowString(x, y, s, fc, bc, sizey, mode)` | 字符串，遇到非 ASCII 可打印字节即停止 |
| `NV3007_ShowStringCenter(y, s, fc, bc, sizey, mode)` | 按 `strlen` 水平居中 |
| `NV3007_ShowNum(x, y, num, len, fc, bc, sizey, mode)` | 定宽整数，前导零显示为空格 |
| `NV3007_ShowFloatNum(x, y, num, pre, len, fc, bc, sizey, mode)` | 浮点数，`pre` 位小数，`len` 为不含小数点的总位数 |
| `NV3007_ShowPicture(x, y, width, height, pic)` | RGB565 大端位图，越界则不绘制 |
| `NV3007_EnterSleep(void)` | 写 0x28、0x10 进入睡眠 |
| `NV3007_ExitSleep(void)` | 写 0x11、0x29 唤醒并开显示 |

## 注意事项

- `NV3007_Init()` 内含硬件复位（RST 高 10 ms、低 10 ms、高 120 ms）和完整寄存器序列：`0xFF=0xA5` 解锁，电源、gamma、GOA、栅极等寄存器，MADCTL（0x36），`0xFF=0x00` 加锁，`0x3A=0x05` 设为 RGB565，最后 `0x11` 后延时 220 ms、`0x29` 后延时 200 ms。
- `NV3007_Init()` 只能调用一次：总线已初始化时 `spi_bus_initialize()` 返回 `ESP_ERR_INVALID_STATE`，而调用点用 `ESP_ERROR_CHECK()` 包裹，会直接中止程序。
- `NV3007_Deinit()` 会进入睡眠、拉低 BLK 并释放 SPI 总线；释放后要继续绘图必须先重新 `NV3007_Init()`。
- 坐标裁剪不一致：`NV3007_Fill()`、`NV3007_FillHLine()` 会把坐标限制在屏内，`NV3007_ShowPicture()` 越界时直接返回不绘制，而 `NV3007_DrawPoint()` / `NV3007_DrawLine()` 完全不裁剪，调用方要自己保证坐标合法。
- `NV3007_ShowChar()` 只认 `sizey` 为 12、16、24，其它值静默返回、什么都不画。
- 字库只有 ASCII 0x20~0x7E：`NV3007_ShowString()` 在第一个超出该范围的字节处停止，UTF-8 中文不会显示；`NV3007_ShowChar()` 直接把字符减去 `' '` 当数组下标用，传入非 ASCII 字符会越界读字库。
- `mode` 参数：0 为非叠加模式，每个字符行一次 SPI 传输并写入背景色 `bc`；非 0 为叠加模式，只画前景点，逐像素调用 `NV3007_DrawPoint()`，开销更大。
- 颜色是 RGB565 16 位。头文件提供 `NV3007_` 前缀的整套颜色宏，另有 `WHITE`/`BLACK`/`RED` 等无前缀向后兼容别名；这些别名是全局宏，与其它组件重名会互相覆盖。
- `NV3007_ShowPicture()` 的数据按行优先、每像素 2 字节的 RGB565 大端排列，长度必须是 `width*height*2`，驱动不做字节序转换。
- 大块填充复用静态缓冲：`NV3007_Fill()` 在像素数大于 32 时按 `NV3007_BUF_SIZE` 分块发送，否则逐像素 `NV3007_WriteHalfWord()`；缓冲区 `g_buf` 是 .bss 里的静态数组，不占 PSRAM。驱动内部没有互斥锁，`g_buf` 被所有调用共享，不要从多个任务并发调用绘图 API（本项目只有 `display_task` 在用）。
- SPI 传输是同步轮询 `spi_device_polling_transmit()`，总线以 `SPI_DMA_CH_AUTO` 申请 DMA 通道；CS 由 `pre_cb`/`post_cb` 在每次传输前后拉低、拉高，未使用硬件 CS 和 MISO 引脚。
- 引脚与项目内其它外设不冲突：SDMMC 用 GPIO39~44，ESP-Hosted SDIO 用 GPIO14~19 加复位 GPIO54，按键用 GPIO0/GPIO1。
