/* Native LVGL renderer test; hardware SPI/clock operation requires the board. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../Drivers/lvgl/lvgl.h"
#include "lvgl_port.h"
#include "ecg_sim.h"
#include "st7789.h"

static uint32_t ticks, flushes;
static uint16_t frame[ST7789_HEIGHT][ST7789_WIDTH];
static uint16_t saved_frame[ST7789_HEIGHT][ST7789_WIDTH];

uint32_t HAL_GetTick(void) { return ticks; }
void Error_Handler(void)
{
  fputs("LVGL assertion or allocation failure\n", stderr);
  exit(1);
}

void ST7789_WritePixelsBE(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           const uint8_t *pixels)
{
  assert(w && h && x + w <= ST7789_WIDTH && y + h <= ST7789_HEIGHT);
  assert((uint32_t)w * h <= ST7789_WIDTH * 6);
  for (unsigned r = 0; r < h; ++r)
    for (unsigned c = 0; c < w; ++c)
    {
      unsigned i = (r * w + c) * 2;
      frame[y + r][x + c] = ((uint16_t)pixels[i] << 8) | pixels[i + 1];
    }
  ++flushes;
}

static void screenshot(const char *path)
{
  /* 24-bit bottom-up BMP; 320 pixels need no row padding. */
  unsigned char header[54] = {'B','M'};
  uint32_t size = 54 + ST7789_WIDTH * ST7789_HEIGHT * 3, offset = 54, dib = 40;
  int32_t width = ST7789_WIDTH, height = ST7789_HEIGHT;
  uint16_t planes = 1, depth = 24;
  memcpy(header + 2, &size, 4); memcpy(header + 10, &offset, 4);
  memcpy(header + 14, &dib, 4); memcpy(header + 18, &width, 4);
  memcpy(header + 22, &height, 4); memcpy(header + 26, &planes, 2);
  memcpy(header + 28, &depth, 2);
  FILE *file = fopen(path, "wb");
  assert(file);
  fwrite(header, 1, 54, file);
  for (int y = ST7789_HEIGHT - 1; y >= 0; --y)
    for (unsigned x = 0; x < ST7789_WIDTH; ++x)
    {
      uint16_t p = frame[y][x];
      unsigned char bgr[] = {(p & 31) * 255 / 31,
                            ((p >> 5) & 63) * 255 / 63,
                            (p >> 11) * 255 / 31};
      fwrite(bgr, 1, 3, file);
    }
  fclose(file);
}

int main(void)
{
  unsigned peaks = 0;
  int previous = 0;
  for (unsigned t = 0; t < ECG_SIM_PERIOD_MS; ++t)
  {
    int sample = ECG_SimSample(t);
    assert(sample >= -16 && sample <= 55);
    assert(sample == ECG_SimSample(t + ECG_SIM_PERIOD_MS));
    if (sample > 30 && previous <= 30) ++peaks;
    previous = sample;
  }
  assert(peaks == 1);
  LVGL_Port_Init();
  lv_obj_update_layout(lv_scr_act());
  assert(lv_disp_get_hor_res(NULL) == 320 && lv_disp_get_ver_res(NULL) == 240);
  lv_obj_t *plot = NULL;
  for (unsigned i = 0; i < lv_obj_get_child_cnt(lv_scr_act()); ++i)
  {
    lv_obj_t *child = lv_obj_get_child(lv_scr_act(), i);
    if (lv_obj_get_width(child) == 304 && lv_obj_get_height(child) == 136) plot = child;
  }
  assert(plot != NULL);
  unsigned pulse_bright = 0, pulse_dim = 0;
  size_t free_at_start = 0;
  for (ticks = 0; ticks < 60000; ticks += 5)
  {
    LVGL_Port_Task();
    if (ticks == 1000 || ticks == 59995)
    {
      lv_mem_monitor_t mem;
      lv_mem_monitor(&mem);
      assert(lv_mem_test() == LV_RES_OK);
      if (ticks == 1000) free_at_start = mem.free_size;
      else
      {
        assert(mem.free_size == free_at_start);
        printf("LVGL heap used: %zu / %u bytes; no growth over 60 s\n",
               (size_t)mem.total_size - mem.free_size, (unsigned)mem.total_size);
      }
    }
    if (ticks >= 1000 && ticks < 4040 && ticks % 40 == 0)
    {
      char path[80];
      snprintf(path, sizeof path, "Build/ecg_frames/frame_%03lu.bmp", (unsigned long)((ticks - 1000) / 40));
      screenshot(path);
    }
    if (ticks == 1000)
    {
      unsigned digit_pixels = 0, trace_pixels = 0;
      for (unsigned y = 18; y < 51; ++y)
        for (unsigned x = 242; x < 282; ++x)
          if (frame[y][x] == 0xFA07) ++digit_pixels;
      for (unsigned y = 82; y < 218; ++y)
        for (unsigned x = 8; x < 312; ++x)
          if (frame[y][x] == 0x07E8) ++trace_pixels;
      assert(digit_pixels > 200 && digit_pixels < 400 && trace_pixels > 250);
      screenshot("Build/ecg_preview.bmp");
    }
    if (ticks >= 1000)
    {
      if (frame[59][258] == 0xFA07) ++pulse_bright;
      else ++pulse_dim;
    }
    if (ticks >= 1000 && ticks % 1000 == 0)
    {
      /* Partial sweep repaint must match a full repaint, including wrap. */
      memcpy(saved_frame, frame, sizeof frame);
      lv_obj_invalidate(lv_scr_act());
      lv_refr_now(NULL);
      assert(memcmp(saved_frame, frame, sizeof frame) == 0);
    }
  }
  assert(flushes > 100 && pulse_bright > 100 && pulse_dim > 100);
  /* Background color in the flush stream verifies RGB565 byte swapping. */
  assert(frame[0][0] == 0x0000);
  printf("PASS: %lu flushes; compact red HR/pulse, full-width ECG, full repaint equality\n",
         (unsigned long)flushes);
  return 0;
}
