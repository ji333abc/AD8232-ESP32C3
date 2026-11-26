#pragma once

#include <stdint.h>

#define ST7789_RGB(r, g, b) (((r) & 0xf8u) << 8 | ((g) & 0xfcu) << 3 | (b) >> 3)

void st7789_init(void);
void st7789_fill(int x, int y, int width, int height, uint16_t color);
// Pixels are stored in LCD byte order, high byte first on the wire.
void st7789_write(int x, int y, int width, int height, const uint16_t *pixels_be);
