// Host check of the combiner planner: replays a captured gfx task
// (gfx_task.bin, as dl_test does), plans every distinct combiner state the
// frame uses, runs the planned PICA stages in software and compares them
// with the RDP's (A - B) * C + D over a spread of texel and shade values.
//
//   tev_test gfx_task.bin [max error in 1/255, default 3]
//
// Sources the planner approximates on purpose (noise, key centre/scale, K4,
// K5 as one half; LOD fraction as zero; COMBINED in cycle 0 as zero) are
// approximated the same way here, so what remains is the planner's own
// error: a lost constant, a wrong operand, or the PICA clamping a negative
// A - B between stages.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

#include "rt64_3ds_dl.h"
#include "rt64_3ds_tev.h"
#include "ultramodern/ultra64.h"

namespace {

struct V4 { float r, g, b, a; };

V4 rgba8(uint32_t c) {   // citro3d constants are 0xAABBGGRR
    return { (c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, (c >> 24) / 255.0f };
}
V4 bytes4(const uint8_t* p) { return { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f }; }
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// The PICA: each stage's result is clamped before the next reads it.
V4 run_tev(const rt64_3ds::TevPlan& p, int stages, V4 shade, V4 t0, V4 t1, bool* odd) {
    V4 prev = { 0, 0, 0, 0 };
    const V4 buf = rgba8(p.buffer_color);
    for (int i = 0; i < stages; i++) {
        const rt64_3ds::TevStage& st = p.stage[i];
        const V4 k = rgba8(st.constant);
        auto src = [&](GPU_TEVSRC s) -> V4 {
            switch (s) {
                case GPU_PRIMARY_COLOR: return shade;
                case GPU_TEXTURE0: return t0;
                case GPU_TEXTURE1: return t1;
                case GPU_CONSTANT: return k;
                case GPU_PREVIOUS: if (i == 0) *odd = true; return prev;
                case GPU_PREVIOUS_BUFFER: return buf;
                default: *odd = true; return { 0, 0, 0, 0 };
            }
        };
        // Only the operands the function reads (unused ones hold PREVIOUS).
        auto operands = [](GPU_COMBINEFUNC fn) {
            return fn == GPU_REPLACE ? 1 : ((fn == GPU_INTERPOLATE || fn == GPU_MULTIPLY_ADD || fn == GPU_ADD_MULTIPLY) ? 3 : 2);
        };
        float rgb[3][3] = {}, al[3] = {};
        for (int j = 0; j < 3; j++) {
            if (j >= operands(st.func_rgb)) continue;
            V4 v = src(st.src_rgb[j]);
            if (st.op_rgb[j] == GPU_TEVOP_RGB_SRC_ALPHA) { rgb[j][0] = rgb[j][1] = rgb[j][2] = v.a; }
            else { if (st.op_rgb[j] != GPU_TEVOP_RGB_SRC_COLOR) *odd = true; rgb[j][0] = v.r; rgb[j][1] = v.g; rgb[j][2] = v.b; }
        }
        for (int j = 0; j < 3; j++) {
            if (j >= operands(st.func_a)) continue;
            V4 w = src(st.src_a[j]);
            if (st.op_a[j] != GPU_TEVOP_A_SRC_ALPHA) *odd = true;
            al[j] = w.a;
        }
        auto f = [&](GPU_COMBINEFUNC fn, float a, float b, float c) {
            switch (fn) {
                case GPU_REPLACE: return a;
                case GPU_MODULATE: return a * b;
                case GPU_ADD: return a + b;
                case GPU_ADD_SIGNED: return a + b - 0.5f;
                case GPU_INTERPOLATE: return a * c + b * (1.0f - c);
                case GPU_SUBTRACT: return a - b;
                case GPU_MULTIPLY_ADD: return a * b + c;
                case GPU_ADD_MULTIPLY: return (a + b) * c;
                default: *odd = true; return 0.0f;
            }
        };
        V4 out;
        out.r = clamp01(f(st.func_rgb, rgb[0][0], rgb[1][0], rgb[2][0]));
        out.g = clamp01(f(st.func_rgb, rgb[0][1], rgb[1][1], rgb[2][1]));
        out.b = clamp01(f(st.func_rgb, rgb[0][2], rgb[1][2], rgb[2][2]));
        out.a = clamp01(f(st.func_a, al[0], al[1], al[2]));
        prev = out;
    }
    return prev;
}

// The RDP colour combiner, from the input tables (not the planner's).
V4 run_rdp(const rt64_3ds::DrawRecord& d, V4 shade, V4 t0, V4 t1, bool* negative) {
    const V4 prim = bytes4(d.prim), env = bytes4(d.env);
    const float half = 0.5f, lod = 0.0f, primlod = d.prim_lod_frac / 255.0f;
    const uint32_t w0 = d.cc_w0, w1 = d.cc_w1;
    const uint32_t a[2] = { (w0 >> 20) & 0xF, (w0 >> 5) & 0xF }, c[2] = { (w0 >> 15) & 0x1F, w0 & 0x1F };
    const uint32_t b[2] = { (w1 >> 28) & 0xF, (w1 >> 24) & 0xF }, dd[2] = { (w1 >> 15) & 7, (w1 >> 6) & 7 };
    const uint32_t Aa[2] = { (w0 >> 12) & 7, (w1 >> 21) & 7 }, Ac[2] = { (w0 >> 9) & 7, (w1 >> 18) & 7 };
    const uint32_t Ab[2] = { (w1 >> 12) & 7, (w1 >> 3) & 7 }, Ad[2] = { (w1 >> 9) & 7, w1 & 7 };
    const int cycles = ((d.othermode_h >> 20) & 3) == 1 ? 2 : 1;
    V4 comb = { 0, 0, 0, 0 };
    for (int cy = 0; cy < cycles; cy++) {
        auto rgb = [&](int which, uint32_t sel, int ch) -> float {   // which: 0 A, 1 B, 2 C, 3 D
            auto chan = [&](const V4& v) { return ch == 0 ? v.r : (ch == 1 ? v.g : v.b); };
            switch (sel) {
                case 0: return chan(comb);
                case 1: return chan(t0);
                case 2: return chan(t1);
                case 3: return chan(prim);
                case 4: return chan(shade);
                case 5: return chan(env);
            }
            if (which == 0) return sel == 6 ? 1.0f : (sel == 7 ? half : 0.0f);           // 1, noise
            if (which == 1) return (sel == 6 || sel == 7) ? half : 0.0f;                  // key centre, K4
            if (which == 3) return sel == 6 ? 1.0f : 0.0f;
            switch (sel) {                                                                 // C
                case 6: return half;  case 7: return comb.a; case 8: return t0.a; case 9: return t1.a;
                case 10: return prim.a; case 11: return shade.a; case 12: return env.a; case 13: return lod;
                case 14: return primlod; case 15: return half; default: return 0.0f;
            }
        };
        auto alpha = [&](bool is_c, uint32_t sel) -> float {
            switch (sel) {
                case 0: return is_c ? lod : comb.a;
                case 1: return t0.a; case 2: return t1.a; case 3: return prim.a; case 4: return shade.a; case 5: return env.a;
                case 6: return is_c ? primlod : 1.0f;
                default: return 0.0f;
            }
        };
        V4 out;
        float* o[3] = { &out.r, &out.g, &out.b };
        for (int ch = 0; ch < 3; ch++) {
            float diff = rgb(0, a[cy], ch) - rgb(1, b[cy], ch);
            if (diff < 0.0f) *negative = true;
            *o[ch] = clamp01(diff * rgb(2, c[cy], ch) + rgb(3, dd[cy], ch));
        }
        float adiff = alpha(false, Aa[cy]) - alpha(false, Ab[cy]);
        if (adiff < 0.0f) *negative = true;
        out.a = clamp01(adiff * alpha(true, Ac[cy]) + alpha(false, Ad[cy]));
        comb = out;
    }
    return comb;
}

const char* kRgbName[32] = { "COMB", "T0", "T1", "PRIM", "SHADE", "ENV", "1/CTR/SCALE", "NOISE/K4/COMB_A", "T0_A", "T1_A", "PRIM_A",
                             "SHADE_A", "ENV_A", "LOD", "PRIM_LOD", "K5" };
const char* kAName[8] = { "COMB/LOD", "T0", "T1", "PRIM", "SHADE", "ENV", "1/PRIM_LOD", "0" };

}   // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: tev_test gfx_task.bin [max error in 1/255]\n"); return 2; }
    const float tolerance = (argc > 2 ? atof(argv[2]) : 3.0f) / 255.0f;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    OSTask task;
    std::vector<uint8_t> rdram(16u * 1024 * 1024);
    if (fread(&task, 1, sizeof(OSTask), f) != sizeof(OSTask) || fread(rdram.data(), 1, rdram.size(), f) != rdram.size()) {
        fprintf(stderr, "short capture\n");
        return 2;
    }
    fclose(f);
    rt64_3ds::Interpreter interp(rdram.data());
    rt64_3ds::FrameRecord frame;
    interp.run(task.t.data_ptr, frame);

    static const V4 kInputs[] = {
        { 0, 0, 0, 0 }, { 1, 1, 1, 1 }, { 1, 0, 0, 1 }, { 0, 1, 0, 0.5f }, { 0.25f, 0.5f, 0.75f, 0.25f },
        { 0.9f, 0.1f, 0.4f, 0.8f }, { 0.5f, 0.5f, 0.5f, 0 }, { 0.2f, 0.7f, 0.1f, 1 },
    };
    // One line per distinct combiner state; the first draw that used it.
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, size_t> seen;
    int states = 0, bad = 0, clamp_only = 0;
    for (size_t i = 0; i < frame.draws.size(); i++) {
        const rt64_3ds::DrawRecord& d = frame.draws[i];
        uint32_t cyc = (d.othermode_h >> 20) & 3;
        if (cyc >= 2) continue;   // copy and fill: no combiner
        uint32_t prim, env;
        memcpy(&prim, d.prim, 4); memcpy(&env, d.env, 4);
        auto key = std::make_tuple(d.cc_w0, d.cc_w1, d.othermode_h & 0x300000, prim, env, (uint32_t)d.prim_lod_frac, (uint32_t)d.kind);
        if (seen.count(key)) continue;
        seen[key] = i;
        states++;
        rt64_3ds::TevPlan plan;
        rt64_3ds::plan_tev(d, plan);
        float worst = 0.0f;
        bool odd = false, negative_at_worst = false;
        for (const V4& sh : kInputs) for (const V4& a : kInputs) for (const V4& b : kInputs) {
            bool negative = false;
            V4 want = run_rdp(d, sh, a, b, &negative);
            V4 got = run_tev(plan, plan.combiner_stages, sh, a, b, &odd);
            float e = fmaxf(fmaxf(fabsf(want.r - got.r), fabsf(want.g - got.g)), fmaxf(fabsf(want.b - got.b), fabsf(want.a - got.a)));
            if (e > worst) { worst = e; negative_at_worst = negative; }
        }
        if (worst <= tolerance && !odd) continue;
        // Errors only where the RDP's A - B went negative are the PICA's
        // per-stage clamp (the general two-stage form), a known limit.
        const int ncycles = cyc == 1 ? 2 : 1;
        bool clamp = negative_at_worst && plan.combiner_stages > ncycles;
        if (clamp) clamp_only++; else bad++;
        const uint32_t w0 = d.cc_w0, w1 = d.cc_w1;
        printf("%s draw %zu: error %.0f/255%s, %d stages, %d fallbacks, buffer %08x | cc %06x %08x prim %08x env %08x cycles %d\n",
               clamp ? "clamp" : "BAD  ", i, worst * 255.0f, odd ? " (unexpected source/op)" : "", plan.combiner_stages, plan.fallbacks,
               plan.buffer_color, w0 & 0xFFFFFF, w1, prim, env, cyc == 1 ? 2 : 1);
        printf("      rgb (%s - %s) * %s + %s   alpha (%s - %s) * %s + %s\n",
               kRgbName[(w0 >> 20) & 0xF], kRgbName[(w1 >> 28) & 0xF], kRgbName[((w0 >> 15) & 0x1F) & 15], kRgbName[(w1 >> 15) & 7],
               kAName[(w0 >> 12) & 7], kAName[(w1 >> 12) & 7], kAName[(w0 >> 9) & 7], kAName[(w1 >> 9) & 7]);
    }
    printf("%d combiner states in %zu draws: %d wrong, %d off only by the PICA's per-stage clamp\n", states, frame.draws.size(), bad, clamp_only);
    return bad ? 1 : 0;
}
