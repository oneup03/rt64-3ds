// reverb_match <log.txt>...: the reverb settings dsp_reverb_match.cpp
// chooses for each effect a game set (its log's "dsp-reverb: effect"
// lines), run through OpenDSP's firmware reverb (firmware/reverb.c, built
// for the host) and measured as the game's effect is: decay (RT60) and
// loudness against the game's, for the model and for the firmware.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "dsp_reverb_match.h"

extern "C" {
#include "reverb.h"
}

namespace {

constexpr int F = 160;
constexpr float kRate = 32728.0f;       // effects.c's SAMPLE_RATE
constexpr int kLineMax[4] = { 1777, 1543, 1291, 1009 };

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
s16 to_q15(float v) {
    const float q = clampf(v, -1.0f, 1.0f) * 32767.0f;
    return (s16)(q < 0 ? q - 0.5f : q + 0.5f);
}

// opendspReverbSet's arithmetic (library/source/effects.c), as firmware params.
reverb_params_t params(const recomp3ds::ReverbMatch& m) {
    reverb_params_t p = {};
    const float size = 0.35f + 0.65f * clampf(m.room_size, 0.0f, 1.0f);
    const float decay = clampf(m.decay_time, 0.1f, 10.0f), width = clampf(m.width, 0.0f, 1.0f), level = clampf(m.level, 0.0f, 1.0f);
    float energy = 0.0f;
    p.enable = 1;
    for (int k = 0; k < 4; k++) {
        u16 length = (u16)(kLineMax[k] * size + 0.5f);
        if (length < 160) length = 160;
        p.length[k] = length;
        const float g = powf(10.0f, -3.0f * length / (decay * kRate));
        p.gain[k] = to_q15(g);
        energy += g * g / 4;
    }
    p.damping = to_q15(clampf(m.damping, 0.0f, 1.0f) * 0.9f);
    p.diffusion = to_q15(clampf(m.diffusion, 0.0f, 1.0f) * 0.7f);
    p.out_same = to_q15(level * (1.0f + width) * 0.5f);
    p.out_cross = to_q15(level * (1.0f - width) * 0.5f);
    const float input = 0.87f * sqrtf(1.0f - energy);
    p.input_gain = to_q15(input > 1.0f ? 1.0f : input);
    return p;
}

// The firmware's response to an impulse of `amp` on the bus's front left and
// right: the left output's energy per frame (per unit impulse), times the
// return volume squared.
recomp3ds::ReverbResponse firmware_response(const recomp3ds::ReverbMatch& m) {
    static s32 buf[4][2 * F];
    s32* quad[4] = { buf[0], buf[1], buf[2], buf[3] };
    // Near full scale: lower, the firmware's quantization and its quiet
    // detector (which clears a tail within +-2) cut the measured decay short.
    const int amp = 30000;
    reverb_init();
    reverb_params_t off = {};
    reverb_set(0, &off);
    const reverb_params_t p = params(m);
    reverb_set(0, &p);
    recomp3ds::ReverbResponse r;
    double peak = 0.0;
    for (int b = 0; b < (int)(2.0f * kRate) / F; b++) {
        memset(buf, 0, sizeof(buf));
        if (b == 0) buf[0][0] = buf[1][0] = amp;
        reverb_run(0, quad);
        // In blocks of 32 samples, as dsp_reverb_match.cpp measures.
        double frame = 0.0;
        for (int q = 0; q < F; q += 32) {
            double e = 0.0;
            for (int n = q; n < q + 32; n++) e += (double)buf[0][2 * n] * buf[0][2 * n];
            e = e / ((double)amp * amp) * m.volume * m.volume;
            r.block.push_back(e);
            if (e > peak) peak = e;
            frame += e;
        }
        if (b > 8 && frame < peak * 1e-7) break;
    }
    recomp3ds::reverb_measure(r);
    return r;
}

double db(double ratio) { return 10.0 * log10(ratio > 0.0 ? ratio : 1e-30); }

}   // namespace

int main(int argc, char** argv) {
    std::vector<recomp3ds::ReverbConfig> seen;
    for (int a = 1; a < argc; a++) {
        FILE* f = fopen(argv[a], "r");
        if (f == nullptr) { perror(argv[a]); return 1; }
        char line[512];
        recomp3ds::ReverbConfig c;
        int want = -1;
        while (fgets(line, sizeof(line), f)) {
            const char* s = strstr(line, "dsp-reverb: effect ");
            unsigned count; unsigned long length, rate;
            if (s && sscanf(s, "dsp-reverb: effect %u sections, %lu samples at %lu Hz", &count, &length, &rate) == 3) {
                c = recomp3ds::ReverbConfig{};
                c.count = (uint8_t)count; c.length = (uint32_t)length; c.rate = (uint32_t)rate;
                want = (int)count;
            }
            else if (want > 0 && (s = strstr(line, "dsp-reverb:   ")) != nullptr) {
                int i, ff, fb, gain, lp; unsigned long in, out;
                if (sscanf(s, "dsp-reverb:   %d: taps %lu-%lu ff %d fb %d gain %d lp %d", &i, &in, &out, &ff, &fb, &gain, &lp) == 7 && i < 8) {
                    recomp3ds::ReverbConfig::Section& sec = c.sec[i];
                    sec.input = (uint32_t)in; sec.output = (uint32_t)out;
                    sec.ffcoef = (int16_t)ff; sec.fbcoef = (int16_t)fb; sec.gain = (int16_t)gain; sec.lp_gain = (int16_t)lp;
                    sec.chorus = strstr(s, " chorus") != nullptr;
                    want--;
                }
            }
            if (want == 0) {
                bool dup = false;
                for (const auto& o : seen) {
                    if (o.count != c.count || o.length != c.length || o.rate != c.rate) continue;
                    bool same = true;
                    for (int i = 0; i < c.count; i++) {
                        const auto &x = o.sec[i], &y = c.sec[i];
                        if (x.input != y.input || x.output != y.output || x.ffcoef != y.ffcoef || x.fbcoef != y.fbcoef || x.gain != y.gain ||
                            x.lp_gain != y.lp_gain || x.chorus != y.chorus) { same = false; break; }
                    }
                    if (same) { dup = true; break; }
                }
                if (!dup && c.count > 0) seen.push_back(c);
                want = -1;
            }
        }
        fclose(f);
    }
    printf("%zu distinct effects\n", seen.size());
    int worst_bad = 0;
    for (size_t e = 0; e < seen.size(); e++) {
        const recomp3ds::ReverbConfig& c = seen[e];
        recomp3ds::ReverbResponse n64, model;
        const recomp3ds::ReverbMatch m = recomp3ds::reverb_match(c, &n64, &model);
        printf("effect %zu: %u sections, %lu samples at %lu Hz\n", e, (unsigned)c.count, (unsigned long)c.length, (unsigned long)c.rate);
        if (m.echo) {
            // OpenDSP's delay (DSP assembly, no host build): echo n is
            // level x return x feedback^(n-1) on each side, every delay
            // rounded to DSP frames; laid out in 32-sample blocks and
            // measured as the game's response is (an echo train's decay
            // read off its steps, not a formula's).
            const double frames = std::round(m.delay_ms * kRate / (1000.0 * F));
            const double secs = (frames < 1 ? 1 : frames) * F / kRate;
            const double first = (double)m.level * m.volume, fb = m.feedback;
            recomp3ds::ReverbResponse dl;
            for (int n = 0; n < 200; n++) {
                const double amp = first * pow(fb, n);
                if (amp < first * 1e-4) break;
                const size_t at = (size_t)((n + 1) * secs * kRate / 32.0);
                if (dl.block.size() <= at) dl.block.resize(at + 1, 0.0);
                dl.block[at] += amp * amp;
            }
            recomp3ds::reverb_measure(dl);
            const double energy = dl.energy, rt60 = dl.rt60;
            printf("  echo:     delay %.1f ms (%.1f ms in frames) feedback %.2f damping %.2f level %.2f return %.2f\n", m.delay_ms, secs * 1000.0,
                   m.feedback, m.damping, m.level, m.volume);
            printf("  RT60 s:   game %.2f  delay %.2f\n", n64.rt60, rt60);
            printf("  energy:   game %.4f  delay %+.1f dB\n", n64.energy, db(energy / n64.energy));
            const bool bad = fabs(db(energy / n64.energy)) > 1.5 || (n64.rt60 > 0.0 && fabs(log(rt60 / n64.rt60)) > log(1.25));
            if (bad) worst_bad++;
            printf("  %s\n", bad ? "MISMATCH (more than 1.5 dB or 25% in decay)" : "ok");
            continue;
        }
        const recomp3ds::ReverbResponse fw = firmware_response(m);
        // The game's late part (after the early 40 ms, which goes dry: early^2
        // of the energy), against the firmware's whole response.
        const int early_blocks = (int)(0.040 * (268111856.0 / 8192.0) / 32.0 + 0.5);
        recomp3ds::ReverbResponse late;
        for (size_t b = early_blocks; b < n64.block.size(); b++) late.block.push_back(n64.block[b]);
        recomp3ds::reverb_measure(late);
        printf("  settings: early %.2f dry; room %.2f decay %.2f s damping %.2f diffusion %.2f level %.2f return %.2f\n", m.early, m.room_size,
               m.decay_time, m.damping, m.diffusion, m.level, m.volume);
        printf("  early:    %.4f of %.4f (%.0f%%), played dry\n", (double)m.early * m.early, n64.energy, 100.0 * m.early * m.early / n64.energy);
        printf("  RT60 s:   game (late) %.2f  model %.2f  firmware %.2f\n", late.rt60, model.rt60, fw.rt60);
        printf("  energy:   game (late) %.4f  model %+.1f dB  firmware %+.1f dB\n", late.energy, db(model.energy / late.energy), db(fw.energy / late.energy));
        const bool bad = fabs(db(fw.energy / late.energy)) > 1.5 || (late.rt60 > 0.0 && fabs(log(fw.rt60 / late.rt60)) > log(1.25));
        if (bad) worst_bad++;
        printf("  %s\n", bad ? "MISMATCH (more than 1.5 dB or 25% in decay)" : "ok");
    }
    return worst_bad == 0 ? 0 : 2;
}
