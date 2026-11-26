#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t rate_hundredths;
    uint32_t missed_ticks;
    uint32_t adc_errors;
    uint32_t display_drops;
} ecg_web_stats_t;

esp_err_t ecg_web_start(void);
void ecg_web_update_stats(const ecg_web_stats_t *stats);
