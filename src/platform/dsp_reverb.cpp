// The synthesizer's reverb on the DSP's aux bus (dsp_reverb.h).
#include "dsp_reverb.h"
#include "recomp3ds.h"

#include <3ds.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double kDspRate = 268111856.0 / 8192.0;   // ndsp's output rate (32728.5 Hz)
constexpr uint32_t kRing = 4096;                    // samples between the callback and the thread
constexpr uint32_t kRingMask = kRing - 1;
// The bus's round trip: a frame's sends reach the callback after the DSP
// mixed it, the output goes back a callback later with a frame kept in hand,
// and the DSP mixes it into the frame after: about three frames.
constexpr uint32_t kLatency = 3 * 160;
constexpr bool kAuxReturn = false;      // see dsp_reverb_process

// The bus's samples, mono (libaudio's input: 0.707 of the left and right
// sends summed), callback -> thread; the reverb's output, thread -> callback.
float g_in[kRing], g_out[kRing];
volatile uint32_t g_in_w = 0, g_in_r = 0, g_out_w = 0, g_out_r = 0;
bool g_primed = false;                  // the callback's: output is flowing
volatile bool g_running = false;        // an effect runs (the voices send)
volatile uint32_t g_ticks = 0, g_gaps = 0;

// The configuration the hooks last set, taken up by the thread.
LightLock g_lock;
recomp3ds::ReverbConfig g_pending;
bool g_has_pending = false;
recomp3ds::ReverbConfig g_config;       // the thread's

// The running effect (the thread's): the delay line at the DSP's rate.
struct Section {
    uint32_t in_d = 0, out_d = 0;
    float ff = 0.0f, fb = 0.0f, gain = 0.0f;
    bool lp = false, chorus = false;
    float lp_c = 0.0f, lp_g = 0.0f, lp_y = 0.0f;
    uint32_t lead = 0;          // the output read this much nearer the input (kLatency)
};
std::vector<float> g_line;
uint32_t g_mask = 0, g_w = 0;
Section g_sec[8];
int g_count = 0;

inline float sat16(float v) { return v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v); }

// One output sample from one input sample: alFxPull's per-section steps (as
// the microcode's MIXER and POLEF compute them, a block at a time) for a
// single sample. Values stay in the 16-bit range, as in DMEM.
inline float step(float x) {
    float* line = g_line.data();
    const uint32_t m = g_mask, w = g_w;
    line[w & m] = sat16(x);
    float out = 0.0f;
    for (int i = 0; i < g_count; i++) {
        Section& s = g_sec[i];
        const uint32_t ia = (w - s.in_d) & m, ib = (w - s.out_d) & m;
        float a = line[ia], b = line[ib];
        b = sat16(b + s.ff * a);
        if (s.fb != 0.0f) {
            a = sat16(a + s.fb * b);
            line[ia] = a;
        }
        if (s.lp) {
            s.lp_y = sat16(s.lp_c * s.lp_y + s.lp_g * b);
            b = s.lp_y;
        }
        // A chorus section's output tap is resampled from the line and not
        // written back; its modulation (a fraction of a sample) is left out.
        if (!s.chorus) line[ib] = b;
        out = sat16(out + s.gain * (s.lead != 0 ? line[(w - s.out_d + s.lead) & m] : b));
    }
    g_w = w + 1;
    return out;
}

void apply(const recomp3ds::ReverbConfig& c) {
    const bool taps_same = c.count == g_config.count && c.length == g_config.length && c.rate == g_config.rate && [&] {
        for (int i = 0; i < c.count; i++) {
            if (c.sec[i].input != g_config.sec[i].input || c.sec[i].output != g_config.sec[i].output) return false;
        }
        return true;
    }();
    g_config = c;
    if (c.count == 0 || c.rate == 0) {
        g_count = 0;
        g_running = false;
        ndspAuxSetEnable(0, false);
        return;
    }
    // The taps at the DSP's rate; the line a power of two (masked indices).
    const double scale = kDspRate / (double)c.rate;
    const uint32_t need = (uint32_t)((double)c.length * scale) + 16;
    uint32_t size = 1024;
    while (size < need) size <<= 1;
    if (!taps_same || g_line.size() != size) {
        g_line.assign(size, 0.0f);
        g_mask = size - 1;
        g_w = 0;
    }
    g_count = c.count;
    for (int i = 0; i < c.count; i++) {
        const recomp3ds::ReverbConfig::Section& in = c.sec[i];
        Section& s = g_sec[i];
        s.in_d = (uint32_t)lround(in.input * scale);
        s.out_d = (uint32_t)lround(in.output * scale);
        s.ff = in.ffcoef / 32768.0f;
        s.fb = in.fbcoef / 32768.0f;
        s.gain = in.gain / 32768.0f;
        s.chorus = in.chorus;
        // POLEF: y = (fc * y + fgain * x) >> 14, fc = 16384 - fgain; the
        // same time constant at the DSP's rate.
        s.lp = in.lp_gain > 0 && in.lp_gain < 16384;
        if (s.lp) {
            const double c_n64 = 1.0 - in.lp_gain / 16384.0;
            s.lp_c = (float)pow(c_n64, (double)c.rate / kDspRate);
            s.lp_g = 1.0f - s.lp_c;
        }
        if (!taps_same) s.lp_y = 0.0f;
        // A section whose output is its tap as the line holds it (no
        // feedforward, low-pass or chorus) can be heard from the line as it
        // was kLatency samples later, which the bus's round trip takes back.
        s.lead = in.ffcoef == 0 && !s.lp && !s.chorus && s.out_d > s.in_d + kLatency ? kLatency : 0;
    }
    if (!g_running) {
        ndspAuxSetVolume(0, 1.0f);
        ndspAuxSetFrontBypass(0, false);
        ndspAuxSetEnable(0, true);
        g_running = true;
    }
    fprintf(stderr, "dsp-reverb: %u sections, %lu samples at %lu Hz (%lu at the DSP's rate)\n", (unsigned)c.count,
            (unsigned long)c.length, (unsigned long)c.rate, (unsigned long)need);
    for (int i = 0; i < c.count; i++) {
        const recomp3ds::ReverbConfig::Section& in = c.sec[i];
        fprintf(stderr, "dsp-reverb:   %d: taps %lu-%lu ff %d fb %d gain %d lp %d%s\n", i, (unsigned long)in.input,
                (unsigned long)in.output, in.ffcoef, in.fbcoef, in.gain, in.lp_gain, in.chorus ? " chorus" : "");
    }
}

// ndsp's thread, after each frame: the bus's samples out, the reverb in.
// The intermediate mix is 32-bit, four channels (front L/R, back L/R).
void aux_callback(void*, int nsamples, void* bufs[4]) {
    int32_t* fl = (int32_t*)bufs[0];
    int32_t* fr = (int32_t*)bufs[1];
    int32_t* bl = (int32_t*)bufs[2];
    int32_t* br = (int32_t*)bufs[3];
    if (!g_running) {
        for (int i = 0; i < nsamples; i++) fl[i] = fr[i] = bl[i] = br[i] = 0;
        g_primed = false;
        return;
    }
    uint32_t w = g_in_w;
    const uint32_t r_in = g_in_r;
    for (int i = 0; i < nsamples; i++) {
        if (w - r_in >= kRing) break;       // the thread is far behind: drop
        g_in[w & kRingMask] = 0.70710678f * (float)(fl[i] + fr[i]);
        w++;
    }
    __sync_synchronize();
    g_in_w = w;
    // One frame of output kept in hand against a late thread.
    uint32_t r = g_out_r;
    const uint32_t avail = g_out_w - r;
    __sync_synchronize();
    if (!g_primed && avail >= (uint32_t)nsamples * 2) g_primed = true;
    if (g_primed && avail < (uint32_t)nsamples) {
        g_primed = false;
        g_gaps = g_gaps + 1;
    }
    for (int i = 0; i < nsamples; i++) {
        int32_t y = 0;
        if (g_primed) y = (int32_t)g_out[(r++) & kRingMask];
        fl[i] = fr[i] = y;
        bl[i] = br[i] = 0;
    }
    __sync_synchronize();
    g_out_r = r;
}

bool g_off = false;     // REVERB_OFF.TXT beside the executable: no reverb (an A/B switch)

}   // namespace

bool recomp3ds::ReverbConfig::same(const ReverbConfig& o) const {
    if (count != o.count || length != o.length || rate != o.rate) return false;
    for (int i = 0; i < count; i++) {
        const Section& a = sec[i];
        const Section& b = o.sec[i];
        if (a.input != b.input || a.output != b.output || a.ffcoef != b.ffcoef || a.fbcoef != b.fbcoef || a.gain != b.gain ||
            a.lp_gain != b.lp_gain || a.chorus != b.chorus) return false;
    }
    return true;
}

void recomp3ds::dsp_reverb_init() {
    char path[192];
    snprintf(path, sizeof(path), "%s/REVERB_OFF.TXT", recomp3ds::base_path());
    if (FILE* f = fopen(path, "r")) {
        fclose(f);
        g_off = true;
        fprintf(stderr, "dsp-reverb: off (REVERB_OFF.TXT)\n");
    }
    LightLock_Init(&g_lock);
    ndspAuxSetEnable(0, false);
    ndspAuxSetCallback(0, aux_callback, nullptr);
}

void recomp3ds::dsp_reverb_shutdown() {
    g_running = false;
    ndspAuxSetEnable(0, false);
    ndspAuxSetCallback(0, nullptr, nullptr);
}

void recomp3ds::dsp_reverb_configure(const ReverbConfig& c) {
    LightLock_Lock(&g_lock);
    if (!c.same(g_pending) || !g_has_pending) {
        g_pending = c;
        g_has_pending = true;
    }
    LightLock_Unlock(&g_lock);
}

bool recomp3ds::dsp_reverb_active() { return g_running; }

void recomp3ds::dsp_reverb_process() {
    const u64 t0 = svcGetSystemTick();
    LightLock_Lock(&g_lock);
    const bool take = g_has_pending;
    recomp3ds::ReverbConfig c;
    if (take) { c = g_pending; g_has_pending = false; }
    LightLock_Unlock(&g_lock);
    // Not yet: libctru's ndsp never calls aux callbacks (its thread calls
    // the frame callback alone) nor moves the bus's samples between the DSP
    // and the CPU, so an enabled bus mixes its raw sends back into the
    // output. The bus stays off (the voices dry) until that plumbing exists.
    if (take && (g_off || !kAuxReturn)) c = recomp3ds::ReverbConfig{};
    if (take && !c.same(g_config)) apply(c);
    if (!g_running) {
        g_in_r = g_in_w;
        return;
    }
    uint32_t r = g_in_r;
    const uint32_t w = g_in_w;
    __sync_synchronize();
    uint32_t ow = g_out_w;
    const uint32_t or_ = g_out_r;
    while (r != w && ow - or_ < kRing) {
        g_out[ow & kRingMask] = step(g_in[r & kRingMask]);
        r++;
        ow++;
    }
    __sync_synchronize();
    g_in_r = r;
    g_out_w = ow;
    g_ticks = g_ticks + (uint32_t)(svcGetSystemTick() - t0);
}

void recomp3ds::dsp_reverb_take_stats(uint32_t* ticks, uint32_t* gaps) {
    *ticks = g_ticks;
    *gaps = g_gaps;
    g_ticks = 0;
    g_gaps = 0;
}
