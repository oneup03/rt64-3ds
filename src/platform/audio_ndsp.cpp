// ultramodern audio callbacks on ndsp. The game produces 16-bit stereo at the
// rate it asks for; ndsp resamples in hardware, so the samples go out as they
// are, apart from the L/R swap the runtime's byte-swapped RDRAM imposes.
#include <atomic>
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ultramodern/ultramodern.hpp"
#include "recomp3ds_internal.h"
#include "recomp3ds.h"

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
std::atomic<bool> g_enabled{ true };     // the menu's Audio setting
bool g_started = false;
volatile uint32_t g_submitted = 0, g_dropped = 0, g_underruns = 0;

// AUDIO_WAV.TXT beside the executable ("<start s> <length s>"): the game's
// output from <start> seconds of audio in, for <length> seconds, to
// audio.wav there (source material for the HOME Menu banner sound). Kept in
// memory and written once at the end so the card never stalls the audio.
int16_t* g_wav = nullptr;
size_t g_wav_frames = 0, g_wav_cap = 0, g_wav_skip = 0;
bool g_wav_written = false;

void wav_setup() {
    char path[192];
    snprintf(path, sizeof(path), "%s/AUDIO_WAV.TXT", recomp3ds::base_path());
    FILE* f = fopen(path, "r");
    if (f == nullptr) return;
    float start = 0.0f, length = 10.0f;
    if (fscanf(f, "%f %f", &start, &length) < 1) start = 0.0f;
    fclose(f);
    // Counted in frames at the rate the game first asks for (DK64: fixed).
    g_wav_skip = (size_t)(start * (float)g_rate);
    g_wav_cap = (size_t)(length * (float)g_rate);
    g_wav = (int16_t*)malloc(g_wav_cap * 2 * sizeof(int16_t));
    fprintf(stderr, "recomp3ds: recording %.1f s of audio from %.1f s to audio.wav\n", length, start);
}

void wav_record(const int16_t* src, size_t frames) {
    for (size_t i = 0; i < frames && g_wav_frames < g_wav_cap; i++) {
        if (g_wav_skip > 0) { g_wav_skip--; continue; }
        g_wav[g_wav_frames * 2 + 0] = src[i * 2 + 1];      // L/R as played
        g_wav[g_wav_frames * 2 + 1] = src[i * 2 + 0];
        g_wav_frames++;
    }
    if (g_wav_frames < g_wav_cap || g_wav_written) return;
    g_wav_written = true;
    char path[192];
    snprintf(path, sizeof(path), "%s/audio.wav", recomp3ds::base_path());
    FILE* f = fopen(path, "wb");
    if (f == nullptr) return;
    const uint32_t data = (uint32_t)(g_wav_frames * 4), rate = g_rate;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + data); fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    fwrite("data", 1, 4, f); u32(data);
    fwrite(g_wav, 4, g_wav_frames, f);
    fclose(f);
    fprintf(stderr, "recomp3ds: audio.wav written (%zu frames at %lu Hz)\n", g_wav_frames, (unsigned long)rate);
}

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
void recomp3ds::audio_set_enabled(bool on) { g_enabled = on; }
bool recomp3ds::audio_enabled() { return g_enabled; }

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
    static bool wav_checked = false;
    if (!wav_checked) { wav_checked = true; wav_setup(); }
    if (g_wav != nullptr) wav_record(samples, frames);
    size_t done = 0;
    bool starved = !ndspChnIsPlaying(kChannel);
    for (int i = 0; i < kNumBufs && !starved; i++) {
        if (g_bufs[i].status == NDSP_WBUF_QUEUED || g_bufs[i].status == NDSP_WBUF_PLAYING) break;
        if (i == kNumBufs - 1) starved = true;
    }
    if (starved && g_started) g_underruns = g_underruns + 1;
    g_started = true;
    g_submitted = g_submitted + (uint32_t)frames;
    while (done < frames) {
        ndspWaveBuf* wb = &g_bufs[g_next];
        if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE) {
            // The ring is full: the game is ahead of the DSP. Drop the rest.
            g_dropped = g_dropped + (uint32_t)(frames - done);
            return;
        }
        size_t n = frames - done;
        if (n > kMaxFramesPerBuf) {
            n = kMaxFramesPerBuf;
        }
        int16_t* dst = g_storage[g_next];
        const int16_t* src = samples + done * 2;
        // With audio off the task that fills `samples` was skipped: silence,
        // queued all the same so the game's pacing does not change.
        const float vol = g_enabled ? g_volume : 0.0f;
        for (size_t i = 0; i < n; i++) {
            // Swap the pair: RDRAM halfwords are stored xor 2, so L and R arrive reversed.
            dst[i * 2 + 0] = (int16_t)(src[i * 2 + 1] * vol);
            dst[i * 2 + 1] = (int16_t)(src[i * 2 + 0] * vol);
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
    // Only what is left of the buffer being played counts: counting all of it
    // made the game (which sizes each batch from this) produce ~5% less than
    // the DSP plays, so the music ran slow and the DSP ran dry between
    // buffers (pops).
    size_t queued = 0;
    for (int i = 0; i < kNumBufs; i++) {
        if (g_bufs[i].status == NDSP_WBUF_QUEUED) {
            queued += g_bufs[i].nsamples;
        }
        else if (g_bufs[i].status == NDSP_WBUF_PLAYING) {
            u32 pos = ndspChnGetSamplePos(kChannel);
            queued += pos < g_bufs[i].nsamples ? g_bufs[i].nsamples - pos : 0;
        }
    }
    // Like the desktop: report one VI's worth less so the game keeps a margin.
    size_t frames_per_vi = g_rate / 60;
    return queued > frames_per_vi ? queued - frames_per_vi : 0;
}

recomp3ds::AudioCounters recomp3ds::audio_take_counters() {
    AudioCounters c{ g_submitted, g_dropped, g_underruns, g_rate };
    g_submitted = 0; g_dropped = 0; g_underruns = 0;
    return c;
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
