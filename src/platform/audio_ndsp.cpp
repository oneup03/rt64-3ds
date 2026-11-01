// ultramodern audio callbacks on ndsp. The game produces 16-bit stereo at the
// rate it asks for; ndsp resamples in hardware, so the samples go out as they
// are, apart from the L/R swap the runtime's byte-swapped RDRAM imposes.
#include <atomic>
#include <3ds.h>
#include <opendsp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ultramodern/ultramodern.hpp"
#include "recomp3ds_internal.h"
#include "recomp3ds.h"
#include "dsp_synth.h"

namespace {

constexpr int kChannel = 0;
constexpr int kNumBufs = 24;            // ~ a quarter second at 60 chunks/s
constexpr size_t kMaxFramesPerBuf = 4096;
bool g_ready = false;
bool g_opendsp = false;     // OpenDSP's firmware runs (audio_init)
uint32_t g_rate = 32000;
int g_margin_frames = 1;   // video frames reported fewer than are queued (GameDesc::audio_margin_frames)
ndspWaveBuf g_bufs[kNumBufs];
int16_t* g_storage[kNumBufs];
int16_t* g_silence = nullptr;   // a buffer's worth of zeros that every silent buffer queues
int g_next = 0;
float g_volume = 1.0f;
std::atomic<bool> g_enabled{ true };     // the menu's Audio setting
bool g_started = false;
volatile bool g_paused = false;         // audio_set_paused: the HOME prompt is up
volatile uint32_t g_submitted = 0, g_dropped = 0, g_underruns = 0;
volatile uint32_t g_added = 0;          // frames handed to the channel since init (the AI clock)
volatile uint32_t g_low_water = 0xFFFFFFFFu;

// libctru numbers a channel's buffers 1 to 65535 and then from 1 again, and
// OpenDSP (1.2.2) plays the lowest number it holds next: at the wrap the
// last buffers numbered before it stayed in its five-slot queue for good,
// later buffers beyond the slots left were dropped while libctru counted
// them played, and the stream (and the game's music, which keeps pace with
// it) ran about a quarter fast from 18 minutes in. So the channel is
// renumbered before its numbers wrap (renumber()).
constexpr u16 kRenumberAt = 0xFF00;
u16 g_last_seq = 0;                     // the number libctru gave the last buffer queued
volatile uint32_t g_renumbering = 0;    // odd while renumber() runs
volatile uint32_t g_held_played = 0;    // audio_frames_played while it does

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

// Clears the channel (libctru numbers from 1 again, and the next buffer it
// sends carries the partial reset that empties OpenDSP's queue) and queues
// again what had yet to play, from where the DSP is, oldest first. The
// played count holds still meanwhile, so the DSP synthesizer's clock (which
// never steps back) does not jump ahead by the queue cleared.
void renumber() {
    int order[kNumBufs], count = 0, playing = -1;
    const u32 pos = ndspChnGetSamplePos(kChannel);
    for (int k = 0; k < kNumBufs; k++) {
        const int i = (g_next + k) % kNumBufs;
        if (g_bufs[i].status == NDSP_WBUF_PLAYING) playing = i;
        if (g_bufs[i].status == NDSP_WBUF_QUEUED || g_bufs[i].status == NDSP_WBUF_PLAYING) order[count++] = i;
    }
    g_held_played = g_added - (uint32_t)recomp3ds::audio_frames_queued();
    __sync_synchronize();
    g_renumbering = g_renumbering + 1;
    __sync_synchronize();
    ndspChnWaveBufClear(kChannel);
    ndspChnSetInterp(kChannel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(kChannel, (float)g_rate);
    ndspChnSetFormat(kChannel, NDSP_FORMAT_STEREO_PCM16);
    for (int k = 0; k < count; k++) {
        ndspWaveBuf* wb = &g_bufs[order[k]];
        if (order[k] == playing) {
            // The one playing: the rest of it.
            if (pos >= wb->nsamples) continue;
            wb->data_pcm16 += pos * 2;
            wb->nsamples -= pos;
        }
        ndspChnWaveBufAdd(kChannel, wb);
        g_last_seq = wb->sequence_id;
    }
    __sync_synchronize();
    g_renumbering = g_renumbering + 1;
    fprintf(stderr, "recomp3ds: audio stream renumbered (%d buffers queued again, %lu frames into the first)\n", count,
            (unsigned long)pos);
}

}   // namespace

bool recomp3ds::audio_init() {
    // OpenDSP's firmware (third_party/OpenDSP, embedded) under libctru's
    // ndsp: no dumped dspfirm.cdc needed, and its aux buses run the reverb
    // (dsp_reverb.cpp). Should it fail, the console's own firmware from
    // sdmc:/3ds/dspfirm.cdc, the component reset first (OpenDSP's init set it).
    if (R_SUCCEEDED(opendspInit())) {
        g_opendsp = true;
        u16 version = 0;
        u32 caps = 0;
        opendspGetFirmwareVersion(&version);
        opendspGetCapabilities(&caps);
        fprintf(stderr, "recomp3ds: OpenDSP firmware %u.%u (capabilities %08lx)\n", (unsigned)(version >> 8), (unsigned)(version & 0xFF),
                (unsigned long)caps);
    }
    else {
        ndspUseComponent(nullptr, 0, 0xFF, 0xFF);
        if (R_FAILED(ndspInit())) {
            fprintf(stderr, "recomp3ds: OpenDSP and ndspInit failed (no sdmc:/3ds/dspfirm.cdc either); audio off\n");
            return false;
        }
        fprintf(stderr, "recomp3ds: OpenDSP failed to start; the console's DSP firmware (dspfirm.cdc) runs instead\n");
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
    g_silence = (int16_t*)linearAlloc(kMaxFramesPerBuf * 2 * sizeof(int16_t));
    if (g_silence != nullptr) {
        memset(g_silence, 0, kMaxFramesPerBuf * 2 * sizeof(int16_t));
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)g_silence, kMaxFramesPerBuf * 2 * sizeof(int16_t));
    }
    g_ready = true;
    return true;
}

void recomp3ds::audio_set_volume(float v) { g_volume = v; }
void recomp3ds::audio_set_enabled(bool on) { g_enabled = on; }

// The HOME prompt: every DSP channel stops where it is (the AI stream and
// the DSP synthesizer's voices, so their clock stops with them), and the
// game's audio thread is held at its next buffer (audio_queue_samples)
// rather than running the music on unheard: on resuming, sound and game
// carry on together.
void recomp3ds::audio_set_paused(bool paused) {
    if (paused == g_paused) return;
    g_paused = paused;
    if (!g_ready) return;
    for (int ch = 0; ch < 24; ch++) ndspChnSetPaused(ch, paused);
}
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
    // Held while paused. Quitting from the prompt never resumes: the thread
    // stays here through the exit (let go, it ran the game's audio code on
    // memory the exit was tearing down: a data abort in n_alAdpcmPull).
    while (g_paused) svcSleepThread(10 * 1000000ll);
    if (!g_ready) return;
    size_t frames = sample_count / 2;
    static bool wav_checked = false;
    if (!wav_checked) { wav_checked = true; wav_setup(); }
    if (g_wav != nullptr) wav_record(samples, frames);
    size_t done = 0;
    if (g_started) {
        const size_t before = audio_frames_queued();
        if (before < g_low_water) g_low_water = (uint32_t)before;
    }
    bool starved = !ndspChnIsPlaying(kChannel);
    for (int i = 0; i < kNumBufs && !starved; i++) {
        if (g_bufs[i].status == NDSP_WBUF_QUEUED || g_bufs[i].status == NDSP_WBUF_PLAYING) break;
        if (i == kNumBufs - 1) starved = true;
    }
    if (starved && g_started) {
        g_underruns = g_underruns + 1;
        // The channel stopped for want of data: give it its rate, format
        // and interpolation again before it restarts, so a restart never
        // plays at the DSP's own rate (slow and flat after an underrun).
        ndspChnSetInterp(kChannel, NDSP_INTERP_LINEAR);
        ndspChnSetRate(kChannel, (float)g_rate);
        ndspChnSetFormat(kChannel, NDSP_FORMAT_STEREO_PCM16);
    }
    // After the check above: the cleared channel reads as stopped until
    // ndsp's next frame.
    if (g_opendsp && g_last_seq >= kRenumberAt) renumber();
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
        // With audio off the task that fills `samples` was skipped, and with
        // the voices on the DSP (dsp_synth.h) too: silence, queued all the
        // same so the game's pacing (and the DSP synthesizer's clock) does
        // not change, from the one buffer of zeros (no copy, no flush).
        if ((!g_enabled || recomp3ds::dsp_synth_enabled()) && g_silence != nullptr) {
            dst = g_silence;
        }
        else {
            const float vol = g_enabled ? g_volume : 0.0f;
            for (size_t i = 0; i < n; i++) {
                // Swap the pair: RDRAM halfwords are stored xor 2, so L and R arrive reversed.
                dst[i * 2 + 0] = (int16_t)(src[i * 2 + 1] * vol);
                dst[i * 2 + 1] = (int16_t)(src[i * 2 + 0] * vol);
            }
            // The kernel's own flush: DSP_FlushDataCache is a request to the
            // DSP module on core 1, which the game's audio thread then waited
            // on (dsp_synth.cpp found it costing whole frames).
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, n * 2 * sizeof(int16_t));
        }
        wb->data_pcm16 = dst;
        wb->nsamples = (u32)n;
        wb->looping = false;
        ndspChnWaveBufAdd(kChannel, wb);
        g_last_seq = wb->sequence_id;
        // After the add: a reader that sees the new count sees the buffer
        // queued, so the played count (added minus queued) never runs ahead.
        __sync_synchronize();
        g_added = g_added + (uint32_t)n;
        g_next = (g_next + 1) % kNumBufs;
        done += n;
    }
}

size_t recomp3ds::audio_frames_queued() {
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
    return queued;
}

uint32_t recomp3ds::audio_frames_added() { return g_added; }

uint32_t recomp3ds::audio_frames_played() {
    // During renumber() (or across it) the count before it.
    const uint32_t renumbering = g_renumbering;
    if (renumbering & 1) return g_held_played;
    __sync_synchronize();
    const uint32_t added = g_added;
    __sync_synchronize();
    const uint32_t played = added - (uint32_t)audio_frames_queued();
    __sync_synchronize();
    return g_renumbering == renumbering ? played : g_held_played;
}

uint32_t recomp3ds::audio_rate() { return g_rate; }

size_t recomp3ds::audio_frames_remaining() {
    // Like the desktop: report a margin's worth less (one VI unless the game
    // asks for more) so the game keeps that much in reserve. Buffers of
    // asynchronous audio tasks still to run count as queued before the margin
    // is taken (ultramodern::pending_audio_bytes): with the margin taken from
    // the DSP's queue alone, Goemon's audio manager saw a full frame too much
    // whenever the DSP held less than the margin, stayed at its smallest
    // frame (720 of the 735 a frame plays) and the DSP ran dry every frame.
    const size_t queued = audio_frames_queued() + ultramodern::pending_audio_bytes() / 4;
    const size_t margin = (size_t)(g_rate / 60) * (size_t)g_margin_frames;
    return queued > margin ? queued - margin : 0;
}

void recomp3ds::audio_set_margin_frames(int video_frames) {
    g_margin_frames = video_frames < 0 ? 0 : video_frames;
}

recomp3ds::AudioCounters recomp3ds::audio_take_counters() {
    AudioCounters c{ g_submitted, g_dropped, g_underruns, g_rate, g_low_water };
    g_low_water = 0xFFFFFFFFu;
    g_submitted = 0; g_dropped = 0; g_underruns = 0;
    return c;
}

bool recomp3ds::audio_opendsp() { return g_opendsp; }

void recomp3ds::audio_shutdown() {
    if (g_ready) {
        g_ready = false;     // the audio task thread may still queue samples
        ndspChnWaveBufClear(kChannel);
        if (g_opendsp) opendspExit();
        else ndspExit();
        for (int i = 0; i < kNumBufs; i++) {
            linearFree(g_storage[i]);
        }
    }
}
