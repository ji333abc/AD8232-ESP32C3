#include "st7789.h"
#include "spi.h"

/* Blocking SPI with bounded timeout. CS remains asserted for each transaction. */
static void transmit(const uint8_t *data, uint16_t size)
{
  if (HAL_SPI_Transmit(&hspi1, (uint8_t *)data, size, 1000) != HAL_OK)
  {
    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
    Error_Handler();
  }
}

static void command(uint8_t cmd, const uint8_t *data, uint16_t size)
{
  HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_RESET);
  transmit(&cmd, 1);
  if (size != 0)
  {
    HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET);
    transmit(data, size);
  }
  HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
}

static void reg8(uint8_t cmd, uint8_t value)
{
  command(cmd, &value, 1);
}

static void window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  uint16_t x0 = x + ST7789_X_OFFSET;
  uint16_t y0 = y + ST7789_Y_OFFSET;
  uint16_t x1 = x0 + w - 1U;
  uint16_t y1 = y0 + h - 1U;
  uint8_t column[] = {x0 >> 8, x0 & 255U, x1 >> 8, x1 & 255U};
  uint8_t row[] = {y0 >> 8, y0 & 255U, y1 >> 8, y1 & 255U};
  uint8_t cmd = 0x2C;
  command(0x2A, column, sizeof column);
  command(0x2B, row, sizeof row);
  HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_RESET);
  transmit(&cmd, 1);
  HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET);
}

void ST7789_Backlight(uint8_t on)
{
  HAL_GPIO_WritePin(TFT_BL_GPIO_Port, TFT_BL_Pin,
                   on ? ST7789_BL_ACTIVE :
                   (ST7789_BL_ACTIVE == GPIO_PIN_SET ? GPIO_PIN_RESET : GPIO_PIN_SET));
}

void ST7789_Init(void)
{
  static const uint8_t porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
  static const uint8_t power[] = {0xA4, 0xA1};
  ST7789_Backlight(0);
  HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);
  HAL_Delay(20);
  HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_RESET);
  HAL_Delay(20);
  HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);
  HAL_Delay(150);
  command(0x01, 0, 0); /* Software reset */
  HAL_Delay(150);
  command(0x11, 0, 0); /* Sleep out */
  HAL_Delay(120);
  reg8(0x3A, 0x55); /* RGB565, high byte first on SPI */
  reg8(0x36, ST7789_MADCTL);
  command(0xB2, porch, sizeof porch);
  reg8(0xB7, 0x35); /* Gate control */
  reg8(0xBB, 0x19); /* VCOM */
  reg8(0xC0, 0x2C); /* LCM control */
  reg8(0xC2, 0x01); /* Enable VRH/VDV setting */
  reg8(0xC3, 0x12); /* VRH */
  reg8(0xC4, 0x20); /* VDV */
  reg8(0xC6, 0x0F); /* Frame rate control */
  command(0xD0, power, sizeof power);
  command(ST7789_INVERSION ? 0x21 : 0x20, 0, 0);
  command(0x13, 0, 0); /* Normal display mode */
  ST7789_FillScreen(TFT_BLACK);
  command(0x29, 0, 0); /* Display on */
  HAL_Delay(20);
  ST7789_Backlight(1);
}

void ST7789_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     uint16_t color)
{
  uint8_t buffer[256]; /* Only 128 pixels, no full framebuffer in 20 KB RAM. */
  uint32_t count;
  if (x >= ST7789_WIDTH || y >= ST7789_HEIGHT || w == 0 || h == 0)
    return;
  if (w > ST7789_WIDTH - x) w = ST7789_WIDTH - x;
  if (h > ST7789_HEIGHT - y) h = ST7789_HEIGHT - y;
  for (uint16_t i = 0; i < sizeof buffer; i += 2)
  {
    buffer[i] = color >> 8;
    buffer[i + 1] = color & 255U;
  }
  window(x, y, w, h);
  count = (uint32_t)w * h;
  while (count != 0)
  {
    uint16_t pixels = count > 128U ? 128U : (uint16_t)count;
    transmit(buffer, pixels * 2U);
    count -= pixels;
  }
  HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
}

void ST7789_FillScreen(uint16_t color)
{
  ST7789_FillRect(0, 0, ST7789_WIDTH, ST7789_HEIGHT, color);
}

void ST7789_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
  ST7789_FillRect(x, y, 1, 1, color);
}

void ST7789_WritePixelsBE(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           const uint8_t *pixels)
{
  if (pixels == 0 || w == 0 || h == 0 ||
      x >= ST7789_WIDTH || y >= ST7789_HEIGHT ||
      w > ST7789_WIDTH - x || h > ST7789_HEIGHT - y) return;
  uint32_t bytes = (uint32_t)w * h * 2U;
  window(x, y, w, h);
  while (bytes != 0)
  {
    uint16_t size = bytes > 32768U ? 32768U : (uint16_t)bytes;
    transmit(pixels, size);
    pixels += size;
    bytes -= size;
  }
  HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
}

/* Five columns per glyph, least significant bit is the top row. */
static const uint8_t letters[26][5] = {
  {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
  {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},
  {0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
  {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
  {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},
  {0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
  {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},
  {0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},
  {0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
  {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},
  {0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
  {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};
static const uint8_t digits[10][5] = {
  {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
  {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
  {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E}
};

static uint8_t glyph_column(char c, uint8_t col)
{
  if (col >= 5) return 0;
  if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
  if (c >= 'A' && c <= 'Z') return letters[c - 'A'][col];
  if (c >= '0' && c <= '9') return digits[c - '0'][col];
  if (c == '-') return 0x08;
  if (c == ':') return col == 2 ? 0x36 : 0;
  if (c == '.') return col == 2 ? 0x60 : 0;
  if (c == '/') return (uint8_t)(0x20U >> col);
  return 0;
}

void ST7789_DrawString(uint16_t x, uint16_t y, const char *text,
                       uint16_t color, uint16_t background, uint8_t scale)
{
  uint8_t row[6U * 4U * 2U];
  if (text == 0 || scale == 0 || scale > 4) return;
  while (*text != '\0')
  {
    if ((uint32_t)x + 6U * scale > ST7789_WIDTH ||
        (uint32_t)y + 8U * scale > ST7789_HEIGHT) return;
    window(x, y, 6U * scale, 8U * scale);
    for (uint8_t r = 0; r < 8; ++r)
    {
      for (uint8_t col = 0; col < 6; ++col)
      {
        uint16_t pixel = glyph_column(*text, col) & (1U << r) ? color : background;
        for (uint8_t s = 0; s < scale; ++s)
        {
          uint8_t i = (col * scale + s) * 2U;
          row[i] = pixel >> 8;
          row[i + 1] = pixel & 255U;
        }
      }
      for (uint8_t s = 0; s < scale; ++s)
        transmit(row, 6U * scale * 2U);
    }
    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
    x += 6U * scale;
    ++text;
  }
}

void ST7789_Demo(void)
{
  static const uint16_t colors[] = {
    TFT_RED,TFT_GREEN,TFT_BLUE,TFT_CYAN,TFT_MAGENTA,TFT_YELLOW
  };
  for (uint8_t i = 0; i < 3; ++i)
  {
    ST7789_FillScreen(colors[i]);
    HAL_Delay(600);
  }
  ST7789_FillScreen(TFT_BLACK);
  ST7789_DrawString(12, 16, "STM32F103", TFT_CYAN, TFT_BLACK, 3);
  ST7789_DrawString(12, 54, "ST7789 SPI TFT", TFT_WHITE, TFT_BLACK, 2);
  ST7789_DrawString(12, 82, "240 X 320 RGB565", TFT_WHITE, TFT_BLACK, 2);
  for (uint8_t i = 0; i < 6; ++i)
    ST7789_FillRect(i * 40U, 118, 40, 64, colors[i]);
  ST7789_DrawString(12, 198, "DISPLAY OK", TFT_GREEN, TFT_BLACK, 3);
  ST7789_DrawString(12, 238, "SPI1 4 MHZ", TFT_WHITE, TFT_BLACK, 2);
  ST7789_FillRect(10, 274, 220, 22, TFT_WHITE);
  ST7789_FillRect(12, 276, 216, 18, TFT_BLACK);
  /* Corner markers help identify orientation and clipped address windows. */
  ST7789_DrawPixel(0, 0, TFT_WHITE);
  ST7789_DrawPixel(239, 0, TFT_WHITE);
  ST7789_DrawPixel(0, 319, TFT_WHITE);
  ST7789_DrawPixel(239, 319, TFT_WHITE);
}

void ST7789_DemoTick(void)
{
  static uint32_t last;
  static uint16_t progress;
  uint32_t now = HAL_GetTick();
  if (now - last < 50U) return;
  last = now;
  if (progress >= 216U)
  {
    progress = 0;
    ST7789_FillRect(12, 276, 216, 18, TFT_BLACK);
  }
  else
  {
    ST7789_FillRect(12 + progress, 276, 4, 18, TFT_CYAN);
    progress += 4;
  }
}
