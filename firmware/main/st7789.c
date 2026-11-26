#include <string.h>

#include "board_config.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "st7789.h"

enum { LCD_TRANSFER_BYTES = 4096 };

static spi_device_handle_t lcd;
static DMA_ATTR uint16_t fill_pixels[LCD_TRANSFER_BYTES / sizeof(uint16_t)];

static void transmit(const void *data, size_t bytes)
{
    spi_transaction_t transfer = {.length = bytes * 8};
    if (bytes <= sizeof(transfer.tx_data)) {
        transfer.flags = SPI_TRANS_USE_TXDATA;
        memcpy(transfer.tx_data, data, bytes);
    } else {
        transfer.tx_buffer = data;
    }
    ESP_ERROR_CHECK(spi_device_transmit(lcd, &transfer));
}

static void send_command(uint8_t command, const uint8_t *data, size_t size)
{
    gpio_set_level(BOARD_LCD_CS, 0);
    gpio_set_level(BOARD_LCD_DC, 0);
    transmit(&command, 1);
    if (size > 0) {
        gpio_set_level(BOARD_LCD_DC, 1);
        transmit(data, size);
    }
    gpio_set_level(BOARD_LCD_CS, 1);
}

static void begin_pixels(int x, int y, int width, int height)
{
    const uint16_t left = x + BOARD_LCD_X_OFFSET;
    const uint16_t top = y + BOARD_LCD_Y_OFFSET;
    const uint16_t right = left + width - 1;
    const uint16_t bottom = top + height - 1;
    const uint8_t columns[] = {left >> 8, left & 0xff, right >> 8, right & 0xff};
    const uint8_t rows[] = {top >> 8, top & 0xff, bottom >> 8, bottom & 0xff};
    const uint8_t write = 0x2C;

    send_command(0x2A, columns, sizeof(columns));
    send_command(0x2B, rows, sizeof(rows));
    gpio_set_level(BOARD_LCD_CS, 0);
    gpio_set_level(BOARD_LCD_DC, 0);
    transmit(&write, 1);
    gpio_set_level(BOARD_LCD_DC, 1);
}

void st7789_write(int x, int y, int width, int height, const uint16_t *pixels_be)
{
    size_t remaining = (size_t)width * height;
    begin_pixels(x, y, width, height);
    while (remaining > 0) {
        const size_t capacity = LCD_TRANSFER_BYTES / sizeof(*pixels_be);
        const size_t count = remaining > capacity ? capacity : remaining;
        transmit(pixels_be, count * sizeof(*pixels_be));
        pixels_be += count;
        remaining -= count;
    }
    gpio_set_level(BOARD_LCD_CS, 1);
}

void st7789_fill(int x, int y, int width, int height, uint16_t color)
{
    const size_t capacity = sizeof(fill_pixels) / sizeof(fill_pixels[0]);
    size_t remaining = (size_t)width * height;
    const size_t initial = remaining > capacity ? capacity : remaining;
    const uint16_t color_be = __builtin_bswap16(color);
    for (size_t i = 0; i < initial; ++i) {
        fill_pixels[i] = color_be;
    }
    begin_pixels(x, y, width, height);
    while (remaining > 0) {
        const size_t count = remaining > capacity ? capacity : remaining;
        transmit(fill_pixels, count * sizeof(fill_pixels[0]));
        remaining -= count;
    }
    gpio_set_level(BOARD_LCD_CS, 1);
}

void st7789_init(void)
{
    const gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << BOARD_LCD_RST) | (1ULL << BOARD_LCD_DC) |
                        (1ULL << BOARD_LCD_CS) | (1ULL << BOARD_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_set_level(BOARD_LCD_CS, 1));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_LCD_RST, 1));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_LCD_BL, !BOARD_LCD_BL_ACTIVE_LEVEL));
    ESP_ERROR_CHECK(gpio_config(&outputs));

    const spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCK,
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_TRANSFER_BYTES,
    };
    const spi_device_interface_config_t device = {
        .clock_speed_hz = BOARD_LCD_SPI_HZ,
        .mode = BOARD_LCD_SPI_MODE,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(BOARD_LCD_SPI, &device, &lcd));

    // Retain the known-good STM32 panel register sequence and delays.
    static const struct {
        uint8_t command;
        uint8_t size;
        uint8_t data[5];
        uint16_t wait_ms;
    } init[] = {
        {0x01, 0, {0}, 150}, {0x11, 0, {0}, 120},
        {0x3A, 1, {BOARD_LCD_COLMOD}, 0}, {0x36, 1, {BOARD_LCD_MADCTL}, 0},
        {0xB2, 5, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 0}, {0xB7, 1, {0x35}, 0},
        {0xBB, 1, {0x19}, 0}, {0xC0, 1, {0x2C}, 0}, {0xC2, 1, {0x01}, 0},
        {0xC3, 1, {0x12}, 0}, {0xC4, 1, {0x20}, 0}, {0xC6, 1, {0x0F}, 0},
        {0xD0, 2, {0xA4, 0xA1}, 0}, {BOARD_LCD_INVERT ? 0x21 : 0x20, 0, {0}, 0},
        {0x13, 0, {0}, 0},
    };

    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); ++i) {
        send_command(init[i].command, init[i].data, init[i].size);
        if (init[i].wait_ms) {
            vTaskDelay(pdMS_TO_TICKS(init[i].wait_ms));
        }
    }
    st7789_fill(0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, 0);
    send_command(0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_BL, BOARD_LCD_BL_ACTIVE_LEVEL);
}
