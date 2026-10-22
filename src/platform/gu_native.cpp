// libultra's matrix helpers as host functions.
//
// A game lists them in its recompiler config's `ignored` (its 3DS copy;
// DK64's Makefile makes it): they are not recompiled, and the calls to
// them, from its code and its patches, resolve to these functions of the
// same names, native ARM code on the game's thread instead of recompiled
// MIPS: the recompiled versions do every floating-point operation through
// the context in memory, and DK64's attract demo spent 9 % of its game
// core in them.
//
// Each is libultra's own C (the DK64 decompilation's src/dk64_boot/gu), in
// the same operation order, with the same casts (trunc.w.s is a C cast in
// recomp.h too) and the same calls: sinf, cosf and sqrtf go to the game's
// recompiled libultra versions (renamed `*_recomp`), so the results are the
// recompiled functions' bit for bit. tools/gu_test checks that against the
// recompiled bodies on the host over random inputs.
//
// Only with RT64_3DS_GU_NATIVE (the game's CMake sets it beside the ignored
// list): defined here, the symbols satisfy the game's references before the
// linker reaches its archive, so its own recompiled versions are never
// linked, whatever their strength: Banjo booted to a black screen with
// weak definitions here, its guRotateF calling a sqrtf_recomp it does not
// have. A game that does not opt in gets no definitions from this file.
//
// Arguments as the MIPS o32 ABI passes them: a0-a3 in r4-r7, floats after
// a pointer argument in the integer registers as their bits, the rest on
// the stack from sp+0x10. RDRAM words are host-endian at the N64 address
// less 0x80000000 (recomp.h's MEM_W).
#include <cmath>
#include <cstdint>
#include <cstring>

#include "recomp.h"
#include "recomp3ds_internal.h"

#ifdef RT64_3DS_GU_NATIVE

// The game's recompiled libultra sinf, cosf and sqrtf under N64Recomp's
// renamed symbols (a game whose symbols name them otherwise cannot opt in).
extern "C" {
void sinf_recomp(uint8_t* rdram, recomp_context* ctx);
void cosf_recomp(uint8_t* rdram, recomp_context* ctx);
void sqrtf_recomp(uint8_t* rdram, recomp_context* ctx);
}

namespace {

inline uint8_t* at(uint8_t* rdram, uint32_t addr) { return rdram + (uint32_t)(addr - 0x80000000u); }
inline float ldf(uint8_t* rdram, uint32_t addr) { float v; memcpy(&v, at(rdram, addr), 4); return v; }
inline void stf(uint8_t* rdram, uint32_t addr, float v) { memcpy(at(rdram, addr), &v, 4); }
inline uint32_t ldw(uint8_t* rdram, uint32_t addr) { uint32_t v; memcpy(&v, at(rdram, addr), 4); return v; }
inline void stw(uint8_t* rdram, uint32_t addr, uint32_t v) { memcpy(at(rdram, addr), &v, 4); }
inline uint32_t arg(const recomp_context* ctx, int n) {
    switch (n) {
        case 0: return (uint32_t)ctx->r4;
        case 1: return (uint32_t)ctx->r5;
        case 2: return (uint32_t)ctx->r6;
        case 3: return (uint32_t)ctx->r7;
        default: return 0;
    }
}
// A float argument's bits: the register for the first four, the stack after.
inline float argf(uint8_t* rdram, const recomp_context* ctx, int n) {
    uint32_t bits = n < 4 ? arg(ctx, n) : ldw(rdram, (uint32_t)ctx->r29 + 4 * n);
    float v; memcpy(&v, &bits, 4); return v;
}
inline uint32_t argp(uint8_t* rdram, const recomp_context* ctx, int n) {
    return n < 4 ? arg(ctx, n) : ldw(rdram, (uint32_t)ctx->r29 + 4 * n);
}

// The game's libultra sinf, cosf and sqrtf (recompiled, renamed), on the
// caller's context: they clobber caller-saved registers, as the recompiled
// helpers did, and every argument is read before the first call.
inline float call_f(void (*f)(uint8_t*, recomp_context*), uint8_t* rdram, recomp_context* ctx, float x) {
    ctx->f12.fl = x;
    f(rdram, ctx);
    return ctx->f0.fl;
}

// Mtx: 16 words of integer parts, then 16 of fractions (s15.16 split).
void l2f(uint8_t* rdram, float mf[4][4], uint32_t m) {
    uint32_t ai = m, af = m + 32;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 2; j++) {
            const uint32_t wi = ldw(rdram, ai), wf = ldw(rdram, af);
            const uint32_t e1 = (wi & 0xffff0000u) | ((wf >> 16) & 0xffffu);
            const uint32_t e2 = ((wi << 16) & 0xffff0000u) | (wf & 0xffffu);
            const int32_t q1 = (int32_t)e1, q2 = (int32_t)e2;
            mf[i][j * 2] = (float)q1 * (1.0f / 65536.0f);        // FIX32TOF: cvt.s.w, then x 2^-16
            mf[i][j * 2 + 1] = (float)q2 * (1.0f / 65536.0f);
            ai += 4; af += 4;
        }
    }
}

void f2l(uint8_t* rdram, const float mf[4][4], uint32_t m) {
    uint32_t m1 = m, m2 = m + 32;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 2; c++) {
            const int32_t tmp1 = (int32_t)(mf[r][2 * c] * 65536.0f);        // FTOFIX32: trunc.w.s
            const int32_t tmp2 = (int32_t)(mf[r][2 * c + 1] * 65536.0f);
            stw(rdram, m1, ((uint32_t)tmp1 & 0xffff0000u) | (((uint32_t)(tmp2 >> 16)) & 0xffffu));
            stw(rdram, m2, (((uint32_t)tmp1 << 16) & 0xffff0000u) | ((uint32_t)tmp2 & 0xffffu));
            m1 += 4; m2 += 4;
        }
    }
}

inline void identf(float mf[4][4]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) mf[r][c] = r == c ? 1.0f : 0.0f;
}

inline void load(uint8_t* rdram, float mf[4][4], uint32_t p) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) mf[r][c] = ldf(rdram, p + (r * 4 + c) * 4);
}
inline void store(uint8_t* rdram, uint32_t p, const float mf[4][4]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) stf(rdram, p + (r * 4 + c) * 4, mf[r][c]);
}

inline void xfmf(const float mf[4][4], float x, float y, float z, float* ox, float* oy, float* oz) {
    *ox = mf[0][0] * x + mf[1][0] * y + mf[2][0] * z + mf[3][0];
    *oy = mf[0][1] * x + mf[1][1] * y + mf[2][1] * z + mf[3][1];
    *oz = mf[0][2] * x + mf[1][2] * y + mf[2][2] * z + mf[3][2];
}

}   // namespace

extern "C" {

void guMtxL2F(uint8_t* rdram, recomp_context* ctx) {
    float mf[4][4];
    l2f(rdram, mf, arg(ctx, 1));
    store(rdram, arg(ctx, 0), mf);
}

void guMtxF2L(uint8_t* rdram, recomp_context* ctx) {
    float mf[4][4];
    load(rdram, mf, arg(ctx, 0));
    f2l(rdram, mf, arg(ctx, 1));
}

void guMtxIdentF(uint8_t* rdram, recomp_context* ctx) {
    float mf[4][4];
    identf(mf);
    store(rdram, arg(ctx, 0), mf);
}

void guMtxIdent(uint8_t* rdram, recomp_context* ctx) {
    float mf[4][4];
    identf(mf);
    f2l(rdram, mf, arg(ctx, 0));
}

void guMtxCatF(uint8_t* rdram, recomp_context* ctx) {
    float mf[4][4], nf[4][4], temp[4][4];
    load(rdram, mf, arg(ctx, 0));
    load(rdram, nf, arg(ctx, 1));
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            temp[i][j] = 0.0f;
            for (int k = 0; k < 4; k++) temp[i][j] += mf[i][k] * nf[k][j];
        }
    }
    store(rdram, arg(ctx, 2), temp);        // res may be an input: written after
}

void guMtxXFMF(uint8_t* rdram, recomp_context* ctx) {
    float mf[4][4], ox, oy, oz;
    load(rdram, mf, arg(ctx, 0));
    xfmf(mf, argf(rdram, ctx, 1), argf(rdram, ctx, 2), argf(rdram, ctx, 3), &ox, &oy, &oz);
    stf(rdram, argp(rdram, ctx, 4), ox);
    stf(rdram, argp(rdram, ctx, 5), oy);
    stf(rdram, argp(rdram, ctx, 6), oz);
}

void guMtxXFML(uint8_t* rdram, recomp_context* ctx) {
    float mf[4][4], ox, oy, oz;
    l2f(rdram, mf, arg(ctx, 0));
    xfmf(mf, argf(rdram, ctx, 1), argf(rdram, ctx, 2), argf(rdram, ctx, 3), &ox, &oy, &oz);
    stf(rdram, argp(rdram, ctx, 4), ox);
    stf(rdram, argp(rdram, ctx, 5), oy);
    stf(rdram, argp(rdram, ctx, 6), oz);
}

void guNormalize(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t px = arg(ctx, 0), py = arg(ctx, 1), pz = arg(ctx, 2);
    float x = ldf(rdram, px), y = ldf(rdram, py), z = ldf(rdram, pz);
    const float m = 1.0f / call_f(sqrtf_recomp, rdram, ctx, x * x + y * y + z * z);
    stf(rdram, px, x * m);
    stf(rdram, py, y * m);
    stf(rdram, pz, z * m);
}

void guRotateF(uint8_t* rdram, recomp_context* ctx) {
    static const float dtor = (float)(3.1415926 / 180.0);
    const uint32_t p = arg(ctx, 0);
    float a = argf(rdram, ctx, 1), x = argf(rdram, ctx, 2), y = argf(rdram, ctx, 3), z = argf(rdram, ctx, 4);
    // guNormalize(&x, &y, &z)
    const float m = 1.0f / call_f(sqrtf_recomp, rdram, ctx, x * x + y * y + z * z);
    x *= m; y *= m; z *= m;
    a *= dtor;
    const float sine = call_f(sinf_recomp, rdram, ctx, a);
    const float cosine = call_f(cosf_recomp, rdram, ctx, a);
    float t = 1 - cosine;
    const float ab = x * y * t, bc = y * z * t, ca = z * x * t;
    float mf[4][4];
    identf(mf);
    t = x * x;
    mf[0][0] = t + cosine * (1 - t);
    mf[2][1] = bc - x * sine;
    mf[1][2] = bc + x * sine;
    t = y * y;
    mf[1][1] = t + cosine * (1 - t);
    mf[2][0] = ca + y * sine;
    mf[0][2] = ca - y * sine;
    t = z * z;
    mf[2][2] = t + cosine * (1 - t);
    mf[1][0] = ab - z * sine;
    mf[0][1] = ab + z * sine;
    store(rdram, p, mf);
}

void guTranslateF(uint8_t* rdram, recomp_context* ctx) {
    float mf[4][4];
    identf(mf);
    mf[3][0] = argf(rdram, ctx, 1);
    mf[3][1] = argf(rdram, ctx, 2);
    mf[3][2] = argf(rdram, ctx, 3);
    store(rdram, arg(ctx, 0), mf);
}

void guScaleF(uint8_t* rdram, recomp_context* ctx) {
    float mf[4][4];
    identf(mf);
    mf[0][0] = argf(rdram, ctx, 1);
    mf[1][1] = argf(rdram, ctx, 2);
    mf[2][2] = argf(rdram, ctx, 3);
    mf[3][3] = 1;
    store(rdram, arg(ctx, 0), mf);
}

}   // extern "C"

#endif   // RT64_3DS_GU_NATIVE

// Referenced from the run loop so this object is linked in: the game's
// recompiled code and patches are linked after this library, and nothing
// else here uses these functions.
int recomp3ds::gu_native_count() {
#ifdef RT64_3DS_GU_NATIVE
    return 11;
#else
    return 0;
#endif
}
