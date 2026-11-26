#include "ecg_filter.h"

#include <math.h>

void ecg_filter_init(ecg_filter_t *filter, float sample_rate, float notch_hz,
                     float notch_q, float lowpass_hz, int32_t spike_step,
                     int32_t spike_neighbors)
{
    // Normalize the W3C/RBJ biquad coefficients by a0 once at startup.
    const float radians_per_hz = 6.283185307179586f / sample_rate;
    const float notch_angle = notch_hz * radians_per_hz;
    const float notch_cos = cosf(notch_angle);
    const float notch_alpha = sinf(notch_angle) / (2.0f * notch_q);
    const float notch_gain = 1.0f / (1.0f + notch_alpha);

    const float lowpass_angle = lowpass_hz * radians_per_hz;
    const float lowpass_cos = cosf(lowpass_angle);
    // The Butterworth Q is 1/sqrt(2).
    const float lowpass_alpha = sinf(lowpass_angle) * 0.7071067811865475f;
    const float lowpass_gain = 1.0f / (1.0f + lowpass_alpha);
    const float lowpass_b1 = (1.0f - lowpass_cos) * lowpass_gain;

    *filter = (ecg_filter_t) {
        .notch = {
            .b0 = notch_gain, .b1 = -2.0f * notch_cos * notch_gain, .b2 = notch_gain,
            .a1 = -2.0f * notch_cos * notch_gain, .a2 = (1.0f - notch_alpha) * notch_gain,
        },
        .lowpass = {
            .b0 = 0.5f * lowpass_b1, .b1 = lowpass_b1, .b2 = 0.5f * lowpass_b1,
            .a1 = -2.0f * lowpass_cos * lowpass_gain, .a2 = (1.0f - lowpass_alpha) * lowpass_gain,
        },
        .spike_step = spike_step,
        .spike_neighbors = spike_neighbors,
    };
}

void ecg_filter_reset(ecg_filter_t *filter)
{
    filter->has_pending = false;
    filter->previous.valid = false;
    filter->primed = false;
}

static float biquad_sample(ecg_biquad_t *stage, float input)
{
    const float output = stage->b0 * input + stage->z1;
    stage->z1 = stage->b1 * input - stage->a1 * output + stage->z2;
    stage->z2 = stage->b2 * input - stage->a2 * output;
    return output;
}

static bool isolated_spike(const ecg_filter_t *filter, int32_t left, int32_t center, int32_t right)
{
    const int32_t neighbor_delta = left - right;
    const int32_t from_left = center - left;
    const int32_t from_right = center - right;

    return neighbor_delta >= -filter->spike_neighbors && neighbor_delta <= filter->spike_neighbors &&
           ((from_left >= filter->spike_step && from_right >= filter->spike_step) ||
            (from_left <= -filter->spike_step && from_right <= -filter->spike_step));
}

bool ecg_filter_push(ecg_filter_t *filter, ecg_wave_sample_t input, ecg_wave_sample_t *result)
{
    if (!filter->has_pending) {
        filter->pending = input;
        filter->has_pending = true;
        return false;
    }

    const ecg_wave_sample_t center = filter->pending;
    const ecg_wave_sample_t left = filter->previous;
    const bool left_contiguous = left.valid && center.sequence - left.sequence == 1;
    const bool right_contiguous = input.valid && input.sequence - center.sequence == 1;
    const bool spike = center.valid && left_contiguous && right_contiguous &&
                       isolated_spike(filter, left.code, center.code, input.code);

    *result = center;
    result->valid = center.valid && !spike;
    filter->pending = input;
    filter->previous = center;
    filter->previous.valid = result->valid;

    if (!result->valid || !left_contiguous) {
        filter->primed = false;
    }
    if (result->valid) {
        if (!filter->primed) {
            // Center on the first usable code so DC cannot cause a startup transient.
            filter->reference = center.code;
            filter->notch.z1 = filter->notch.z2 = 0.0f;
            filter->lowpass.z1 = filter->lowpass.z2 = 0.0f;
            filter->primed = true;
        }
        const float centered = center.code - filter->reference;
        const float notched = biquad_sample(&filter->notch, centered);
        const float smoothed = biquad_sample(&filter->lowpass, notched);
        result->code = (int32_t)lroundf(smoothed + filter->reference);
    }
    return true;
}
