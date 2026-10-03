// naudio (n_aspMain) audio microcode on the CPU.
//
// The command semantics below were transcribed from the recompiled DK64
// microcode (rsp/n_aspMain.cpp in the game repository): every buffer address
// a command names is offset by 0x4F0 in DMEM, the resample table lives at
// DMEM 0xB0, the ADPCM codebook is loaded to 0x3F0, and the mixer state area
// is 0xFA0. The arithmetic mirrors the vector unit step for step (VMULF
// rounding, saturating adds, the 16.16 volume ramps) so the output matches
// the microcode bit for bit. Verified with tools/naudio_test on captured
// tasks: identical to the recompiled microcode (x86 SIMD and scalar builds)
// on every RDRAM range of 8 consecutive DK64 tasks. On the 3DS the recompiled
// (ARM, scalar) microcode itself deviates in rare envelope-init cases, so a
// lockstep run there can report a handful of one-LSB mismatches that are the
// reference's, not this code's.
//
// Dispatch table of this microcode (opcode -> handler):
//   0 NOP            1 ADPCM         2 CLEARBUFF   3 ENVMIXER
//   4 LOADBUFF       5 RESAMPLE      6 SAVEBUFF    7/8/12 MIXER
//   9 SETVOL        10 DMEMMOVE     11 LOADADPCM  13 INTERLEAVE
//  14 (writes the SETVOL rate slots)  15 SETLOOP
#include "naudio_hle.h"
#include "audio_hle_simd.h"
#include "audio_hle_diff.h"

#include <cstdio>
#include <cstring>

#include "librecomp/rsp.hpp"

namespace {

constexpr uint32_t kBufBase = 0x4F0;        // DMEM offset of every command buffer address
constexpr uint32_t kBook = 0x3F0;           // ADPCM codebook (LOADADPCM target)
constexpr uint32_t kState = 0xFA0;          // envmixer / resampler scratch and state
constexpr uint32_t kLut = 0xB0;             // resample coefficient table, 64 rows of 4
constexpr uint32_t kLoopAddr = 0xE;         // SETLOOP stores the loop state address here
constexpr uint32_t kCount = 0x170;          // bytes per fixed-size buffer (184 samples)
constexpr uint32_t kMainL = 0x9D0, kMainR = 0xB40, kAuxL = 0xCB0, kAuxR = 0xE20;

// DMEM, kept in the same byte-swapped layout as RDRAM (byte a at [a ^ 3]) so
// aligned DMA is a plain copy.
alignas(8) uint8_t g_dm[0x1000];
typedef int16_t __attribute__((may_alias)) dm_i16;
typedef uint32_t __attribute__((may_alias)) dm_u32;
namespace ahle = recomp3ds::ahle;

inline uint8_t db(uint32_t a) { return g_dm[(a & 0xFFF) ^ 3]; }
inline void dbw(uint32_t a, uint8_t v) { g_dm[(a & 0xFFF) ^ 3] = v; }
// Halfwords and words: one native access when aligned (a halfword at even
// address a sits at a ^ 2, a word at an aligned address as is), bytes
// otherwise (the RSP allows unaligned vector loads).
inline int16_t dh(uint32_t a) {
    if (__builtin_expect((a & 1) == 0, 1)) return *(const dm_i16*)(g_dm + ((a & 0xFFE) ^ 2));
    return (int16_t)((db(a) << 8) | db(a + 1));
}
inline void dhw(uint32_t a, int32_t v) {
    if (__builtin_expect((a & 1) == 0, 1)) { *(dm_i16*)(g_dm + ((a & 0xFFE) ^ 2)) = (int16_t)v; return; }
    dbw(a, (uint8_t)(v >> 8)); dbw(a + 1, (uint8_t)v);
}
inline uint32_t dw(uint32_t a) {
    if (__builtin_expect((a & 3) == 0, 1)) return *(const dm_u32*)(g_dm + (a & 0xFFC));
    return ((uint32_t)db(a) << 24) | ((uint32_t)db(a + 1) << 16) | ((uint32_t)db(a + 2) << 8) | db(a + 3);
}
inline void dww(uint32_t a, uint32_t v) {
    if (__builtin_expect((a & 3) == 0, 1)) { *(dm_u32*)(g_dm + (a & 0xFFC)) = v; return; }
    dbw(a, v >> 24); dbw(a + 1, v >> 16); dbw(a + 2, v >> 8); dbw(a + 3, v);
}

inline uint8_t rb(const uint8_t* rdram, uint32_t p) { return rdram[(p & 0xFFFFFF) ^ 3]; }
inline void rbw(uint8_t* rdram, uint32_t p, uint8_t v) { rdram[(p & 0xFFFFFF) ^ 3] = v; }

#if defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 6
inline int32_t sat16(int32_t v) { int32_t r; __asm__("ssat %0, #16, %1" : "=r"(r) : "r"(v)); return r; }
#else
inline int32_t sat16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }
#endif
inline int16_t clamp16(int32_t v) { return (int16_t)sat16(v); }

// Buffers as native halfword arrays. With both ends 4-byte aligned the
// XOR-2 halfword layout permutes every buffer the same way (raw index j is
// sample j ^ 1), so element-wise operations can run on the raw arrays.
inline dm_i16* dmraw(uint32_t a) { return (dm_i16*)(g_dm + (a & 0xFFF)); }
inline bool raw_ok(uint32_t a, uint32_t bytes) { return (a & 3) == 0 && (a & 0xFFF) + bytes <= 0x1000; }

// (2ab + 0x8000) >> 16, saturated: VMULF.
inline int16_t vmulf(int32_t a, int32_t b) { return clamp16((a * b + 0x4000) >> 15); }

// RSP DMA as librecomp implements it: an inclusive length, the RDRAM address
// aligned down to 8, DMEM wrapping at 4 KB.
void dma_read(const uint8_t* rdram, uint32_t dmem, uint32_t dram, uint32_t len) {
    dram &= 0xFFFFF8;
    if ((dmem & 3) == 0 && dmem + len <= 0x1000) {
        uint32_t whole = len & ~3u;
        memcpy(&g_dm[dmem], &rdram[dram & 0xFFFFFF], whole);
        for (uint32_t i = whole; i < len; i++) dbw(dmem + i, rb(rdram, dram + i));
        return;
    }
    for (uint32_t i = 0; i < len; i++) dbw(dmem + i, rb(rdram, dram + i));
}

void dma_write(uint8_t* rdram, uint32_t dmem, uint32_t dram, uint32_t len) {
    dram &= 0xFFFFF8;
    if ((dmem & 3) == 0 && dmem + len <= 0x1000) {
        uint32_t whole = len & ~3u;
        memcpy(&rdram[dram & 0xFFFFFF], &g_dm[dmem], whole);
        for (uint32_t i = whole; i < len; i++) rbw(rdram, dram + i, db(dmem + i));
        return;
    }
    for (uint32_t i = 0; i < len; i++) rbw(rdram, dram + i, db(dmem + i));
}

recomp3ds::AudioHleStats g_stats{};
RspUcodeFunc* g_reference = nullptr;
bool g_phase_invert = true;     // NaudioVariant::Dk64

// ---------------------------------------------------------------- commands

void cmd_clearbuff(uint32_t w0, uint32_t w1) {
    uint32_t a = (w0 & 0xFFFF) + kBufBase;
    int32_t n = w1 & 0xFFFF;
    uint32_t bytes = n <= 0 ? 16 : ((uint32_t)(n + 15) & ~15u);
    if (raw_ok(a, bytes)) {
        memset(g_dm + (a & 0xFFF), 0, bytes);
        return;
    }
    do {
        for (int i = 0; i < 16; i++) dbw(a + i, 0);
        a += 16;
        n -= 16;
    } while (n > 0);
}

void cmd_dmemmove(uint32_t w0, uint32_t w1) {
    uint32_t src = (w0 & 0xFFFF) + kBufBase;
    uint32_t dst = (w1 >> 16) + kBufBase;
    int32_t n = w1 & 0xFFFF;
    do {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = db(src + i);
        for (int i = 0; i < 16; i++) dbw(dst + i, t[i]);
        src += 16;
        dst += 16;
        n -= 16;
    } while (n > 0);
}

void cmd_loadbuff(const uint8_t* rdram, uint32_t w0, uint32_t w1) {
    uint32_t count = (w0 >> 12) & 0xFFF;
    if (count == 0) return;
    dma_read(rdram, (w0 & 0xFFF) + kBufBase, w1 & 0xFFFFFF, count);
}

void cmd_savebuff(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    uint32_t count = (w0 >> 12) & 0xFFF;
    if (count == 0) return;
    dma_write(rdram, (w0 & 0xFFF) + kBufBase, w1 & 0xFFFFFF, count);
}

void cmd_loadadpcm(const uint8_t* rdram, uint32_t w0, uint32_t w1) {
    dma_read(rdram, kBook, w1 & 0xFFFFFF, w0 & 0xFFFF);
}

void cmd_setloop(uint32_t w1) {
    dww(kLoopAddr, w1 & 0xFFFFFF);
}

// SETVOL parks its values in the state area, where ENVMIXER picks them up
// (and saves them with the rest of its state).
void cmd_setvol(uint32_t w0, uint32_t w1) {
    uint32_t flags = w0 >> 16;
    uint32_t base;
    if (flags & 4) {
        if (flags & 2) {            // A_LEFT | A_VOL: volume, dry, wet
            dhw(kState + 0x50, w0 & 0xFFFF);
            dhw(kState + 0x4C, w1 >> 16);
            dhw(kState + 0x4E, w1 & 0xFFFF);
            return;
        }
        base = kState + 0x46;       // A_RIGHT | A_VOL: right target, rate
    }
    else {
        base = kState + 0x40;       // A_RATE: left target, rate
    }
    dhw(base, w0 & 0xFFFF);
    dhw(base + 2, w1 >> 16);
    dhw(base + 4, w1 & 0xFFFF);
}

void cmd_interleave() {
    uint32_t l = kMainL, r = kMainR, o = kBufBase;
    for (int it = 0; it < 23; it++) {
        for (int k = 0; k < 8; k++) {
            dhw(o + 4 * k, dh(l + 2 * k));
            dhw(o + 4 * k + 2, dh(r + 2 * k));
        }
        l += 16;
        r += 16;
        o += 32;
    }
}

// out = out * 0x7FFF + in * gain, VMULF/VMACF rounding.
void cmd_mixer(uint32_t w0, uint32_t w1) {
    int32_t gain = (int16_t)(w0 & 0xFFFF);
    uint32_t out = (w1 & 0xFFFF) + kBufBase;
    uint32_t in = (w1 >> 16) + kBufBase;
    if (raw_ok(out, kCount) && raw_ok(in, kCount) && (in == out || in + kCount <= out || out + kCount <= in)) {
        ahle::mix_fixed((ahle::word_t*)dmraw(out), (const ahle::word_t*)dmraw(in), gain, kCount / 4);
        return;
    }
    for (uint32_t i = 0; i < kCount; i += 2) {
        int32_t acc = dh(out + i) * 0x7FFF + dh(in + i) * gain;
        dhw(out + i, clamp16((acc + 0x4000) >> 15));
    }
}

// VADPCM decode: 9-byte frames of 16 4-bit samples, order-2 prediction with
// an 8-entry cascade, exactly as the vector code evaluates it.
void cmd_adpcm(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    uint32_t state = w0 & 0xFFFFFF;
    uint32_t flags = w1 >> 28;
    int32_t count = (w1 >> 16) & 0xFFF;
    uint32_t hdr = ((w1 >> 12) & 0xF) + kBufBase;
    uint32_t out = (w1 & 0xFFF) + kBufBase;

    int32_t p2, p1;
    if (flags & 1) {
        for (int i = 0; i < 32; i++) dbw(out + i, 0);
        p2 = p1 = 0;
    }
    else {
        uint32_t src = (flags & 2) ? dw(kLoopAddr) : state;
        dma_read(rdram, out, src, 32);
        p2 = dh(out + 0x1C);
        p1 = dh(out + 0x1E);
    }
    out += 0x20;

    if (count != 0) {
        do {
            uint8_t h = db(hdr);
            uint32_t data = hdr + 1;
            int32_t scale = h >> 4;
            uint32_t book = kBook + ((h & 0xF) << 5);
            int32_t c1[8], c2[8];
            for (int i = 0; i < 8; i++) {
                c1[i] = dh(book + 2 * i);
                c2[i] = dh(book + 16 + 2 * i);
            }
            // nibble << 12 (sign in the top bit), then scaled; scales above
            // 12 stay at 12 because the multiply is skipped.
            int32_t mult = scale < 12 ? (1 << (scale + 4)) : 0;
            ahle::AdpcmBook bk;
            ahle::adpcm_book(bk, c1, c2);
            for (int g = 0; g < 2; g++) {
                int32_t s[8];
                for (int i = 0; i < 4; i++) {
                    uint8_t b = db(data + g * 4 + i);
                    s[2 * i] = (int16_t)((b & 0xF0) << 8);
                    s[2 * i + 1] = (int16_t)((b & 0x0F) << 12);
                }
                if (mult) {
                    for (int i = 0; i < 8; i++) s[i] = (s[i] * mult) >> 16;
                }
                int32_t o[8];
                ahle::adpcm_half(bk, s, p2, p1, o);
                for (int i = 0; i < 8; i++) dhw(out + 2 * i, o[i]);
                out += 16;
                p2 = o[6];
                p1 = o[7];
            }
            hdr += 9;
            count -= 32;
        } while (count > 0);
    }
    dma_write(rdram, out - 0x20, state, 32);
}

// 4-tap table resampler, 184 outputs, 16.16 position with the table row
// picked by the top 6 bits of the fraction.
void cmd_resample(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    uint32_t state = w0 & 0xFFFFFF;
    bool init = (w1 >> 30) != 0;
    uint32_t base = ((w1 >> 2) & 0xFFF) + kBufBase - 8;   // history precedes the input
    int32_t pitch = (w1 >> 14) & 0xFFFF;                    // unity = 0x8000
    uint32_t out = (w1 & 3) ? (kBufBase + 0x170) : kBufBase;

    uint32_t accum;
    if (init) {
        for (int i = 0; i < 8; i++) dbw(base + i, 0);
        accum = 0;
    }
    else {
        dma_read(rdram, kState, state, 16);
        for (int i = 0; i < 8; i++) dbw(base + i, db(kState + i));
        accum = (uint16_t)dh(kState + 8);
    }

    // Positions for the first block: accum + 2k * pitch.
    int32_t ipos[8];
    uint32_t frac[8];
    for (int k = 0; k < 8; k++) {
        uint32_t acc = accum + (uint32_t)(2 * k) * (uint32_t)pitch;
        ipos[k] = (int16_t)(acc >> 16);
        frac[k] = acc & 0xFFFF;
    }
    auto store_tables = [&]() {
        for (int k = 0; k < 8; k++) {
            dhw(kState + 0x10 + 2 * k, (int32_t)(base + 2 * ipos[k]));
            dhw(kState + 0x20 + 2 * k, (int32_t)(kLut + (frac[k] >> 10) * 8));
        }
    };
    // The microcode keeps the per-lane sample and table addresses in DMEM
    // (kState + 0x10 / 0x20) and reloads them each block; they are computed
    // here as it would read them (16-bit truncated) and stored once at the end.
    // Fast path: the input span and the coefficient table copied into plain
    // arrays, when the output does not overlap the input (the microcode
    // would otherwise read samples it has just written).
    const uint32_t last = (uint32_t)(accum + 2u * (uint32_t)pitch * 184u) >> 16;   // furthest start sample
    const uint32_t span = (last + 8) * 2;                                         // bytes from base
    const uint32_t out0 = out;
    // Exact check: block b reads samples [ipos(b, 0), ipos(b, 7) + 3] after
    // blocks 0..b-1 wrote [out0, out0 + 16 b). A read below that frontier
    // would see new output, which the copied input would not.
    bool overlap = false;
    {
        int32_t ip0 = ipos[0], ip7 = ipos[7];
        uint32_t f7 = frac[7];
        for (int b = 0; b < 23 && !overlap; b++) {
            uint32_t lo = base + 2 * (uint32_t)ip0, hi = base + 2 * ((uint32_t)ip7 + 4);
            uint32_t wlo = out0, whi = out0 + 16 * (uint32_t)b;
            if (lo < whi && hi > wlo) overlap = true;
            uint32_t pos7 = ((uint32_t)ip7 << 16) | f7;
            uint32_t a0 = pos7 + 2u * (uint32_t)pitch, a7 = pos7 + 16u * (uint32_t)pitch;
            ip0 = (int16_t)(a0 >> 16); ip7 = (int16_t)(a7 >> 16); f7 = a7 & 0xFFFF;
        }
    }
#ifdef NAUDIO_DEBUG_PATHS
    fprintf(stderr, "resample: base %03x span %u out %03x pitch %04x overlap %d\n", base, span, out0, pitch, (int)overlap);
#endif
    if (!overlap && span <= 0x1000 && ((base + span) & 0xFFFF) > base && base + span <= 0x1000 && pitch >= 0) {
        int16_t x[0x800];
        for (uint32_t i = 0; i < span / 2; i++) x[i] = dh(base + 2 * i);
        // Row r: the words (tap 1, tap 0) and (tap 3, tap 2) (raw order).
        const ahle::word_t* lutw = (const ahle::word_t*)dmraw(kLut);
        for (int block = 0; block < 23; block++) {
            int16_t o[8];
            for (int k = 0; k < 8; k++) {
                // Lane k reads samples ipos[k] .. +3 (the table stores base +
                // 2 * ipos truncated to 16 bits; ipos stays small here).
                const int16_t* xs = &x[(uint16_t)ipos[k]];
                const ahle::word_t* l = lutw + (frac[k] >> 10) * 2;
                o[k] = (int16_t)ahle::tap4(xs[0], xs[1], xs[2], xs[3], l[0], l[1]);
            }
            uint32_t pos7 = ((uint32_t)ipos[7] << 16) | frac[7];
            for (int k = 0; k < 8; k++) {
                uint32_t acc = pos7 + (uint32_t)(2 * (k + 1)) * (uint32_t)pitch;
                ipos[k] = (int16_t)(acc >> 16);
                frac[k] = acc & 0xFFFF;
            }
            for (int k = 0; k < 8; k++) dhw(out + 2 * k, o[k]);
            out += 16;
        }
        store_tables();
        uint32_t sa = (uint16_t)dh(kState + 0x10);
        for (int i = 0; i < 8; i++) dbw(kState + i, db(sa + i));
        dhw(kState + 8, (int32_t)frac[0]);
        dma_write(rdram, kState, state, 16);
        return;
    }

    for (int block = 0; block < 23; block++) {
        int16_t o[8];
        for (int k = 0; k < 8; k++) {
            uint32_t sa = (base + 2 * (uint32_t)ipos[k]) & 0xFFFF;
            uint32_t la = (kLut + (frac[k] >> 10) * 8) & 0xFFFF;
            int32_t t0 = vmulf(dh(sa), dh(la));
            int32_t t1 = vmulf(dh(sa + 2), dh(la + 2));
            int32_t t2 = vmulf(dh(sa + 4), dh(la + 4));
            int32_t t3 = vmulf(dh(sa + 6), dh(la + 6));
            o[k] = clamp16(clamp16(t0 + t1) + clamp16(t2 + t3));
        }
        // Next block: continue from lane 7's position.
        uint32_t pos7 = ((uint32_t)ipos[7] << 16) | frac[7];
        for (int k = 0; k < 8; k++) {
            uint32_t acc = pos7 + (uint32_t)(2 * (k + 1)) * (uint32_t)pitch;
            ipos[k] = (int16_t)(acc >> 16);
            frac[k] = acc & 0xFFFF;
        }
        for (int k = 0; k < 8; k++) dhw(out + 2 * k, o[k]);
        out += 16;
    }
    store_tables();

    // State: the 4 samples at the next position, then its fraction.
    uint32_t sa = (uint16_t)dh(kState + 0x10);
    for (int i = 0; i < 8; i++) dbw(kState + i, db(sa + i));
    dhw(kState + 8, (int32_t)frac[0]);
    dma_write(rdram, kState, state, 16);
}

// Envelope mixer: adds the 184 input samples into the four output busses
// with linear 16.16 volume ramps (per lane offsets within a block), dry and
// wet gains, and the phase-invert bit the game hides in the gains' LSBs.
void cmd_envmixer(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    uint32_t state = w1 & 0xFFFFFF;
    bool init = (w0 >> 16) & 1;
    int32_t cvolR = (int16_t)(w0 & 0xFFFF);

    if (!init) {
        dma_read(rdram, kState, state, 0x50);
    }
    // SETVOL block (part of the saved state on continue).
    int32_t ltgt = dh(kState + 0x40), lratm = dh(kState + 0x42);
    uint32_t lratl = (uint16_t)dh(kState + 0x44);
    int32_t rtgt = dh(kState + 0x46), rratm = dh(kState + 0x48);
    uint32_t rratl = (uint16_t)dh(kState + 0x4A);
    int32_t dry = dh(kState + 0x4C), wet = dh(kState + 0x4E);
    int32_t cvolL = dh(kState + 0x50);
    // DK64's build: v9, the dry LSB, inverts the left input; v8 ^ v9 ^ v9,
    // the wet LSB, the right one. Banjo's build inverts neither.
    int32_t maskL = (g_phase_invert && (dry & 1)) ? -1 : 0;
    int32_t maskR = (g_phase_invert && (wet & 1)) ? -1 : 0;

    static const uint32_t kFrac[8] = { 0x2000, 0x4000, 0x6000, 0x8000, 0xA000, 0xC000, 0xE000, 0xFFFF };

    int32_t lhi[8], rhi[8];
    uint32_t llo[8], rlo[8];
    uint32_t in = kBufBase, mainL = kMainL, mainR = kMainR, auxL = kAuxL, auxR = kAuxR;
    int blocks_done = 0;

    auto clampL = [&](int k) { lhi[k] = lratm < 0 ? (lhi[k] > ltgt ? lhi[k] : ltgt) : (lhi[k] < ltgt ? lhi[k] : ltgt); };
    auto clampR = [&](int k) { rhi[k] = rratm < 0 ? (rhi[k] > rtgt ? rhi[k] : rtgt) : (rhi[k] < rtgt ? rhi[k] : rtgt); };
    auto addL = [&](int k) {
        uint32_t lo = llo[k] + lratl;
        int32_t carry = lo >> 16;
        llo[k] = lo & 0xFFFF;
        lhi[k] = clamp16(lhi[k] + lratm + carry);
    };
    auto addR = [&](int k) {
        uint32_t lo = rlo[k] + rratl;
        int32_t carry = lo >> 16;
        rlo[k] = lo & 0xFFFF;
        rhi[k] = clamp16(rhi[k] + rratm + carry);
    };
    const bool raw = raw_ok(in, 16 * 23) && raw_ok(mainL, 16 * 23) && raw_ok(mainR, 16 * 23) && raw_ok(auxL, 16 * 23) && raw_ok(auxR, 16 * 23);
    auto apply = [&]() {
        if (raw) {
            // Raw halfword r of a block is lane r ^ 1; the mixing kernel
            // (audio_hle_simd.h) takes gain words in that order, and the
            // phase inversion (x ^ -1) applies to both halves of a word.
            int32_t gld[8], glw[8], grd[8], grw[8];
            for (int r = 0; r < 8; r++) {
                int k = r ^ 1;
                gld[r] = vmulf(lhi[k], dry); glw[r] = vmulf(lhi[k], wet);
                grd[r] = vmulf(rhi[k], dry); grw[r] = vmulf(rhi[k], wet);
            }
            uint32_t kld[4], klw[4], krd[4], krw[4];
            const bool uld = ahle::raw_gain_words(gld, kld), ulw = ahle::raw_gain_words(glw, klw);
            const bool urd = ahle::raw_gain_words(grd, krd), urw = ahle::raw_gain_words(grw, krw);
            const ahle::word_t* xw = (const ahle::word_t*)dmraw(in);
            uint32_t xl[4], xr[4];
            for (int m = 0; m < 4; m++) { xl[m] = xw[m] ^ (uint32_t)maskL; xr[m] = xw[m] ^ (uint32_t)maskR; }
            ahle::mix_block_any((ahle::word_t*)dmraw(mainL), xl, kld, uld);
            ahle::mix_block_any((ahle::word_t*)dmraw(auxL), xl, klw, ulw);
            ahle::mix_block_any((ahle::word_t*)dmraw(mainR), xr, krd, urd);
            ahle::mix_block_any((ahle::word_t*)dmraw(auxR), xr, krw, urw);
            in += 16; mainL += 16; mainR += 16; auxL += 16; auxR += 16;
            blocks_done++;
            return;
        }
        for (int k = 0; k < 8; k++) {
            int32_t x = dh(in + 2 * k);
            int32_t xl = x ^ maskL, xr = x ^ maskR;
            int32_t ldry = vmulf(lhi[k], dry), lwet = vmulf(lhi[k], wet);
            int32_t rdry = vmulf(rhi[k], dry), rwet = vmulf(rhi[k], wet);
            dhw(mainL + 2 * k, clamp16((dh(mainL + 2 * k) * 0x7FFF + xl * ldry + 0x4000) >> 15));
            dhw(auxL + 2 * k, clamp16((dh(auxL + 2 * k) * 0x7FFF + xl * lwet + 0x4000) >> 15));
            dhw(mainR + 2 * k, clamp16((dh(mainR + 2 * k) * 0x7FFF + xr * rdry + 0x4000) >> 15));
            dhw(auxR + 2 * k, clamp16((dh(auxR + 2 * k) * 0x7FFF + xr * rwet + 0x4000) >> 15));
        }
        in += 16; mainL += 16; mainR += 16; auxL += 16; auxR += 16;
        blocks_done++;
    };
    auto save_left = [&]() {
        for (int k = 0; k < 8; k++) {
            dhw(kState + 2 * k, lhi[k]);
            dhw(kState + 0x10 + 2 * k, (int32_t)llo[k]);
        }
    };

    if (init) {
        // acc = (frac * ratel) >> 16 + frac * ratem + cvol << 16, read back
        // through VMADH (high part saturated) and VMADN (low part forced to
        // 0 or 0xFFFF when the high part does not fit 16 bits).
        auto split = [](int64_t acc, int32_t& hi, uint32_t& lo) {
            int64_t hm = acc >> 16;
            hi = clamp16((int32_t)(hm > 0x7FFFFFFF ? 0x7FFFFFFF : (hm < -0x80000000LL ? -0x80000000LL : hm)));
            lo = hm > 32767 ? 0xFFFF : (hm < -32768 ? 0 : ((uint32_t)acc & 0xFFFF));
        };
        for (int k = 0; k < 8; k++) {
            int64_t acc = (int64_t)((kFrac[k] * lratl) >> 16) + (int64_t)kFrac[k] * lratm + ((int64_t)cvolL << 16);
            split(acc, lhi[k], llo[k]);
            clampL(k);
            acc = (int64_t)((kFrac[k] * rratl) >> 16) + (int64_t)kFrac[k] * rratm + ((int64_t)cvolR << 16);
            split(acc, rhi[k], rlo[k]);
            clampR(k);
        }
        apply();
    }
    else {
        for (int k = 0; k < 8; k++) {
            lhi[k] = dh(kState + 2 * k);
            llo[k] = (uint16_t)dh(kState + 0x10 + 2 * k);
            rhi[k] = dh(kState + 0x20 + 2 * k);
            rlo[k] = (uint16_t)dh(kState + 0x30 + 2 * k);
        }
    }
    // The left ramp state goes to DMEM after every block in the microcode;
    // nothing reads it back within the command, so once at the end.
    while (blocks_done < 23) {
        for (int k = 0; k < 8; k++) { addL(k); clampL(k); addR(k); clampR(k); }
        apply();
    }
    save_left();
    for (int k = 0; k < 8; k++) {
        dhw(kState + 0x20 + 2 * k, rhi[k]);
        dhw(kState + 0x30 + 2 * k, (int32_t)rlo[k]);
    }
    dma_write(rdram, kState, state, 0x50);
}

// -------------------------------------------------------------- task runner

void run_command(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    g_stats.commands++;
    switch (w0 >> 24) {
        case 0: break;
        case 1: cmd_adpcm(rdram, w0, w1); break;
        case 2: cmd_clearbuff(w0, w1); break;
        case 3: cmd_envmixer(rdram, w0, w1); break;
        case 4: cmd_loadbuff(rdram, w0, w1); break;
        case 5: cmd_resample(rdram, w0, w1); break;
        case 6: cmd_savebuff(rdram, w0, w1); break;
        case 7: case 8: case 12: cmd_mixer(w0, w1); break;
        case 9: cmd_setvol(w0, w1); break;
        case 10: cmd_dmemmove(w0, w1); break;
        case 11: cmd_loadadpcm(rdram, w0, w1); break;
        case 13: cmd_interleave(); break;
        case 14:
            // This microcode's table sends the pole filter to the tail of
            // SETVOL's rate branch: three halfwords into the state.
            dhw(kState + 0x40, w0 & 0xFFFF);
            dhw(kState + 0x42, w1 >> 16);
            dhw(kState + 0x44, w1 & 0xFFFF);
            break;
        case 15: cmd_setloop(w1); break;
        default: {
            // A command this interpreter lacks: count it, and name each
            // opcode once in the log.
            g_stats.unknown_opcodes++;
            static uint32_t seen[8] = {};
            const uint32_t op = w0 >> 24;
            if (!(seen[op >> 5] & (1u << (op & 31)))) {
                seen[op >> 5] |= 1u << (op & 31);
                fprintf(stderr, "naudio-hle: unknown command %u (%08lx %08lx)\n", (unsigned)op, (unsigned long)w0, (unsigned long)w1);
            }
            break;
        }
    }
}

inline uint32_t rw(const uint8_t* rdram, uint32_t p) {
    return ((uint32_t)rb(rdram, p) << 24) | ((uint32_t)rb(rdram, p + 1) << 16) | ((uint32_t)rb(rdram, p + 2) << 8) | rb(rdram, p + 3);
}

void run_list(uint8_t* rdram, uint32_t list, uint32_t bytes) {
    uint32_t n = bytes / 8;
    for (uint32_t i = 0; i < n; i++) {
        run_command(rdram, rw(rdram, list + i * 8), rw(rdram, list + i * 8 + 4));
    }
}

void run_task(uint8_t* rdram, const OSTask& task) {
    // Same DMEM preparation as librecomp's run_task: tables from the ucode
    // data, task struct at 0xFC0 (only its bytes matter for stale-state
    // fidelity, so copy it the same way).
    dma_read(rdram, 0, task.t.ucode_data, 0xF80);
    memcpy(&g_dm[0xFC0], &task, sizeof(OSTask));
    run_list(rdram, task.t.data_ptr & 0xFFFFFF, task.t.data_size);
    g_stats.tasks++;
}

// Differential testing (audio_hle_diff.cpp): the RDRAM range one command writes.
bool command_range(uint32_t w0, uint32_t w1, uint32_t* addr, uint32_t* len) {
    switch (w0 >> 24) {
        case 1: *addr = w0 & 0xFFFFF8; *len = 32; return true;
        case 3: *addr = w1 & 0xFFFFF8; *len = 0x50; return true;
        case 5: *addr = w0 & 0xFFFFF8; *len = 16; return true;
        case 6: {
            uint32_t count = (w0 >> 12) & 0xFFF;
            if (count == 0) return false;
            *addr = w1 & 0xFFFFF8; *len = count;
            return true;
        }
        default: return false;
    }
}

int g_diff_verbose = 0;     // mismatches still to dump in full (host tools)

// The command list chunk (0x2B0) and the task copy (0xFC0) differ from the
// reference's by construction: this interpreter reads the list from RDRAM.
bool dmem_ignored(uint32_t a) {
    return (a >= 0x2B0 && a < 0x4F0) || a >= 0xFC0;
}

// A DMEM mismatch in full: the volume block going in, and per output buffer
// how many samples differ and the first ones.
void dump_mismatch(uint32_t i, uint32_t w0, uint32_t w1, const uint8_t* pre_dm, const uint8_t* mine, const uint8_t* ref_dm) {
    auto h = [](const uint8_t* m, uint32_t a) { return (int16_t)((m[a ^ 3] << 8) | m[(a + 1) ^ 3]); };
    fprintf(stderr, "  cmd %u %08x %08x, state 40..50 before:", i, w0, w1);
    for (uint32_t a = 0x40; a <= 0x50; a += 2) fprintf(stderr, " %04x", (uint16_t)h(pre_dm, kState + a));
    fprintf(stderr, "\n");
    static const uint32_t bufs[4] = { kMainL, kMainR, kAuxL, kAuxR };
    static const char* names[4] = { "mainL", "mainR", "auxL", "auxR" };
    for (int b = 0; b < 4; b++) {
        int nd = 0, neg = 0;
        for (uint32_t k = 0; k < kCount; k += 2) {
            int16_t m = h(mine, bufs[b] + k), r = h(ref_dm, bufs[b] + k);
            if (m != r) { nd++; if (m == -r || m + r == -1) neg++; }
        }
        fprintf(stderr, "  %s: %d/%u differ (%d negated); mine/ref/before:", names[b], nd, kCount / 2, neg);
        for (uint32_t k = 0; k < 16; k += 2) fprintf(stderr, " %d/%d/%d", h(mine, bufs[b] + k), h(ref_dm, bufs[b] + k), h(pre_dm, bufs[b] + k));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  input:");
    for (uint32_t k = 0; k < 16; k += 2) fprintf(stderr, " %d", h(pre_dm, kBufBase + k));
    fprintf(stderr, "\n  state 00..50 after mine/ref:");
    for (uint32_t a = 0; a < 0x50; a += 2) if (h(mine, kState + a) != h(ref_dm, kState + a)) fprintf(stderr, " %02x:%04x/%04x", a, (uint16_t)h(mine, kState + a), (uint16_t)h(ref_dm, kState + a));
    fprintf(stderr, "\n");
}

recomp3ds::AudioHleFamily g_family = {
    "naudio-hle", g_dm, &g_stats, run_command, command_range, dmem_ignored, dump_mismatch, {},
};

}   // namespace

RspExitReason recomp3ds::naudio_hle_run(uint8_t* rdram, uint32_t ucode_addr) {
    OSTask task;
    memcpy(&task, &dmem[0xFC0], sizeof(OSTask));
    if (g_reference == nullptr) {
        run_task(rdram, task);
    }
    else {
        recomp3ds::audio_hle_lockstep(g_family, rdram, task, ucode_addr, g_reference, &g_diff_verbose);
    }
    return RspExitReason::Broke;
}

void recomp3ds::naudio_hle_set_reference(RspUcodeFunc* reference) {
    g_reference = reference;
}

void recomp3ds::naudio_hle_set_variant(NaudioVariant variant) {
    g_phase_invert = variant == NaudioVariant::Dk64;
}

void recomp3ds::naudio_hle_set_diff_verbose(int mismatches) {
    g_diff_verbose = mismatches;
}

const recomp3ds::AudioHleStats& recomp3ds::naudio_hle_stats() {
    return g_stats;
}

const uint8_t* recomp3ds::naudio_hle_dmem() {
    return g_dm;
}

void recomp3ds::naudio_hle_reset_dmem() {
    memset(g_dm, 0, sizeof(g_dm));
}
