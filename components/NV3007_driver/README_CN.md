# NV3007_driver — NV3007 SPI LCD 驱动

## 简介

NV3007 芯片的 SPI LCD 驱动。142×428 分辨率，通过硬件 SPI 驱动。

## 方向配置

```c
#define USE_HORIZONTIAL  2   // 0=竖屏 1=竖屏180° 2=横屏 3=横屏
#define LANDSCAPE_ALT    1   // 横屏下两个方向，不对就换
```

## 竖屏/横屏自动计算

- 竖屏：LCD_W=142, LCD_H=428
- 横屏：LCD_W=428, LCD_H=142

## 引脚（默认）

| 引脚 | 功能 |
|------|------|
| GPIO20 | MOSI |
| GPIO21 | CLK |
| GPIO22 | CS |
| GPIO4 | DC |
| GPIO5 | RST |
| GPIO6 | BLK |

SPI 主机：SPI2_HOST，80MHz，模式 0。

## API

提供点、线、圆、矩形、三角形、字符（12/16/24pt）、数字、图片等绘制函数。

## 注意事项

- 使用 `USE_HORIZONTIAL` 和 `LANDSCAPE_ALT` 控制画面方向，MADCTL 寄存器 (0x36) 自动配置
- 字体数据嵌入在 `lcd_font.h`，三种字号按需选择
- SPI 使用 DMA 模式传输（`SPI_DMA_CH_AUTO`），CS 由软件 pre/post callback 控制
- 与 SDMMC 引脚不冲突
