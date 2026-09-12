/**
 * @file    NV3007_driver.h
 * @brief   NV3007 LCD Driver for ESP32P4
 */

#ifndef _NV3007_DRIVER_H_
#define _NV3007_DRIVER_H_

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * 显示方向配置
 *============================================================================*/
#define USE_HORIZONTIAL  2   /* 0:竖屏  1:竖屏180°  2:横屏  3:横屏 */
#define LANDSCAPE_ALT    1   /* 0或1: 横屏下的两个方向，不对就换 */

/*============================================================================
 * 屏幕分辨率 (根据方向自动计算)
 *============================================================================*/
#if USE_HORIZONTIAL == 0 || USE_HORIZONTIAL == 1
    #define LCD_W  142
    #define LCD_H  428
#else
    #define LCD_W  428
    #define LCD_H  142
#endif

/*============================================================================
 * 显示偏移量调整
 *============================================================================*/
#define LCD_X_OFFSET  0x0C   /* 列偏移量 */
#define LCD_Y_OFFSET  0x00   /* 行偏移量 */

/*============================================================================
 * 引脚定义 (ESP32P4 引脚分配)
 *============================================================================*/
#ifndef NV3007_PIN_MOSI
    #define NV3007_PIN_MOSI   GPIO_NUM_20
#endif
#ifndef NV3007_PIN_CLK
    #define NV3007_PIN_CLK    GPIO_NUM_21
#endif
#ifndef NV3007_PIN_CS
    #define NV3007_PIN_CS     GPIO_NUM_22
#endif
#ifndef NV3007_PIN_DC
    #define NV3007_PIN_DC     GPIO_NUM_4
#endif
#ifndef NV3007_PIN_RST
    #define NV3007_PIN_RST    GPIO_NUM_5
#endif
#ifndef NV3007_PIN_BLK
    #define NV3007_PIN_BLK    GPIO_NUM_6
#endif

/* SPI 主机选择 */
#ifndef NV3007_SPI_HOST
    #define NV3007_SPI_HOST   SPI2_HOST
#endif

/* SPI 时钟频率 */
#ifndef NV3007_SPI_CLOCK_HZ
    #define NV3007_SPI_CLOCK_HZ   (80 * 1000 * 1000)
#endif

/*============================================================================
 * 引脚控制宏
 *============================================================================*/
#define NV3007_CS_Set()   gpio_set_level(NV3007_PIN_CS, 1)
#define NV3007_CS_Clr()   gpio_set_level(NV3007_PIN_CS, 0)
#define NV3007_DC_Set()   gpio_set_level(NV3007_PIN_DC, 1)
#define NV3007_DC_Clr()   gpio_set_level(NV3007_PIN_DC, 0)
#define NV3007_RST_Set()  gpio_set_level(NV3007_PIN_RST, 1)
#define NV3007_RST_Clr()  gpio_set_level(NV3007_PIN_RST, 0)
#define NV3007_BLK_Set()  gpio_set_level(NV3007_PIN_BLK, 1)
#define NV3007_BLK_Clr()  gpio_set_level(NV3007_PIN_BLK, 0)

/*============================================================================
 * 内部缓冲区大小
 *============================================================================*/
#define NV3007_BUF_SIZE      (LCD_W * 10 * 2)
#define NV3007_MAX_BUF_SIZE  (LCD_W * LCD_H * 2)

/*============================================================================
 * 颜色定义 (RGB565)
 *============================================================================*/
#define NV3007_WHITE        0xFFFF
#define NV3007_BLACK        0x0000
#define NV3007_BLUE         0x001F
#define NV3007_BRED         0xF81F
#define NV3007_GRED         0xFFE0
#define NV3007_GBLUE        0x07FF
#define NV3007_RED          0xF800
#define NV3007_MAGENTA      0xF81F
#define NV3007_GREEN        0x07E0
#define NV3007_CYAN         0x7FFF
#define NV3007_YELLOW       0xFFE0
#define NV3007_BROWN        0xBC40
#define NV3007_BRRED        0xFC07
#define NV3007_GRAY         0x8430
#define NV3007_DARKBLUE     0x01CF
#define NV3007_LIGHTBLUE    0x7D7C
#define NV3007_GRAYBLUE     0x5458
#define NV3007_LIGHTGREEN   0x841F
#define NV3007_LGRAY        0xC618
#define NV3007_LGRAYBLUE    0xA651
#define NV3007_LBBLUE       0x2B12

/* 向后兼容的颜色别名 */
#define WHITE      NV3007_WHITE
#define BLACK      NV3007_BLACK
#define BLUE       NV3007_BLUE
#define RED        NV3007_RED
#define MAGENTA    NV3007_MAGENTA
#define GREEN      NV3007_GREEN
#define CYAN       NV3007_CYAN
#define YELLOW     NV3007_YELLOW
#define BROWN      NV3007_BROWN
#define GRAY       NV3007_GRAY
#define LIGHTBLUE  NV3007_LIGHTBLUE
#define LIGHTGREEN NV3007_LIGHTGREEN

/*============================================================================
 * API 函数声明
 *============================================================================*/

/* --- 初始化与反初始化 --- */
void NV3007_Init(void);
void NV3007_Deinit(void);

/* --- 底层 SPI 写操作 --- */
void NV3007_WriteReg(uint8_t reg);
void NV3007_WriteByte(uint8_t dat);
void NV3007_WriteHalfWord(uint16_t dat);
void NV3007_WriteData(const uint8_t *dat, int len);

/* --- 显示窗口设置 --- */
void NV3007_SetWindow(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye);

/* --- 填充 --- */
void NV3007_Fill(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t color);
void NV3007_FastFill(uint16_t color);

/* --- 画点画线 --- */
void NV3007_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void NV3007_DrawLine(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t color);
void NV3007_FillHLine(uint16_t x, uint16_t y, uint16_t w, uint16_t color);  /* 快速水平线填充 */

/* --- 矩形 --- */
void NV3007_DrawRectangle(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t color);
void NV3007_DrawFillRectangle(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t color);

/* --- 圆形 --- */
void NV3007_DrawCircle(uint16_t xc, uint16_t yc, uint16_t r, uint16_t color, uint16_t mode);

/* --- 三角形 --- */
void NV3007_DrawTriangle(uint16_t x, uint16_t y, uint16_t xs, uint16_t ys,
                         uint16_t xe, uint16_t ye, uint16_t color);

/* --- 字符显示 --- */
void NV3007_ShowChar(uint16_t x, uint16_t y, uint8_t num, uint16_t fc,
                     uint16_t bc, uint8_t sizey, uint8_t mode);
void NV3007_ShowString(uint16_t x, uint16_t y, const char *s, uint16_t fc,
                       uint16_t bc, uint16_t sizey, uint8_t mode);
void NV3007_ShowStringCenter(uint16_t y, const char *s, uint16_t fc,
                             uint16_t bc, uint8_t sizey, uint8_t mode);

/* --- 数字显示 --- */
void NV3007_ShowNum(uint16_t x, uint16_t y, uint32_t num, uint8_t len,
                    uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void NV3007_ShowFloatNum(uint16_t x, uint16_t y, float num, uint8_t pre,
                         uint8_t len, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);

/* --- 图片显示 --- */
void NV3007_ShowPicture(uint16_t x, uint16_t y, uint16_t width,
                        uint16_t height, const uint8_t pic[]);

/* --- 休眠控制 --- */
void NV3007_EnterSleep(void);
void NV3007_ExitSleep(void);

#ifdef __cplusplus
}
#endif

#endif /* _NV3007_DRIVER_H_ */
