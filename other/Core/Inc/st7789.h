#ifndef ST7789_H
#define ST7789_H

#include "main.h"

/* Native 240 x 320 panel, rotated to landscape. */
#define ST7789_WIDTH  320U
#define ST7789_HEIGHT 240U
#define ST7789_X_OFFSET 0U
#define ST7789_Y_OFFSET 0U
#define ST7789_MADCTL 0xA0U /* MY | MV: opposite landscape; 0xA8 for BGR. */
#define ST7789_INVERSION 0U /* This module showed complemented colors with INVON. */
#define ST7789_BL_ACTIVE GPIO_PIN_SET

#define TFT_BLACK   0x0000U
#define TFT_WHITE   0xFFFFU
#define TFT_RED     0xF800U
#define TFT_GREEN   0x07E0U
#define TFT_BLUE    0x001FU
#define TFT_CYAN    0x07FFU
#define TFT_MAGENTA 0xF81FU
#define TFT_YELLOW  0xFFE0U

void ST7789_Init(void);
void ST7789_Backlight(uint8_t on);
void ST7789_FillScreen(uint16_t color);
void ST7789_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     uint16_t color);
void ST7789_DrawPixel(uint16_t x, uint16_t y, uint16_t color);
/* Packed RGB565 in SPI byte order; rectangle must be wholly on-screen. */
void ST7789_WritePixelsBE(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           const uint8_t *pixels);
/* Built-in 5 x 7 font: A-Z, a-z (shown uppercase), digits, space, . : - / */
void ST7789_DrawString(uint16_t x, uint16_t y, const char *text,
                       uint16_t color, uint16_t background, uint8_t scale);
void ST7789_Demo(void);
void ST7789_DemoTick(void);

#endif
