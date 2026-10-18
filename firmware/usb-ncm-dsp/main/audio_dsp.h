#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_DSP_SAMPLE_RATE 16000
#define AUDIO_DSP_FFT_SIZE 1024
#define AUDIO_DSP_HOP_SIZE 256
#define AUDIO_DSP_PACKET_SIZE (AUDIO_DSP_HOP_SIZE * 2)

esp_err_t audio_dsp_init(void);
bool audio_dsp_process_hop(const int16_t samples[AUDIO_DSP_HOP_SIZE],
                           uint8_t heights[8]);
