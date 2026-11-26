#include "ecg_qrs.h"

#include <math.h>
#include <string.h>

enum {
    WARMUP_MS = 1500,
    LOOKBACK_MS = 32,
    MIN_CANDIDATE_MS = 16,
    MAX_CANDIDATE_MS = 175,
    MAX_SHORT_GAP_MS = 25,
    RATE_TIMEOUT_MS = 2600,
    MIN_RATE_INTERVALS = 3,
    RATE_SMOOTH_MS = 2800,
};

static unsigned ticks(const ecg_qrs_t *qrs, unsigned ms)
{
    return (qrs->sample_hz * ms + 999) / 1000;
}

void ecg_qrs_reset(ecg_qrs_t *qrs)
{
    memset(&qrs->state, 0, sizeof(qrs->state));
}

void ecg_qrs_init(ecg_qrs_t *qrs, unsigned sample_hz)
{
    qrs->sample_hz = sample_hz;
    const unsigned lookback = ticks(qrs, LOOKBACK_MS);
    qrs->lookback_samples = lookback < ECG_QRS_PEAK_LOOKBACK ? lookback : ECG_QRS_PEAK_LOOKBACK;
    // First-order smoothing coefficients, with 2*pi*cutoff in the numerator.
    qrs->baseline_alpha = 70.0f / (sample_hz + 70.0f);
    qrs->peak_baseline_alpha = 14.0f / (sample_hz + 14.0f);
    qrs->band_alpha = 160.0f / (sample_hz + 160.0f);
    qrs->envelope_alpha = 40.0f / (sample_hz + 40.0f);
    qrs->noise_alpha = 2.0f / (sample_hz + 2.0f);
    qrs->minimum_envelope = 8.0f * 360.0f / sample_hz;
    ecg_qrs_reset(qrs);
}

uint16_t ecg_qrs_bpm(const ecg_qrs_t *qrs, uint32_t sequence)
{
    if (sequence - qrs->state.last_sample > ticks(qrs, MAX_SHORT_GAP_MS) ||
        sequence - qrs->state.last_peak > ticks(qrs, RATE_TIMEOUT_MS)) {
        return 0;
    }
    return qrs->state.bpm;
}

static void restart_envelope(ecg_qrs_t *qrs, int32_t code, bool learning)
{
    qrs->state.baseline = code;
    qrs->state.peak_baseline = code;
    qrs->state.smooth = 0;
    qrs->state.band = 0;
    qrs->state.envelope = 0;
    qrs->state.recent_next = 0;
    qrs->state.recent_count = 0;
    qrs->state.candidate = false;
    qrs->state.warmup = ticks(qrs, learning ? WARMUP_MS : LOOKBACK_MS);
    qrs->state.learning = learning;
    qrs->state.primed = true;
}

static ecg_qrs_peak_t recent_peak(const ecg_qrs_t *qrs)
{
    ecg_qrs_peak_t peak = {0};
    for (unsigned age = 0; age < qrs->state.recent_count; ++age) {
        const unsigned index = (qrs->state.recent_next + ECG_QRS_PEAK_LOOKBACK - 1 - age) %
                               ECG_QRS_PEAK_LOOKBACK;
        const ecg_qrs_peak_t point = qrs->state.recent[index];
        if (age == 0 || point.amplitude > peak.amplitude) {
            peak = point;
        }
    }
    return peak;
}

static void update_rate(ecg_qrs_t *qrs, unsigned rr)
{
    memmove(qrs->state.rr + 1, qrs->state.rr,
            (ECG_QRS_RATE_WINDOW - 1) * sizeof(qrs->state.rr[0]));
    qrs->state.rr[0] = rr;
    if (qrs->state.rr_count < ECG_QRS_RATE_WINDOW) {
        ++qrs->state.rr_count;
    }
    const unsigned count = qrs->state.rr_count;
    if (count < MIN_RATE_INTERVALS) {
        return;
    }

    // A false beat can split one interval into two short intervals. The median
    // of six also averages the middle pair when short and long intervals alternate.
    unsigned sorted[ECG_QRS_RATE_WINDOW];
    memcpy(sorted, qrs->state.rr, count * sizeof(sorted[0]));
    for (unsigned i = 1; i < count; ++i) {
        const unsigned value = sorted[i];
        unsigned j = i;
        while (j > 0 && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }
    const float median = (sorted[(count - 1) / 2] + sorted[count / 2]) * 0.5f;
    const float target_bpm = 60.0f * qrs->sample_hz / median;
    if (qrs->state.bpm == 0) {
        qrs->state.smoothed_bpm = target_bpm;
    } else {
        // Weight by elapsed time so smoothing is similar at different rates.
        const float weight = (float)rr / (rr + ticks(qrs, RATE_SMOOTH_MS));
        qrs->state.smoothed_bpm += weight * (target_bpm - qrs->state.smoothed_bpm);
    }
    qrs->state.bpm = (uint16_t)(qrs->state.smoothed_bpm + 0.5f);
}

static void clear_rate(ecg_qrs_t *qrs)
{
    qrs->state.rr_count = 0;
    qrs->state.smoothed_bpm = 0;
    qrs->state.bpm = 0;
}

static bool accept_candidate(ecg_qrs_t *qrs, uint32_t *peak_sequence)
{
    const uint32_t peak = qrs->state.candidate_peak.sequence;
    const uint32_t rr = peak - qrs->state.last_peak;
    if (qrs->state.has_peak && (rr < ticks(qrs, ECG_QRS_MIN_RR_MS) || rr > INT32_MAX)) {
        return false;
    }
    if (qrs->state.has_peak && rr <= ticks(qrs, ECG_QRS_MAX_RR_MS)) {
        update_rate(qrs, rr);
    } else {
        clear_rate(qrs);
    }
    qrs->state.signal += 0.25f * (qrs->state.candidate_energy - qrs->state.signal);
    qrs->state.has_peak = true;
    qrs->state.last_peak = peak;
    *peak_sequence = peak;
    return true;
}

bool ecg_qrs_push(ecg_qrs_t *qrs, ecg_wave_sample_t sample, uint32_t *peak_sequence)
{
    if (!sample.valid) {
        return false;
    }
    const uint32_t gap = sample.sequence - qrs->state.last_sample;
    if (!qrs->state.primed || gap > ticks(qrs, MAX_SHORT_GAP_MS)) {
        ecg_qrs_reset(qrs);
        restart_envelope(qrs, sample.code, true);
    } else if (gap != 1) {
        restart_envelope(qrs, sample.code, false);
    }
    qrs->state.last_sample = sample.sequence;
    if (qrs->state.has_peak && sample.sequence - qrs->state.last_peak > ticks(qrs, RATE_TIMEOUT_MS)) {
        qrs->state.has_peak = false;
        qrs->state.signal = 0;
        clear_rate(qrs);
    }

    // Remove slow baseline changes, then smooth twice in the QRS band.
    qrs->state.baseline += qrs->baseline_alpha * (sample.code - qrs->state.baseline);
    const float centered = sample.code - qrs->state.baseline;
    qrs->state.smooth += qrs->band_alpha * (centered - qrs->state.smooth);
    const float band = qrs->state.band + qrs->band_alpha * (qrs->state.smooth - qrs->state.band);
    const float slope = fabsf(band - qrs->state.band);
    qrs->state.band = band;
    qrs->state.envelope += qrs->envelope_alpha * (slope - qrs->state.envelope);

    // Locate the peak on the displayed waveform using a slower baseline than the detector.
    qrs->state.peak_baseline += qrs->peak_baseline_alpha * (sample.code - qrs->state.peak_baseline);
    const ecg_qrs_peak_t point = {
        .sequence = sample.sequence,
        .amplitude = fabsf(sample.code - qrs->state.peak_baseline),
    };
    qrs->state.recent[qrs->state.recent_next] = point;
    qrs->state.recent_next = (qrs->state.recent_next + 1) % ECG_QRS_PEAK_LOOKBACK;
    if (qrs->state.recent_count < qrs->lookback_samples) {
        ++qrs->state.recent_count;
    }
    qrs->state.noise += qrs->noise_alpha * (qrs->state.envelope - qrs->state.noise);
    if (qrs->state.warmup) {
        if (qrs->state.learning) {
            qrs->state.signal = fmaxf(qrs->state.signal, qrs->state.envelope);
        }
        --qrs->state.warmup;
        return false;
    }
    if (!qrs->state.has_peak) {
        // Recover after an unusually strong startup transient.
        qrs->state.signal -= 0.15f * qrs->noise_alpha * qrs->state.signal;
    }

    // Gate the integrated slope by the noise floor and recently accepted complexes.
    const float threshold = fmaxf(qrs->minimum_envelope,
                                  fmaxf(2.2f * qrs->state.noise, 0.24f * qrs->state.signal));
    if (qrs->state.envelope > threshold) {
        if (!qrs->state.candidate) {
            qrs->state.candidate = true;
            qrs->state.candidate_start = sample.sequence;
            qrs->state.candidate_energy = 0;
            qrs->state.candidate_peak = recent_peak(qrs);
        }
        qrs->state.candidate_energy = fmaxf(qrs->state.candidate_energy, qrs->state.envelope);
        if (point.amplitude > qrs->state.candidate_peak.amplitude) {
            qrs->state.candidate_peak = point;
        }
        return false;
    }

    if (!qrs->state.candidate) {
        return false;
    }
    qrs->state.candidate = false;
    const uint32_t width = sample.sequence - qrs->state.candidate_start;
    return width >= ticks(qrs, MIN_CANDIDATE_MS) && width <= ticks(qrs, MAX_CANDIDATE_MS) &&
           accept_candidate(qrs, peak_sequence);
}
