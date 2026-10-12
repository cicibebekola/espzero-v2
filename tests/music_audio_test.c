#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcm_resampler.h"
#include "minimp3.h"

typedef struct { int16_t *data; size_t frames; } capture_t;
static bool capture(const int16_t *pcm, size_t frames, void *arg)
{
    capture_t *out = arg;
    assert(frames <= 256);
    assert(out->frames + frames < 48000);
    memcpy(out->data + out->frames * 2, pcm, frames * 4);
    out->frames += frames;
    return true;
}
static bool reject(const int16_t *pcm, size_t frames, void *arg)
{ (void)pcm; (void)frames; (void)arg; return false; }
int main(void)
{
    int16_t input[2000];
    for (int i = 0; i < 1000; ++i) { input[i*2] = i*20-10000; input[i*2+1] = 10000-i*20; }
    capture_t one = {.data=calloc(96000, sizeof(int16_t))};
    capture_t split = {.data=calloc(96000, sizeof(int16_t))};
    assert(one.data && split.data);
    pcm_resampler_t state = {0};
    assert(pcm_resample(&state,input,1000,2,44100,44100,capture,&one));
    assert(one.frames == 1000);
    assert(memcmp(input,one.data,sizeof(input)) == 0); /* No half-stereo-frame loss. */
    int rates[] = {8000,11025,12000,16000,22050,24000,32000,44100,48000};
    int outputs[] = {16000,32000,44100,48000};
    for (size_t r=0; r<sizeof(rates)/sizeof(rates[0]); ++r) {
        for (size_t o=0; o<sizeof(outputs)/sizeof(outputs[0]); ++o) {
            one.frames=split.frames=0;
            pcm_resampler_t a={0}, b={0};
            assert(pcm_resample(&a,input,1000,2,rates[r],outputs[o],capture,&one));
            for (size_t i=0; i<1000;) {
                size_t n=1000-i < 37 ? 1000-i : 37;
                assert(pcm_resample(&b,input+i*2,n,2,rates[r],outputs[o],capture,&split));
                i+=n;
            }
            assert(one.frames == (size_t)((uint64_t)1000*outputs[o]/rates[r]));
            assert(one.frames == split.frames);
            assert(memcmp(one.data,split.data,one.frames*4)==0);
        }
    }
    state=(pcm_resampler_t){0}; one.frames=0;
    assert(pcm_resample(&state,input,1000,1,22050,44100,capture,&one));
    assert(one.frames==2000);
    for(size_t i=0;i<one.frames;++i) assert(one.data[i*2]==one.data[i*2+1]);
    state=(pcm_resampler_t){0};
    assert(!pcm_resample(&state,input,1000,2,44100,44100,reject,NULL));
    assert(!pcm_resample(&state,input,1000,3,44100,44100,capture,&one));
    assert(!pcm_resample(&state,input,1000,2,0,44100,capture,&one));
    /* A valid MPEG1 Layer III frame with empty/silent granules. */
    unsigned char mp3[417]={0xff,0xfb,0x90,0x00};
    int16_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_t decoder; mp3dec_frame_info_t info;
    mp3dec_init(&decoder);
    int samples=mp3dec_decode_frame(&decoder,mp3,sizeof(mp3),decoded,&info);
    assert(samples==1152 && info.channels==2 && info.hz==44100 && info.frame_bytes==417);
    free(one.data);free(split.data);
    puts("PASS: stereo MP3 frame decode, PCM preservation, mono conversion, 36 rate combinations, chunk continuity, and sink cancellation.");
    return 0;
}
