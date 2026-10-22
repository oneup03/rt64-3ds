// gu_test: the host functions in src/platform/gu_native.cpp against the
// recompiled libultra matrix helpers, bit for bit, over random inputs.
//
// The recompiled bodies come from a game's RecompiledFuncs (extracted by
// build.sh before the game's config ignores them: guMtx*, guRotateF,
// guTranslateF, guScaleF, guNormalize, and the renamed sinf, cosf and
// sqrtf), compiled here with recomp.h as on the 3DS, and called under the
// same name with a "ref_" prefix (build.sh renames them).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include <xmmintrin.h>
#include <pmmintrin.h>
#include "recomp.h"

extern "C" {
#define F(n) void n(uint8_t*, recomp_context*); void ref_##n(uint8_t*, recomp_context*);
F(guMtxL2F) F(guMtxF2L) F(guMtxIdentF) F(guMtxIdent) F(guMtxCatF) F(guMtxXFMF) F(guMtxXFML) F(guNormalize) F(guRotateF) F(guTranslateF) F(guScaleF)
#undef F
}

namespace {
constexpr uint32_t kRdram = 4 << 20;
constexpr uint32_t kBase = 0x80000000u;
uint8_t g_a[kRdram], g_b[kRdram];
std::mt19937 g_rng(12345);

inline void stw(uint8_t* rd, uint32_t a, uint32_t v) { memcpy(rd + (a - kBase), &v, 4); }
inline uint32_t ldw(const uint8_t* rd, uint32_t a) { uint32_t v; memcpy(&v, rd + (a - kBase), 4); return v; }
inline void stf(uint8_t* rd, uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); stw(rd, a, v); }
inline uint32_t bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }

float rnd(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }
// Values a game's matrices hold: positions to thousands, scales, rotations,
// the odd zero and negative zero.
float rnd_elem() {
    switch (g_rng() % 8) {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return rnd(-1.0f, 1.0f);
        case 3: return rnd(-8192.0f, 8192.0f);
        case 4: return rnd(-0.01f, 0.01f);
        default: return rnd(-100.0f, 100.0f);
    }
}
void fill_matf(uint8_t* rd, uint32_t p) { for (int i = 0; i < 16; i++) stf(rd, p + 4 * i, rnd_elem()); }
void fill_mtx(uint8_t* rd, uint32_t p) {
    // An Mtx made from a float matrix (F2L's layout), so the halves agree.
    for (int r = 0; r < 4; r++) for (int c = 0; c < 2; c++) {
        const int32_t t1 = (int32_t)(rnd_elem() * 65536.0f), t2 = (int32_t)(rnd_elem() * 65536.0f);
        stw(rd, p + (r * 2 + c) * 4, ((uint32_t)t1 & 0xffff0000u) | (((uint32_t)(t2 >> 16)) & 0xffffu));
        stw(rd, p + 32 + (r * 2 + c) * 4, (((uint32_t)t1 << 16) & 0xffff0000u) | ((uint32_t)t2 & 0xffffu));
    }
}

struct Case {
    const char* name;
    void (*host)(uint8_t*, recomp_context*);
    void (*ref)(uint8_t*, recomp_context*);
    void (*setup)(uint8_t* rd, recomp_context* ctx);
    std::vector<std::pair<uint32_t, uint32_t>> outputs;   // (address, bytes) compared
};

// Fixed addresses in the test RDRAM: inputs, outputs, a stack.
constexpr uint32_t A = 0x80200000, B = 0x80200100, C = 0x80200200, O = 0x80200300, SP = 0x80300000;

// The game's boot segment (ROM 0x1000 on, at the entry point 0x80000400):
// the recompiled bodies read their constants from it (guRotateF's dtor,
// sinf's and cosf's polynomial tables). RDRAM holds host-endian words.
std::vector<uint8_t> g_boot;
bool load_boot(const char* rom) {
    FILE* f = fopen(rom, "rb");
    if (f == nullptr) return false;
    fseek(f, 0x1000, SEEK_SET);
    std::vector<uint8_t> raw(1 << 20);
    const size_t n = fread(raw.data(), 1, raw.size(), f);
    fclose(f);
    g_boot.assign(n & ~3u, 0);
    for (size_t i = 0; i + 3 < n; i += 4) {
        const uint32_t w = ((uint32_t)raw[i] << 24) | ((uint32_t)raw[i + 1] << 16) | ((uint32_t)raw[i + 2] << 8) | raw[i + 3];
        memcpy(&g_boot[i], &w, 4);
    }
    return true;
}

int fails = 0, total = 0;
void run(const Case& cs, int n) {
    int bad = 0;
    for (int i = 0; i < n; i++) {
        memset(g_a, 0, sizeof(g_a));
        memcpy(g_a + 0x400, g_boot.data(), g_boot.size());
        recomp_context ctx = {};
        ctx.r29 = (gpr)(int32_t)SP;
        cs.setup(g_a, &ctx);
        memcpy(g_b, g_a, sizeof(g_a));
        recomp_context ca = ctx, cb = ctx;
        // MIPS I float mode, as the runtime starts the game: the odd
        // registers alias the even ones' high halves.
        ca.f_odd = &ca.f0.u32h;
        cb.f_odd = &cb.f0.u32h;
        cs.host(g_a, &ca);
        cs.ref(g_b, &cb);
        bool same = true;
        for (const auto& o : cs.outputs) {
            if (memcmp(g_a + (o.first - kBase), g_b + (o.first - kBase), o.second) != 0) same = false;
        }
        if (!same) {
            bad++;
            if (bad <= 3) {
                printf("  %s: case %d differs:", cs.name, i);
                for (const auto& o : cs.outputs) for (uint32_t k = 0; k < o.second; k += 4) {
                    const uint32_t x = ldw(g_a, o.first + k), y = ldw(g_b, o.first + k);
                    if (x != y) printf(" [%08x] host %08x ref %08x", o.first + k, x, y);
                }
                printf("\n");
            }
        }
    }
    total++;
    if (bad) fails++;
    printf("%-14s %s (%d of %d differ)\n", cs.name, bad ? "FAIL" : "ok", bad, n);
}

void args3f(uint8_t* rd, recomp_context* ctx, uint32_t p) {
    ctx->r4 = (gpr)(int32_t)p;
    ctx->r5 = (gpr)(int32_t)bits(rnd_elem());
    ctx->r6 = (gpr)(int32_t)bits(rnd_elem());
    ctx->r7 = (gpr)(int32_t)bits(rnd_elem());
    (void)rd;
}

}   // namespace

int main(int argc, char** argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 20000;
    if (argc < 3 || !load_boot(argv[2])) { fprintf(stderr, "usage: gu_test <cases> <rom.z64>\n"); return 1; }
    // The 3DS game threads run with flush-to-zero and default NaN (FPSCR
    // 0x03000000): the same here for both sides.
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    run({ "guMtxL2F", guMtxL2F, ref_guMtxL2F, [](uint8_t* rd, recomp_context* c) { fill_mtx(rd, B); c->r4 = (gpr)(int32_t)O; c->r5 = (gpr)(int32_t)B; }, { { O, 64 } } }, n);
    run({ "guMtxF2L", guMtxF2L, ref_guMtxF2L, [](uint8_t* rd, recomp_context* c) { fill_matf(rd, A); c->r4 = (gpr)(int32_t)A; c->r5 = (gpr)(int32_t)O; }, { { O, 64 } } }, n);
    run({ "guMtxIdentF", guMtxIdentF, ref_guMtxIdentF, [](uint8_t* rd, recomp_context* c) { fill_matf(rd, O); c->r4 = (gpr)(int32_t)O; }, { { O, 64 } } }, 4);
    run({ "guMtxIdent", guMtxIdent, ref_guMtxIdent, [](uint8_t* rd, recomp_context* c) { fill_matf(rd, O); c->r4 = (gpr)(int32_t)O; }, { { O, 64 } } }, 4);
    run({ "guMtxCatF", guMtxCatF, ref_guMtxCatF, [](uint8_t* rd, recomp_context* c) { fill_matf(rd, A); fill_matf(rd, B); c->r4 = (gpr)(int32_t)A; c->r5 = (gpr)(int32_t)B; c->r6 = (gpr)(int32_t)O; }, { { O, 64 } } }, n);
    // res as an input
    run({ "guMtxCatF(in)", guMtxCatF, ref_guMtxCatF, [](uint8_t* rd, recomp_context* c) { fill_matf(rd, A); fill_matf(rd, B); c->r4 = (gpr)(int32_t)A; c->r5 = (gpr)(int32_t)B; c->r6 = (gpr)(int32_t)A; }, { { A, 64 } } }, n);
    run({ "guMtxXFMF", guMtxXFMF, ref_guMtxXFMF, [](uint8_t* rd, recomp_context* c) { fill_matf(rd, A); args3f(rd, c, A); stw(rd, SP + 0x10, O); stw(rd, SP + 0x14, O + 4); stw(rd, SP + 0x18, O + 8); }, { { O, 12 } } }, n);
    run({ "guMtxXFML", guMtxXFML, ref_guMtxXFML, [](uint8_t* rd, recomp_context* c) { fill_mtx(rd, B); args3f(rd, c, B); stw(rd, SP + 0x10, O); stw(rd, SP + 0x14, O + 4); stw(rd, SP + 0x18, O + 8); }, { { O, 12 } } }, n);
    run({ "guNormalize", guNormalize, ref_guNormalize, [](uint8_t* rd, recomp_context* c) { stf(rd, O, rnd(-100, 100)); stf(rd, O + 4, rnd(-100, 100)); stf(rd, O + 8, rnd(-100, 100)); c->r4 = (gpr)(int32_t)O; c->r5 = (gpr)(int32_t)(O + 4); c->r6 = (gpr)(int32_t)(O + 8); }, { { O, 12 } } }, n);
    run({ "guRotateF", guRotateF, ref_guRotateF, [](uint8_t* rd, recomp_context* c) {
              c->r4 = (gpr)(int32_t)O; c->r5 = (gpr)(int32_t)bits(rnd(-720.0f, 720.0f));
              float x = rnd(-1, 1), y = rnd(-1, 1), z = rnd(-1, 1);
              if (g_rng() % 4 == 0) { x = 0; y = 1; z = 0; }          // the axes the games use
              c->r6 = (gpr)(int32_t)bits(x); c->r7 = (gpr)(int32_t)bits(y); stw(rd, SP + 0x10, bits(z)); }, { { O, 64 } } }, n);
    run({ "guTranslateF", guTranslateF, ref_guTranslateF, [](uint8_t* rd, recomp_context* c) { args3f(rd, c, O); }, { { O, 64 } } }, n);
    run({ "guScaleF", guScaleF, ref_guScaleF, [](uint8_t* rd, recomp_context* c) { args3f(rd, c, O); }, { { O, 64 } } }, n);
    printf("%d of %d functions match\n", total - fails, total);
    return fails == 0 ? 0 : 2;
}
