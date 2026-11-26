#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ECG_SIGNAL_STARTING,
    ECG_SIGNAL_LIVE,
    ECG_SIGNAL_SIMULATION,
    ECG_SIGNAL_LEADS_OFF,
    ECG_SIGNAL_ADC_ERROR,
    ECG_SIGNAL_INPUT_CLIP,
} ecg_signal_t;

void ecg_display_start(void);
bool ecg_display_post(uint32_t sequence, int16_t raw, ecg_signal_t signal);

#if ECG_WIFI
typedef struct {
    uint32_t sequence;
    ecg_signal_t signal;
    uint16_t bpm;
    uint16_t samples;
    unsigned peaks;
    bool incremental;
} ecg_display_snapshot_t;

// Writes up to capacity filtered codes after the cursor, or the full window if
// the cursor is absent or too old. QRS peak ages for the full window follow.
// The buffer must have room for capacity + ECG_DISPLAY_PEAK_CAPACITY entries.
ecg_display_snapshot_t ecg_display_snapshot(int32_t *data, size_t capacity, const uint32_t *after);
#endif
