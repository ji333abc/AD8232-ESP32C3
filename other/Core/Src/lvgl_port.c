#include "lvgl_port.h"
#include "st7789.h"
#include "ecg_sim.h"
#include "../../Drivers/lvgl/lvgl.h"

#define PLOT_X 8
#define PLOT_Y 82
#define PLOT_W 304
#define PLOT_H 136
#define TRACE_COLOR 0x00FF40
#define HR_COLOR 0xFF4038
#define SWEEP_GAP 8U

/* Small partial buffer and 608-byte sweep history; no extra font required. */
static lv_color_t pixels[ST7789_WIDTH * 6U];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t display_driver;
static lv_obj_t *plot, *heart_rate;
static int16_t samples[PLOT_W];
static uint16_t cursor;
static uint32_t sample_time, last_tick;
static uint32_t pulse_until;
static uint8_t pulse_on;
static int16_t previous_sample;

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
  ST7789_WritePixelsBE((uint16_t)area->x1, (uint16_t)area->y1,
      (uint16_t)lv_area_get_width(area), (uint16_t)lv_area_get_height(area),
      (const uint8_t *)colors);
  lv_disp_flush_ready(driver);
}

static void label(lv_obj_t *screen, int x, int y, const char *text, uint32_t color)
{
  lv_obj_t *obj = lv_label_create(screen);
  lv_label_set_text_static(obj, text);
  lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
  lv_obj_set_pos(obj, x, y);
}

static void rect(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *style,
                  int x1, int y1, int x2, int y2)
{
  lv_area_t area = {x1, y1, x2, y2};
  lv_draw_rect(ctx, style, &area);
}

static void draw_hr(lv_event_t *event)
{
  /* Compact 33-pixel seven-segment numerals avoid another font. */
  static const uint8_t masks[] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};
  static const uint8_t segments[7][4] = {
    {3,0,13,2}, {14,3,16,14}, {14,18,16,29}, {3,30,13,32},
    {0,18,2,29}, {0,3,2,14}, {3,15,13,17}
  };
  static const uint8_t heart[] = {0x66,0xFF,0xFF,0x7E,0x3C,0x18};
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
  lv_area_t box;
  lv_obj_get_coords(lv_event_get_target(event), &box);
  lv_draw_rect_dsc_t style;
  lv_draw_rect_dsc_init(&style);
  style.bg_color = lv_color_hex(HR_COLOR);
  for (unsigned digit = 0; digit < 2; ++digit)
  {
    unsigned number = digit == 0 ? ECG_SIM_BPM / 10 : ECG_SIM_BPM % 10;
    int x = box.x1 + 2 + digit * 23;
    for (unsigned s = 0; s < 7; ++s)
      if (masks[number] & (1U << s))
        rect(ctx, &style, x + segments[s][0], box.y1 + 4 + segments[s][1],
             x + segments[s][2], box.y1 + 4 + segments[s][3]);
  }
  style.bg_color = lv_color_hex(pulse_on ? HR_COLOR : 0x381410);
  for (unsigned y = 0; y < 6; ++y)
    for (unsigned x = 0; x < 8; ++x)
      if (heart[y] & (0x80U >> x))
        rect(ctx, &style, box.x1 + 16 + x * 2, box.y1 + 43 + y * 2,
             box.x1 + 17 + x * 2, box.y1 + 44 + y * 2);
}

static void draw_trace(lv_event_t *event)
{
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
  lv_area_t box;
  lv_obj_get_coords(lv_event_get_target(event), &box);
  lv_draw_rect_dsc_t style;
  lv_draw_rect_dsc_init(&style);
  style.bg_opa = LV_OPA_COVER;
  /* Narrow LVGL invalidations avoid sending the whole trace over SPI. */
  int first = LV_MAX(0, ctx->clip_area->x1 - box.x1 - 1);
  int last = LV_MIN(PLOT_W - 1, ctx->clip_area->x2 - box.x1);
  style.bg_color = lv_color_hex(TRACE_COLOR);
  for (int x = first; x <= last; ++x)
  {
    unsigned distance = (x + PLOT_W - cursor) % PLOT_W;
    if (distance < SWEEP_GAP) continue;
    int y = box.y1 + 88 - samples[x];
    int prev = x == 0 || distance == SWEEP_GAP ? y : box.y1 + 88 - samples[x-1];
    rect(ctx, &style, box.x1 + x, LV_MIN(y, prev), box.x1 + x, LV_MAX(y, prev) + 1);
  }
}

static void invalidate_columns(unsigned first, unsigned last)
{
  lv_area_t box;
  lv_obj_get_coords(plot, &box);
  box.x2 = box.x1 + (int)last;
  box.x1 += first;
  lv_obj_invalidate_area(plot, &box);
}

static void update(lv_timer_t *timer)
{
  (void)timer;
  uint32_t now = HAL_GetTick();
  uint32_t count = (now - last_tick) / ECG_SIM_STEP_MS;
  if (count == 0) return;
  /* Bounded catch-up after a stalled main loop; retain the latest window. */
  last_tick += count * ECG_SIM_STEP_MS;
  if (count > PLOT_W)
  {
    sample_time += (count - PLOT_W) * ECG_SIM_STEP_MS;
    count = PLOT_W;
  }
  unsigned first = cursor == 0 ? PLOT_W - 1 : cursor - 1;
  unsigned length = count + SWEEP_GAP + 2;
  while (count--)
  {
    samples[cursor] = ECG_SimSample(sample_time);
    if (samples[cursor] > 30 && previous_sample <= 30) pulse_until = now + 120;
    previous_sample = samples[cursor];
    sample_time += ECG_SIM_STEP_MS;
    cursor = (cursor + 1U) % PLOT_W;
  }
  uint8_t active = (int32_t)(pulse_until - now) > 0;
  if (active != pulse_on)
  {
    pulse_on = active;
    lv_area_t heart_box;
    lv_obj_get_coords(heart_rate, &heart_box);
    heart_box.x1 += 16;
    heart_box.x2 = heart_box.x1 + 15;
    heart_box.y1 += 43;
    heart_box.y2 = heart_box.y1 + 11;
    lv_obj_invalidate_area(heart_rate, &heart_box);
  }
  if (length >= PLOT_W) lv_obj_invalidate(plot);
  else if (first + length < PLOT_W) invalidate_columns(first, first + length);
  else
  {
    invalidate_columns(first, PLOT_W - 1);
    invalidate_columns(0, (first + length) % PLOT_W);
  }
}

void LVGL_Port_Init(void)
{
  cursor = 0;
  pulse_until = 0;
  pulse_on = 0;
  previous_sample = 0;
  sample_time = 0;
  for (unsigned x = 0; x < PLOT_W; ++x)
  {
    samples[x] = ECG_SimSample(sample_time);
    sample_time += ECG_SIM_STEP_MS;
  }
  last_tick = HAL_GetTick();
  lv_init();
  lv_disp_draw_buf_init(&draw_buffer, pixels, NULL, ST7789_WIDTH * 6U);
  lv_disp_drv_init(&display_driver);
  display_driver.hor_res = ST7789_WIDTH;
  display_driver.ver_res = ST7789_HEIGHT;
  display_driver.draw_buf = &draw_buffer;
  display_driver.flush_cb = flush;
  display_driver.antialiasing = 0;
  if (lv_disp_drv_register(&display_driver) == NULL) Error_Handler();
  lv_obj_t *screen = lv_scr_act();
  lv_obj_remove_style_all(screen);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_text_font(screen, LV_FONT_DEFAULT, 0);
  label(screen, 10, 14, "ECG MONITOR", 0xC8D4CD);
  label(screen, 10, 39, "SIMULATION", 0xB8A66A);
  label(screen, 10, 64, "ECG", TRACE_COLOR);
  label(screen, 210, 24, "HR", HR_COLOR);
  label(screen, 286, 32, "bpm", HR_COLOR);
  plot = lv_obj_create(screen);
  lv_obj_remove_style_all(plot);
  lv_obj_clear_flag(plot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(plot, PLOT_X, PLOT_Y);
  lv_obj_set_size(plot, PLOT_W, PLOT_H);
  lv_obj_set_style_bg_color(plot, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(plot, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(plot, draw_trace, LV_EVENT_DRAW_MAIN, NULL);
  heart_rate = lv_obj_create(screen);
  lv_obj_remove_style_all(heart_rate);
  lv_obj_clear_flag(heart_rate, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(heart_rate, 240, 14);
  lv_obj_set_size(heart_rate, 46, 56);
  lv_obj_add_event_cb(heart_rate, draw_hr, LV_EVENT_DRAW_MAIN, NULL);
  label(screen, 10, 224, "SIMULATED ECG", 0x6D8274);
  if (lv_timer_create(update, 40, NULL) == NULL) Error_Handler();
}

void LVGL_Port_Task(void)
{
  lv_timer_handler();
}
