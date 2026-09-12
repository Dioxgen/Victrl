# NV3007_driver — NV3007 SPI LCD driver

> **English** | [中文](README_CN.md)

## Overview

An NV3007 SPI LCD driver for the ESP32-P4. The panel is 142×428; the coordinate system is chosen at compile time by `USE_HORIZONTIAL`, and the current configuration is landscape, so `LCD_W=428` and `LCD_H=142`. It uses `SPI2_HOST` from `esp_driver_spi` at 80 MHz, SPI mode 0 (CPOL=0, CPHA=0), half duplex, with CS driven in software from the transfer pre/post callbacks and DC/RST/BLK on plain GPIOs. The driver provides fills, points, lines, rectangles, circles, triangles, ASCII text, numbers and bitmap drawing plus sleep/wake functions; the fonts are 6×12, 8×16 and 12×24 ASCII bitmaps compiled into `lcd_font.h`, with no Chinese font.

## Orientation configuration

```c
#define USE_HORIZONTIAL  2   /* 0:竖屏  1:竖屏180°  2:横屏  3:横屏 */
#define LANDSCAPE_ALT    1   /* 0或1: 横屏下的两个方向，不对就换 */
```

These two macros live in `include/NV3007_driver.h` and have no `#ifndef` guard, so the header is the only place to change them; overriding them with `-D` at build time triggers a macro redefinition and the value in the header still wins. They decide the resolution, which axis the offsets apply to, and the MADCTL (0x36) value written by `NV3007_Init()`: portrait is `LCD_W=142`, `LCD_H=428` and landscape is `LCD_W=428`, `LCD_H=142`; the current configuration (`2`/`1`) is landscape with `0x36=0xA0`. The orientation cannot be changed at run time — edit the macros and rebuild.

## Pins (defaults)

| Signal | Macro | Pin |
|--------|-------|-----|
| MOSI | `NV3007_PIN_MOSI` | GPIO20 |
| CLK | `NV3007_PIN_CLK` | GPIO21 |
| CS | `NV3007_PIN_CS` | GPIO22 |
| DC | `NV3007_PIN_DC` | GPIO4 |
| RST | `NV3007_PIN_RST` | GPIO5 |
| BLK | `NV3007_PIN_BLK` | GPIO6 |

Every pin macro is guarded by `#ifndef`, so it can be overridden with `-D` at build time without editing the header.

| Bus parameter | Macro / location | Value |
|---------------|------------------|-------|
| SPI host | `NV3007_SPI_HOST` | `SPI2_HOST` |
| Clock | `NV3007_SPI_CLOCK_HZ` | 80 MHz |
| SPI mode | `spi_device_interface_config_t.mode` | 0 (CPOL=0, CPHA=0) |
| Device options | `spi_device_interface_config_t` | `SPI_DEVICE_HALFDUPLEX`, `spics_io_num=-1`, `miso_io_num=-1`, `queue_size=4` |
| DMA | `spi_bus_initialize()` | `SPI_DMA_CH_AUTO`, `max_transfer_sz=NV3007_MAX_BUF_SIZE` |
| Panel offsets | `LCD_X_OFFSET` / `LCD_Y_OFFSET` | `0x0C` / `0x00` (in landscape `0x0C` applies to the row axis) |
| Buffers | `NV3007_BUF_SIZE` / `NV3007_MAX_BUF_SIZE` | `LCD_W*10*2` / `LCD_W*LCD_H*2` |

## API

| Function | Description |
|----------|-------------|
| `NV3007_Init(void)` | Initialises the SPI bus and GPIOs, performs the hardware reset, sends the full register sequence and turns the display on |
| `NV3007_Deinit(void)` | Enters sleep, turns the backlight off and releases the bus with `spi_bus_free()` |
| `NV3007_WriteReg(uint8_t reg)` | Writes a command (DC low) |
| `NV3007_WriteByte(uint8_t dat)` | Writes one data byte |
| `NV3007_WriteHalfWord(uint16_t dat)` | Writes two data bytes, high byte first |
| `NV3007_WriteData(const uint8_t *dat, int len)` | Writes `len` data bytes |
| `NV3007_SetWindow(xs, ys, xe, ye)` | Sets the display window (0x2A/0x2B) and enters RAM write (0x2C) |
| `NV3007_Fill(xs, ys, xe, ye, color)` | Area fill, coordinates clipped to the panel |
| `NV3007_FastFill(color)` | Full-screen fill |
| `NV3007_DrawPoint(x, y, color)` | Single pixel |
| `NV3007_DrawLine(xs, ys, xe, ye, color)` | Line; horizontal and vertical lines take the fast `FillHLine`/`Fill` path |
| `NV3007_FillHLine(x, y, w, color)` | Horizontal line of width `w`, one window command plus chunked transfers |
| `NV3007_DrawRectangle(xs, ys, xe, ye, color)` | Outline rectangle |
| `NV3007_DrawFillRectangle(xs, ys, xe, ye, color)` | Filled rectangle, internally just `NV3007_Fill()` |
| `NV3007_DrawCircle(xc, yc, r, color, mode)` | Circle; `mode` non-zero means filled |
| `NV3007_DrawTriangle(x, y, xs, ys, xe, ye, color)` | Outline triangle |
| `NV3007_ShowChar(x, y, num, fc, bc, sizey, mode)` | Single character; `sizey` may only be 12/16/24 |
| `NV3007_ShowString(x, y, s, fc, bc, sizey, mode)` | String; stops at the first non-printable-ASCII byte |
| `NV3007_ShowStringCenter(y, s, fc, bc, sizey, mode)` | Horizontally centred using `strlen` |
| `NV3007_ShowNum(x, y, num, len, fc, bc, sizey, mode)` | Fixed-width integer, leading zeros drawn as spaces |
| `NV3007_ShowFloatNum(x, y, num, pre, len, fc, bc, sizey, mode)` | Float with `pre` decimals; `len` is the total digit count excluding the decimal point |
| `NV3007_ShowPicture(x, y, width, height, pic)` | RGB565 big-endian bitmap; nothing is drawn if it does not fit |
| `NV3007_EnterSleep(void)` | Writes 0x28 and 0x10 to enter sleep |
| `NV3007_ExitSleep(void)` | Writes 0x11 and 0x29 to wake up and turn the display on |

## Notes

- `NV3007_Init()` performs a hardware reset (RST high 10 ms, low 10 ms, high 120 ms) and the full register sequence: `0xFF=0xA5` unlock, the power, gamma, GOA and gate registers, MADCTL (0x36), `0xFF=0x00` lock, `0x3A=0x05` to select RGB565, and finally `0x11` followed by a 220 ms delay and `0x29` followed by a 200 ms delay.
- `NV3007_Init()` may only be called once: on an already-initialised bus `spi_bus_initialize()` returns `ESP_ERR_INVALID_STATE`, and the call site wraps it in `ESP_ERROR_CHECK()`, which aborts.
- `NV3007_Deinit()` enters sleep, drives BLK low and frees the SPI bus; drawing again after that requires a fresh `NV3007_Init()`.
- Clipping is inconsistent: `NV3007_Fill()` and `NV3007_FillHLine()` clamp their coordinates to the panel and `NV3007_ShowPicture()` returns without drawing when it does not fit, but `NV3007_DrawPoint()` and `NV3007_DrawLine()` do not clip at all — callers must pass valid coordinates.
- `NV3007_ShowChar()` only accepts `sizey` values of 12, 16 and 24; any other value returns silently and draws nothing.
- The fonts are ASCII 0x20–0x7E only: `NV3007_ShowString()` stops at the first byte outside that range, so UTF-8 Chinese is never rendered, and `NV3007_ShowChar()` uses the character minus `' '` directly as an array index, so a non-ASCII character reads past the end of the font table.
- The `mode` argument: 0 is the non-overlay mode, which writes the background colour `bc` and sends one SPI transfer per character row; non-zero is the overlay mode, which draws only foreground pixels through `NV3007_DrawPoint()` one pixel at a time and costs more.
- Colours are 16-bit RGB565. The header defines the full set of `NV3007_`-prefixed colour macros plus unprefixed backwards-compatible aliases such as `WHITE`/`BLACK`/`RED`; those aliases are global macros and will silently clash with same-named macros in other components.
- `NV3007_ShowPicture()` expects row-major RGB565 data, two big-endian bytes per pixel, exactly `width*height*2` bytes long; the driver performs no byte swapping.
- Large fills reuse a static buffer: `NV3007_Fill()` sends in `NV3007_BUF_SIZE` chunks when the pixel count exceeds 32 and falls back to per-pixel `NV3007_WriteHalfWord()` otherwise; the `g_buf` buffer is a static array in .bss and does not live in PSRAM. The driver has no mutex and `g_buf` is shared by every call, so do not call the drawing API concurrently from several tasks (in this project only `display_task` uses it).
- SPI transfers are synchronous polling via `spi_device_polling_transmit()`, and the bus requests a DMA channel with `SPI_DMA_CH_AUTO`; CS is driven low and high around each transfer by `pre_cb`/`post_cb`, with no hardware CS and no MISO pin in use.
- The pins do not collide with anything else in the project: SDMMC uses GPIO39–44, ESP-Hosted SDIO uses GPIO14–19 plus reset GPIO54, and the buttons use GPIO0/GPIO1.
