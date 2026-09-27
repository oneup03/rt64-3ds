// ultramodern audio callbacks on ndsp. The game produces 16-bit stereo at the
// rate it asks for; ndsp resamples in hardware, so the samples go out as they
// are, apart from the L/R swap the runtime's byte-swapped RDRAM imposes.
#include <3ds.h>
#include <cstdio>
#include <cstring>

#include "ultramodern/ultramodern.hpp"
#include "recomp3ds_internal.h"

namespace {

constexpr int kChannel = 0;
constexpr int kNumBufs = 24;            // ~ a quarter second at 60 chunks/s
constexpr size_t kMaxFramesPerBuf = 4096;
bool g_ready = false;
uint32_t g_rate = 32000;
ndspWaveBuf g_bufs[kNumBufs];
int16_t* g_storage[kNumBufs];
int g_next = 0;
float g_volume = 1.0f;

}   // namespace

bool recomp3ds::audio_init() {
    if (R_FAILED(ndspInit())) {
        fprintf(stderr, "recomp3ds: ndspInit failed (no sdmc:/3ds/dspfirm.cdc?); audio off\n");
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(kChannel);
    ndspChnSetInterp(kChannel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(kChannel, (float)g_rate);
    ndspChnSetFormat(kChannel, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(kChannel, mix);
    for (int i = 0; i < kNumBufs; i++) {
        g_storage[i] = (int16_t*)linearAlloc(kMaxFramesPerBuf * 2 * sizeof(int16_t));
        memset(&g_bufs[i], 0, sizeof(g_bufs[i]));
        g_bufs[i].status = NDSP_WBUF_DONE;
    }
    g_ready = true;
    return true;
}

void recomp3ds::audio_set_volume(float v) { g_volume = v; }

void recomp3ds::audio_set_frequency(uint32_t freq) {
    g_rate = freq;
    if (g_ready) {
        ndspChnSetRate(kChannel, (float)freq);
    }
}

void recomp3ds::audio_queue_samples(int16_t* samples, size_t sample_count) {
    if (!g_ready) {
        return;
    }
    size_t frames = sample_count / 2;
    size_t done = 0;
    while (done < frames) {
        ndspWaveBuf* wb = &g_bufs[g_next];
        if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE) {
            // The ring is full: the game is ahead of the DSP. Drop the rest.
            return;
        }
        size_t n = frames - done;
        if (n > kMaxFramesPerBuf) {
            n = kMaxFramesPerBuf;
        }
        int16_t* dst = g_storage[g_next];
        const int16_t* src = samples + done * 2;
        for (size_t i = 0; i < n; i++) {
            // Swap the pair: RDRAM halfwords are stored xor 2, so L and R arrive reversed.
            dst[i * 2 + 0] = (int16_t)(src[i * 2 + 1] * g_volume);
            dst[i * 2 + 1] = (int16_t)(src[i * 2 + 0] * g_volume);
        }
        DSP_FlushDataCache(dst, n * 2 * sizeof(int16_t));
        wb->data_pcm16 = dst;
        wb->nsamples = (u32)n;
        wb->looping = false;
        ndspChnWaveBufAdd(kChannel, wb);
        g_next = (g_next + 1) % kNumBufs;
        done += n;
    }
}

size_t recomp3ds::audio_frames_remaining() {
    if (!g_ready) {
        return 0;
    }
    size_t queued = 0;
    for (int i = 0; i < kNumBufs; i++) {
        if (g_bufs[i].status == NDSP_WBUF_QUEUED || g_bufs[i].status == NDSP_WBUF_PLAYING) {
            queued += g_bufs[i].nsamples;
        }
    }
    // Like the desktop: report one VI's worth less so the game keeps a margin.
    size_t frames_per_vi = g_rate / 60;
    return queued > frames_per_vi ? queued - frames_per_vi : 0;
}

void recomp3ds::audio_shutdown() {
    if (g_ready) {
        g_ready = false;     // the audio task thread may still queue samples
        ndspChnWaveBufClear(kChannel);
        ndspExit();
        for (int i = 0; i < kNumBufs; i++) {
            linearFree(g_storage[i]);
        }
    }
}
