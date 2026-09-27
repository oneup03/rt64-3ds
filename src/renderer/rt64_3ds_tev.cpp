// Combiner planner. Each RDP cycle computes (A - B) * C + D per channel; the
// PICA offers REPLACE, MODULATE, ADD, SUBTRACT, INTERPOLATE and
// MULTIPLY_ADD per stage with one constant colour per stage. Common forms
// map to one stage; the general form takes two (SUBTRACT then MULTIPLY_ADD
// on PREVIOUS). Sources the PICA lacks (noise, LOD fractions, key/convert
// constants) become constants.
#include "rt64_3ds_tev.h"

#include <cstring>

namespace rt64_3ds {

namespace {

enum Src : uint8_t { S_COMBINED, S_TEX0, S_TEX1, S_PRIM, S_SHADE, S_ENV, S_ONE, S_ZERO,
                     S_COMBINED_A, S_TEX0_A, S_TEX1_A, S_PRIM_A, S_SHADE_A, S_ENV_A,
                     S_LODFRAC, S_PRIMLODFRAC, S_HALF };

Src map_rgb_a(uint32_t v) {   // A input
    switch (v) { case 0: return S_COMBINED; case 1: return S_TEX0; case 2: return S_TEX1; case 3: return S_PRIM; case 4: return S_SHADE;
                 case 5: return S_ENV; case 6: return S_ONE; case 7: return S_HALF; default: return S_ZERO; }
}
Src map_rgb_b(uint32_t v) {
    switch (v) { case 0: return S_COMBINED; case 1: return S_TEX0; case 2: return S_TEX1; case 3: return S_PRIM; case 4: return S_SHADE;
                 case 5: return S_ENV; case 6: return S_HALF; case 7: return S_HALF; default: return S_ZERO; }
}
Src map_rgb_c(uint32_t v) {
    switch (v) { case 0: return S_COMBINED; case 1: return S_TEX0; case 2: return S_TEX1; case 3: return S_PRIM; case 4: return S_SHADE;
                 case 5: return S_ENV; case 6: return S_HALF; case 7: return S_COMBINED_A; case 8: return S_TEX0_A; case 9: return S_TEX1_A;
                 case 10: return S_PRIM_A; case 11: return S_SHADE_A; case 12: return S_ENV_A; case 13: return S_LODFRAC;
                 case 14: return S_PRIMLODFRAC; case 15: return S_HALF; default: return S_ZERO; }
}
Src map_rgb_d(uint32_t v) {
    switch (v) { case 0: return S_COMBINED; case 1: return S_TEX0; case 2: return S_TEX1; case 3: return S_PRIM; case 4: return S_SHADE;
                 case 5: return S_ENV; case 6: return S_ONE; default: return S_ZERO; }
}
Src map_a_abd(uint32_t v) {
    switch (v) { case 0: return S_COMBINED_A; case 1: return S_TEX0_A; case 2: return S_TEX1_A; case 3: return S_PRIM_A; case 4: return S_SHADE_A;
                 case 5: return S_ENV_A; case 6: return S_ONE; default: return S_ZERO; }
}
Src map_a_c(uint32_t v) {
    switch (v) { case 0: return S_LODFRAC; case 1: return S_TEX0_A; case 2: return S_TEX1_A; case 3: return S_PRIM_A; case 4: return S_SHADE_A;
                 case 5: return S_ENV_A; case 6: return S_PRIMLODFRAC; default: return S_ZERO; }
}

bool is_const(Src s) { return s == S_PRIM || s == S_ENV || s == S_ONE || s == S_ZERO || s == S_PRIM_A || s == S_ENV_A || s == S_LODFRAC || s == S_PRIMLODFRAC || s == S_HALF; }

// Which constant colour a source needs (0 none, 1 prim, 2 env, 3 white, 4 black, 5 lod, 6 primlod, 7 half).
int const_id(Src s) {
    switch (s) { case S_PRIM: case S_PRIM_A: return 1; case S_ENV: case S_ENV_A: return 2; case S_ONE: return 3; case S_ZERO: return 4;
                 case S_LODFRAC: return 5; case S_PRIMLODFRAC: return 6; case S_HALF: return 7; default: return 0; }
}

struct Op {                       // one channel's operation on up to 3 sources
    GPU_COMBINEFUNC func;
    Src s[3];
    int n;
};

// Plans one channel of one cycle into 1 or 2 ops.
int plan_channel(Src a, Src b, Src c, Src d, Op* ops) {
    if (c == S_ZERO || a == b) {                     // result = d
        ops[0] = { GPU_REPLACE, { d, d, d }, 1 };
        return 1;
    }
    if (b == S_ZERO) {
        if (d == S_ZERO) { ops[0] = { GPU_MODULATE, { a, c, c }, 2 }; return 1; }
        if (c == S_ONE) { ops[0] = { GPU_ADD, { a, d, d }, 2 }; return 1; }
        ops[0] = { GPU_MULTIPLY_ADD, { a, c, d }, 3 };
        return 1;
    }
    if (d == b) {                                    // (a - b) * c + b = lerp(b, a, c)
        ops[0] = { GPU_INTERPOLATE, { a, b, c }, 3 };
        return 1;
    }
    ops[0] = { GPU_SUBTRACT, { a, b, b }, 2 };
    if (d == S_ZERO) { ops[1] = { GPU_MODULATE, { S_COMBINED, c, c }, 2 }; }
    else { ops[1] = { GPU_MULTIPLY_ADD, { S_COMBINED, c, d }, 3 }; }
    return 2;
}

struct Consts { uint32_t prim, env, lod, primlod; };

uint32_t const_value(int id, const Consts& k) {
    switch (id) { case 1: return k.prim; case 2: return k.env; case 3: return 0xFFFFFFFFu; case 4: return 0x00000000u;
                  case 5: return k.lod; case 6: return k.primlod; case 7: return 0x80808080u; default: return 0; }
}

GPU_TEVSRC tev_src(Src s, bool& alpha_operand) {
    alpha_operand = false;
    switch (s) {
        case S_COMBINED: return GPU_PREVIOUS;
        case S_COMBINED_A: alpha_operand = true; return GPU_PREVIOUS;
        case S_TEX0: return GPU_TEXTURE0;
        case S_TEX0_A: alpha_operand = true; return GPU_TEXTURE0;
        case S_TEX1: return GPU_TEXTURE1;
        case S_TEX1_A: alpha_operand = true; return GPU_TEXTURE1;
        case S_SHADE: return GPU_PRIMARY_COLOR;
        case S_SHADE_A: alpha_operand = true; return GPU_PRIMARY_COLOR;
        case S_PRIM_A: case S_ENV_A: alpha_operand = true; return GPU_CONSTANT;
        default: return GPU_CONSTANT;
    }
}

// Fills a stage from an rgb op and an alpha op (either may be null =
// pass PREVIOUS through). Resolves the constant conflict by keeping the
// first constant and counting the rest as fallbacks.
void build_stage(TevStage& st, const Op* rgb, const Op* alpha, const Consts& k, int& fallbacks) {
    int cid = 0;
    auto choose = [&](Src s) {
        int c = const_id(s);
        if (c == 0) return;
        if (cid == 0) cid = c;
        else if (cid != c) {
            // white/black/half conflicts are tolerable substitutions; others are counted.
            if (!(c == 3 || c == 4 || c == 7 || cid == 3 || cid == 4 || cid == 7)) fallbacks++;
        }
    };
    if (rgb) for (int i = 0; i < rgb->n; i++) choose(rgb->s[i]);
    if (alpha) for (int i = 0; i < alpha->n; i++) choose(alpha->s[i]);
    // Prim/env alpha constants read the constant's alpha, which for the
    // shared constant is the same colour's alpha: prefer prim/env over
    // white/black so the alpha channel is right.
    st.constant = const_value(cid, k);
    // Rebuild the constant from the channels: rgb from the rgb constant and
    // alpha from the alpha constant when they differ.
    int rgb_c = 0, a_c = 0;
    if (rgb) for (int i = 0; i < rgb->n; i++) if (rgb_c == 0) rgb_c = const_id(rgb->s[i]);
    if (alpha) for (int i = 0; i < alpha->n; i++) if (a_c == 0) a_c = const_id(alpha->s[i]);
    uint32_t rgbv = const_value(rgb_c ? rgb_c : cid, k), av = const_value(a_c ? a_c : cid, k);
    st.constant = (rgbv & 0x00FFFFFFu) | (av & 0xFF000000u);

    for (int i = 0; i < 3; i++) {
        st.src_rgb[i] = GPU_PREVIOUS; st.op_rgb[i] = GPU_TEVOP_RGB_SRC_COLOR;
        st.src_a[i] = GPU_PREVIOUS; st.op_a[i] = GPU_TEVOP_A_SRC_ALPHA;
    }
    if (rgb) {
        st.func_rgb = rgb->func;
        for (int i = 0; i < rgb->n; i++) {
            bool ao;
            st.src_rgb[i] = tev_src(rgb->s[i], ao);
            st.op_rgb[i] = ao ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
        }
    }
    else st.func_rgb = GPU_REPLACE;
    if (alpha) {
        st.func_a = alpha->func;
        for (int i = 0; i < alpha->n; i++) {
            bool ao;
            st.src_a[i] = tev_src(alpha->s[i], ao);
            st.op_a[i] = GPU_TEVOP_A_SRC_ALPHA;
        }
    }
    else st.func_a = GPU_REPLACE;
}

}   // namespace

void plan_tev(const DrawRecord& d, TevPlan& plan) {
    plan.stages = 0;
    plan.fallbacks = 0;
    Consts k;
    k.prim = (uint32_t)d.prim[0] | ((uint32_t)d.prim[1] << 8) | ((uint32_t)d.prim[2] << 16) | ((uint32_t)d.prim[3] << 24);
    k.env = (uint32_t)d.env[0] | ((uint32_t)d.env[1] << 8) | ((uint32_t)d.env[2] << 16) | ((uint32_t)d.env[3] << 24);
    k.lod = 0x00000000u;
    uint32_t pl = d.prim_lod_frac;
    k.primlod = pl | (pl << 8) | (pl << 16) | (pl << 24);

    uint32_t cyc = (d.othermode_h >> 20) & 3;
    if (d.kind == DrawRecord::FillRect || cyc == 3) {
        // Fill: the fill colour (parked in prim).
        TevStage& st = plan.stage[plan.stages++];
        Op rgb = { GPU_REPLACE, { S_PRIM, S_PRIM, S_PRIM }, 1 }, a = { GPU_REPLACE, { S_PRIM_A, S_PRIM_A, S_PRIM_A }, 1 };
        build_stage(st, &rgb, &a, k, plan.fallbacks);
        return;
    }
    if (cyc == 2) {   // copy: texel straight through
        TevStage& st = plan.stage[plan.stages++];
        Op rgb = { GPU_REPLACE, { S_TEX0, S_TEX0, S_TEX0 }, 1 }, a = { GPU_REPLACE, { S_TEX0_A, S_TEX0_A, S_TEX0_A }, 1 };
        build_stage(st, &rgb, &a, k, plan.fallbacks);
        return;
    }

    uint32_t w0 = d.cc_w0, w1 = d.cc_w1;
    Src a0 = map_rgb_a((w0 >> 20) & 0xF), c0 = map_rgb_c((w0 >> 15) & 0x1F), Aa0 = map_a_abd((w0 >> 12) & 7), Ac0 = map_a_c((w0 >> 9) & 7);
    Src a1 = map_rgb_a((w0 >> 5) & 0xF), c1 = map_rgb_c(w0 & 0x1F);
    Src b0 = map_rgb_b((w1 >> 28) & 0xF), b1 = map_rgb_b((w1 >> 24) & 0xF);
    Src Aa1 = map_a_abd((w1 >> 21) & 7), Ac1 = map_a_c((w1 >> 18) & 7);
    Src d0 = map_rgb_d((w1 >> 15) & 7), Ab0 = map_a_abd((w1 >> 12) & 7), Ad0 = map_a_abd((w1 >> 9) & 7);
    Src d1 = map_rgb_d((w1 >> 6) & 7), Ab1 = map_a_abd((w1 >> 3) & 7), Ad1 = map_a_abd(w1 & 7);

    int cycles = (cyc == 1) ? 2 : 1;
    for (int cy = 0; cy < cycles; cy++) {
        Op rgb[2], alpha[2];
        int nr = plan_channel(cy ? a1 : a0, cy ? b1 : b0, cy ? c1 : c0, cy ? d1 : d0, rgb);
        int na = plan_channel(cy ? Aa1 : Aa0, cy ? Ab1 : Ab0, cy ? Ac1 : Ac0, cy ? Ad1 : Ad0, alpha);
        // In cycle 0 there is no PREVIOUS: a COMBINED input reads as zero on
        // real hardware; the PICA's stage-0 PREVIOUS is undefined, so map it
        // to the constant black.
        if (cy == 0) {
            for (int i = 0; i < nr; i++) for (int j = 0; j < rgb[i].n; j++) if (rgb[i].s[j] == S_COMBINED && i == 0) rgb[i].s[j] = S_ZERO;
            for (int i = 0; i < na; i++) for (int j = 0; j < alpha[i].n; j++) if (alpha[i].s[j] == S_COMBINED_A && i == 0) alpha[i].s[j] = S_ZERO;
        }
        int n = nr > na ? nr : na;
        for (int i = 0; i < n && plan.stages < 6; i++) {
            TevStage& st = plan.stage[plan.stages++];
            const Op* r = i < nr ? &rgb[i] : nullptr;
            const Op* a = i < na ? &alpha[i] : nullptr;
            // When one channel finishes early its later stages pass PREVIOUS.
            build_stage(st, r, a, k, plan.fallbacks);
        }
    }

    // Fog: the blender mixes the fog colour by the shade alpha (fog factor).
    if ((d.geometry_mode & 0x00010000) && cycles == 2 && ((d.othermode_l >> 30) & 3) == 3 && ((d.othermode_l >> 26) & 3) == 2 && plan.stages < 6) {
        TevStage& st = plan.stage[plan.stages++];
        Op rgb = { GPU_INTERPOLATE, { S_PRIM, S_COMBINED, S_SHADE_A }, 3 };   // src0 * src2 + src1 * (1 - src2)
        Consts kf = k;
        kf.prim = (uint32_t)d.fog[0] | ((uint32_t)d.fog[1] << 8) | ((uint32_t)d.fog[2] << 16) | ((uint32_t)d.fog[3] << 24);
        build_stage(st, &rgb, nullptr, kf, plan.fallbacks);
    }
    if (plan.stages == 0) {
        TevStage& st = plan.stage[plan.stages++];
        Op rgb = { GPU_REPLACE, { S_SHADE, S_SHADE, S_SHADE }, 1 }, a = { GPU_REPLACE, { S_SHADE_A, S_SHADE_A, S_SHADE_A }, 1 };
        build_stage(st, &rgb, &a, k, plan.fallbacks);
    }
}

void apply_tev(const TevPlan& plan) {
    for (int i = 0; i < 6; i++) {
        C3D_TexEnv* env = C3D_GetTexEnv(i);
        if (i < plan.stages) {
            const TevStage& st = plan.stage[i];
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_RGB, st.src_rgb[0], st.src_rgb[1], st.src_rgb[2]);
            C3D_TexEnvSrc(env, C3D_Alpha, st.src_a[0], st.src_a[1], st.src_a[2]);
            C3D_TexEnvOpRgb(env, st.op_rgb[0], st.op_rgb[1], st.op_rgb[2]);
            C3D_TexEnvOpAlpha(env, st.op_a[0], st.op_a[1], st.op_a[2]);
            C3D_TexEnvFunc(env, C3D_RGB, st.func_rgb);
            C3D_TexEnvFunc(env, C3D_Alpha, st.func_a);
            C3D_TexEnvColor(env, st.constant);
        }
        else {
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }
    }
}

}   // namespace rt64_3ds
