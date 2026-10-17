// The game's synthesizer on the 3DS DSP (dsp_synth.h).
//
// Hooks (the game's audio thread) read the synthesizer's structures from
// RDRAM and queue events stamped with the sample of the AI stream they are
// heard at (the AI path still plays the game's frames, as silence, so its
// clock is the one the game's music keeps); a thread woken after every DSP
// frame applies the events the AI stream has reached to the ndsp
// channels and keeps each playing channel a few buffers ahead with samples
// decoded from the ROM (VADPCM as the audio microcode decodes it, through
// audio_hle_simd.h, or 16-bit PCM).
#include "dsp_synth.h"

#include <3ds.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "audio_hle_simd.h"
#include "dsp_reverb.h"
#include "recomp3ds.h"
#include "recomp3ds_internal.h"

namespace ahle = recomp3ds::ahle;

namespace {

// ndsp channel 0 carries the AI stream (audio_ndsp.cpp); the voices take the rest.
constexpr int kFirstChannel = 1;
constexpr int kSlots = 23;
int g_slots = kSlots;                   // kSlots less SynthDesc::reserve_channels
constexpr int kBufs = 3;
constexpr uint32_t kBufFrames = 1024;               // samples a buffer, at the sample's own rate
constexpr double kDspRate = 268111856.0 / 8192.0;   // ndsp's output rate (32728.5 Hz)
constexpr double kFrameSec = 160.0 / kDspRate;      // one DSP frame

// ------------------------------------------------------------------ RDRAM

inline uint32_t off(uint32_t a) { return (a - 0x80000000u) & 0x3FFFFFFu; }
inline uint32_t rw(const uint8_t* rd, uint32_t a) { uint32_t v; memcpy(&v, rd + off(a & ~3u), 4); return v; }
inline int16_t rh(const uint8_t* rd, uint32_t a) { int16_t v; memcpy(&v, rd + (off(a & ~1u) ^ 2), 2); return v; }
inline uint8_t rb(const uint8_t* rd, uint32_t a) { return rd[off(a) ^ 3]; }
// Within the runtime's RDRAM (16 MB, not the console's 8: Banjo keeps its
// voices above 8 MB).
inline bool is_ptr(uint32_t a) { return a >= 0x80000000u && a - 0x80000000u < (uint32_t)recomp::mem_size; }
inline void ww(uint8_t* rd, uint32_t a, uint32_t v) { memcpy(rd + off(a & ~3u), &v, 4); }
inline void wh(uint8_t* rd, uint32_t a, int16_t v) { memcpy(rd + (off(a & ~1u) ^ 2), &v, 2); }

// ------------------------------------------------------------------ waves

struct Wave {
    uint32_t base = 0;          // ROM offset of the data (or an RDRAM address)
    uint32_t len = 0;           // bytes
    uint8_t type = 0;           // 0 VADPCM, 1 16-bit PCM
    uint32_t npred = 0;
    ahle::AdpcmBook book[16];   // per predictor, as the decoder multiplies them
    bool loop = false;
    uint32_t loop_start = 0, loop_end = 0, loop_count = 0;   // samples; count ~0u: forever
    int16_t loop_state[16] = {};
};

// The samples a wave holds.
uint32_t wave_samples(const Wave& w) { return w.type == 0 ? w.len / 9 * 16 : w.len / 2; }

void rom_read(uint32_t addr, uint8_t* dst, size_t n, const uint8_t* rdram) {
    if (is_ptr(addr)) {                          // samples in RDRAM
        for (size_t i = 0; i < n; i++) dst[i] = rb(rdram, addr + (uint32_t)i);
        return;
    }
    if (const recomp::RomStream* s = recomp::get_rom_stream()) { s->read(addr, dst, n); return; }
    const std::span<const uint8_t> rom = recomp::get_rom();
    for (size_t i = 0; i < n; i++) dst[i] = addr + i < rom.size() ? rom[addr + i] : 0;
}

// libaudio's ALWaveTable (with its ALADPCMBook and ALADPCMloop / ALRawLoop).
bool read_wave(const uint8_t* rd, uint32_t w, Wave& out) {
    if (!is_ptr(w)) return false;
    out = Wave{};
    out.base = rw(rd, w);
    out.len = rw(rd, w + 4);
    out.type = rb(rd, w + 8);
    const uint32_t loop = rw(rd, w + 12);
    if (out.type == 0) {
        const uint32_t book = rw(rd, w + 16);
        if (!is_ptr(book) || rw(rd, book) != 2) return false;       // order 2, as the microcode decodes
        out.npred = rw(rd, book + 4);
        if (out.npred == 0 || out.npred > 16) return false;
        for (uint32_t p = 0; p < out.npred; p++) {
            int32_t c1[8], c2[8];
            for (int k = 0; k < 8; k++) {
                c1[k] = rh(rd, book + 8 + 2 * (p * 16 + k));
                c2[k] = rh(rd, book + 8 + 2 * (p * 16 + 8 + k));
            }
            ahle::adpcm_book(out.book[p], c1, c2);
        }
        if (is_ptr(loop)) {
            out.loop_start = rw(rd, loop);
            out.loop_end = rw(rd, loop + 4);
            out.loop_count = rw(rd, loop + 8);
            for (int k = 0; k < 16; k++) out.loop_state[k] = rh(rd, loop + 12 + 2 * k);
        }
    }
    else if (out.type == 1) {
        if (is_ptr(loop)) {
            out.loop_start = rw(rd, loop);
            out.loop_end = rw(rd, loop + 4);
            out.loop_count = rw(rd, loop + 8);
        }
    }
    else {
        return false;
    }
    out.loop = out.loop_count != 0 && out.loop_end > out.loop_start && out.loop_end <= wave_samples(out);
    return true;
}

// ------------------------------------------------------------------ events

enum EvKind : uint8_t { EvStart, EvStop, EvVol, EvPitch, EvPan, EvFx };

struct Event {
    EvKind kind;
    uint32_t key;               // the ALVoice's RDRAM address
    uint32_t due;               // the AI stream's sample it is heard at (audio_frames_played)
    float pitch = 1.0f;
    int32_t vol = 0;
    uint32_t ramp_us = 0;
    int32_t pan = 64, fx = 0;
    bool unity = false;
    bool from_silence = false;  // EvVol: ramp up from silence (alSynStartVoiceParams with a ramp time)
    uint16_t wave = 0;          // EvStart: its slot in g_waves
};

// Events stay small (the queue is scanned every DSP frame): a start's wave
// (with its codebook, about 1 KB) waits in a ring of slots. Starts come a few
// dozen a second and are applied within a fraction of one, so a slot is
// long free by the time the ring comes round to it again.
constexpr uint32_t kWaveSlots = 128;

// ------------------------------------------------------------------ voices

struct Voice {
    uint32_t key = 0;           // 0: free
    int ch = -1;
    bool playing = false;
    bool started = false;       // the DSP has begun playing it (starvation counts after that)
    Wave wave;
    // Decoder: the next sample to produce, the frame it is in, and the
    // decoded frame (VADPCM decodes 16 samples at a time).
    uint32_t pos = 0;
    uint32_t loops_left = 0;
    bool ended = false;
    int32_t frame = -1;
    int16_t pcm[16];
    int32_t hist_old = 0, hist_new = 0;
    uint8_t cache[9 * 64];      // ROM bytes of up to 64 frames
    int32_t cache_first = -1;
    // Parameters (kept across starts, as libaudio's voice keeps them).
    float pitch = 1.0f;
    bool unity = false;
    float vol = 0.0f, vol_target = 0.0f, ramp_left = 0.0f;   // volume 0..1 (squared, as libaudio's), ramp in seconds
    int32_t pan = 64, fx = 0;
    // The channel's buffers.
    ndspWaveBuf bufs[kBufs];
    int16_t* mem[kBufs] = {};
    int next_buf = 0;
    uint32_t last_used = 0;
};

LightLock g_lock;                   // the event queue, the wave slots and the clock mapping
std::vector<Event> g_events;
Wave g_waves[kWaveSlots];
uint32_t g_wave_next = 0;
Voice g_voices[kSlots];
bool g_ready = false;
volatile bool g_enabled = false;
volatile bool g_quit = false;
recomp3ds::SynthDesc g_desc;           // the game's synthesizer (dsp_synth.h)
Thread g_thread = nullptr;
LightEvent g_frame_event;
volatile uint32_t g_dsp_frames = 0;         // DSP frames since init (the ndsp callback counts them)
uint32_t g_use_clock = 0;
const uint8_t* g_rdram = nullptr;

// The clock mapping: synthesizer sample T is heard when the AI stream plays
// its sample T + g_ai_offset. Each audio frame's samples go to the AI path
// (silent) one for one, so the offset holds still (the music keeps the
// game's tempo exactly) and only moves when the AI path drops samples. The
// DSP frames counted by the ndsp callback made a poor clock: a late callback
// stands for several frames, so it fell behind the speaker, and following
// it moved the notes about (the music sped up and slowed down) and behind.
uint32_t g_synth = 0;           // ALSynth (the s every alSyn* call passes)
int32_t g_ai_offset = 0;
int32_t g_ai_measured = 0;      // the previous frame's measurement
bool g_mapped = false;
uint32_t g_played = 0;          // audio_frames_played, never stepping back (the thread's)
uint32_t g_dropped_base = 0;    // ndspGetDroppedFrames at the last stats line

// Statistics (the decoding thread's, read and cleared by the stats line).
volatile uint32_t g_st_starts = 0, g_st_events = 0, g_st_ticks = 0, g_st_starved = 0, g_st_late = 0;
volatile uint32_t g_st_decode = 0, g_st_apply = 0;      // ticks decoding samples, applying events
volatile uint32_t g_st_remaps = 0;
volatile uint32_t g_st_skipped = 0;     // frames built without their commands
uint32_t g_frames_base = 0;     // g_dsp_frames at the last stats line

// The AI stream's samples a DSP frame plays.
uint32_t frame_samples() { return (uint32_t)(recomp3ds::audio_rate() * kFrameSec) + 1; }

// audio_frames_played as the thread last read it, moving forward only (the
// count can dip by a buffer while the audio thread queues one).
uint32_t ai_played() {
    const uint32_t p = recomp3ds::audio_frames_played();
    if ((int32_t)(p - g_played) > 0) g_played = p;
    return g_played;
}

int16_t g_eqpower[128];         // libaudio's equal-power pan and dry/wet curve

void ndsp_frame_callback(void*) {
    g_dsp_frames = g_dsp_frames + 1;
    LightEvent_Signal(&g_frame_event);
}

// ------------------------------------------------------------------ decoding

// Frame f's 9 bytes, from the voice's ROM cache.
const uint8_t* frame_bytes(Voice& v, int32_t f) {
    if (v.cache_first < 0 || f < v.cache_first || f >= v.cache_first + 64) {
        v.cache_first = f;
        rom_read(v.wave.base + 9u * (uint32_t)f, v.cache, sizeof(v.cache), g_rdram);
    }
    return v.cache + 9 * (f - v.cache_first);
}

// Decodes frame f into v.pcm, from the history the voice holds.
void decode_frame(Voice& v, int32_t f) {
    const uint8_t* b = frame_bytes(v, f);
    const uint32_t scale = b[0] >> 4, pred = (b[0] & 0xF) < v.wave.npred ? (b[0] & 0xF) : 0;
    const int32_t mult = scale < 12 ? (0x8000 >> (11 - scale)) : 0;
    for (int h = 0; h < 2; h++) {
        int32_t s[8], o[8];
        for (int i = 0; i < 4; i++) {
            const uint8_t d = b[1 + h * 4 + i];
            s[2 * i] = (int16_t)((d & 0xF0) << 8);
            s[2 * i + 1] = (int16_t)((d & 0x0F) << 12);
        }
        if (mult) for (int i = 0; i < 8; i++) s[i] = (s[i] * mult) >> 16;
        ahle::adpcm_half(v.wave.book[pred], s, v.hist_old, v.hist_new, o);
        for (int i = 0; i < 8; i++) v.pcm[8 * h + i] = (int16_t)o[i];
        v.hist_old = o[6];
        v.hist_new = o[7];
    }
    v.frame = f;
}

void restart_decoder(Voice& v) {
    v.pos = 0;
    v.frame = -1;
    v.hist_old = v.hist_new = 0;
    v.cache_first = -1;
    v.ended = false;
    v.loops_left = v.wave.loop_count;
}

// Up to n samples into dst; fewer only at the wave's end.
uint32_t decode(Voice& v, int16_t* dst, uint32_t n) {
    const Wave& w = v.wave;
    const uint32_t total = wave_samples(w);
    uint32_t done = 0;
    while (done < n && !v.ended) {
        const bool looping = w.loop && v.loops_left != 0;
        const uint32_t stop = looping ? w.loop_end : total;
        if (v.pos >= stop) {
            if (!looping) { v.ended = true; break; }
            // Back to the loop start: its frame decodes from the loop's saved
            // state, the samples before the start skipped (as the microcode's
            // A_LOOP decode is used).
            if (v.loops_left != ~0u) v.loops_left--;
            v.pos = w.loop_start;
            if (w.type == 0) {
                // libaudio (alAdpcmPull) resumes the data at the frame after
                // the one holding the loop start, with A_LOOP: the loop's
                // saved state is that frame's 16 decoded samples, which play
                // from the loop start on, and its last two the history.
                memcpy(v.pcm, w.loop_state, sizeof(v.pcm));
                v.frame = (int32_t)(w.loop_start / 16);
                v.hist_old = w.loop_state[14];
                v.hist_new = w.loop_state[15];
            }
            continue;
        }
        if (w.type == 0) {
            const int32_t f = (int32_t)(v.pos / 16);
            if (f != v.frame) {
                // Frames decode in order; a jump (a loop) has decoded its own.
                decode_frame(v, f);
            }
            const uint32_t in_frame = v.pos & 15;
            uint32_t take = 16 - in_frame;
            if (take > stop - v.pos) take = stop - v.pos;
            if (take > n - done) take = n - done;
            memcpy(dst + done, v.pcm + in_frame, take * 2);
            done += take;
            v.pos += take;
        }
        else {
            uint32_t take = stop - v.pos;
            if (take > n - done) take = n - done;
            if (take > 256) take = 256;
            uint8_t raw[512];
            rom_read(w.base + 2 * v.pos, raw, take * 2, g_rdram);
            for (uint32_t i = 0; i < take; i++) dst[done + i] = (int16_t)((raw[2 * i] << 8) | raw[2 * i + 1]);
            done += take;
            v.pos += take;
        }
    }
    return done;
}

// ------------------------------------------------------------------ channels

void apply_mix(Voice& v) {
    if (v.ch < 0) return;
    const int pan = v.pan < 0 ? 0 : (v.pan > 127 ? 127 : v.pan);
    const int fx = v.fx & 0x7F;
    // Without the reverb a voice plays wholly dry: its fxmix would take the
    // send away and give nothing back (DK64 underwater turns every voice to
    // 127, fully wet, which was silence).
    const float dry = recomp3ds::dsp_reverb_active() ? g_eqpower[fx] / 32767.0f : 1.0f;
    const float l = v.vol * (g_eqpower[pan] / 32767.0f) * dry;
    const float r = v.vol * (g_eqpower[127 - pan] / 32767.0f) * dry;
    float mix[12] = {};
    mix[0] = l;
    mix[1] = r;
    // The wet side to the reverb (aux bus 0's front pair, dsp_reverb.h).
    if (recomp3ds::dsp_reverb_active()) {
        const float wet = g_eqpower[127 - fx] / 32767.0f;
        mix[4] = v.vol * (g_eqpower[pan] / 32767.0f) * wet;
        mix[5] = v.vol * (g_eqpower[127 - pan] / 32767.0f) * wet;
    }
    ndspChnSetMix(v.ch, mix);
}

void apply_rate(Voice& v) {
    if (v.ch < 0) return;
    // The synthesizer's samples play at the AI's rate (as its output would).
    const float ratio = v.unity ? 1.0f : v.pitch;
    ndspChnSetRate(v.ch, (float)recomp3ds::audio_rate() * (ratio > 0.0f ? ratio : 0.0f));
}

void stop_channel(Voice& v) {
    if (v.ch >= 0) ndspChnWaveBufClear(v.ch);
    v.playing = false;
    for (int i = 0; i < kBufs; i++) v.bufs[i].status = NDSP_WBUF_DONE;
}

// Fills every finished buffer the voice has with new samples.
void feed(Voice& v) {
    if (!v.playing) return;
    for (int i = 0; i < kBufs && !v.ended; i++) {
        ndspWaveBuf& b = v.bufs[v.next_buf];
        if (b.status != NDSP_WBUF_DONE && b.status != NDSP_WBUF_FREE) break;
        int16_t* m = v.mem[v.next_buf];
        const u64 td = svcGetSystemTick();
        const uint32_t n = decode(v, m, kBufFrames);
        g_st_decode = g_st_decode + (uint32_t)(svcGetSystemTick() - td);
        if (n == 0) break;
        // The kernel's own flush: DSP_FlushDataCache is a request to the
        // DSP module, which runs on core 1 next to this thread and made every
        // buffer wait for it to be scheduled.
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)m, n * 2);
        b = ndspWaveBuf{};
        b.data_pcm16 = m;
        b.nsamples = n;
        ndspChnWaveBufAdd(v.ch, &b);
        v.next_buf = (v.next_buf + 1) % kBufs;
    }
    const bool now_playing = ndspChnIsPlaying(v.ch);
    if (now_playing) v.started = true;
    else if (v.started && !v.ended) { g_st_starved = g_st_starved + 1; v.started = false; }
}

Voice* find_voice(uint32_t key, bool create) {
    for (Voice& v : g_voices) if (v.key == key) return &v;
    if (!create) return nullptr;
    Voice* pick = nullptr;
    for (Voice& v : g_voices) if (v.ch >= 0 && v.key == 0) { pick = &v; break; }
    // More voices than channels: the least recently used, a silent one first.
    for (int pass = 0; pass < 2 && pick == nullptr; pass++) {
        for (Voice& v : g_voices) {
            if (v.ch < 0) continue;             // a channel reserved for the game
            if (pass == 0 && v.playing) continue;
            if (pick == nullptr || v.last_used < pick->last_used) pick = &v;
        }
    }
    if (pick->key != 0) stop_channel(*pick);
    const int ch = pick->ch;
    int16_t* mem[kBufs];
    for (int i = 0; i < kBufs; i++) mem[i] = pick->mem[i];
    *pick = Voice{};
    pick->ch = ch;
    for (int i = 0; i < kBufs; i++) { pick->mem[i] = mem[i]; pick->bufs[i].status = NDSP_WBUF_DONE; }
    pick->key = key;
    return pick;
}

void apply_event(const Event& e) {
    // Games set a voice's parameters before starting it as well as after:
    // any event but a stop gives the voice a slot to keep them in.
    Voice* v = find_voice(e.key, e.kind != EvStop);
    if (v == nullptr) return;
    v->last_used = ++g_use_clock;
    switch (e.kind) {
        case EvStart:
            stop_channel(*v);
            v->wave = g_waves[e.wave];
            v->unity = e.unity;
            restart_decoder(*v);
            ndspChnReset(v->ch);
            ndspChnSetFormat(v->ch, NDSP_FORMAT_MONO_PCM16);
            ndspChnSetInterp(v->ch, NDSP_INTERP_POLYPHASE);
            v->playing = true;
            v->started = false;
            v->next_buf = 0;
            // alSynStartVoice leaves the envelope as it is (Goemon sets the
            // volume before the start as well as after it).
            apply_rate(*v);
            apply_mix(*v);
            feed(*v);
            g_st_starts = g_st_starts + 1;
            break;
        case EvStop:
            stop_channel(*v);
            break;
        case EvVol: {
            // libaudio's envelope mixer squares the volume it is given
            // (alEnvmixerPull: volume = vol * vol >> 15) before the pan and
            // the dry amount scale it.
            // n_audio takes it as it is ((vol + vol) / 2).
            const int32_t vol = e.vol < 0 ? 0 : e.vol;
            v->vol_target = (float)(g_desc.flavor == recomp3ds::SynthFlavor::Libaudio ? (vol * vol) >> 15 : vol) / 32767.0f;
            v->ramp_left = e.ramp_us / 1e6f;
            if (e.from_silence) v->vol = 0.0f;
            if (v->ramp_left <= 0.0f) v->vol = v->vol_target;
            apply_mix(*v);
            break;
        }
        case EvPitch:
            v->pitch = e.pitch;
            apply_rate(*v);
            break;
        case EvPan:
            v->pan = e.pan;
            apply_mix(*v);
            break;
        case EvFx:
            v->fx = e.fx;
            apply_mix(*v);
            break;
    }
}

void synth_thread(void*) {
    std::vector<Event> due;
    bool sends = false;         // the channels' mixes include the reverb's sends
    while (!g_quit) {
        LightEvent_Wait(&g_frame_event);
        if (g_quit) break;
        const u64 t0 = svcGetSystemTick();
        // Audio Off: the hooks stop, so nothing would ever stop what plays.
        if (!recomp3ds::audio_enabled()) {
            for (Voice& v : g_voices) if (v.playing) stop_channel(v);
            LightLock_Lock(&g_lock);
            g_events.clear();
            LightLock_Unlock(&g_lock);
        }
        recomp3ds::dsp_reverb_process();
        if (recomp3ds::dsp_reverb_active() != sends) {
            sends = !sends;
            for (Voice& v : g_voices) if (v.key != 0) apply_mix(v);
        }
        const uint32_t before = g_played;
        const uint32_t now = ai_played(), frame = frame_samples();
        // The AI stream's time since the last wake (the volume ramps keep it).
        const float elapsed = (float)(now - before) / (float)recomp3ds::audio_rate();
        due.clear();
        LightLock_Lock(&g_lock);
        // Events due before the next frame ends, in their order.
        size_t keep = 0;
        for (size_t i = 0; i < g_events.size(); i++) {
            if ((int32_t)(g_events[i].due - now) <= (int32_t)frame) due.push_back(g_events[i]);
            else g_events[keep++] = g_events[i];
        }
        g_events.resize(keep);
        LightLock_Unlock(&g_lock);
        const u64 ta = svcGetSystemTick();
        for (const Event& e : due) {
            const int32_t behind = (int32_t)(now - e.due);
            if (behind > (int32_t)frame) g_st_late = g_st_late + (uint32_t)((uint64_t)behind * 1000u / recomp3ds::audio_rate());
            apply_event(e);
        }
        g_st_apply = g_st_apply + (uint32_t)(svcGetSystemTick() - ta);
        for (Voice& v : g_voices) {
            if (!v.playing) continue;
            if (v.ramp_left > 0.0f && elapsed > 0.0f) {
                if (v.ramp_left <= elapsed) { v.vol = v.vol_target; v.ramp_left = 0.0f; }
                else if (g_desc.flavor == recomp3ds::SynthFlavor::Libaudio) {
                    // libaudio: a constant ratio per second from the current
                    // volume to the target (from or to silence: one LSB).
                    constexpr float kFloor = 1.0f / 32767.0f;
                    const float from = v.vol > kFloor ? v.vol : kFloor, to = v.vol_target > kFloor ? v.vol_target : kFloor;
                    v.vol = from * powf(to / from, elapsed / v.ramp_left);
                    v.ramp_left -= elapsed;
                }
                else {
                    // n_audio: a constant step (its microcode adds the rate
                    // every 8 samples).
                    v.vol += (v.vol_target - v.vol) * (elapsed / v.ramp_left);
                    v.ramp_left -= elapsed;
                }
                apply_mix(v);
            }
            feed(v);
            if (v.ended && !ndspChnIsPlaying(v.ch)) v.playing = false;
        }
        g_st_ticks = g_st_ticks + (uint32_t)(svcGetSystemTick() - t0);
    }
}

// ------------------------------------------------------------------ reverb

// libaudio's effect as alSynNew set it up: ALSynth::auxBus (+0x34) holds
// one ALFx at +0x20: base +0x14, input +0x18, length +0x1C, delay +0x20 (an
// ALDelay array), section_count +0x24 (u8). ALDelay (40 bytes): input +0,
// output +4, ffcoef +8, fbcoef +0xA, gain +0xC, lp +0x20, rs +0x24; the
// ALLowPass's fgain at +2 (alFxPull, alSynAllocFX in Goemon's build).
// n_audio's (Banjo) is the same ALFx behind a pointer at auxBus + 0x20;
// DK64's is its own: length +0, delay +4, section_count +8 (its sections
// are libaudio's).
bool read_reverb(const uint8_t* rd, uint32_t synth, recomp3ds::ReverbConfig& c) {
    const uint32_t aux = rw(rd, synth + g_desc.aux_bus);
    if (!is_ptr(aux)) return false;
    const uint32_t fx = g_desc.fx == recomp3ds::SynthFx::Embedded ? aux + 0x20 : rw(rd, aux + 0x20);
    if (!is_ptr(fx)) return false;
    const bool dk64 = g_desc.fx == recomp3ds::SynthFx::Dk64;
    const uint32_t delay = rw(rd, fx + (dk64 ? 4 : 0x20)), length = rw(rd, fx + (dk64 ? 0 : 0x1C));
    const uint32_t count = rb(rd, fx + (dk64 ? 8 : 0x24));
    if (!is_ptr(delay) || count == 0 || count > 8 || length == 0 || length > 0x40000) return false;
    c.length = length;
    c.rate = recomp3ds::audio_rate();
    c.count = (uint8_t)count;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t d = delay + 40 * i;
        recomp3ds::ReverbConfig::Section& s = c.sec[i];
        s.input = rw(rd, d);
        s.output = rw(rd, d + 4);
        if (s.input >= length || s.output >= length) return false;
        s.ffcoef = rh(rd, d + 8);
        s.fbcoef = rh(rd, d + 0xA);
        s.gain = rh(rd, d + 0xC);
        const uint32_t lp = rw(rd, d + 0x20);
        s.lp_gain = is_ptr(lp) && g_desc.fx_lowpass ? rh(rd, lp + 2) : 0;
        s.chorus = is_ptr(rw(rd, d + 0x24));
    }
    return true;
}

// ------------------------------------------------------------------ the frame without its commands

// libaudio's ALLink list operations (alUnlink, alLink).
void al_unlink(uint8_t* rd, uint32_t ln) {
    const uint32_t next = rw(rd, ln), prev = rw(rd, ln + 4);
    if (next != 0) ww(rd, next + 4, prev);
    if (prev != 0) ww(rd, prev, next);
}
void al_link(uint8_t* rd, uint32_t ln, uint32_t to) {
    const uint32_t next = rw(rd, to);
    ww(rd, ln + 4, to);
    ww(rd, ln, next);
    if (next != 0) ww(rd, next + 4, ln);
    ww(rd, to, ln);
}

// What the frame function's command building does besides the commands,
// for a frame of `len` samples from the synthesizer's curSamples: each of
// the aux bus's sources (libaudio: a physical voice's envelope mixer;
// n_audio: the N_PVoice itself) takes the updates due in the frame (delta
// up to curSamples + len), keeping the state its pull would have left (the
// start and stop flags, the voices freed), and frees them; then the clock
// moves on. ALParam: next +0, delta +4, type +8, pvoice (a free's) +0xC.
void retire_updates(uint8_t* rd, uint32_t synth, int32_t len) {
    enum { kFreeVoice = 0, kStart = 9, kStartVoiceAlt = 13, kStartVoice = 14, kStopVoice = 15 };
    // The source's ctrlList, ctrlTail, first, volume and motion.
    const bool naudio = g_desc.flavor == recomp3ds::SynthFlavor::NAudio;
    const uint32_t o_list = naudio ? 0x7C : 0x3C, o_tail = naudio ? 0x80 : 0x40, o_first = naudio ? 0x78 : 0x38;
    const uint32_t o_volume = naudio ? 0x5A : 0x1A, o_motion = naudio ? 0x84 : 0x48;
    const int32_t end = (int32_t)rw(rd, synth + g_desc.cur_samples) + len;
    const uint32_t aux = rw(rd, synth + g_desc.aux_bus);
    if (is_ptr(aux)) {
        const uint32_t count = rw(rd, aux + 0x14), sources = rw(rd, aux + 0x1C);
        for (uint32_t i = 0; is_ptr(sources) && i < count && i < 64; i++) {
            const uint32_t src = rw(rd, sources + 4 * i);
            if (!is_ptr(src)) continue;
            for (int guard = 0; guard < 4096; guard++) {
                const uint32_t p = rw(rd, src + o_list);
                if (!is_ptr(p) || (int32_t)rw(rd, p + 4) - end > 0) break;
                switch (rh(rd, p + 8)) {
                    case kStart: case kStartVoiceAlt: case kStartVoice:
                        ww(rd, src + o_motion, 1);              // AL_PLAYING
                        break;
                    case kStopVoice:                            // AL_FILTER_RESET
                        ww(rd, src + o_first, 1);
                        ww(rd, src + o_motion, 0);
                        wh(rd, src + o_volume, 1);
                        break;
                    case kFreeVoice: {                          // onto pLameList
                        const uint32_t pv = rw(rd, p + 0xC);
                        if (is_ptr(pv)) {
                            ww(rd, pv + g_desc.pvoice_offset, 0);
                            al_unlink(rd, pv);
                            al_link(rd, pv, synth + g_desc.lame_list);
                        }
                        break;
                    }
                    default:
                        break;
                }
                const uint32_t next = rw(rd, p);
                ww(rd, src + o_list, next);
                if (next == 0) ww(rd, src + o_tail, 0);
                ww(rd, p, rw(rd, synth + g_desc.param_list));     // __freeParam
                ww(rd, synth + g_desc.param_list, p);
            }
        }
    }
    ww(rd, synth + g_desc.cur_samples, (uint32_t)end);
}

// ------------------------------------------------------------------ hooks

// The AI stream's sample at which synthesizer sample `samples` is heard
// (the game builds audio ahead of the speaker by what the AI path holds),
// kept to the next half second.
uint32_t heard_at(int32_t samples) {
    const uint32_t now = recomp3ds::audio_frames_played();
    LightLock_Lock(&g_lock);
    const bool mapped = g_mapped;
    const uint32_t t = (uint32_t)samples + (uint32_t)g_ai_offset;
    LightLock_Unlock(&g_lock);
    if (!mapped) return now;
    const int32_t ahead = (int32_t)(t - now), most = (int32_t)(recomp3ds::audio_rate() / 2);
    return ahead < 0 ? now : (ahead > most ? now + (uint32_t)most : t);
}

void queue(Event&& e) {
    LightLock_Lock(&g_lock);
    g_events.push_back(std::move(e));
    LightLock_Unlock(&g_lock);
    g_st_events = g_st_events + 1;
}

}   // namespace

// The voice that last started on each physical voice (the hooks'): a start
// from another one is a steal.
struct Owner { uint32_t pv, voice; };
Owner g_owners[64];
int g_owner_count = 0;

bool took_over(uint32_t pv, uint32_t voice) {
    for (int i = 0; i < g_owner_count; i++) {
        if (g_owners[i].pv != pv) continue;
        const bool other = g_owners[i].voice != voice;
        g_owners[i].voice = voice;
        return other;
    }
    if (g_owner_count < 64) g_owners[g_owner_count++] = Owner{ pv, voice };
    return false;
}

// DK64's sound modes move pan inside its envelope mixer, after the call:
// mono centres every voice, headphones halve the spread.
int32_t game_pan(const uint8_t* rd, int32_t pan) {
    if (g_desc.sound_modes == 0) return pan;
    if (rb(rd, g_desc.sound_modes + 1) != 0) return 0x40;
    if (rb(rd, g_desc.sound_modes + 2) != 0) return (pan >> 1) + 0x20;
    return pan;
}

extern "C" void recomp3ds_synth_hook(uint8_t* rdram, void* ctx_, int which) {
    if (!g_ready || !g_enabled || g_desc.flavor == recomp3ds::SynthFlavor::None || !recomp3ds::audio_enabled()) return;
    recomp_context* ctx = static_cast<recomp_context*>(ctx_);
    g_rdram = rdram;
    // libaudio passes the synthesizer first and the voice second; n_audio
    // passes the voice first and keeps the synthesizer in a global (n_syn).
    const bool naudio = g_desc.flavor == recomp3ds::SynthFlavor::NAudio;
    const uint32_t reg[4] = { (uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7 };
    const uint32_t sp = (uint32_t)ctx->r29;
    if (naudio) g_synth = rw(rdram, g_desc.synth_global);
    else if (which != RECOMP3DS_SYNTH_AUDIO_FRAME && is_ptr(reg[0])) g_synth = reg[0];
    if (!is_ptr(g_synth)) return;
    if (which == RECOMP3DS_SYNTH_AUDIO_FRAME) {
        // The frame about to be built starts at the synthesizer's curSamples
        // and goes to the AI path after everything it has been given (the
        // buffers of audio tasks still to finish too).
        const uint32_t at = recomp3ds::audio_frames_added() + (uint32_t)(ultramodern::pending_audio_bytes() / 4);
        const int32_t measured = (int32_t)(at - (uint32_t)rw(rdram, g_synth + g_desc.cur_samples));
        LightLock_Lock(&g_lock);
        // A new offset that two frames agree on (one alone can be a buffer
        // caught between the task's queue and the DSP's).
        if (!g_mapped) g_ai_offset = measured;
        else if (measured != g_ai_offset && measured == g_ai_measured) { g_ai_offset = measured; g_st_remaps = g_st_remaps + 1; }
        g_ai_measured = measured;
        g_mapped = true;
        LightLock_Unlock(&g_lock);
        recomp3ds::ReverbConfig fx;
        if (!read_reverb(rdram, g_synth, fx)) fx = recomp3ds::ReverbConfig{};
        recomp3ds::dsp_reverb_configure(fx);
        return;
    }
    const int vreg = naudio ? 0 : 1;
    const uint32_t voice = reg[vreg];
    if (!is_ptr(voice)) return;
    // The physical voice is what sounds (and what a stolen voice's sound
    // stays on), so it keys the channel. A voice without one (never had
    // one, or another voice took it): the calls do nothing for it.
    const uint32_t pv = rw(rdram, voice + 8);
    if (!is_ptr(pv)) return;
    // The arguments after the voice: registers, then the stack (o32: the
    // fifth word at sp + 16).
    auto arg = [&](int i) -> uint32_t {
        const int n = vreg + 1 + i;
        return n < 4 ? reg[n] : rw(rdram, sp + 4 * n);
    };
    // A call made while the synthesizer fills in a frame's parameters is
    // stamped with that point's sample time (paramSamples), and with the
    // physical voice's delay while another voice's sound leaves it.
    const int32_t param = (int32_t)rw(rdram, g_synth + g_desc.param_samples);
    const int32_t delay = (int32_t)rw(rdram, pv + g_desc.pvoice_offset);
    const uint32_t due = heard_at(param + (delay > 0 ? delay : 0));
    Event e{};
    e.key = pv;
    e.due = due;
    switch (which) {
        case RECOMP3DS_SYNTH_START_VOICE:
        case RECOMP3DS_SYNTH_START_VOICE_PARAMS: {
            Wave wave;
            if (!read_wave(rdram, arg(0), wave)) return;
            if (took_over(pv, voice) && delay > 64) {
                // A stolen voice: the allocation faded what it played over
                // the delay less 64 samples (no call of ours saw that), and
                // the new voice's calls wait for the delay.
                Event f{}; f.key = pv; f.due = heard_at(param);
                f.kind = EvVol; f.vol = 0;
                f.ramp_us = (uint32_t)((uint64_t)(delay - 64) * 1000000u / recomp3ds::audio_rate());
                queue(Event(f));
            }
            LightLock_Lock(&g_lock);
            e.wave = (uint16_t)(g_wave_next++ % kWaveSlots);
            g_waves[e.wave] = wave;
            LightLock_Unlock(&g_lock);
            e.kind = EvStart;
            e.unity = rh(rdram, voice + 26) != 0;       // ALVoice::unityPitch
            queue(Event(e));
            if (which == RECOMP3DS_SYNTH_START_VOICE_PARAMS) {
                // pitch, vol, pan, fxmix and t: the synthesizer sets them with
                // the start (AL_FILTER_START_VOICE_ALT); t is the volume's
                // ramp time, from silence (0: at once).
                float pitch;
                const uint32_t pitch_bits = arg(1);
                memcpy(&pitch, &pitch_bits, 4);
                const int32_t vol = (int16_t)arg(2), pan = game_pan(rdram, (int32_t)(arg(3) & 0xFF)), fx = (int32_t)(arg(4) & 0xFF);
                const int32_t t = (int32_t)rw(rdram, sp + g_desc.start_t_stack);
                Event p{}; p.key = pv; p.due = due;
                p.kind = EvPitch; p.pitch = pitch; queue(Event(p));
                p.kind = EvPan; p.pan = pan; queue(Event(p));
                p.kind = EvFx; p.fx = fx; queue(Event(p));
                p.kind = EvVol; p.vol = (int16_t)vol; p.ramp_us = t > 0 ? (uint32_t)t : 0; p.from_silence = t > 0; queue(Event(p));
            }
            break;
        }
        case RECOMP3DS_SYNTH_STOP_VOICE:
            e.kind = EvStop;
            queue(std::move(e));
            break;
        case RECOMP3DS_SYNTH_SET_VOL:
            e.kind = EvVol;
            e.vol = (int16_t)arg(0);
            e.ramp_us = (int32_t)arg(1) > 0 ? arg(1) : 0;
            queue(std::move(e));
            break;
        case RECOMP3DS_SYNTH_SET_PITCH: {
            e.kind = EvPitch;
            const uint32_t bits = arg(0);
            memcpy(&e.pitch, &bits, 4);
            queue(std::move(e));
            break;
        }
        case RECOMP3DS_SYNTH_SET_PAN:
            e.kind = EvPan;
            e.pan = game_pan(rdram, (int32_t)(arg(0) & 0xFF));
            queue(std::move(e));
            break;
        case RECOMP3DS_SYNTH_SET_FXMIX:
            e.kind = EvFx;
            e.fx = (int32_t)(arg(0) & 0xFF);
            queue(std::move(e));
            break;
        default:
            break;
    }
}

extern "C" int recomp3ds_synth_skip_build(uint8_t* rdram, uint32_t synth, int32_t len, uint32_t list_end) {
    // With the voices on the DSP (or the sound off) the task the list would
    // go to is skipped: build none.
    if (!g_ready || g_desc.flavor == recomp3ds::SynthFlavor::None) return 0;
    if (!g_enabled && recomp3ds::audio_enabled()) return 0;
    if (synth == 0) synth = rw(rdram, g_desc.synth_global);
    if (!is_ptr(synth) || !is_ptr(list_end) || len <= 0) return 0;
    retire_updates(rdram, synth, len);
    // One command (the A_SEGMENT every list starts with) rather than none:
    // an audio manager takes an empty list for no task at all (Goemon's
    // returns before the task, and then never hands the frame's buffer to
    // the AI, whose clock the DSP's events keep).
    ww(rdram, list_end, 0x07000000u);
    ww(rdram, list_end + 4, 0);
    g_st_skipped = g_st_skipped + 1;
    return 1;
}

bool recomp3ds::dsp_synth_init(const SynthDesc& desc) {
    if (desc.flavor == SynthFlavor::None || g_ready) return g_ready;
    g_desc = desc;
    LightLock_Init(&g_lock);
    LightEvent_Init(&g_frame_event, RESET_ONESHOT);
    for (int i = 0; i < 128; i++) g_eqpower[i] = (int16_t)lrint(32767.0 * cos(i * M_PI / 2.0 / 127.0));
    g_slots = kSlots - (int)desc.reserve_channels;
    if (g_slots < 1) g_slots = 1;
    for (int s = 0; s < g_slots; s++) {
        Voice& v = g_voices[s];
        v.ch = kFirstChannel + s;
        for (int i = 0; i < kBufs; i++) {
            v.mem[i] = (int16_t*)linearAlloc(kBufFrames * sizeof(int16_t));
            if (v.mem[i] == nullptr) { fprintf(stderr, "dsp-synth: out of linear memory\n"); return false; }
            v.bufs[i].status = NDSP_WBUF_DONE;
        }
    }
    g_events.reserve(256);
    ndspSetCallback(ndsp_frame_callback, nullptr);
    recomp3ds::dsp_reverb_init();
    // A core the game does not run on (recomp3ds::helper_core): 1 when the
    // system lends it (the audio task, idle while the DSP plays the voices,
    // has it), else 0, with the renderer when the Cores row puts the game on 1.
    int core = recomp3ds::helper_core();
    g_thread = threadCreate(synth_thread, nullptr, 32 * 1024, 0x20, core, false);
    if (g_thread == nullptr && core != 0) { core = 0; g_thread = threadCreate(synth_thread, nullptr, 32 * 1024, 0x20, core, false); }
    if (g_thread == nullptr) { fprintf(stderr, "dsp-synth: no thread\n"); return false; }
    g_ready = true;
    fprintf(stderr, "dsp-synth: ready (%d channels, decoding on core %d)\n", g_slots, core);
    return true;
}

void recomp3ds::dsp_synth_set_enabled(bool on) {
    if (on == g_enabled) return;
    g_enabled = on;
    if (!g_ready) return;
    if (!on) {
        LightLock_Lock(&g_lock);
        g_events.clear();
        LightLock_Unlock(&g_lock);
        // The thread stops the channels at its next frame.
        Event e{}; e.kind = EvStop;
        e.due = recomp3ds::audio_frames_played();
        for (Voice& v : g_voices) if (v.key != 0) { e.key = v.key; queue(Event(e)); }
        recomp3ds::dsp_reverb_configure(recomp3ds::ReverbConfig{});
    }
    fprintf(stderr, "dsp-synth: %s\n", on ? "on (the game's voices play on the DSP)" : "off (the audio task runs)");
}

bool recomp3ds::dsp_synth_enabled() { return g_ready && g_enabled; }

void recomp3ds::dsp_synth_shutdown() {
    if (!g_ready) return;
    g_quit = true;
    LightEvent_Signal(&g_frame_event);
    threadJoin(g_thread, 200 * 1000 * 1000);
    ndspSetCallback(nullptr, nullptr);
    recomp3ds::dsp_reverb_shutdown();
    for (Voice& v : g_voices) if (v.ch >= 0) ndspChnWaveBufClear(v.ch);
    g_ready = false;
}

recomp3ds::DspSynthStats recomp3ds::dsp_synth_take_stats() {
    DspSynthStats s{};
    for (const Voice& v : g_voices) if (v.playing) s.playing++;
    s.starts = g_st_starts; s.events = g_st_events; s.starved = g_st_starved; s.late = g_st_late;
    s.decode_us = (uint32_t)((uint64_t)g_st_ticks * 1000000ull / SYSCLOCK_ARM11);
    s.decode_only_us = (uint32_t)((uint64_t)g_st_decode * 1000000ull / SYSCLOCK_ARM11);
    s.apply_us = (uint32_t)((uint64_t)g_st_apply * 1000000ull / SYSCLOCK_ARM11);
    s.remaps = g_st_remaps;
    s.lists_skipped = g_st_skipped;
    g_st_skipped = 0;
    uint32_t reverb_ticks = 0;
    recomp3ds::dsp_reverb_take_stats(&reverb_ticks, &s.reverb_gaps);
    s.reverb_us = (uint32_t)((uint64_t)reverb_ticks * 1000000ull / SYSCLOCK_ARM11);
    const uint32_t frames = g_dsp_frames, dropped = ndspGetDroppedFrames();
    s.callbacks = frames - g_frames_base;
    s.ndsp_dropped = dropped - g_dropped_base;
    g_frames_base = frames;
    g_dropped_base = dropped;
    g_st_starts = 0; g_st_events = 0; g_st_starved = 0; g_st_late = 0; g_st_ticks = 0; g_st_decode = 0; g_st_apply = 0; g_st_remaps = 0;
    return s;
}
