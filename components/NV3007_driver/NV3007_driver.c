/**
 * @file    NV3007_driver.c
 * @brief   NV3007 LCD Driver for ESP32P4 - optimized hardware SPI
 *
 * SPI mode 0 (CPOL=0, CPHA=0), 80 MHz, software CS via pre/post callback.
 * Uses DMA for large transfers, row-buffered glyph rendering.
 */

#include "NV3007_driver.h"
#include "lcd_font.h"
#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "NV3007";
static spi_device_handle_t g_spi;
static uint8_t  g_buf[NV3007_BUF_SIZE];
static uint16_t g_fc;
static bool     g_br;

/* ---- helpers ---- */
static uint32_t p10(uint8_t n) { uint32_t r = 1; while (n--) r *= 10; return r; }

/* pre/post callbacks for software CS */
static void IRAM_ATTR cs_low(spi_transaction_t *t)  { NV3007_CS_Clr(); }
static void IRAM_ATTR cs_high(spi_transaction_t *t) { NV3007_CS_Set(); }

static void gpo(gpio_num_t p) {
    gpio_config_t c = { .mode = GPIO_MODE_INPUT_OUTPUT,
                        .pull_up_en = GPIO_PULLUP_ENABLE,
                        .pin_bit_mask = 1ULL << p };
    gpio_config(&c);
}

/* ---- low-level SPI ---- */
static void stx(const void *b, size_t n) {
    if (!n) return;
    spi_transaction_t t = { .length = n * 8, .tx_buffer = b };
    ESP_ERROR_CHECK(spi_device_polling_transmit(g_spi, &t));
}

void NV3007_WriteReg(uint8_t r)      { NV3007_DC_Clr(); stx(&r, 1); NV3007_DC_Set(); }
void NV3007_WriteByte(uint8_t d)     { NV3007_DC_Set(); stx(&d, 1); }
void NV3007_WriteHalfWord(uint16_t d) { uint8_t b[2] = { d>>8, d&0xFF }; NV3007_DC_Set(); stx(b, 2); }
void NV3007_WriteData(const uint8_t *d, int n) { NV3007_DC_Set(); stx(d, n); }

/* ---- window ---- */
void NV3007_SetWindow(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye) {
#if USE_HORIZONTIAL == 0 || USE_HORIZONTIAL == 1
    NV3007_WriteReg(0x2A); NV3007_WriteHalfWord(xs + LCD_X_OFFSET); NV3007_WriteHalfWord(xe + LCD_X_OFFSET);
    NV3007_WriteReg(0x2B); NV3007_WriteHalfWord(ys + LCD_Y_OFFSET); NV3007_WriteHalfWord(ye + LCD_Y_OFFSET);
#else
    NV3007_WriteReg(0x2A); NV3007_WriteHalfWord(xs + LCD_Y_OFFSET); NV3007_WriteHalfWord(xe + LCD_Y_OFFSET);
    NV3007_WriteReg(0x2B); NV3007_WriteHalfWord(ys + LCD_X_OFFSET); NV3007_WriteHalfWord(ye + LCD_X_OFFSET);
#endif
    NV3007_WriteReg(0x2C);
}

/* ---- fill buffer (lazy, reused) ---- */
static void ebuf(uint16_t c) {
    if (g_br && c == g_fc) return;
    g_fc = c; g_br = true;
    uint8_t hi = c >> 8, lo = c & 0xFF;
    for (int i = 0; i < NV3007_BUF_SIZE / 2; i++) {
        g_buf[i*2] = hi; g_buf[i*2+1] = lo;
    }
}

/* ---- fill (large: DMA, small: pixel loop) ---- */
void NV3007_Fill(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t c) {
    if (xs >= LCD_W) xs = LCD_W - 1;
    if (xe >= LCD_W) xe = LCD_W - 1;
    if (ys >= LCD_H) ys = LCD_H - 1;
    if (ye >= LCD_H) ye = LCD_H - 1;
    uint32_t n = (uint32_t)(xe-xs+1) * (ye-ys+1);
    NV3007_SetWindow(xs, ys, xe, ye);
    if (n > 32) {
        ebuf(c);
        uint32_t bytes = n * 2;
        while (bytes) {
            uint32_t ch = bytes < NV3007_BUF_SIZE ? bytes : NV3007_BUF_SIZE;
            NV3007_WriteData(g_buf, ch);
            bytes -= ch;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) NV3007_WriteHalfWord(c);
    }
}

void NV3007_FastFill(uint16_t c) { NV3007_Fill(0, 0, LCD_W-1, LCD_H-1, c); }

/* ---- point ---- */
void NV3007_DrawPoint(uint16_t x, uint16_t y, uint16_t c) {
    NV3007_SetWindow(x, y, x, y); NV3007_WriteHalfWord(c);
}

/**
 * @brief 快速水平线段填充 — 一条 SetWindow + DMA 块传输
 */
void NV3007_FillHLine(uint16_t x, uint16_t y, uint16_t w, uint16_t c) {
    if (x+w > LCD_W) w = LCD_W - x;
    if (!w) return;
    NV3007_SetWindow(x, y, x+w-1, y);
    ebuf(c);
    uint32_t bytes = (uint32_t)w * 2;
    while (bytes) {
        uint32_t ch = bytes < NV3007_BUF_SIZE ? bytes : NV3007_BUF_SIZE;
        NV3007_WriteData(g_buf, ch);
        bytes -= ch;
    }
}

/* ---- line (Bresenham) — 水平线走快速路径 ---- */
void NV3007_DrawLine(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t c) {
    if (ys == ye) { NV3007_FillHLine(xs, ys, (xs<xe?xe-xs:xs-xe)+1, c); return; }
    if (xs == xe) { NV3007_Fill(xs, ys<ye?ys:ye, xs, ys<ye?ye:ys, c); return; }
    int dx = (int)xe-(int)xs, dy = (int)ye-(int)ys;
    int ux = xs, uy = ys;
    int ix = (dx>0)?1:((dx==0)?0:-1); if (dx<0) dx = -dx;
    int iy = (dy>0)?1:((dy==0)?0:-1); if (dy<0) dy = -dy;
    int d = dx>dy?dx:dy, xe2=0, ye2=0;
    for (int t=0; t<=d+1; t++) {
        NV3007_DrawPoint(ux, uy, c);
        xe2 += dx; ye2 += dy;
        if (xe2 > d) { xe2 -= d; ux += ix; }
        if (ye2 > d) { ye2 -= d; uy += iy; }
    }
}

/* ---- rect ---- */
void NV3007_DrawRectangle(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t c) {
    NV3007_DrawLine(xs,ys,xe,ys,c); NV3007_DrawLine(xs,ys,xs,ye,c);
    NV3007_DrawLine(xs,ye,xe,ye,c); NV3007_DrawLine(xe,ys,xe,ye,c);
}
void NV3007_DrawFillRectangle(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t c) {
    NV3007_Fill(xs, ys, xe, ye, c);
}

/* ---- circle ---- */
static void c8p(int xc, int yc, int x, int y, uint16_t c) {
    NV3007_DrawPoint(xc+x,yc+y,c); NV3007_DrawPoint(xc-x,yc+y,c);
    NV3007_DrawPoint(xc+x,yc-y,c); NV3007_DrawPoint(xc-x,yc-y,c);
    NV3007_DrawPoint(xc+y,yc+x,c); NV3007_DrawPoint(xc-y,yc+x,c);
    NV3007_DrawPoint(xc+y,yc-x,c); NV3007_DrawPoint(xc-y,yc-x,c);
}
void NV3007_DrawCircle(uint16_t xc, uint16_t yc, uint16_t r, uint16_t cl, uint16_t m) {
    int x=0, y=r, d=3-2*r;
    while (x<=y) {
        if (m) { for (int yi=x; yi<=y; yi++) c8p(xc,yc,x,yi,cl); }
        else     c8p(xc,yc,x,y,cl);
        if (d<0) d+=4*x+6; else { d+=4*(x-y)+10; y--; } x++;
    }
}

/* ---- triangle ---- */
void NV3007_DrawTriangle(uint16_t x, uint16_t y, uint16_t xs, uint16_t ys,
                         uint16_t xe, uint16_t ye, uint16_t c) {
    NV3007_DrawLine(x,y,xs,ys,c); NV3007_DrawLine(xs,ys,xe,ye,c); NV3007_DrawLine(xe,ye,x,y,c);
}

/* ---- char (non-overlay: row-buffered. overlay: per-pixel) ---- */
void NV3007_ShowChar(uint16_t x, uint16_t y, uint8_t num, uint16_t fc, uint16_t bc,
                     uint8_t sy, uint8_t mode) {
    uint8_t sx = sy/2;
    uint16_t tn = (sx/8 + ((sx%8)?1:0)) * sy;
    num -= ' ';
    NV3007_SetWindow(x, y, x+sx-1, y+sy-1);

    if (!mode) {
        uint16_t row[12];  /* max 12 columns for 24pt */
        for (uint16_t i=0; i<tn; i++) {
            uint8_t g;
            if      (sy==12) g = ascii_1206[num][i];
            else if (sy==16) g = ascii_1608[num][i];
            else if (sy==24) g = ascii_2412[num][i];
            else return;
            for (int b=0,k=0; b<8 && k<sx; b++,k++)
                row[k] = (g & (1<<b)) ? fc : bc;
            NV3007_WriteData((uint8_t*)row, sx*2);
        }
    } else {
        uint16_t x0 = x;
        for (uint16_t i=0; i<tn; i++) {
            uint8_t g;
            if      (sy==12) g = ascii_1206[num][i];
            else if (sy==16) g = ascii_1608[num][i];
            else if (sy==24) g = ascii_2412[num][i];
            else return;
            for (int t=0; t<8; t++) {
                if (g & (1<<t)) NV3007_DrawPoint(x, y, fc);
                if (++x - x0 == sx) { x = x0; y++; break; }
            }
        }
    }
}

/* ---- string ---- */
void NV3007_ShowString(uint16_t x, uint16_t y, const char *s, uint16_t fc,
                       uint16_t bc, uint16_t sy, uint8_t m) {
    while (*s >= ' ' && *s <= '~') {
        if (x > LCD_W-1 || y > LCD_H-1) return;
        NV3007_ShowChar(x, y, *s, fc, bc, sy, m);
        x += sy/2; s++;
    }
}
void NV3007_ShowStringCenter(uint16_t y, const char *s, uint16_t fc, uint16_t bc,
                             uint8_t sy, uint8_t m) {
    uint16_t x = (LCD_W - (uint16_t)strlen(s)*(sy/2)) / 2;
    NV3007_ShowString(x, y, s, fc, bc, sy, m);
}

/* ---- numbers ---- */
void NV3007_ShowNum(uint16_t x, uint16_t y, uint32_t n, uint8_t len,
                    uint16_t fc, uint16_t bc, uint8_t sy, uint8_t m) {
    uint8_t sz = sy/2, en = 0;
    for (uint8_t t=0; t<len; t++) {
        uint8_t d = (n / p10(len-t-1)) % 10;
        if (!en && t<len-1 && d==0) {
            NV3007_ShowChar(x+t*sz, y, ' ', fc, bc, sy, m); continue;
        }
        en = 1; NV3007_ShowChar(x+t*sz, y, '0'+d, fc, bc, sy, m);
    }
}
void NV3007_ShowFloatNum(uint16_t x, uint16_t y, float v, uint8_t pr, uint8_t len,
                         uint16_t fc, uint16_t bc, uint8_t sy, uint8_t m) {
    uint8_t sz = sy/2;
    uint32_t iv = (uint32_t)(v * p10(pr) + 0.5f);
    for (uint8_t i=0; i<=len; i++) {
        if (i == len-pr) {
            NV3007_ShowChar(x+i*sz, y, '.', fc, bc, sy, m); continue;
        }
        uint32_t d = (iv / p10(len - (i>len-pr?i-1:i) - 1)) % 10;
        NV3007_ShowChar(x+i*sz, y, '0'+d, fc, bc, sy, m);
    }
}

/* ---- picture ---- */
void NV3007_ShowPicture(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t pic[]) {
    if (x+w>LCD_W || y+h>LCD_H) return;
    NV3007_SetWindow(x, y, x+w-1, y+h-1);
    uint32_t t = (uint32_t)w * h * 2;
    for (uint32_t i=0; i<t; i+=NV3007_BUF_SIZE) {
        uint32_t c = t-i < NV3007_BUF_SIZE ? t-i : NV3007_BUF_SIZE;
        NV3007_WriteData(pic+i, c);
    }
}

/* ---- sleep ---- */
void NV3007_EnterSleep(void) { NV3007_WriteReg(0x28); vTaskDelay(pdMS_TO_TICKS(120)); NV3007_WriteReg(0x10); vTaskDelay(pdMS_TO_TICKS(50)); }
void NV3007_ExitSleep(void)  { NV3007_WriteReg(0x11); vTaskDelay(pdMS_TO_TICKS(120)); NV3007_WriteReg(0x29); }

/* ---- init sequence ---- */
#define C8(r,d) do { NV3007_WriteReg(r); NV3007_WriteByte(d); } while(0)

void NV3007_Init(void) {
    ESP_LOGI(TAG, "NV3007 init HW SPI mode 0, %d MHz", NV3007_SPI_CLOCK_HZ/1000000);

    /* SPI bus + device */
    spi_bus_config_t bc = {
        .mosi_io_num = NV3007_PIN_MOSI, .sclk_io_num = NV3007_PIN_CLK,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = NV3007_MAX_BUF_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(NV3007_SPI_HOST, &bc, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dc = {
        .clock_speed_hz = NV3007_SPI_CLOCK_HZ,
        .mode = 0,  /* CPOL=0, CPHA=0 per NV3007 datasheet 4.1.1.1 */
        .spics_io_num = -1, .queue_size = 4,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .pre_cb = cs_low, .post_cb = cs_high,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(NV3007_SPI_HOST, &dc, &g_spi));

    /* control GPIOs */
    gpo(NV3007_PIN_RST); gpo(NV3007_PIN_DC); gpo(NV3007_PIN_BLK); gpo(NV3007_PIN_CS);
    NV3007_CS_Set(); NV3007_DC_Set();

    /* HW reset */
    NV3007_RST_Set(); vTaskDelay(pdMS_TO_TICKS(10));
    NV3007_RST_Clr(); vTaskDelay(pdMS_TO_TICKS(10));
    NV3007_RST_Set(); vTaskDelay(pdMS_TO_TICKS(120));
    NV3007_BLK_Set();

    /* register sequence */
    C8(0xFF, 0xA5); vTaskDelay(pdMS_TO_TICKS(1));
    C8(0x9A,0x08); C8(0x9B,0x08); C8(0x9C,0xB0); C8(0x9D,0x16); C8(0x9E,0xC4);

    /* power */
    { uint8_t d[] = {0x55,0x04}; NV3007_WriteReg(0x8F); NV3007_WriteData(d,2); }
    C8(0x84,0x90); C8(0x83,0x7B); C8(0x85,0x33);

    /* GAMMA */
    { const uint8_t g[] = {
        0x60,0x00,0x70,0x00, 0x61,0x02,0x71,0x02, 0x62,0x04,0x72,0x04,
        0x6C,0x29,0x7C,0x29, 0x6D,0x31,0x7D,0x31, 0x6E,0x0F,0x7E,0x0F,
        0x66,0x21,0x76,0x21, 0x68,0x3A,0x78,0x3A, 0x63,0x07,0x73,0x07,
        0x64,0x05,0x74,0x05, 0x65,0x02,0x75,0x02, 0x67,0x23,0x77,0x23,
        0x69,0x08,0x79,0x08, 0x6A,0x13,0x7A,0x13, 0x6B,0x13,0x7B,0x13,
        0x6F,0x00,0x7F,0x00 };
        for (int i=0; i<sizeof(g); i+=2) C8(g[i], g[i+1]); }

    /* source driver */
    C8(0x50,0x00); C8(0x52,0xD6); C8(0x53,0x08); C8(0x54,0x08); C8(0x55,0x1E); C8(0x56,0x1C);

    /* GOA map_sel */
    { uint8_t d[]={0x2B,0x24,0x00}; NV3007_WriteReg(0xA0); NV3007_WriteData(d,3); }
    C8(0xA1,0x87); C8(0xA2,0x86); C8(0xA5,0x00); C8(0xA6,0x00); C8(0xA7,0x00);
    C8(0xA8,0x36); C8(0xA9,0x7E); C8(0xAA,0x7E);

    /* B9-C6 */
    C8(0xB9,0x85); C8(0xBA,0x84); C8(0xBB,0x83); C8(0xBC,0x82); C8(0xBD,0x81);
    C8(0xBE,0x80); C8(0xBF,0x01); C8(0xC0,0x02); C8(0xC1,0x00); C8(0xC2,0x00);
    C8(0xC3,0x00); C8(0xC4,0x33); C8(0xC5,0x7E); C8(0xC6,0x7E);

    /* gate signals */
    { uint8_t d[]={0x33,0x33}; NV3007_WriteReg(0xC8); NV3007_WriteData(d,2); }
    C8(0xC9,0x68); C8(0xCA,0x69); C8(0xCB,0x6A); C8(0xCC,0x6B);
    { uint8_t d[]={0x33,0x33}; NV3007_WriteReg(0xCD); NV3007_WriteData(d,2); }
    C8(0xCE,0x6C); C8(0xCF,0x6D); C8(0xD0,0x6E); C8(0xD1,0x6F);

    /* multi-output */
    { uint8_t d[]={0x03,0x67}; NV3007_WriteReg(0xAB); NV3007_WriteData(d,2); }
    { uint8_t d[]={0x03,0x6B}; NV3007_WriteReg(0xAC); NV3007_WriteData(d,2); }
    { uint8_t d[]={0x03,0x68}; NV3007_WriteReg(0xAD); NV3007_WriteData(d,2); }
    { uint8_t d[]={0x03,0x6C}; NV3007_WriteReg(0xAE); NV3007_WriteData(d,2); }

    /* B3-B8 */
    C8(0xB3,0x00); C8(0xB4,0x00); C8(0xB5,0x00); C8(0xB6,0x32); C8(0xB7,0x7E); C8(0xB8,0x7E);

    /* power management */
    C8(0xE0,0x00);
    { uint8_t d[]={0x03,0x0F}; NV3007_WriteReg(0xE1); NV3007_WriteData(d,2); }
    C8(0xE2,0x04); C8(0xE3,0x01); C8(0xE4,0x0E); C8(0xE5,0x01); C8(0xE6,0x19);
    C8(0xE7,0x10); C8(0xE8,0x10); C8(0xEA,0x12); C8(0xEB,0xD0); C8(0xEC,0x04);
    C8(0xED,0x07); C8(0xEE,0x07); C8(0xEF,0x09); C8(0xF0,0xD0); C8(0xF1,0x0E);

    /* F9/F2 fix */
    NV3007_WriteByte(0x17);
    { uint8_t d[]={0x2C,0x1B,0x0B,0x20}; NV3007_WriteReg(0xF2); NV3007_WriteData(d,4); }

    /* 1-dot, TE */
    C8(0xE9,0x29); C8(0xEC,0x04);
    C8(0x35,0x00);
    { uint8_t d[]={0x00,0x10}; NV3007_WriteReg(0x44); NV3007_WriteData(d,2); }
    C8(0x46,0x10);

    /* MADCTL — screen rotation */
    #if USE_HORIZONTIAL == 0
        C8(0x36, 0x00);  /* portrait */
    #elif USE_HORIZONTIAL == 1
        C8(0x36, 0x80);  /* portrait 180 (MY) */
    #elif USE_HORIZONTIAL == 3
        C8(0x36, LANDSCAPE_ALT ? 0x60 : 0xA0);  /* landscape */
    #elif USE_HORIZONTIAL == 2
        C8(0x36, LANDSCAPE_ALT ? 0xA0 : 0x60);  /* landscape */
    #endif

    /* exit sleep, display on */
    C8(0xFF,0x00); C8(0x3A,0x05);
    NV3007_WriteReg(0x11); vTaskDelay(pdMS_TO_TICKS(220));
    NV3007_WriteReg(0x29); vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "NV3007 ready");
}

void NV3007_Deinit(void) {
    NV3007_EnterSleep(); NV3007_BLK_Clr(); spi_bus_free(NV3007_SPI_HOST);
}
