#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t sequence;
    int32_t code;
    bool valid;
} ecg_wave_sample_t;

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float z1, z2;
} ecg_biquad_t;

typedef struct {
    ecg_biquad_t notch;
    ecg_biquad_t lowpass;
    ecg_wave_sample_t previous;
    ecg_wave_sample_t pending;
    float reference;
    int32_t spike_step;
    int32_t spike_neighbors;
    bool has_pending;
    bool primed;
} ecg_filter_t;

void ecg_filter_init(ecg_filter_t *filter, float sample_rate, float notch_hz,
                     float notch_q, float lowpass_hz, int32_t spike_step,
                     int32_t spike_neighbors);
void ecg_filter_reset(ecg_filter_t *filter);

// Each result describes the preceding input and retains its sequence number.
bool ecg_filter_push(ecg_filter_t *filter, ecg_wave_sample_t input, ecg_wave_sample_t *result);
