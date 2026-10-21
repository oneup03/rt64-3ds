// OpenDSP's reverb settings for a game's effect (dsp_reverb_match.h).
#include "dsp_reverb_match.h"

#include <cmath>
#include <cstring>

namespace {

constexpr double kDspRate = 268111856.0 / 8192.0;   // ndsp's output rate (32728.5 Hz)
constexpr int kBlock = 32;                          // about 1 ms: the decay is measured in these
constexpr int kMaxBlocks = (int)(2.0 * kDspRate) / kBlock;   // 2 s of response at most, the rest extrapolated
constexpr float kImpulse = 1000.0f;                 // well inside the 16-bit range the N64's sections saturate at
constexpr int kLineMax[4] = { 1777, 1543, 1291, 1009 };     // OpenDSP's lines (EXT_REVERB_LINE_MAX)
// The all-pass in each loop (LOOP_LENGTHS): on average its length more delay
// round the loop, which the firmware's gains (from the line alone) leave out.
constexpr int kLoopLength[4] = { 79, 97, 113, 151 };
// The bus's return volume: the firmware holds it in Q14, clamped at 0x7FFF.
constexpr float kReturnMax = 1.99f;
// The effect's early part, played dry (ReverbMatch::early): 40 ms.
constexpr int kEarlyBlocks = (int)(0.040 * kDspRate / kBlock + 0.5);
constexpr double kLineMean = (1777 + 1543 + 1291 + 1009) / 4.0;

inline float sat16(float v) { return v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Schroeder's backward integration: RT60 from the slope between -5 and
// -25 dB (or what the window reached), and the whole energy (the tail past
// the window extrapolated at that decay).
void measure_impl(recomp3ds::ReverbResponse& r) {
    const int n = (int)r.block.size();
    std::vector<double> edc(n + 1, 0.0);
    for (int i = n - 1; i >= 0; i--) edc[i] = edc[i + 1] + r.block[i];
    r.energy = edc[0];
    r.rt60 = 0.0;
    if (edc[0] <= 0.0) return;
    auto db = [&](int i) { return 10.0 * log10(edc[i] / edc[0] + 1e-30); };
    int i5 = 0, i25 = -1;
    for (int i = 0; i < n; i++) {
        if (db(i) > -5.0) i5 = i;
        if (db(i) <= -25.0) { i25 = i; break; }
    }
    const int end = i25 >= 0 ? i25 : n - 1;
    const double fall = db(i5) - db(end);
    const double secs = (double)(end - i5) * kBlock / kDspRate;
    r.rt60 = fall > 1.0 && secs > 0.0 ? 60.0 * secs / fall : 10.0;
    if (i25 < 0 && n > 0) {
        const double per_block = pow(10.0, -6.0 * kBlock / (r.rt60 * kDspRate));
        r.energy += r.block[n - 1] * per_block / (1.0 - per_block);
    }
}

// Records a block's energy; true once the last `window` blocks (an echo
// period: the longest delay, so the gaps between echoes don't count) have
// fallen 70 dB below the loudest block.
bool record(recomp3ds::ReverbResponse& r, double e, double& peak, double& recent, int window) {
    e /= (double)kImpulse * kImpulse;
    r.block.push_back(e);
    if (e > peak) peak = e;
    recent += e;
    const int n = (int)r.block.size();
    if (n > window) recent -= r.block[n - 1 - window];
    return n > window && recent < peak * 1e-7;
}

}   // namespace

void recomp3ds::reverb_measure(ReverbResponse& r) { measure_impl(r); }

// alFxPull's sections, as the microcode's MIXER and POLEF compute them, per
// sample at the DSP's rate, on an impulse sent equally to the bus's left and
// right: libaudio's input is 0.707 of the two summed, and the mono output is
// heard on both sides.
recomp3ds::ReverbResponse recomp3ds::reverb_n64_response(const ReverbConfig& c) {
    struct Sec { uint32_t in_d, out_d; float ff, fb, gain; bool lp, chorus; float lp_c, lp_g, lp_y; };
    ReverbResponse r;
    if (c.count == 0 || c.rate == 0) return r;
    const double scale = kDspRate / (double)c.rate;
    uint32_t size = 1024;
    while (size < (uint32_t)((double)c.length * scale) + 16) size <<= 1;
    std::vector<float> line(size, 0.0f);
    const uint32_t m = size - 1;
    Sec sec[8];
    for (int i = 0; i < c.count; i++) {
        const ReverbConfig::Section& in = c.sec[i];
        Sec& s = sec[i];
        s.in_d = (uint32_t)lround(in.input * scale);
        s.out_d = (uint32_t)lround(in.output * scale);
        s.ff = in.ffcoef / 32768.0f;
        s.fb = in.fbcoef / 32768.0f;
        s.gain = in.gain / 32768.0f;
        s.chorus = in.chorus;
        // POLEF: y = (fc * y + fgain * x) >> 14, fc = 16384 - fgain; the
        // same time constant at the DSP's rate.
        s.lp = in.lp_gain > 0 && in.lp_gain < 16384;
        s.lp_c = s.lp ? (float)pow(1.0 - in.lp_gain / 16384.0, (double)c.rate / kDspRate) : 0.0f;
        s.lp_g = 1.0f - s.lp_c;
        s.lp_y = 0.0f;
    }
    r.block.reserve(kMaxBlocks);
    uint32_t w = 0, longest = 0;
    for (int i = 0; i < c.count; i++) longest = sec[i].out_d > longest ? sec[i].out_d : longest;
    const int window = (int)(longest / kBlock) + 2;
    double peak = 0.0, recent = 0.0;
    for (int b = 0; b < kMaxBlocks; b++) {
        double e = 0.0;
        for (int k = 0; k < kBlock; k++, w++) {
            const float x = (b == 0 && k == 0) ? 0.70710678f * 2.0f * kImpulse : 0.0f;
            line[w & m] = sat16(x);
            float out = 0.0f;
            for (int i = 0; i < c.count; i++) {
                Sec& s = sec[i];
                const uint32_t ia = (w - s.in_d) & m, ib = (w - s.out_d) & m;
                float a = line[ia], v = line[ib];
                v = sat16(v + s.ff * a);
                if (s.fb != 0.0f) {
                    a = sat16(a + s.fb * v);
                    line[ia] = a;
                }
                if (s.lp) {
                    s.lp_y = sat16(s.lp_c * s.lp_y + s.lp_g * v);
                    v = s.lp_y;
                }
                // A chorus section's output tap is resampled from the line
                // and not written back; its modulation is left out.
                if (!s.chorus) line[ib] = v;
                out = sat16(out + s.gain * v);
            }
            e += (double)out * out;
        }
        if (record(r, e, peak, recent, window)) break;
    }
    measure_impl(r);
    return r;
}

// firmware/reverb.c's network as opendspReverbSet (library/source/effects.c)
// sets it: the bus's four channels summed and halved, times the input gain;
// four lines (each with its loop's all-pass as its mean delay) whose outputs are low-passed (the damping) and mixed back
// through a Householder matrix (each line's own low-passed output less half
// the sum) at the decay's gains; left from lines 0 and 2, right from 1 and 3,
// mixed by the width.
recomp3ds::ReverbResponse recomp3ds::reverb_opendsp_response(const ReverbMatch& mt) {
    const float size = 0.35f + 0.65f * clampf(mt.room_size, 0.0f, 1.0f);
    const float decay = clampf(mt.decay_time, 0.1f, 10.0f);
    int len[4];
    float g[4], energy = 0.0f;
    for (int k = 0; k < 4; k++) {
        len[k] = (int)(kLineMax[k] * size + 0.5f);
        if (len[k] < 160) len[k] = 160;
        g[k] = powf(10.0f, -3.0f * len[k] / (decay * (float)kDspRate));
        if (g[k] > 0.99f) g[k] = 0.99f;     // the firmware's GAIN_MAX
        energy += g[k] * g[k] / 4.0f;
    }
    const float input = fminf(1.0f, 0.87f * sqrtf(1.0f - energy));
    const float d = clampf(mt.damping, 0.0f, 1.0f) * 0.9f;
    const float level = clampf(mt.level, 0.0f, 1.0f), width = clampf(mt.width, 0.0f, 1.0f);
    const float same = level * (1.0f + width) * 0.5f, cross = level * (1.0f - width) * 0.5f;
    std::vector<float> line[4];
    // Each line's delay with its loop's all-pass as that much more delay.
    for (int k = 0; k < 4; k++) line[k].assign(len[k] + kLoopLength[k], 0.0f);
    int pos[4] = {};
    float lp[4] = {};
    ReverbResponse r;
    r.block.reserve(kMaxBlocks);
    const int window = len[0] / kBlock + 2;
    double peak = 0.0, recent = 0.0;
    for (int b = 0; b < kMaxBlocks; b++) {
        double e = 0.0;
        for (int n = 0; n < kBlock; n++) {
            const float x = (b == 0 && n == 0) ? (2.0f * kImpulse) * 0.5f * input : 0.0f;
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                const float out = line[k][pos[k]];
                lp[k] = out + d * (lp[k] - out);
                sum += lp[k];
            }
            for (int k = 0; k < 4; k++) {
                line[k][pos[k]] = x + g[k] * (lp[k] - 0.5f * sum);
                if (++pos[k] == (int)line[k].size()) pos[k] = 0;
            }
            const float left = 0.5f * (lp[0] + lp[2]), right = 0.5f * (lp[1] + lp[3]);
            const float out = same * left + cross * right;
            e += (double)out * out;
        }
        if (record(r, e, peak, recent, window)) break;
    }
    measure_impl(r);
    return r;
}

recomp3ds::ReverbMatch recomp3ds::reverb_match(const ReverbConfig& c, ReverbResponse* n64_out, ReverbResponse* matched_out) {
    ReverbMatch mt;
    const ReverbResponse n64 = reverb_n64_response(c);
    if (n64_out != nullptr) *n64_out = n64;
    const double scale = c.rate != 0 ? kDspRate / (double)c.rate : 1.0;
    // One echo: a single heard section reading the newest input, feeding its
    // output back with no feedforward (Goemon's 162 ms). Its echoes come at
    // the taps' distance, each fb times the last, the first gain x 0.707 x
    // (L + R) on both sides; OpenDSP's delay gives level x feedback^(n-1) on
    // each side, in steps of a DSP frame (4.9 ms), 5 to 975 ms.
    if (c.count == 1 && c.rate != 0) {
        const ReverbConfig::Section& s = c.sec[0];
        const float ms = (float)(s.output - s.input) * 1000.0f / (float)c.rate;
        if (s.input == 0 && s.ffcoef == 0 && s.fbcoef != 0 && s.gain != 0 && !s.chorus && ms >= 5.0f && ms <= 975.0f) {
            mt.echo = true;
            mt.delay_ms = ms;
            mt.feedback = clampf(fabsf(s.fbcoef / 32768.0f), 0.0f, 0.95f);
            if (s.lp_gain > 0 && s.lp_gain < 16384) mt.damping = clampf((float)pow(1.0 - s.lp_gain / 16384.0, 1.0 / scale) / 0.9f, 0.0f, 1.0f);
            const float first = 1.41421356f * fabsf(s.gain / 32768.0f);
            mt.level = clampf(first, 0.01f, 1.0f);
            mt.volume = first > 1.0f ? fminf(first, kReturnMax) : 1.0f;
            mt.decay_time = (float)n64.rt60;
            return mt;
        }
    }
    // The echoes' spacing: the heard sections' taps apart, against OpenDSP's
    // lines (35% to 100% of them).
    double spacing = 0.0;
    int heard = 0;
    bool allpass = false;
    float pole = 0.0f;
    for (int i = 0; i < c.count; i++) {
        const ReverbConfig::Section& s = c.sec[i];
        if (s.ffcoef != 0 && s.fbcoef != 0) allpass = true;
        if (s.lp_gain > 0 && s.lp_gain < 16384) pole = fmaxf(pole, (float)pow(1.0 - s.lp_gain / 16384.0, 1.0 / scale));
        if (s.gain == 0 || s.output <= s.input) continue;
        spacing += (double)(s.output - s.input) * scale;
        heard++;
    }
    const float size = heard > 0 ? clampf((float)(spacing / heard / kLineMean), 0.35f, 1.0f) : 0.6f;
    mt.room_size = (size - 0.35f) / 0.65f;
    // The game's low-pass pole as OpenDSP's in-loop one (damping x 0.9).
    mt.damping = clampf(pole / 0.9f, 0.0f, 1.0f);
    mt.diffusion = allpass ? 0.7f : 0.4f;
    mt.width = 0.0f;            // the N64's reverb is mono, on both sides
    mt.level = 1.0f;
    // Early and late: the first kEarlyMs go dry (ReverbMatch::early); the
    // reverb matches the rest, its decay and energy measured on that part.
    ReverbResponse late;
    double early = 0.0;
    for (size_t b = 0; b < n64.block.size(); b++) {
        if ((int)b < kEarlyBlocks) early += n64.block[b];
        else late.block.push_back(n64.block[b]);
    }
    reverb_measure(late);
    mt.early = (float)sqrt(early);
    mt.decay_time = clampf((float)late.rt60, 0.1f, 10.0f);
    // The damping shortens OpenDSP's broadband decay below its decay time:
    // once more, scaled by what the model measured.
    ReverbResponse od = reverb_opendsp_response(mt);
    if (od.rt60 > 0.0 && late.rt60 > 0.0) {
        mt.decay_time = clampf((float)(mt.decay_time * late.rt60 / od.rt60), 0.1f, 10.0f);
        od = reverb_opendsp_response(mt);
    }
    // The level that gives the late part's energy; past OpenDSP's 1, the bus's return.
    const float gain = od.energy > 0.0 ? (float)sqrt(late.energy / od.energy) : 0.0f;
    mt.level = clampf(gain, 0.0f, 1.0f);
    mt.volume = gain > 1.0f ? fminf(gain, kReturnMax) : 1.0f;
    if (matched_out != nullptr) {
        // The model at the chosen level (and return).
        ReverbMatch at = mt;
        *matched_out = reverb_opendsp_response(at);
        for (double& e : matched_out->block) e *= (double)mt.volume * mt.volume;
        matched_out->energy *= (double)mt.volume * mt.volume;
    }
    return mt;
}
