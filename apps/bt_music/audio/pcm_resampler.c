#include "pcm_resampler.h"
#include <string.h>

bool pcm_resample(pcm_resampler_t *state, const int16_t *input, size_t frames,
                  int channels, int rate, int output_rate, pcm_sink_t sink, void *arg)
{
    if (!state || !input || !sink || (channels != 1 && channels != 2) || rate < 8000 || rate > 48000 || output_rate < 16000 || output_rate > 48000)
        return false;
    if (state->rate != rate || state->output_rate != output_rate) {
        memset(state, 0, sizeof(*state)); state->rate = rate; state->output_rate = output_rate;
    }
    int16_t output[256 * 2]; size_t used = 0;
    for (size_t i = 0; i < frames; ++i) {
        int16_t left = input[i * channels], right = input[i * channels + (channels == 2)];
        if (!state->initialized) {
            state->previous[0] = left; state->previous[1] = right; state->initialized = true;
        }
        state->phase += output_rate;
        while (state->phase >= (uint32_t)rate) {
            state->phase -= rate;
            output[used*2] = left + ((int64_t)state->previous[0] - left) * state->phase / output_rate;
            output[used*2+1] = right + ((int64_t)state->previous[1] - right) * state->phase / output_rate;
            if (++used == 256) {
                if (!sink(output, used, arg)) return false;
                used = 0;
            }
        }
        state->previous[0] = left; state->previous[1] = right;
    }
    return used == 0 || sink(output, used, arg);
}
