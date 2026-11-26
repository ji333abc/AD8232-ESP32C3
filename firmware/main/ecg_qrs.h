#pragma once

#include "ecg_filter.h"

enum {
    ECG_QRS_MIN_RR_MS = 250,
    ECG_QRS_MAX_RR_MS = 2400,
    ECG_QRS_PEAK_LOOKBACK = 32,
    ECG_QRS_RATE_WINDOW = 6,
};

typedef struct {
    uint32_t sequence;
    float amplitude;
} ecg_qrs_peak_t;

typedef struct {
    unsigned sample_hz;
    unsigned lookback_samples;
    float baseline_alpha;
    float peak_baseline_alpha;
    float band_alpha;
    float envelope_alpha;
    float noise_alpha;
    float minimum_envelope;
    struct {
        ecg_qrs_peak_t recent[ECG_QRS_PEAK_LOOKBACK];
        ecg_qrs_peak_t candidate_peak;
        uint32_t last_sample;
        uint32_t last_peak;
        uint32_t candidate_start;
        unsigned recent_next;
        unsigned recent_count;
        unsigned warmup;
        unsigned rr[ECG_QRS_RATE_WINDOW];
        unsigned rr_count;
        float baseline;
        float peak_baseline;
        float smooth;
        float band;
        float envelope;
        float noise;
        float signal;
        float candidate_energy;
        float smoothed_bpm;
        uint16_t bpm;
        bool primed;
        bool has_peak;
        bool candidate;
        bool learning;
    } state;
} ecg_qrs_t;

void ecg_qrs_init(ecg_qrs_t *qrs, unsigned sample_hz);
void ecg_qrs_reset(ecg_qrs_t *qrs);
bool ecg_qrs_push(ecg_qrs_t *qrs, ecg_wave_sample_t sample, uint32_t *peak_sequence);
uint16_t ecg_qrs_bpm(const ecg_qrs_t *qrs, uint32_t sequence);
