#include "audio_dsp.h"

#include "esp_dsp.h"

#include <math.h>
#include <string.h>

#define MIN_FREQ_HZ 40.0f
#define MAX_FREQ_HZ 7500.0f
#define DYNAMIC_RANGE_DB 30.0f
#define HEADROOM_DB 8.0f
#define ATTACK_SECONDS 0.025f
#define DECAY_SECONDS 0.10f
#define AGC_ATTACK_SECONDS 0.35f
#define AGC_DECAY_SECONDS 2.0f
#define MIN_REFERENCE 0.003f
#define SILENCE_RMS 0.0005f
#define TONAL_PEAK_WEIGHT 0.55f
#define DSP_PI 3.14159265358979323846f
static float frame[AUDIO_DSP_FFT_SIZE];
__attribute__((aligned(16))) static float fft_data[2 * AUDIO_DSP_FFT_SIZE];
static float window_values[AUDIO_DSP_FFT_SIZE];
static float window_sum;
static float bin_magnitude[AUDIO_DSP_FFT_SIZE / 2 + 1];
static float smoothed[8];
static float reference_level = MIN_REFERENCE;
static bool initialized;

static const float band_gain[8] = {
    1.7783f, 1.5849f, 1.5849f, 1.5849f,
    1.2589f, 1.0f, 0.7079f, 0.5012f,
};

esp_err_t audio_dsp_init(void)
{
    esp_err_t err = dsps_fft2r_init_fc32(NULL, AUDIO_DSP_FFT_SIZE);
    if (err != ESP_OK) return err;

    memset(frame, 0, sizeof(frame));
    memset(smoothed, 0, sizeof(smoothed));
    reference_level = MIN_REFERENCE;
    window_sum = 0.0f;
    for (unsigned i = 0; i < AUDIO_DSP_FFT_SIZE; ++i) {
        window_values[i] = 0.5f - 0.5f * cosf(
            2.0f * DSP_PI * (float)i / (float)(AUDIO_DSP_FFT_SIZE - 1));
        window_sum += window_values[i];
    }
    initialized = true;
    return ESP_OK;
}

bool audio_dsp_process_hop(const int16_t samples[AUDIO_DSP_HOP_SIZE],
                           uint8_t heights[8])
{
    if (!initialized || samples == NULL || heights == NULL) return false;

    memmove(frame, frame + AUDIO_DSP_HOP_SIZE,
            (AUDIO_DSP_FFT_SIZE - AUDIO_DSP_HOP_SIZE) * sizeof(frame[0]));
    for (unsigned i = 0; i < AUDIO_DSP_HOP_SIZE; ++i) {
        frame[AUDIO_DSP_FFT_SIZE - AUDIO_DSP_HOP_SIZE + i]
            = (float)samples[i] / 32768.0f;
    }

    float power_sum = 0.0f;
    for (unsigned i = 0; i < AUDIO_DSP_FFT_SIZE; ++i) {
        power_sum += frame[i] * frame[i];
    }
    const bool silence = sqrtf(power_sum / AUDIO_DSP_FFT_SIZE) < SILENCE_RMS;

    if (silence) {
        memset(bin_magnitude, 0, sizeof(bin_magnitude));
    } else {
        for (unsigned i = 0; i < AUDIO_DSP_FFT_SIZE; ++i) {
            fft_data[2 * i] = frame[i] * window_values[i];
            fft_data[2 * i + 1] = 0.0f;
        }
        if (dsps_fft2r_fc32(fft_data, AUDIO_DSP_FFT_SIZE) != ESP_OK
            || dsps_bit_rev_fc32(fft_data, AUDIO_DSP_FFT_SIZE) != ESP_OK) {
            return false;
        }
        const float scale = 2.0f / window_sum;
        for (unsigned i = 0; i <= AUDIO_DSP_FFT_SIZE / 2; ++i) {
            bin_magnitude[i] = hypotf(fft_data[2 * i], fft_data[2 * i + 1]) * scale;
        }
    }

    float levels[8];
    float peak = 0.0f;
    for (unsigned band = 0; band < 8; ++band) {
        const float low = MIN_FREQ_HZ * powf(MAX_FREQ_HZ / MIN_FREQ_HZ,
                                              (float)band / 8.0f);
        const float high = MIN_FREQ_HZ * powf(MAX_FREQ_HZ / MIN_FREQ_HZ,
                                               (float)(band + 1) / 8.0f);
        unsigned first = (unsigned)ceilf(low * AUDIO_DSP_FFT_SIZE
                                         / AUDIO_DSP_SAMPLE_RATE);
        unsigned last = (unsigned)ceilf(high * AUDIO_DSP_FFT_SIZE
                                        / AUDIO_DSP_SAMPLE_RATE);
        if (last > AUDIO_DSP_FFT_SIZE / 2) last = AUDIO_DSP_FFT_SIZE / 2;
        if (first < 1) first = 1;
        if (last <= first) last = first + 1;

        float sum_squares = 0.0f;
        float maximum = 0.0f;
        unsigned count = 0;
        for (unsigned bin = first; bin < last; ++bin) {
            const float magnitude = bin_magnitude[bin];
            sum_squares += magnitude * magnitude;
            if (magnitude > maximum) maximum = magnitude;
            ++count;
        }
        const float rms = count ? sqrtf(sum_squares / count) : 0.0f;
        levels[band] = fmaxf(rms, TONAL_PEAK_WEIGHT * maximum) * band_gain[band];
        if (levels[band] > peak) peak = levels[band];
    }

    const float dt = (float)AUDIO_DSP_HOP_SIZE / AUDIO_DSP_SAMPLE_RATE;
    const float agc_time = peak > reference_level ? AGC_ATTACK_SECONDS : AGC_DECAY_SECONDS;
    const float agc_alpha = 1.0f - expf(-dt / agc_time);
    reference_level += agc_alpha * (peak - reference_level);
    if (reference_level < MIN_REFERENCE) reference_level = MIN_REFERENCE;

    for (unsigned band = 0; band < 8; ++band) {
        const float relative_db = 20.0f * log10f(fmaxf(levels[band], 1e-12f)
                                                  / reference_level);
        float target = 8.0f * (relative_db + DYNAMIC_RANGE_DB - HEADROOM_DB)
                     / DYNAMIC_RANGE_DB;
        if (target < 0.0f) target = 0.0f;
        if (target > 8.0f) target = 8.0f;

        const float smoothing_time = target > smoothed[band]
                                   ? ATTACK_SECONDS : DECAY_SECONDS;
        const float alpha = 1.0f - expf(-dt / smoothing_time);
        smoothed[band] += alpha * (target - smoothed[band]);
        heights[band] = (uint8_t)lrintf(smoothed[band]);
    }
    return true;
}
