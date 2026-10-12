#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t phase;
    int rate;
    int output_rate;
    bool initialized;
    int16_t previous[2];
} pcm_resampler_t;
typedef bool (*pcm_sink_t)(const int16_t *stereo, size_t frames, void *arg);
/* Linear conversion to negotiated A2DP stereo PCM, in 256-frame chunks. */
bool pcm_resample(pcm_resampler_t *state, const int16_t *input, size_t frames,
                  int channels, int rate, int output_rate, pcm_sink_t sink, void *arg);
