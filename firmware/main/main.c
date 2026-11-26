#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "board_config.h"
#include "driver/ledc.h"
#include "ecg_display.h"
#if ECG_WIFI
#include "ecg_web.h"
#endif
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define UART_OUTPUT_ENABLED (ECG_UART_OUTPUT || ECG_DIAGNOSTICS)

enum {
    ADS1115_REG_CONVERSION = 0x00,
    ADS1115_REG_CONFIG = 0x01,
    ADS1115_OS = 1u << 15,
    ADS1115_SINGLE_SHOT = 1u << 8,
    ADS1115_DISABLE_COMPARATOR = 3u,
    ADS1115_SETTINGS = BOARD_ADS1115_MUX_BITS | BOARD_ADS1115_PGA_BITS |
                       ADS1115_SINGLE_SHOT | BOARD_ADS1115_RATE_BITS |
                       ADS1115_DISABLE_COMPARATOR,
};

typedef struct {
    int16_t raw;
    uint32_t sequence;
    int64_t timestamp_us;
} sample_t;

typedef struct {
    sample_t sample;
    esp_err_t status;
    bool lod_plus;
    bool lod_minus;
} capture_t;

typedef struct {
    uint32_t adc_errors;
    uint32_t timeouts;
    uint32_t missed_ticks;
    uint32_t queue_drops;
    uint32_t lcd_drops;
    uint32_t rate_hundredths;
    sample_t latest;
    bool lod_plus;
    bool lod_minus;
    esp_err_t adc_status;
} diagnostics_t;

static i2c_master_dev_handle_t adc;
static QueueHandle_t sample_queue;
static QueueHandle_t diagnostics_queue;

static void ads1115_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_ADS1115_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };

    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &device_config, &adc));
}

static esp_err_t ads1115_read_register(uint8_t reg, uint16_t *value)
{
    uint8_t bytes[2];
    esp_err_t err = i2c_master_transmit_receive(adc, &reg, sizeof(reg), bytes,
                                                sizeof(bytes), ECG_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        *value = ((uint16_t)bytes[0] << 8) | bytes[1];
    }
    return err;
}

static esp_err_t ads1115_start(sample_t *sample)
{
    const uint16_t command = ADS1115_OS | ADS1115_SETTINGS;
    const uint8_t bytes[] = {ADS1115_REG_CONFIG, command >> 8, command & 0xff};
    const esp_err_t err = i2c_master_transmit(adc, bytes, sizeof(bytes), ECG_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        sample->timestamp_us = esp_timer_get_time();
    }
    return err;
}

static esp_err_t ads1115_read_result(uint16_t config, sample_t *sample)
{
    if ((config & ~ADS1115_OS) != ADS1115_SETTINGS) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t raw;
    const esp_err_t err = ads1115_read_register(ADS1115_REG_CONVERSION, &raw);
    if (err == ESP_OK) {
        sample->raw = (int16_t)raw;
    }
    return err;
}

static bool ads1115_advance(capture_t *pending, capture_t *next, bool have_pending)
{
    uint16_t config = 0;
    esp_err_t ready = ads1115_read_register(ADS1115_REG_CONFIG, &config);
    if (ready == ESP_OK && !(config & ADS1115_OS)) {
        // A delayed trigger can still be converting at the next tick. Keep its
        // original sequence and skip this trigger without discarding that shot.
        if (have_pending && pending->status == ESP_OK &&
            esp_timer_get_time() - pending->sample.timestamp_us < ECG_CONVERSION_TIMEOUT_US) {
            return false;
        }
        ready = ESP_ERR_TIMEOUT;
    }
    if (have_pending && pending->status == ESP_OK) {
        // Retrieve the old result before starting another conversion.
        pending->status = ready == ESP_OK ? ads1115_read_result(config, &pending->sample) : ready;
    }
    next->status = ready == ESP_OK ? ads1115_start(&next->sample) : ready;
    next->lod_plus = gpio_get_level(BOARD_AD8232_LOD_PLUS);
    next->lod_minus = gpio_get_level(BOARD_AD8232_LOD_MINUS);
    return true;
}

static int16_t simulated_raw(uint32_t sequence)
{
    // P/QRS/T timing from the STM32 ECG_SimSample template, scaled to ADC codes.
    static const struct {
        uint16_t phase;
        int16_t offset;
    } shape[] = {
        {0, 0}, {100, 0}, {145, 350}, {190, 0}, {270, 0}, {295, -450},
        {320, 2750}, {345, -800}, {380, 0}, {460, 0}, {545, 750}, {650, 0}, {1000, 0},
    };

    const uint32_t samples_per_minute = 60u * ECG_SAMPLE_HZ;
    const uint64_t phase_numerator = (uint64_t)(sequence - 1) * ECG_SIM_BPM;
    const int32_t phase = (phase_numerator % samples_per_minute) * 1000u / samples_per_minute;
    for (size_t i = 1; i < sizeof(shape) / sizeof(shape[0]); ++i) {
        if (phase <= shape[i].phase) {
            const int32_t elapsed = phase - shape[i - 1].phase;
            const int32_t span = shape[i].phase - shape[i - 1].phase;
            const int32_t change = shape[i].offset - shape[i - 1].offset;
            return ECG_SIM_BASELINE_RAW + shape[i - 1].offset + change * elapsed / span;
        }
    }
    return ECG_SIM_BASELINE_RAW;
}

static void sampling_task(void *arg)
{
    uint32_t sequence = 0;
    diagnostics_t report = {0};
    capture_t pending = {0};
    bool have_pending = false;
    uint32_t successes = 0;
    int64_t window_start = esp_timer_get_time();

    while (true) {
        const uint32_t ticks = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        sequence += ticks;
        report.missed_ticks += ticks - 1;
        capture_t capture = {.sample.sequence = sequence};

        if (ECG_SIMULATE) {
            capture.sample.timestamp_us = esp_timer_get_time();
            capture.sample.raw = simulated_raw(sequence);
        } else {
            if (!ads1115_advance(&pending, &capture, have_pending)) {
                ++report.missed_ticks;
                continue;
            }
            const capture_t completed = pending;
            pending = capture;
            if (!have_pending) {
                have_pending = true;
                continue;
            }
            capture = completed;
        }

        const sample_t sample = capture.sample;
        report.adc_status = capture.status;
        report.lod_plus = capture.lod_plus;
        report.lod_minus = capture.lod_minus;
        if (report.adc_status == ESP_OK) {
            report.latest = sample;
            successes++;
            if (UART_OUTPUT_ENABLED && xQueueSend(sample_queue, &sample, 0) != pdPASS) {
                report.queue_drops++;
            }
        } else {
            report.adc_errors++;
            report.timeouts += report.adc_status == ESP_ERR_TIMEOUT;
        }

        ecg_signal_t signal = ECG_SIMULATE ? ECG_SIGNAL_SIMULATION : ECG_SIGNAL_LIVE;
        if (report.adc_status != ESP_OK) {
            signal = ECG_SIGNAL_ADC_ERROR;
        } else if (report.lod_plus || report.lod_minus) {
            signal = ECG_SIGNAL_LEADS_OFF;
        }
        if (!ecg_display_post(sample.sequence, sample.raw, signal)) {
            report.lcd_drops++;
        }

        const int64_t now = esp_timer_get_time();
        const int64_t elapsed = now - window_start;
        if (elapsed >= ECG_DIAG_INTERVAL_US) {
            if (ECG_DIAGNOSTICS || ECG_WIFI) {
                report.rate_hundredths = ((int64_t)successes * 100000000 + elapsed / 2) / elapsed;
            }
            if (ECG_DIAGNOSTICS) {
                xQueueOverwrite(diagnostics_queue, &report);
            }
#if ECG_WIFI
            const ecg_web_stats_t stats = {
                .rate_hundredths = report.rate_hundredths,
                .missed_ticks = report.missed_ticks,
                .adc_errors = report.adc_errors,
                .display_drops = report.lcd_drops,
            };
            ecg_web_update_stats(&stats);
#endif
            successes = 0;
            window_start = now;
        }
    }
}

static void uart_task(void *arg)
{
    for (;;) {
        sample_t sample;
        if (xQueueReceive(sample_queue, &sample, pdMS_TO_TICKS(20)) == pdPASS) {
            char line[12];
            const int length = snprintf(line, sizeof(line), "%d\r\n", (int)sample.raw);
            uart_write_bytes(BOARD_UART, line, length);
        }

        diagnostics_t report;
        if (ECG_DIAGNOSTICS && xQueueReceive(diagnostics_queue, &report, 0) == pdPASS) {
            char line[256];
            const int length = snprintf(
                line, sizeof(line),
                "# hz=%" PRIu32 ".%02" PRIu32 " seq=%" PRIu32 " t_us=%" PRId64
                " adc=%s errors=%" PRIu32 " timeouts=%" PRIu32 " missed=%" PRIu32
                " drops=%" PRIu32 " lcd_drops=%" PRIu32 " lod_plus=%d lod_minus=%d\r\n",
                report.rate_hundredths / 100, report.rate_hundredths % 100,
                report.latest.sequence, report.latest.timestamp_us,
                esp_err_to_name(report.adc_status), report.adc_errors, report.timeouts,
                report.missed_ticks, report.queue_drops, report.lcd_drops, report.lod_plus, report.lod_minus);
            if (length > 0 && length < (int)sizeof(line)) {
                uart_write_bytes(BOARD_UART, line, length);
            }
        }
    }
}

static void sampling_tick(void *task)
{
    xTaskNotifyGive((TaskHandle_t)task);
}

static void board_init(void)
{
    ESP_ERROR_CHECK(gpio_set_level(BOARD_BUZZER, !BOARD_BUZZER_ACTIVE_LEVEL));
    ESP_ERROR_CHECK(gpio_set_direction(BOARD_BUZZER, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AD8232_SDN, !ECG_SIMULATE));
    ESP_ERROR_CHECK(gpio_set_direction(BOARD_AD8232_SDN, GPIO_MODE_OUTPUT));
    const gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << BOARD_AD8232_LOD_PLUS) | (1ULL << BOARD_AD8232_LOD_MINUS),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&inputs));
}

static void uart_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = BOARD_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(BOARD_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(BOARD_UART, BOARD_UART_TX, BOARD_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(BOARD_UART, 256, 0, 0, NULL, 0));
}

static esp_err_t play_startup_beeps(void)
{
    if (ECG_STARTUP_BEEPS == 0) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = ECG_STARTUP_BEEP_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t output = {
        .gpio_num = BOARD_BUZZER,
        .speed_mode = timer.speed_mode,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = timer.timer_num,
        .duty = 0,
        .flags.output_invert = !BOARD_BUZZER_ACTIVE_LEVEL,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), "buzzer", "PWM timer setup failed");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&output), "buzzer", "PWM output setup failed");
    ESP_RETURN_ON_ERROR(ledc_set_duty(output.speed_mode, output.channel, 1u << (LEDC_TIMER_8_BIT - 1)),
                        "buzzer", "PWM duty setup failed");

    for (int beep = 0; beep < ECG_STARTUP_BEEPS; ++beep) {
        ESP_RETURN_ON_ERROR(ledc_update_duty(output.speed_mode, output.channel), "buzzer", "PWM start failed");
        vTaskDelay(pdMS_TO_TICKS(ECG_STARTUP_BEEP_MS));
        // Idle zero before GPIO inversion keeps the active-low driver off.
        ESP_RETURN_ON_ERROR(ledc_stop(output.speed_mode, output.channel, 0), "buzzer", "PWM stop failed");
        if (beep + 1 < ECG_STARTUP_BEEPS) {
            vTaskDelay(pdMS_TO_TICKS(ECG_STARTUP_BEEP_GAP_MS));
        }
    }
    return ESP_OK;
}

void app_main(void)
{
    board_init();
    if (UART_OUTPUT_ENABLED) {
        uart_init();
        sample_queue = xQueueCreate(ECG_QUEUE_LENGTH, sizeof(sample_t));
        ESP_ERROR_CHECK(sample_queue ? ESP_OK : ESP_ERR_NO_MEM);
    }
    if (ECG_DIAGNOSTICS) {
        diagnostics_queue = xQueueCreate(1, sizeof(diagnostics_t));
        ESP_ERROR_CHECK(diagnostics_queue ? ESP_OK : ESP_ERR_NO_MEM);
        char line[96];
        const int length = snprintf(line, sizeof(line), "# boot app=%s reset=%d buzzer=%d\r\n",
                                    esp_app_get_description()->version, (int)esp_reset_reason(),
                                    ECG_STARTUP_BUZZER);
        if (length > 0 && length < (int)sizeof(line)) {
            uart_write_bytes(BOARD_UART, line, length);
        }
    }
    if (!ECG_SIMULATE) {
        ads1115_init();
    }
    ecg_display_start();

    TaskHandle_t sampler;
    if (UART_OUTPUT_ENABLED) {
        ESP_ERROR_CHECK(xTaskCreate(uart_task, "ecg_uart", 3072, NULL, 4, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
    }
    // Run the short I2C exchanges ahead of the TCP/IP task.
    ESP_ERROR_CHECK(xTaskCreate(sampling_task, "ecg_sample", 3072, NULL, ECG_SAMPLE_TASK_PRIORITY, &sampler) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);

    const esp_timer_create_args_t timer_config = {
        .callback = sampling_tick,
        .arg = sampler,
        .name = "ecg_sample",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_config, &timer));
    const uint64_t period_us = (1000000u + ECG_SAMPLE_HZ / 2) / ECG_SAMPLE_HZ;
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, period_us));
    if (ECG_DIAGNOSTICS) {
        static const char ready[] = "# boot acquisition ready\r\n";
        uart_write_bytes(BOARD_UART, ready, sizeof(ready) - 1);
    }
    if (ECG_STARTUP_BUZZER) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(play_startup_beeps());
    }
#if ECG_WIFI
    const esp_err_t web_status = ecg_web_start();
    if (ECG_DIAGNOSTICS) {
        char line[96];
        const int length = snprintf(line, sizeof(line), "# boot wifi=%s\r\n",
                                    esp_err_to_name(web_status));
        if (length > 0 && length < (int)sizeof(line)) {
            uart_write_bytes(BOARD_UART, line, length);
        }
    }
#endif
}
