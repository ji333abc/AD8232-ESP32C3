#include <string.h>

#include "board_config.h"
#include "ecg_display.h"
#include "ecg_filter.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "st7789.h"

_Static_assert(ECG_DISPLAY_NOTCH_HZ > 0 && 2 * ECG_DISPLAY_NOTCH_HZ < ECG_SAMPLE_HZ,
               "The display notch frequency must be below Nyquist");
_Static_assert(ECG_DISPLAY_LOWPASS_HZ > 0 && 2 * ECG_DISPLAY_LOWPASS_HZ < ECG_SAMPLE_HZ,
               "The display low-pass frequency must be below Nyquist");

enum {
    PLOT_X = 8, PLOT_Y = 82, PLOT_WIDTH = BOARD_LCD_WIDTH - 2 * PLOT_X,
    PLOT_HEIGHT = 156, PLOT_BASELINE = 108, SWEEP_GAP = 8, STRIP_WIDTH = 16,
    PLOT_SAMPLES = ECG_SAMPLE_HZ * ECG_DISPLAY_SECONDS,
    SCOPE_HISTORY_SAMPLES = PLOT_SAMPLES + ECG_SAMPLE_HZ,
    SCOPE_PRETRIGGER_SAMPLES = ECG_SAMPLE_HZ / 5,
    SCOPE_REFRESH_US = 250000,
    DISPLAY_DATA_TIMEOUT_US = 500000,
    GRID_DIVISIONS_PER_SECOND = 5,
    GRID_COLUMNS = ECG_DISPLAY_SECONDS * GRID_DIVISIONS_PER_SECOND,
    GRID_HEIGHT = 20,
    PLOT_CLIP_TOP = 1u << 0,
    PLOT_CLIP_BOTTOM = 1u << 1,
    LCD_QUEUE_LENGTH = 256,
    COLOR_GRID = ST7789_RGB(64, 36, 40),
    COLOR_GREEN = ST7789_RGB(0, 255, 64),
    COLOR_RED = ST7789_RGB(255, 64, 56),
    COLOR_TEXT = ST7789_RGB(200, 212, 205),
    COLOR_AMBER = ST7789_RGB(184, 166, 106),
};

typedef struct {
    uint32_t sequence;
    int16_t raw;
    ecg_signal_t signal;
} display_sample_t;

typedef struct {
    uint8_t top;
    uint8_t bottom;
    uint8_t last;
    uint8_t clipped_edges;
    bool valid;
    bool joins_previous;
    bool dirty;
} plot_column_t;

typedef struct {
    plot_column_t columns[PLOT_WIDTH];
    uint32_t sequence;
    unsigned slot;
    int column;
    int32_t baseline_q8;
    bool has_sample;
    bool last_valid;
    bool column_has_gap;
} plot_t;

typedef struct {
    int32_t codes[SCOPE_HISTORY_SAMPLES];
    uint32_t sequence;
    unsigned next;
    unsigned count;
} scope_history_t;

typedef struct {
    bool raw_pressed;
    bool stable_pressed;
    int64_t raw_since_us;
    int64_t pressed_since_us;
} boot_button_t;

static QueueHandle_t display_queue;
static plot_t plot;
static scope_history_t scope;
static DMA_ATTR uint16_t pixels[STRIP_WIDTH * PLOT_HEIGHT];
#if ECG_WIFI
static portMUX_TYPE scope_lock = portMUX_INITIALIZER_UNLOCKED;
static ecg_signal_t scope_signal = ECG_SIGNAL_STARTING;
static uint16_t scope_bpm;
static uint32_t scope_peaks[ECG_DISPLAY_PEAK_CAPACITY];
static unsigned scope_peaks_next;
static unsigned scope_peaks_count;
#endif

static void clear_column(plot_t *wave, int x)
{
    plot_column_t *column = &wave->columns[x];
    if (column->valid) {
        column->valid = false;
        column->dirty = true;
        if (x + 1 < PLOT_WIDTH && wave->columns[x + 1].joins_previous) {
            wave->columns[x + 1].dirty = true;
        }
    }
}

static void clear_plot(plot_t *wave)
{
    memset(wave, 0, sizeof(*wave));
    for (int x = 0; x < PLOT_WIDTH; ++x) {
        wave->columns[x].dirty = true;
    }
}

static void plot_sample(plot_t *wave, const ecg_wave_sample_t *sample, int sweep_gap)
{
    uint32_t elapsed = wave->has_sample ? sample->sequence - wave->sequence : 0;
    if (elapsed >= PLOT_SAMPLES) {
        clear_plot(wave);
        elapsed = 0;
    }
    const int previous_column = wave->column;
    const unsigned advanced_slot = wave->slot + elapsed;
    const int next_column = advanced_slot * PLOT_WIDTH / PLOT_SAMPLES;
    wave->slot = advanced_slot % PLOT_SAMPLES;
    wave->column = next_column % PLOT_WIDTH;
    if (next_column != previous_column) {
        wave->column_has_gap = false;
    }
    for (int step = previous_column + 1; step <= next_column; ++step) {
        const int x = step % PLOT_WIDTH;
        clear_column(wave, x);
        if (sweep_gap > 0) {
            clear_column(wave, (x + sweep_gap) % PLOT_WIDTH);
        }
    }

    if (!sample->valid) {
        // A bad point also breaks a column shared with a neighboring usable point.
        wave->column_has_gap = true;
        clear_column(wave, wave->column);
    }

    const bool valid = sample->valid && !wave->column_has_gap;
    if (valid) {
        const int32_t code_q8 = sample->code * 256;
        if (!wave->last_valid || elapsed != 1) {
            wave->baseline_q8 = code_q8;
        }
        const int32_t offset = code_q8 - wave->baseline_q8;
        wave->baseline_q8 += offset / (2 * ECG_SAMPLE_HZ);
        int y = PLOT_BASELINE - offset / (256 * ECG_DISPLAY_CODES_PER_PIXEL);
        uint8_t clipped_edge = 0;
        if (y < 0) {
            y = 0;
            clipped_edge = PLOT_CLIP_TOP;
        } else if (y >= PLOT_HEIGHT) {
            y = PLOT_HEIGHT - 1;
            clipped_edge = PLOT_CLIP_BOTTOM;
        }

        plot_column_t *column = &wave->columns[wave->column];
        if (!column->valid) {
            column->top = y;
            column->bottom = y;
            column->clipped_edges = 0;
            column->joins_previous = wave->last_valid && elapsed == 1 &&
                                     wave->column == previous_column + 1;
        } else {
            // Preserve narrow extrema when several samples share one display column.
            column->top = y < column->top ? y : column->top;
            column->bottom = y > column->bottom ? y : column->bottom;
        }
        column->clipped_edges |= clipped_edge;
        column->valid = true;
        column->last = y;
        column->dirty = true;
    }
    wave->last_valid = valid;
    wave->has_sample = true;
    wave->sequence = sample->sequence;
}

static void scope_push(scope_history_t *history, const ecg_wave_sample_t *sample)
{
    const uint32_t elapsed = history->count ? sample->sequence - history->sequence : 1;
    if (elapsed == 0) {
        return;
    }
    if (elapsed >= SCOPE_HISTORY_SAMPLES) {
        history->count = 0;
    }
    const unsigned missing = history->count ? elapsed - 1 : 0;
    for (unsigned i = 0; i < missing; ++i) {
        history->codes[history->next] = INT32_MIN;
        history->next = (history->next + 1) % SCOPE_HISTORY_SAMPLES;
    }
    history->codes[history->next] = sample->valid ? sample->code : INT32_MIN;
    history->next = (history->next + 1) % SCOPE_HISTORY_SAMPLES;
    history->sequence = sample->sequence;
    const unsigned count = history->count + missing + 1;
    history->count = count < SCOPE_HISTORY_SAMPLES ? count : SCOPE_HISTORY_SAMPLES;
}

static int32_t scope_code_at(const scope_history_t *history, unsigned age)
{
    const unsigned index = history->next + SCOPE_HISTORY_SAMPLES - 1 - age;
    return history->codes[index % SCOPE_HISTORY_SAMPLES];
}

static unsigned scope_frame_start(const scope_history_t *history)
{
    const unsigned latest = PLOT_SAMPLES - 1;
    if (scope_code_at(history, 0) == INT32_MIN) {
        return latest;
    }
    int32_t low = INT32_MAX;
    int32_t high = INT32_MIN;
    for (unsigned age = 0; age < history->count; ++age) {
        const int32_t code = scope_code_at(history, age);
        if (code != INT32_MIN) {
            low = code < low ? code : low;
            high = code > high ? code : high;
        }
    }
    if (high - low < 2 * ECG_DISPLAY_CODES_PER_PIXEL) {
        return latest;
    }

    const int32_t level = low + (high - low) / 2;
    const unsigned oldest_trigger = history->count - 1 - SCOPE_PRETRIGGER_SAMPLES;
    // Use the newest rising edge that has enough samples on both sides for a full frame.
    for (unsigned age = latest - SCOPE_PRETRIGGER_SAMPLES; age <= oldest_trigger; ++age) {
        const int32_t previous = scope_code_at(history, age + 1);
        if (previous != INT32_MIN && previous < level && scope_code_at(history, age) >= level) {
            return age + SCOPE_PRETRIGGER_SAMPLES;
        }
    }
    return latest;
}

static void draw_scope_frame(plot_t *wave, const scope_history_t *history)
{
    if (history->count < PLOT_SAMPLES) {
        return;
    }
    const unsigned start = scope_frame_start(history);
    const uint32_t end_sequence = history->sequence - (start - (PLOT_SAMPLES - 1));
    if (wave->has_sample && wave->sequence == end_sequence) {
        return;
    }
    clear_plot(wave);
    for (unsigned slot = 0; slot < PLOT_SAMPLES; ++slot) {
        const unsigned age = start - slot;
        const int32_t code = scope_code_at(history, age);
        const ecg_wave_sample_t sample = {
            .sequence = history->sequence - age,
            .code = code,
            .valid = code != INT32_MIN,
        };
        plot_sample(wave, &sample, 0);
    }
}

static void draw_grid(int start, int width)
{
    const uint16_t color = __builtin_bswap16(COLOR_GRID);
    memset(pixels, 0, width * PLOT_HEIGHT * sizeof(pixels[0]));
    for (int y = PLOT_BASELINE % GRID_HEIGHT; y < PLOT_HEIGHT; y += GRID_HEIGHT) {
        for (int x = 0; x < width; ++x) {
            pixels[y * width + x] = color;
        }
    }
    // Round each absolute time division so the 0.2 s spacing does not accumulate pixel error.
    for (int division = 0; division <= GRID_COLUMNS; ++division) {
        const int x = (division * (PLOT_WIDTH - 1) + GRID_COLUMNS / 2) / GRID_COLUMNS - start;
        if (x >= 0 && x < width) {
            for (int y = 0; y < PLOT_HEIGHT; ++y) {
                pixels[y * width + x] = color;
            }
        }
    }
}

static void flush_plot(plot_t *wave)
{
    for (int start = 0; start < PLOT_WIDTH;) {
        if (!wave->columns[start].dirty) {
            ++start;
            continue;
        }
        int width = 1;
        while (width < STRIP_WIDTH && start + width < PLOT_WIDTH &&
               wave->columns[start + width].dirty) {
            ++width;
        }
        draw_grid(start, width);
        for (int local_x = 0; local_x < width; ++local_x) {
            const int x = start + local_x;
            plot_column_t *column = &wave->columns[x];
            column->dirty = false;
            if (column->valid) {
                int top = column->top;
                int bottom = column->bottom;
                if (x > 0 && column->joins_previous && wave->columns[x - 1].valid) {
                    const int previous = wave->columns[x - 1].last;
                    top = previous < top ? previous : top;
                    bottom = previous > bottom ? previous : bottom;
                }
                for (int y = top; y <= bottom; ++y) {
                    pixels[y * width + local_x] = __builtin_bswap16(COLOR_GREEN);
                }
                if (column->clipped_edges & PLOT_CLIP_TOP) {
                    pixels[local_x] = __builtin_bswap16(COLOR_AMBER);
                }
                if (column->clipped_edges & PLOT_CLIP_BOTTOM) {
                    pixels[(PLOT_HEIGHT - 1) * width + local_x] = __builtin_bswap16(COLOR_AMBER);
                }
            }
        }
        st7789_write(PLOT_X + start, PLOT_Y, width, PLOT_HEIGHT, pixels);
        start += width;
    }
}

static void draw_text(int x, int y, const char *text, int cells, int scale, uint16_t color)
{
    // 5x7 uppercase glyphs from the STM32 display driver.
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
        {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
    };
    const int width = cells * 6 * scale;
    const int height = 7 * scale;
    const uint16_t color_be = __builtin_bswap16(color);
    memset(pixels, 0, width * height * sizeof(pixels[0]));
    for (int i = 0; i < cells && text[i]; ++i) {
        if (text[i] < 'A' || text[i] > 'Z') {
            continue;
        }
        const uint8_t *glyph = letters[text[i] - 'A'];
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < 5 * scale; ++col) {
                if (glyph[col / scale] & (1u << (row / scale))) {
                    pixels[row * width + i * 6 * scale + col] = color_be;
                }
            }
        }
    }
    st7789_write(x, y, width, height, pixels);
}

static void draw_heart_rate(uint16_t bpm)
{
    static const uint8_t digit_segments[] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f,
    };
    static const uint8_t segments[7][4] = {
        {3, 0, 11, 3}, {14, 3, 3, 12}, {14, 18, 3, 12}, {3, 30, 11, 3},
        {0, 18, 3, 12}, {0, 3, 3, 12}, {3, 15, 11, 3},
    };
    uint8_t masks[] = {0, 0x40, 0x40};
    if (bpm) {
        masks[0] = bpm >= 100 ? digit_segments[bpm / 100] : 0;
        masks[1] = bpm >= 10 ? digit_segments[(bpm / 10) % 10] : 0;
        masks[2] = digit_segments[bpm % 10];
    }
    st7789_fill(222, 18, 59, 33, 0);
    for (int digit = 0; digit < 3; ++digit) {
        for (int seg = 0; seg < 7; ++seg) {
            if (masks[digit] & (1u << seg)) {
                const uint8_t *rect = segments[seg];
                st7789_fill(222 + digit * 21 + rect[0], 18 + rect[1], rect[2], rect[3], COLOR_RED);
            }
        }
    }
}

static void draw_status(ecg_signal_t signal)
{
    static const char *const statuses[] = {
        [ECG_SIGNAL_STARTING] = "STARTING", [ECG_SIGNAL_LIVE] = "LIVE",
        [ECG_SIGNAL_SIMULATION] = "SIMULATION", [ECG_SIGNAL_LEADS_OFF] = "LEADS OFF",
        [ECG_SIGNAL_ADC_ERROR] = "ADC ERROR",
        [ECG_SIGNAL_INPUT_CLIP] = "INPUT CLIP",
    };
    uint16_t color = COLOR_AMBER;
    if (signal == ECG_SIGNAL_LIVE) {
        color = COLOR_GREEN;
    } else if (signal == ECG_SIGNAL_LEADS_OFF || signal == ECG_SIGNAL_ADC_ERROR ||
               signal == ECG_SIGNAL_INPUT_CLIP) {
        color = COLOR_RED;
    }
    draw_text(10, 39, statuses[signal], 12, 2, color);
}

static bool boot_short_press(boot_button_t *button, int64_t now_us)
{
    const bool pressed = gpio_get_level(BOARD_BOOT) == BOARD_BOOT_ACTIVE_LEVEL;
    if (pressed != button->raw_pressed) {
        button->raw_pressed = pressed;
        button->raw_since_us = now_us;
    }
    if (pressed == button->stable_pressed || now_us - button->raw_since_us < ECG_BOOT_DEBOUNCE_MS * 1000) {
        return false;
    }
    button->stable_pressed = pressed;
    if (pressed) {
        button->pressed_since_us = now_us;
        return false;
    }
    return button->pressed_since_us > 0 &&
           now_us - button->pressed_since_us <= ECG_BOOT_SHORT_PRESS_MAX_MS * 1000;
}

static void display_task(void *arg)
{
    st7789_init();
    static const uint16_t bars[] = {
        ST7789_RGB(255, 0, 0), ST7789_RGB(0, 255, 0),
        ST7789_RGB(0, 0, 255), ST7789_RGB(255, 255, 255),
    };
    const int bar_width = BOARD_LCD_WIDTH / (sizeof(bars) / sizeof(bars[0]));
    for (int i = 0; i < sizeof(bars) / sizeof(bars[0]); ++i) {
        st7789_fill(i * bar_width, 0, bar_width, BOARD_LCD_HEIGHT, bars[i]);
    }
    vTaskDelay(pdMS_TO_TICKS(ECG_LCD_SELF_TEST_MS));
    st7789_fill(0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, 0);
    draw_text(10, 14, "ECG MONITOR", 11, 2, COLOR_TEXT);
    draw_text(10, 64, "SWEEP", 5, 2, COLOR_GREEN);
    draw_text(192, 24, "HR", 2, 2, COLOR_RED);
    draw_text(286, 32, "BPM", 3, 1, COLOR_RED);
    draw_heart_rate(0);

    const bool initial_press = gpio_get_level(BOARD_BOOT) == BOARD_BOOT_ACTIVE_LEVEL;
    boot_button_t button = {.raw_pressed = initial_press, .stable_pressed = initial_press};
    ecg_signal_t current = ECG_SIGNAL_STARTING;
    ecg_signal_t shown = current;
    bool scope_mode = false;
    int64_t last_scope_refresh_us = 0;
    unsigned clip_hold = 0;
    uint16_t bpm = 0;
    uint16_t shown_bpm = 0;
    int64_t last_filtered_us = 0;
    ecg_filter_t filter;
    ecg_qrs_t qrs;
    ecg_qrs_init(&qrs, ECG_SAMPLE_HZ);
    if (ECG_DIAGNOSTICS) {
        static const char initializing[] = "# boot LCD ready, initializing display filter\r\n";
        uart_write_bytes(BOARD_UART, initializing, sizeof(initializing) - 1);
    }
    ecg_filter_init(&filter, ECG_SAMPLE_HZ, ECG_DISPLAY_NOTCH_HZ, ECG_DISPLAY_NOTCH_Q,
                    ECG_DISPLAY_LOWPASS_HZ, ECG_DISPLAY_SPIKE_STEP_CODES,
                    ECG_DISPLAY_SPIKE_NEIGHBORS_CODES);
    draw_status(current);
    clear_plot(&plot);
    flush_plot(&plot);
    xQueueReset(display_queue);  // Start the visible sweep after the self-test.
    if (ECG_DIAGNOSTICS) {
        static const char ready[] = "# boot display ready\r\n";
        uart_write_bytes(BOARD_UART, ready, sizeof(ready) - 1);
    }
    TickType_t frame = xTaskGetTickCount();

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        const bool toggled = boot_short_press(&button, now_us);
        if (toggled) {
            scope_mode = !scope_mode;
            draw_text(10, 64, scope_mode ? "SCOPE" : "SWEEP", 5, 2, COLOR_GREEN);
            clear_plot(&plot);
        }

        display_sample_t sample;
        while (xQueueReceive(display_queue, &sample, 0) == pdPASS) {
            const ecg_signal_t previous = current;
            const bool live = sample.signal == ECG_SIGNAL_LIVE;
            const bool clipped = live && (sample.raw <= ECG_DISPLAY_RAIL_LOW_CODE ||
                                          sample.raw >= ECG_DISPLAY_RAIL_HIGH_CODE);
            current = sample.signal;
            if (!live) {
                clip_hold = 0;
            } else {
                if (clipped) {
                    clip_hold = ECG_DISPLAY_CLIP_HOLD_SAMPLES;
                }
                if (clip_hold > 0) {
                    current = ECG_SIGNAL_INPUT_CLIP;
                    --clip_hold;
                }
            }

            const ecg_wave_sample_t input = {
                .sequence = sample.sequence,
                .code = sample.raw,
                .valid = (live || sample.signal == ECG_SIGNAL_SIMULATION) && !clipped,
            };
            ecg_wave_sample_t filtered;
            if (ecg_filter_push(&filter, input, &filtered)) {
                const bool usable_signal = current == ECG_SIGNAL_LIVE || current == ECG_SIGNAL_SIMULATION;
                if (current != previous &&
                    (current == ECG_SIGNAL_LEADS_OFF || current == ECG_SIGNAL_INPUT_CLIP)) {
                    ecg_qrs_reset(&qrs);
                }
                uint32_t peak_sequence;
                const bool beat = usable_signal && ecg_qrs_push(&qrs, filtered, &peak_sequence);
                bpm = usable_signal ? ecg_qrs_bpm(&qrs, filtered.sequence) : 0;
                last_filtered_us = now_us;
#if ECG_WIFI
                portENTER_CRITICAL(&scope_lock);
                scope_signal = current;
                scope_bpm = bpm;
                if (beat) {
                    scope_peaks[scope_peaks_next] = peak_sequence;
                    scope_peaks_next = (scope_peaks_next + 1) % ECG_DISPLAY_PEAK_CAPACITY;
                    if (scope_peaks_count < ECG_DISPLAY_PEAK_CAPACITY) {
                        ++scope_peaks_count;
                    }
                }
#else
                (void)beat;
#endif
                scope_push(&scope, &filtered);
#if ECG_WIFI
                portEXIT_CRITICAL(&scope_lock);
#endif
                if (!scope_mode) {
                    plot_sample(&plot, &filtered, SWEEP_GAP);
                }
            }
        }
        if (current != shown) {
            draw_status(current);
            shown = current;
        }
        if (now_us - last_filtered_us > DISPLAY_DATA_TIMEOUT_US) {
            bpm = 0;
        }
        if (bpm != shown_bpm) {
            draw_heart_rate(bpm);
            shown_bpm = bpm;
        }
        if (scope_mode && (toggled || now_us - last_scope_refresh_us >= SCOPE_REFRESH_US)) {
            draw_scope_frame(&plot, &scope);
            last_scope_refresh_us = now_us;
        }
        flush_plot(&plot);
        vTaskDelayUntil(&frame, pdMS_TO_TICKS(ECG_DISPLAY_REFRESH_MS));
    }
}

void ecg_display_start(void)
{
    const gpio_config_t boot_input = {
        .pin_bit_mask = 1ULL << BOARD_BOOT,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&boot_input));
    display_queue = xQueueCreate(LCD_QUEUE_LENGTH, sizeof(display_sample_t));
    ESP_ERROR_CHECK(display_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(display_task, "ecg_lcd", 4096, NULL, 3, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
}

bool ecg_display_post(uint32_t sequence, int16_t raw, ecg_signal_t signal)
{
    const display_sample_t sample = {.sequence = sequence, .raw = raw, .signal = signal};
    return xQueueSend(display_queue, &sample, 0) == pdPASS;
}

#if ECG_WIFI
ecg_display_snapshot_t ecg_display_snapshot(int32_t *data, size_t capacity, const uint32_t *after)
{
    portENTER_CRITICAL(&scope_lock);
    const size_t available = scope.count < capacity ? scope.count : capacity;
    const uint32_t elapsed = after ? scope.sequence - *after : 0;
    const bool incremental = after && available && elapsed <= available;
    const size_t count = incremental ? elapsed : available;
    ecg_display_snapshot_t result = {
        .sequence = scope.sequence,
        .signal = scope_signal,
        .bpm = scope_bpm,
        .samples = count,
        .incremental = incremental,
    };
    const size_t first = (scope.next + SCOPE_HISTORY_SAMPLES - count) % SCOPE_HISTORY_SAMPLES;
    const size_t contiguous = SCOPE_HISTORY_SAMPLES - first;
    const size_t head = count < contiguous ? count : contiguous;
    memcpy(data, scope.codes + first, head * sizeof(*data));
    memcpy(data + head, scope.codes, (count - head) * sizeof(*data));
    for (unsigned i = 0; i < scope_peaks_count; ++i) {
        const unsigned index = (scope_peaks_next + ECG_DISPLAY_PEAK_CAPACITY - 1 - i) %
                               ECG_DISPLAY_PEAK_CAPACITY;
        const uint32_t age = scope.sequence - scope_peaks[index];
        if (age >= available) {
            break;
        }
        data[count + result.peaks++] = age;
    }
    portEXIT_CRITICAL(&scope_lock);
    return result;
}
#endif
