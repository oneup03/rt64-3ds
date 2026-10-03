// libaudio's ABI 1 audio microcode (aspMain) on the CPU.
//
// Transcribed from the recompiled microcode of Mystical Ninja Starring
// Goemon (rsp/aspMain.cpp in the game repository, text at 0x04001080, its
// DMEM data from ROM 0x7E960). The main loop reads the command list in
// chunks of 0x140 bytes into DMEM 0x380 and dispatches on (w0 >> 24) & 0x7F
// through the table at DMEM 0x10:
//
//   0 SPNOOP      1 ADPCM       2 CLEARBUFF   3 ENVMIXER
//   4 LOADBUFF    5 RESAMPLE    6 SAVEBUFF    7 SEGMENT
//   8 SETBUFF     9 SETVOL     10 DMEMMOVE   11 LOADADPCM
//  12 MIXER      13 INTERLEAVE 14 POLEF      15 SETLOOP
//
// DMEM: 0x000 constants, the dispatch table, the ADPCM nibble masks, the
// envelope ramp steps (0xB0) and the resample table (0xC0, 64 rows of 4
// taps); 0x320 the segment table (only segment 0 is cleared per task: the
// microcode's clearing loop never advances); 0x360 the SETBUFF/SETVOL block
// (in, out, count, left/right volume, aux buffers, then the envelope
// targets, rates, dry and wet gains, whose first word SETLOOP overwrites with
// the ADPCM loop address); 0x380 the list chunk; 0x4C0 the ADPCM codebook;
// buffers at 0x5C0 + the command's offset; 0xF90 the per-command state that
// RESAMPLE, ENVMIXER and POLEF move to and from RDRAM (ENVMIXER's spills
// into the task copy at 0xFC0, as does its aux output without A_AUX at
// 0xFE0). Some commands read what earlier ones (or tasks) left in DMEM, so
// all of it is kept exactly as the microcode leaves it.
//
// The arithmetic mirrors the vector unit: VMULF/VMACF rounding and
// saturation, the 32-bit wrap of VMADH sums, VMADN's saturated low word,
// VCL (an unsigned minimum here) and VGE as the envelope clamps, LRV's
// shifted coefficient vectors, and LQV/SQV moving only up to the next
// 16-byte boundary. Checked with tools/naudio_test (HLE=aspmain) against the
// x86 build of the recompiled microcode, command by command and on whole
// tasks.
#include "aspmain_hle.h"
#include "audio_hle_simd.h"

#include <cstdio>
#include <cstring>

#include "librecomp/rsp.hpp"
#ifdef __3DS__
#include <3ds.h>
#else
#include <ctime>
#endif

namespace {

constexpr uint32_t kBuf = 0x5C0;            // every buffer offset in a command is relative to this
constexpr uint32_t kRamp = 0xB0;            // ENVMIXER's per-lane ramp positions (k + 1) / 8
constexpr uint32_t kLut = 0xC0;             // RESAMPLE's table, 64 rows of 4 taps
constexpr uint32_t kSeg = 0x320;            // segment table, a word per segment
constexpr uint32_t kIn = 0x360, kOut = 0x362, kCount = 0x364;      // SETBUFF
constexpr uint32_t kVolL = 0x366, kVolR = 0x368;                    // SETVOL A_VOL
constexpr uint32_t kDryR = 0x36A, kWetL = 0x36C, kWetR = 0x36E;     // SETBUFF A_AUX
constexpr uint32_t kEnv = 0x370;            // SETVOL: L target, rate hi, rate lo; R ...; dry; wet
constexpr uint32_t kLoop = 0x370;           // SETLOOP's word, over the left target and rate
constexpr uint32_t kList = 0x380, kChunk = 0x140;
constexpr uint32_t kBook = 0x4C0;           // LOADADPCM's target: codebook, POLEF coefficients
constexpr uint32_t kState = 0xF90;          // per-command state moved to and from RDRAM
constexpr uint32_t kNoAux = 0xFE0;          // ENVMIXER's aux outputs without A_AUX

// DMEM, kept in the same byte-swapped layout as RDRAM (byte a at [a ^ 3]) so
// aligned DMA is a plain copy.
alignas(8) uint8_t g_dm[0x1000];
typedef int16_t __attribute__((may_alias)) dm_i16;
typedef uint32_t __attribute__((may_alias)) dm_u32;
namespace ahle = recomp3ds::ahle;

inline uint8_t db(uint32_t a) { return g_dm[(a & 0xFFF) ^ 3]; }
inline void dbw(uint32_t a, uint8_t v) { g_dm[(a & 0xFFF) ^ 3] = v; }
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
inline uint16_t dhu(uint32_t a) { return (uint16_t)dh(a); }

#if defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 6
inline int32_t sat16(int32_t v) { int32_t r; __asm__("ssat %0, #16, %1" : "=r"(r) : "r"(v)); return r; }
#else
inline int32_t sat16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }
#endif
inline bool fits16(int32_t v) { return v >= -32768 && v <= 32767; }

// VMULF: (2ab + 0x8000) >> 16, saturated.
inline int32_t vmulf(int32_t a, int32_t b) { return sat16((a * b + 0x4000) >> 15); }
// VMULF by 0x7FFF, then VMACF of x * g: what every mixing step does.
inline int32_t mix(int32_t o, int32_t x, int32_t g) { return sat16((o * 0x7FFF + x * g + 0x4000) >> 15); }
// The low word VMADN returns: the accumulator's, unless its upper 32 bits
// leave the 16-bit range, then 0 or 0xFFFF.
inline uint32_t sat_lo(int32_t hi, uint32_t lo) { return fits16(hi) ? (lo & 0xFFFF) : (hi < 0 ? 0 : 0xFFFF); }

// Buffers as native halfword arrays. With both ends 4-byte aligned the
// XOR-2 halfword layout permutes every buffer the same way (raw index j is
// sample j ^ 1), so element-wise operations can run on the raw arrays.
inline dm_i16* dmraw(uint32_t a) { return (dm_i16*)(g_dm + (a & 0xFFF)); }
// [a, a + bytes) aligned to `align`, inside DMEM without wrapping.
inline bool flat(uint32_t a, uint32_t bytes, uint32_t align) { return (a & (align - 1)) == 0 && a + bytes <= 0x1000; }
inline bool disjoint(uint32_t a, uint32_t b, uint32_t bytes) { return a + bytes <= b || b + bytes <= a; }

void dma_read(const uint8_t* rdram, uint32_t dmem, uint32_t dram, uint32_t len) {
    recomp3ds::audio_hle_dma_read(g_dm, rdram, dmem, dram, len);
}
void dma_write(uint8_t* rdram, uint32_t dmem, uint32_t dram, uint32_t len) {
    recomp3ds::audio_hle_dma_write(g_dm, rdram, dmem, dram, len);
}

// A vector register for the paths that follow the microcode's loads and
// stores exactly (unaligned or overlapping buffers): byte i of the register
// is the i-th byte in memory order, lane k bytes 2k and 2k + 1.
struct VReg {
    uint8_t b[16];
    int32_t lane(int k) const { return (int16_t)((b[2 * k] << 8) | b[2 * k + 1]); }
    void set(int k, int32_t v) { b[2 * k] = (uint8_t)(v >> 8); b[2 * k + 1] = (uint8_t)v; }
};
// LQV/SQV: up to the next 16-byte boundary only.
inline void lqv(VReg& v, uint32_t a) { for (uint32_t i = 0, n = 16 - (a & 15); i < n; i++) v.b[i] = db(a + i); }
inline void sqv(const VReg& v, uint32_t a) { for (uint32_t i = 0, n = 16 - (a & 15); i < n; i++) dbw(a + i, v.b[i]); }

recomp3ds::AudioHleStats g_stats{};
// Low quality (aspmain_hle_set_low_quality): half-rate sub-frames.
bool g_low = false;
bool g_nofx = false;                // the reverb left out at full rate (host tools: HLE_NOFX)
// The main buses' DMEM addresses, as the last INTERLEAVE named them (libaudio
// keeps them fixed; these are Goemon's until one is seen). In low quality the
// buses hold half-rate samples from their start, and a voice that libaudio
// mixes in part of the way through a sub-frame (a note starting, a volume
// change) writes at its offset into the bus: that offset is halved too.
uint32_t g_bus_l = 0x5C0 + 0x440, g_bus_r = 0x5C0 + 0x580;
inline uint32_t half_rate_addr(uint32_t a, uint32_t bus) {
    const uint32_t off = (a - bus) & 0xFFF;
    return off < 0x200 ? bus + ((off >> 1) & ~1u) : a;
}
bool g_in_fx = false;               // between a sub-frame's first MIXER and its INTERLEAVE: the reverb
int32_t g_up_l = 0, g_up_r = 0;     // the last half-rate output frame, for the upsampler
RspUcodeFunc* g_reference = nullptr;
int g_diff_verbose = 0;

// DMEM 0x000-0x0BF as Goemon's aspMain data loads it (ROM 0x7E960): the
// vector constants, the dispatch table, the ADPCM nibble masks and shifts,
// the resampler's and the envelope mixer's lane tables. The commands below
// build these numbers in; the resample table that follows is read from DMEM.
const uint8_t kConstants[0xC0] = {
    0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0xFF, 0xFF, 0x00, 0x20, 0x08, 0x00, 0x7F, 0xFF, 0x40, 0x00,
    0x11, 0x18, 0x14, 0x70, 0x11, 0xDC, 0x1B, 0x38, 0x12, 0x14, 0x18, 0x7C, 0x12, 0x54, 0x12, 0xD0,
    0x12, 0xEC, 0x13, 0x28, 0x14, 0x0C, 0x12, 0x94, 0x1E, 0x24, 0x13, 0x8C, 0x17, 0x0C, 0x14, 0x4C,
    0xF0, 0x00, 0x0F, 0x00, 0x00, 0xF0, 0x00, 0x0F, 0x00, 0x01, 0x00, 0x10, 0x01, 0x00, 0x10, 0x00,
    0x00, 0x02, 0x00, 0x04, 0x00, 0x06, 0x00, 0x08, 0x00, 0x0A, 0x00, 0x0C, 0x00, 0x0E, 0x00, 0x10,
    0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x04, 0x00, 0x08, 0x00, 0x10, 0x01, 0x00, 0x02, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x20, 0x00, 0x40, 0x00, 0x60, 0x00, 0x80, 0x00, 0xA0, 0x00, 0xC0, 0x00, 0xE0, 0x00, 0xFF, 0xFF,
};
// The same in DMEM's byte order, for a plain compare.
struct ConstantsRaw {
    alignas(4) uint8_t b[0xC0];
    ConstantsRaw() { for (uint32_t a = 0; a < 0xC0; a++) b[a ^ 3] = kConstants[a]; }
};
const ConstantsRaw g_constants_raw;

// A command whose result depends on vector lanes earlier commands left
// behind (a partial LQV of an unaligned buffer): count it, name it once.
void note_inexact(uint32_t op, uint32_t addr) {
    g_stats.inexact++;
    static uint32_t seen = 0;
    if (!(seen & (1u << op))) {
        seen |= 1u << op;
        fprintf(stderr, "aspmain-hle: command %u reads an unaligned buffer (%03x); lanes from earlier commands are not tracked\n",
                (unsigned)op, (unsigned)(addr & 0xFFF));
    }
}

// Before a command that uses the constants: still as built in? Another
// game's build of the microcode, or a command that wrote over them (the
// microcode would then compute with the damaged values), makes the result
// inexact.
void check_constants(uint32_t op) {
    const dm_u32* a = (const dm_u32*)g_dm;
    const dm_u32* b = (const dm_u32*)g_constants_raw.b;
    uint32_t diff = 0;
    for (int i = 0; i < 0xC0 / 4; i++) diff |= a[i] ^ b[i];
    if (__builtin_expect(diff == 0, 1)) return;
    g_stats.inexact++;
    static bool said = false;
    if (!said) {
        said = true;
        fprintf(stderr, "aspmain-hle: DMEM 0x000-0x0BF differs from the constants this interpreter assumes (command %u)\n", (unsigned)op);
    }
}

// The RDRAM address of a command's w1: offset + segment base. The segment
// number is the whole top byte, as the microcode indexes its table.
inline uint32_t seg_addr(uint32_t w1) {
    return (w1 & 0xFFFFFF) + dw(kSeg + (w1 >> 24) * 4);
}

// ---------------------------------------------------------------- commands

void cmd_clearbuff(uint32_t w0, uint32_t w1) {
    const uint32_t count = w1 & 0xFFFF;
    if (count == 0) return;
    const uint32_t a = ((w0 & 0xFFFF) + kBuf) & 0xFFF;
    const uint32_t bytes = (count + 15) & ~15u;
    if (flat(a, bytes, 4)) { memset(g_dm + a, 0, bytes); return; }
    for (uint32_t i = 0; i < bytes; i++) dbw(a + i, 0);
}

// 16 bytes at a time, each read before it is written.
void cmd_dmemmove(uint32_t w0, uint32_t w1) {
    const uint32_t count = w1 & 0xFFFF;
    if (count == 0) return;
    const uint32_t src = ((w0 & 0xFFFF) + kBuf) & 0xFFF, dst = ((w1 >> 16) + kBuf) & 0xFFF;
    const uint32_t bytes = (count + 15) & ~15u;
    if (flat(src, bytes, 4) && flat(dst, bytes, 4) && (dst <= src || dst >= src + bytes)) {
        memmove(g_dm + dst, g_dm + src, bytes);
        return;
    }
    for (uint32_t off = 0; off < bytes; off += 16) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = db(src + off + i);
        for (int i = 0; i < 16; i++) dbw(dst + off + i, t[i]);
    }
}

void cmd_loadbuff(const uint8_t* rdram, uint32_t w1) {
    const uint32_t count = dhu(kCount);
    if (count == 0) return;
    dma_read(rdram, dhu(kIn), seg_addr(w1), count);
}

void cmd_savebuff(uint8_t* rdram, uint32_t w1) {
    const uint32_t count = dhu(kCount);
    if (count == 0) return;
    dma_write(rdram, dhu(kOut), seg_addr(w1), count);
}

// Low quality: what the reverb would save to its delay lines, as silence.
void cmd_savebuff_silence(uint8_t* rdram, uint32_t w1) {
    const uint32_t count = dhu(kCount);
    if (count == 0) return;
    const uint32_t a = seg_addr(w1) & 0xFFFFF8;
    if (a + count <= 0x1000000) memset(&rdram[a], 0, count);
    else for (uint32_t i = 0; i < count; i++) rdram[((a + i) & 0xFFFFFF) ^ 3] = 0;      // wraps at 16 MB, as the DMA would
}

void cmd_segment(uint32_t w1) {
    dww(kSeg + (w1 >> 24) * 4, w1 & 0xFFFFFF);
}

void cmd_setbuff(uint32_t w0, uint32_t w1) {
    const uint32_t a = w0 + kBuf, b = (w1 >> 16) + kBuf, c = w1 + kBuf;
    if ((w0 >> 16) & 8) {           // A_AUX: dry right, wet left, wet right
        dhw(kWetR, c);
        dhw(kDryR, a);
        dhw(kWetL, b);
    }
    else {                          // in, out, count (the count without the base)
        dhw(kIn, a);
        dhw(kOut, b);
        dhw(kCount, w1);
    }
}

void cmd_setvol(uint32_t w0, uint32_t w1) {
    const uint32_t f = w0 >> 16;
    if (f & 8) {                    // A_AUX: dry and wet gains
        dhw(kEnv + 0xC, w0);
        dhw(kEnv + 0xE, w1);
    }
    else if (f & 4) {               // A_VOL: the starting volume
        dhw((f & 2) ? kVolL : kVolR, w0);
    }
    else {                          // A_RATE: target, rate (16.16)
        const uint32_t base = (f & 2) ? kEnv : kEnv + 6;
        dhw(base, w0);
        dhw(base + 2, w1 >> 16);
        dhw(base + 4, w1);
    }
}

void cmd_loadadpcm(const uint8_t* rdram, uint32_t w0, uint32_t w1) {
    dma_read(rdram, kBook, seg_addr(w1), w0 & 0xFFFF);
}

void cmd_setloop(uint32_t w1) {
    dww(kLoop, seg_addr(w1));
}

void cmd_interleave(uint32_t w1) {
    const uint32_t count = dhu(kCount);
    uint32_t out = dhu(kOut);
    if (count == 0) return;
    uint32_t l = (w1 >> 16) + kBuf, r = (w1 & 0xFFFF) + kBuf;
    g_bus_l = l & 0xFFF;
    g_bus_r = r & 0xFFF;
    const uint32_t blocks = (count + 15) / 16;
    const uint32_t lo = l & 0xFFF, ro = r & 0xFFF, oo = out & 0xFFF;
    if (flat(lo, blocks * 16, 16) && flat(ro, blocks * 16, 16) && flat(oo, blocks * 32, 2) &&
        (oo + blocks * 32 <= lo || lo + blocks * 16 <= oo) && (oo + blocks * 32 <= ro || ro + blocks * 16 <= oo)) {
        if (g_low) {
            // The buses hold half as many frames at half the rate: each one
            // goes out twice, the first time averaged with the one before
            // (linear interpolation, half a frame late).
            int32_t pl = g_up_l, pr = g_up_r;
            for (uint32_t j = 0; j < blocks * 4; j++) {
                const int32_t cl = dh(lo + 2 * j), cr = dh(ro + 2 * j);
                dhw(oo + 8 * j, (pl + cl) >> 1);
                dhw(oo + 8 * j + 2, (pr + cr) >> 1);
                dhw(oo + 8 * j + 4, cl);
                dhw(oo + 8 * j + 6, cr);
                pl = cl; pr = cr;
            }
            g_up_l = pl; g_up_r = pr;
            return;
        }
        for (uint32_t j = 0; j < blocks * 8; j++) {
            dhw(oo + 4 * j, dh(lo + 2 * j));
            dhw(oo + 4 * j + 2, dh(ro + 2 * j));
        }
        return;
    }
    // Per block: both loads, then sixteen SSVs.
    VReg v1{}, v2{};
    if ((l | r) & 15) note_inexact(13, (l & 15) ? l : r);
    for (uint32_t b = 0; b < blocks; b++) {
        lqv(v1, l);
        lqv(v2, r);
        for (int k = 0; k < 8; k++) {
            dbw(out + 4 * k, v1.b[2 * k]); dbw(out + 4 * k + 1, v1.b[2 * k + 1]);
            dbw(out + 4 * k + 2, v2.b[2 * k]); dbw(out + 4 * k + 3, v2.b[2 * k + 1]);
        }
        l += 16; r += 16; out += 32;
    }
}

// out = out * 0x7FFF + in * gain, 32 bytes per pass.
void cmd_mixer(uint32_t w0, uint32_t w1) {
    const uint32_t count = dhu(kCount);
    if (count == 0) return;
    uint32_t out = (w1 & 0xFFFF) + kBuf, in = (w1 >> 16) + kBuf;
    const int32_t gain = (int16_t)(w0 & 0xFFFF);
    const uint32_t bytes = (count + 31) & ~31u;
    const uint32_t oo = out & 0xFFF, io = in & 0xFFF;
    if (flat(oo, bytes, 16) && flat(io, bytes, 16) && (oo == io || disjoint(oo, io, bytes))) {
        ahle::mix_fixed((ahle::word_t*)(g_dm + oo), (const ahle::word_t*)(g_dm + io), gain, bytes / 4);
        return;
    }
    // The microcode's own order: the next input is loaded between the two
    // halves' stores, the next output after them.
    if ((out | in) & 15) note_inexact(12, (out & 15) ? out : in);
    VReg v27{}, v29{}, v26{}, v28{};
    lqv(v27, out); lqv(v29, in); lqv(v26, out + 16); lqv(v28, in + 16);
    int32_t left = (int32_t)count;
    do {
        for (int k = 0; k < 8; k++) v27.set(k, mix(v27.lane(k), v29.lane(k), gain));
        left -= 32;
        in += 32;
        sqv(v27, out);
        for (int k = 0; k < 8; k++) v26.set(k, mix(v26.lane(k), v28.lane(k), gain));
        lqv(v29, in);
        lqv(v28, in + 16);
        sqv(v26, out + 16);
        out += 32;
        lqv(v27, out);
        lqv(v26, out + 16);
    } while (left > 0);
}

// VADPCM: 9-byte frames (a header with scale and predictor, 16 4-bit
// samples), two halves of 8, order-2 prediction through the codebook's
// shifted second row, each half's sum wrapped to 32 bits then >> 11.
void cmd_adpcm(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    uint32_t in = dhu(kIn);
    uint32_t out = dhu(kOut);
    int32_t left = dhu(kCount);
    const uint32_t state = seg_addr(w1);
    const uint32_t flags = w0 >> 16;

    // The history: zeros (two SQVs, so only up to each 16-byte boundary) or
    // the saved 16 samples; then the last 8 by LQV into a cleared register.
    VReg hist{};
    if (flags & 1) {
        sqv(hist, out);
        sqv(hist, out + 16);
    }
    else {
        dma_read(rdram, out & 0xFFF, (flags & 2) ? dw(kLoop) : state, 32);
    }
    lqv(hist, out + 16);
    int32_t p6 = hist.lane(6), p7 = hist.lane(7);
    out += 32;

    // The microcode reads each frame (header, data, codebook rows) during
    // the previous one, before that one's output is stored: overlapping
    // buffers see the same bytes here.
    struct Frame { int32_t scale; int32_t c1[8], c2[8]; uint8_t d[8]; };
    auto fetch = [](uint32_t at, Frame& fr) {
        const uint8_t hdr = db(at);
        const uint32_t book = kBook + ((hdr & 0xF) << 5);
        fr.scale = hdr >> 4;
        for (int i = 0; i < 8; i++) fr.d[i] = db(at + 1 + i);
        for (int i = 0; i < 8; i++) {
            fr.c1[i] = dh(book + 2 * i);
            fr.c2[i] = dh(book + 16 + 2 * i);
        }
    };
    Frame cur, next;
    fetch(in, cur);
    if (left != 0) {
        do {
            in += 9;
            fetch(in, next);
            // nibble << 12, scaled down by VMUDM unless the scale is 12 or more.
            const int32_t mult = cur.scale < 12 ? (0x8000 >> (11 - cur.scale)) : 0;
            int32_t o[16];
            ahle::AdpcmBook book;
            ahle::adpcm_book(book, cur.c1, cur.c2);
            for (int h = 0; h < 2; h++) {
                int32_t sm[8];
                for (int i = 0; i < 4; i++) {
                    const uint8_t b = cur.d[h * 4 + i];
                    sm[2 * i] = (int16_t)((b & 0xF0) << 8);
                    sm[2 * i + 1] = (int16_t)((b & 0x0F) << 12);
                }
                if (mult) for (int i = 0; i < 8; i++) sm[i] = (sm[i] * mult) >> 16;
                int32_t* oh = o + 8 * h;
                ahle::adpcm_half(book, sm, p6, p7, oh);
                p6 = oh[6];
                p7 = oh[7];
            }
            for (int i = 0; i < 16; i++) dhw(out + 2 * i, o[i]);
            out += 32;
            left -= 32;
            cur = next;
        } while (left > 0);
    }
    dma_write(rdram, (out - 32) & 0xFFF, state, 32);
}

// Pole filter (libaudio's two-pole IIR), 8 samples a step through the
// impulse responses in the codebook: out = (row 1 * y[-2] + row 2 * y[-1]
// + the gain-scaled row 2 through this step's earlier inputs + in * gain)
// >> 14, y being the previous step's last outputs. The scaled row stays in
// the codebook.
void cmd_polef(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    uint32_t in = dhu(kIn);
    uint32_t out = dhu(kOut);
    int32_t left = dhu(kCount);
    if (left == 0) return;
    const int32_t gain = (int16_t)(w0 & 0xFFFF);
    const uint32_t g4 = (w0 << 2) & 0xFFFF;
    const uint32_t state = seg_addr(w1);
    dww(kState, 0);
    if (!((w0 >> 16) & 1)) dma_read(rdram, kState, state, 8);
    int32_t c1[8], c2[8], sc2[8];
    for (int k = 0; k < 8; k++) {
        c2[k] = dh(kBook + 16 + 2 * k);
        sc2[k] = (int16_t)((c2[k] * (int32_t)g4) >> 16);   // VMUDM: the middle word
    }
    for (int k = 0; k < 8; k++) dhw(kBook + 16 + 2 * k, sc2[k]);
    for (int k = 0; k < 8; k++) c1[k] = dh(kBook + 2 * k);
    int32_t p6 = dh(kState + 4), p7 = dh(kState + 6);

    int32_t x[8];
    for (int k = 0; k < 8; k++) x[k] = dh(in + 2 * k);
    do {
        int32_t o[8];
        for (int k = 0; k < 8; k++) {
            uint32_t acc = (uint32_t)(c1[k] * p6) + (uint32_t)(c2[k] * p7) + (uint32_t)(x[k] * gain);
            for (int j = 0; j < k; j++) acc += (uint32_t)(sc2[k - 1 - j] * x[j]);
            o[k] = sat16((int32_t)acc >> 14);
        }
        in += 16;
        left -= 16;
        for (int k = 0; k < 8; k++) x[k] = dh(in + 2 * k);   // the next input, before this output lands
        for (int k = 0; k < 8; k++) dhw(out + 2 * k, o[k]);
        p6 = o[6];
        p7 = o[7];
        out += 16;
    } while (left > 0);
    dma_write(rdram, (out - 8) & 0xFFF, state, 8);
}

// 4-tap table resampler: positions in 16.16 (pitch is 1.15, so each output
// steps 2 * pitch), the table row from the fraction's top 6 bits, 8 outputs
// per block. The per-lane sample and table addresses go through DMEM 0xFB0
// and 0xFC0, and the state keeps the 4 samples at the next position, its
// fraction, and 16 bytes of input from a 16-byte boundary of the buffer.
void cmd_resample(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    const int32_t in0 = dh(kIn);
    uint32_t out = (uint32_t)(int32_t)dh(kOut);
    int32_t left = dh(kCount);
    const uint32_t state = seg_addr(w1);
    dww(kState + 0x40, state);
    const uint32_t flags = w0 >> 16;
    if (flags & 1) {
        dhw(kState + 8, 0);
        dww(kState, 0); dww(kState + 4, 0);
    }
    else {
        dma_read(rdram, kState, state, 32);
    }
    int32_t base = in0;
    if (flags & 2) {
        const int32_t back = dh(kState + 0xA);
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = db(kState + 0x10 + i);
        for (int i = 0; i < 16; i++) dbw((uint32_t)(base - 16 + i), t[i]);
        base -= back;
    }
    base -= 8;
    const uint32_t frac0 = dhu(kState + 8);
    {
        uint8_t t[8];
        for (int i = 0; i < 8; i++) t[i] = db(kState + i);
        for (int i = 0; i < 8; i++) dbw((uint32_t)(base + i), t[i]);
    }
    const uint32_t pitch = w0 & 0xFFFF;
    const int32_t base16 = (int16_t)base;     // MTC2: the register holds its low half

    // Lane k of a block: VMADM gives the position's integer part (saturated),
    // VMADN its fraction (saturated low word), then the sample address
    // (base + 2 * ipos, 16 bits) and the table row address.
    int32_t ipos[8];
    uint32_t frac[8];
    auto positions = [&](int64_t p, int first) {
        for (int k = 0; k < 8; k++) {
            const int64_t acc = p + (int64_t)(2 * (k + first)) * pitch;
            const int32_t hi = (int32_t)(acc >> 16);
            ipos[k] = sat16(hi);
            frac[k] = sat_lo(hi, (uint32_t)acc);
        }
    };
    auto sample_addr = [&](int k) -> uint32_t {
        const int32_t a = base16 + 2 * (int32_t)(uint16_t)ipos[k];
        return (uint32_t)(int32_t)(int16_t)sat_lo(a >> 16, (uint32_t)a);
    };
    auto table_addr = [&](int k) -> uint32_t { return kLut + (frac[k] >> 10) * 8; };
    auto store_tables = [&]() {
        for (int k = 0; k < 8; k++) {
            dhw(kState + 0x20 + 2 * k, (int32_t)sample_addr(k));
            dhw(kState + 0x30 + 2 * k, (int32_t)table_addr(k));
        }
    };

    positions((int64_t)frac0, 0);
    store_tables();
    // SSV of byte 7: the low byte of lane 3's fraction, the high byte of lane 4's.
    dbw(kState + 8, (uint8_t)frac[3]);
    dbw(kState + 9, (uint8_t)(frac[4] >> 8));

    const int blocks = left > 0 ? (left + 15) / 16 : 1;
    // Fast path: the whole input span in a plain array, when no block can
    // read output the earlier ones wrote (the microcode would see it).
    const uint32_t span_end = (uint32_t)(((int64_t)frac0 + 2 * (int64_t)pitch * (8 * blocks - 1)) >> 16) + 4;   // samples
    const uint32_t bo = (uint32_t)base & 0xFFF, oo = out & 0xFFF;
    const bool fast = span_end <= 0x800 && (base & 1) == 0 && bo + 2 * span_end <= kState &&
                      flat(oo, 16 * blocks, 16) && (bo + 2 * span_end <= oo || oo + 16 * blocks <= bo) &&
                      (oo >= kLut + 0x200 || oo + 16 * blocks <= kLut);
    if (fast) {
        // Positions this small never saturate, so lane k of block b sits at
        // exactly frac0 + 2 * pitch * (8b + k): one running 16.16 position.
        int16_t xs[0x800];
        for (uint32_t i = 0; i < span_end; i++) xs[i] = dh(bo + 2 * i);
        // Row r: two words, (tap 1, tap 0) and (tap 3, tap 2) (raw order).
        const ahle::word_t* lutw = (const ahle::word_t*)(g_dm + kLut);
        const uint32_t step = 2 * pitch;
        uint32_t pos = frac0;
        if (g_low) {
            // Half rate: every other output, packed at the start of the
            // buffer; the position ends where the full rate's would.
            dm_i16* d = dmraw(oo);
            uint32_t p = frac0;
            for (int j = 0; j < 4 * blocks; j++, p += 2 * step) {
                const int16_t* x = &xs[p >> 16];
                const ahle::word_t* c = lutw + ((p & 0xFFFF) >> 10) * 2;
                d[j ^ 1] = (int16_t)ahle::tap4(x[0], x[1], x[2], x[3], c[0], c[1]);
            }
            pos = frac0 + step * (uint32_t)(8 * blocks);
        }
        else for (int b = 0; b < blocks; b++) {
            dm_i16* d = dmraw(oo + 16 * b);
            for (int k = 0; k < 8; k++, pos += step) {
                const int16_t* x = &xs[pos >> 16];
                const ahle::word_t* c = lutw + ((pos & 0xFFFF) >> 10) * 2;
                d[k ^ 1] = (int16_t)ahle::tap4(x[0], x[1], x[2], x[3], c[0], c[1]);
            }
        }
        // The lanes of the block after the last, for the state and tables.
        positions((int64_t)(pos - step), 1);
        store_tables();
        out += 16 * blocks;
    }
    else {
      const uint32_t exact_out = out;
      for (int b = 0; b < blocks; b++) {
        int32_t o[8];
        for (int k = 0; k < 8; k++) {
            const uint32_t ta = table_addr(k);
            const uint32_t sa = sample_addr(k);
            const int32_t x0 = dh(sa), x1 = dh(sa + 2), x2 = dh(sa + 4), x3 = dh(sa + 6);
            const int32_t m0 = vmulf(x0, dh(ta)), m1 = vmulf(x1, dh(ta + 2));
            const int32_t m2 = vmulf(x2, dh(ta + 4)), m3 = vmulf(x3, dh(ta + 6));
            o[k] = sat16(sat16(m0 + m1) + sat16(m2 + m3));
        }
        // The next block continues from lane 7's position.
        positions(((int64_t)ipos[7] << 16) + frac[7], 1);
        store_tables();
        VReg v;
        for (int k = 0; k < 8; k++) v.set(k, o[k]);
        sqv(v, out);
        out += 16;
      }
      // Low quality on the exact path: keep every other output.
      if (g_low) for (int j = 1; j < 4 * blocks; j++) dhw(exact_out + 2 * j, dh(exact_out + 4 * j));
    }

    // State: the next position's fraction and 4 samples, and 16 bytes from
    // the input's 16-byte grid at the next sample.
    dhw(kState + 8, (int32_t)frac[0]);
    int32_t sa = (int32_t)sample_addr(0);
    {
        uint8_t t[8];
        for (int i = 0; i < 8; i++) t[i] = db((uint32_t)(sa + i));
        for (int i = 0; i < 8; i++) dbw(kState + i, t[i]);
    }
    sa += 8;
    int32_t mis = (sa - dh(kIn)) & 0xF;
    sa -= mis;
    if (mis != 0) mis = 16 - mis;
    dhw(kState + 0xA, mis);
    {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = db((uint32_t)(sa + i));
        for (int i = 0; i < 16; i++) dbw(kState + 0x10 + i, t[i]);
    }
    dma_write(rdram, kState, dw(kState + 0x40), 32);
}

// Envelope ramps. A volume is a 16.16 pair per lane, multiplied by the 16.16
// rate once per block (VMUDL, VMADM, VMADN, VMADH) and clamped to the target
// in its integer part only: an unsigned minimum (VCL) when the rate's
// integer part is positive, a signed maximum (VGE) otherwise.
// The accumulator's sum (F * rlo >> 16) + V * rlo + F * rhi + (V * rhi << 16)
// is formed in 32-bit halves: each product fits in 32 bits, and their high
// halves and the carry out of the low ones make the upper word.
inline void ramp_step(int32_t& v, uint32_t& f, int32_t rhi, uint32_t rlo) {
    const int32_t b = v * (int32_t)rlo, c = (int32_t)f * rhi;
    const uint32_t lo = ((f * rlo) >> 16) + ((uint32_t)b & 0xFFFF) + ((uint32_t)c & 0xFFFF);
    const int32_t hi = (b >> 16) + (c >> 16) + (int32_t)(lo >> 16) + v * rhi;
    f = sat_lo(hi, lo);
    v = sat16(hi);
}
inline int32_t ramp_clamp(int32_t v, int32_t target, int32_t rhi) {
    if (rhi > 0) return (uint16_t)v >= (uint16_t)target ? target : v;
    return v >= target ? v : target;
}
// A_INIT: the first block interpolates from the starting volume towards
// volume * rate across the lanes (steps (k + 1) / 8 from DMEM 0xB0).
void ramp_init(int32_t v0, int32_t rhi, uint32_t rlo, const uint32_t* t, int32_t* v, uint32_t* f) {
    const int32_t acc = v0 * (int32_t)rlo;                                          // VMUDM
    const int32_t hi = (int32_t)((uint32_t)(acc >> 16) + (uint32_t)(v0 * rhi));      // VMADH
    const uint32_t dlo = sat_lo(hi, (uint32_t)acc);                                 // VMADN
    const int32_t dhi = sat16(sat16(hi) - v0);                                      // VSUB
    for (int k = 0; k < 8; k++) {
        const int32_t c = (int32_t)t[k] * dhi;                                        // VMADN
        const uint32_t lo = ((t[k] * dlo) >> 16) + ((uint32_t)c & 0xFFFF);            // VMUDL
        const int32_t h = (c >> 16) + (int32_t)(lo >> 16) + v0;                       // VMADH
        f[k] = sat_lo(h, lo);
        v[k] = sat16(h);
    }
}

// Envelope mixer: the input into main left/right and aux (wet) left/right
// with per-lane ramped volumes times the dry and wet gains.
void cmd_envmixer(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    const uint32_t state = seg_addr(w1);
    const uint32_t flags = w0 >> 16;
    const bool init = flags & 1;
    int32_t prm[8];
    int32_t lv[8], rv[8];
    uint32_t lf[8], rf[8];
    if (init) {
        for (int k = 0; k < 8; k++) prm[k] = dh(kEnv + 2 * k);
    }
    else {
        dma_read(rdram, kState, state, 0x50);
        for (int k = 0; k < 8; k++) {
            lv[k] = dh(kState + 2 * k);
            lf[k] = dhu(kState + 0x10 + 2 * k);
            rv[k] = dh(kState + 0x20 + 2 * k);
            rf[k] = dhu(kState + 0x30 + 2 * k);
            prm[k] = dh(kState + 0x40 + 2 * k);
        }
    }
    const int32_t ltgt = prm[0], lrhi = prm[1], rtgt = prm[3], rrhi = prm[4], dry = prm[6], wet = prm[7];
    const uint32_t lrlo = (uint16_t)prm[2], rrlo = (uint16_t)prm[5];
    uint32_t in = (uint32_t)(int32_t)dh(kIn), ml = (uint32_t)(int32_t)dh(kOut), mr = (uint32_t)(int32_t)dh(kDryR);
    uint32_t al = (uint32_t)(int32_t)dh(kWetL), ar = (uint32_t)(int32_t)dh(kWetR);
    const int32_t count = dh(kCount);
    const bool aux = flags & 8;
    if (!aux) al = ar = kNoAux;
    const uint32_t astep = aux ? 16 : 0;
    uint32_t t[8];
    for (int k = 0; k < 8; k++) t[k] = dhu(kRamp + 2 * k);

    const int32_t rest = init ? count - 16 : count;
    const int blocks = (init ? 1 : 0) + (rest > 0 ? (rest + 15) / 16 : 1);
    const uint32_t bytes = 16u * (uint32_t)blocks;

    // Fast path: aligned buffers that do not overlap one another or the
    // state, element-wise on the raw halfword arrays.
    const uint32_t io = in & 0xFFF, mlo = ml & 0xFFF, mro = mr & 0xFFF, alo = al & 0xFFF, aro = ar & 0xFFF;
    auto clear_of_state = [](uint32_t a, uint32_t n) { return a + n <= kState || a >= 0xFF0; };
    bool fast = flat(io, bytes, 16) && flat(mlo, bytes, 16) && flat(mro, bytes, 16) &&
                disjoint(io, mlo, bytes) && disjoint(io, mro, bytes) && disjoint(mlo, mro, bytes) &&
                clear_of_state(io, bytes) && clear_of_state(mlo, bytes) && clear_of_state(mro, bytes);
    if (fast && aux) {
        fast = flat(alo, bytes, 16) && flat(aro, bytes, 16) && disjoint(alo, aro, bytes) &&
               disjoint(io, alo, bytes) && disjoint(io, aro, bytes) && disjoint(mlo, alo, bytes) &&
               disjoint(mlo, aro, bytes) && disjoint(mro, alo, bytes) && disjoint(mro, aro, bytes) &&
               clear_of_state(alo, bytes) && clear_of_state(aro, bytes);
    }
    if (!fast && ((in | ml | mr | (aux ? (al | ar) : 0)) & 15)) {
        note_inexact(3, (in & 15) ? in : (ml & 15) ? ml : (mr & 15) ? mr : (al & 15) ? al : ar);
    }

    VReg v17{}, v29{}, v27{}, v28{}, v26{};     // the exact path's registers
    int32_t gdl[8], gwl[8], gdr[8], gwr[8];
    // A step is a function of (V, F) alone: once one leaves all of a side's
    // lanes as they were, every later one does too (most voices: the
    // maximum rate pins V to the target), and that side's gains stay.
    bool lsteady = false, rsteady = false;
    auto advance = [](int32_t* v, uint32_t* f, int32_t tgt, int32_t rhi, uint32_t rlo, bool& steady) {
        bool same = true;
        for (int k = 0; k < 8; k++) {
            int32_t nv = v[k];
            uint32_t nf = f[k];
            ramp_step(nv, nf, rhi, rlo);
            nv = ramp_clamp(nv, tgt, rhi);
            same &= nv == v[k] && nf == f[k];
            v[k] = nv;
            f[k] = nf;
        }
        steady = same;
    };
    // The mixing kernel's gain words (audio_hle_simd.h), raw order, rebuilt
    // before the next mix whenever the gains have been recomputed.
    uint32_t kgdl[4], kgdr[4], kgwl[4], kgwr[4];
    bool udl = false, udr = false, uwl = false, uwr = false;    // one gain for the whole block (a settled ramp)
    bool kg_dirty = true;
    auto gains = [&](const int32_t* v, int32_t* gd, int32_t* gw) {
        for (int k = 0; k < 8; k++) { gd[k] = vmulf(v[k], dry); gw[k] = vmulf(v[k], wet); }
        kg_dirty = true;
    };
    if (g_low) {
        // Half rate (any alignment: the loop below addresses DMEM sample by
        // sample): the input and the dry buses hold one frame for every
        // two; block b's gains mix frames 8 * (b / 2) .. on even b, and every
        // block still advances the ramps, so the saved volumes are the full
        // rate's. The wet buses stay as CLEARBUFF left them: no reverb.
        auto dry_gains = [&](const int32_t* v, int32_t* gd) { for (int k = 0; k < 8; k++) gd[k] = vmulf(v[k], dry); kg_dirty = true; };
        // Where this voice's half-rate samples go: its offset into each bus
        // halved (a voice that starts part of the way through the sub-frame).
        const uint32_t hml = half_rate_addr(mlo, g_bus_l), hmr = half_rate_addr(mro, g_bus_r);
        // Whole words when the input and both buses are word aligned, inside
        // DMEM and apart (as the sample-by-sample loop would see them).
        const uint32_t hbytes = 16u * (uint32_t)((blocks + 1) / 2);
        const bool words = ((io | hml | hmr) & 3) == 0 && io + hbytes <= 0x1000 && hml + hbytes <= 0x1000 && hmr + hbytes <= 0x1000 &&
                           disjoint(io, hml, hbytes) && disjoint(io, hmr, hbytes) && disjoint(hml, hmr, hbytes);
        for (int b = 0; b < blocks; b++) {
            const bool first_init = init && b == 0;
            if (first_init) {
                ramp_init(dh(kVolL), lrhi, lrlo, t, lv, lf);
                ramp_init(dh(kVolR), rrhi, rrlo, t, rv, rf);
                for (int k = 0; k < 8; k++) {
                    lv[k] = ramp_clamp(lv[k], ltgt, lrhi);
                    rv[k] = ramp_clamp(rv[k], rtgt, rrhi);
                }
                dry_gains(lv, gdl);
                dry_gains(rv, gdr);
            }
            else {
                if (!lsteady) { advance(lv, lf, ltgt, lrhi, lrlo, lsteady); dry_gains(lv, gdl); }
                if (!rsteady) { advance(rv, rf, rtgt, rrhi, rrlo, rsteady); dry_gains(rv, gdr); }
            }
            if ((b & 1) == 0) {
                const uint32_t o = 16 * (uint32_t)(b >> 1);     // bytes: 8 half-rate samples a step
                if (words) {
                    if (kg_dirty) {
                        udl = ahle::lane_gain_words(gdl, kgdl);
                        udr = ahle::lane_gain_words(gdr, kgdr);
                        kg_dirty = false;
                    }
                    const ahle::word_t* xw = (const ahle::word_t*)(g_dm + io + o);
                    const uint32_t xv[4] = { xw[0], xw[1], xw[2], xw[3] };
                    ahle::mix_block_any((ahle::word_t*)(g_dm + hml + o), xv, kgdl, udl);
                    ahle::mix_block_any((ahle::word_t*)(g_dm + hmr + o), xv, kgdr, udr);
                }
                else for (int k = 0; k < 8; k++) {
                    const int32_t xs = dh(io + o + 2 * k);
                    const uint32_t al = hml + o + 2 * k, ar = hmr + o + 2 * k;
                    dhw(al, mix(dh(al), xs, gdl[k]));
                    dhw(ar, mix(dh(ar), xs, gdr[k]));
                }
            }
            if (!first_init && b == blocks - 1) {
                for (int k = 0; k < 8; k++) { dhw(kState + 2 * k, lv[k]); dhw(kState + 0x10 + 2 * k, (int32_t)lf[k]); }
            }
        }
    }
    else for (int b = 0; b < blocks; b++) {
        const bool first_init = init && b == 0;
        // This block's volumes and gains.
        if (first_init) {
            ramp_init(dh(kVolL), lrhi, lrlo, t, lv, lf);
            ramp_init(dh(kVolR), rrhi, rrlo, t, rv, rf);
            for (int k = 0; k < 8; k++) {
                lv[k] = ramp_clamp(lv[k], ltgt, lrhi);
                rv[k] = ramp_clamp(rv[k], rtgt, rrhi);
            }
            gains(lv, gdl, gwl);
            gains(rv, gdr, gwr);
        }
        else {
            if (!lsteady) { advance(lv, lf, ltgt, lrhi, lrlo, lsteady); gains(lv, gdl, gwl); }
            if (!rsteady) { advance(rv, rf, rtgt, rrhi, rrlo, rsteady); gains(rv, gdr, gwr); }
        }
        if (fast) {
            if (kg_dirty) {
                udl = ahle::lane_gain_words(gdl, kgdl); udr = ahle::lane_gain_words(gdr, kgdr);
                uwl = ahle::lane_gain_words(gwl, kgwl); uwr = ahle::lane_gain_words(gwr, kgwr);
                kg_dirty = false;
            }
            const dm_i16* x = dmraw(io + 16 * b);
            const ahle::word_t* xw = (const ahle::word_t*)(g_dm + io + 16 * b);
            const uint32_t xv[4] = { xw[0], xw[1], xw[2], xw[3] };
            ahle::mix_block_any((ahle::word_t*)(g_dm + mlo + 16 * b), xv, kgdl, udl);
            ahle::mix_block_any((ahle::word_t*)(g_dm + mro + 16 * b), xv, kgdr, udr);
            if (aux) {
                ahle::mix_block_any((ahle::word_t*)(g_dm + alo + 16 * b), xv, kgwl, uwl);
                ahle::mix_block_any((ahle::word_t*)(g_dm + aro + 16 * b), xv, kgwr, uwr);
            }
            else {
                // Both aux outputs land on 0xFE0. The first A_INIT block
                // stores the left one before loading the right; later
                // blocks load both first, so the right one alone remains.
                dm_i16* d = dmraw(kNoAux);
                for (int r = 0; r < 8; r++) {
                    const int k = r ^ 1;
                    int32_t a = d[r];
                    if (first_init) a = mix(a, x[r], gwl[k]);
                    d[r] = (int16_t)mix(a, x[r], gwr[k]);
                }
            }
            if (!first_init && b == blocks - 1) {
                for (int k = 0; k < 8; k++) { dhw(kState + 2 * k, lv[k]); dhw(kState + 0x10 + 2 * k, (int32_t)lf[k]); }
            }
        }
        else if (first_init) {
            lqv(v17, in); lqv(v29, ml); lqv(v27, al);
            for (int k = 0; k < 8; k++) {
                v29.set(k, mix(v29.lane(k), v17.lane(k), gdl[k]));
                v27.set(k, mix(v27.lane(k), v17.lane(k), gwl[k]));
            }
            sqv(v29, ml); sqv(v27, al);
            lqv(v28, mr); lqv(v26, ar);
            for (int k = 0; k < 8; k++) {
                v28.set(k, mix(v28.lane(k), v17.lane(k), gdr[k]));
                v26.set(k, mix(v26.lane(k), v17.lane(k), gwr[k]));
            }
            sqv(v28, mr); sqv(v26, ar);
        }
        else {
            lqv(v17, in); lqv(v29, ml); lqv(v27, al);
            for (int k = 0; k < 8; k++) { dhw(kState + 2 * k, lv[k]); dhw(kState + 0x10 + 2 * k, (int32_t)lf[k]); }
            lqv(v28, mr); lqv(v26, ar);
            for (int k = 0; k < 8; k++) {
                v29.set(k, mix(v29.lane(k), v17.lane(k), gdl[k]));
                v27.set(k, mix(v27.lane(k), v17.lane(k), gwl[k]));
                v28.set(k, mix(v28.lane(k), v17.lane(k), gdr[k]));
                v26.set(k, mix(v26.lane(k), v17.lane(k), gwr[k]));
            }
            sqv(v29, ml); sqv(v27, al); sqv(v28, mr); sqv(v26, ar);
        }
        in += 16; ml += 16; mr += 16; al += astep; ar += astep;
    }
    for (int k = 0; k < 8; k++) {
        dhw(kState + 0x20 + 2 * k, rv[k]);
        dhw(kState + 0x30 + 2 * k, (int32_t)rf[k]);
        dhw(kState + 0x40 + 2 * k, prm[k]);
    }
    dma_write(rdram, kState, state, 0x50);
}

// -------------------------------------------------------------- task runner

void run_command_inner(uint8_t* rdram, uint32_t op, uint32_t w0, uint32_t w1);

// Host ticks for the per-opcode accounting (AudioHleStats::op_ticks).
inline uint32_t op_tick() {
#ifdef __3DS__
    return (uint32_t)svcGetSystemTick();
#else
    // Host tools (naudio_test --bench): nanoseconds.
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
#endif
}

// The per-command clock and the constants check are diagnostics, and at the
// Old 3DS clock two system calls and a 192-byte compare per command cost
// about a tenth of the task: they run in one task out of kSampleEvery, whose
// times count kSampleEvery-fold, and the constants are also checked once at
// the start of every task.
constexpr uint32_t kSampleEvery = 16;
bool g_sample_task = false;

void run_command(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    g_stats.commands++;
    const uint32_t op = (w0 >> 24) & 0x7F;
    if (__builtin_expect(!g_sample_task, 1)) {
        run_command_inner(rdram, op, w0, w1);
        return;
    }
    if ((1u << (op & 31)) & ((1u << 1) | (1u << 3) | (1u << 5) | (1u << 12) | (1u << 14))) check_constants(op);
    const uint32_t t0 = op_tick();
    run_command_inner(rdram, op, w0, w1);
    if (op < 16) g_stats.op_ticks[op] += (op_tick() - t0) * kSampleEvery;
}

void run_command_inner(uint8_t* rdram, uint32_t op, uint32_t w0, uint32_t w1) {
    if (g_low || g_nofx) {
        // A sub-frame's reverb runs from its first MIXER to its INTERLEAVE
        // (the voices before it mix through ENVMIXER alone). Left out: its
        // mixing, filtering and delay-line loads are skipped and its saves
        // write silence, so the delay lines are clean when full quality
        // returns. SETBUFF and SETVOL still record their fields.
        if (op == 7 || op == 13) g_in_fx = false;
        else if (op == 12) g_in_fx = true;
        if (g_in_fx) {
            switch (op) {
                case 2: case 4: case 10: case 12: case 14: return;
                case 6: cmd_savebuff_silence(rdram, w1); return;
                default: break;
            }
        }
    }
    switch (op) {
        case 0: break;
        case 1: cmd_adpcm(rdram, w0, w1); break;
        case 2: cmd_clearbuff(w0, w1); break;
        case 3: cmd_envmixer(rdram, w0, w1); break;
        case 4: cmd_loadbuff(rdram, w1); break;
        case 5: cmd_resample(rdram, w0, w1); break;
        case 6: cmd_savebuff(rdram, w1); break;
        case 7: cmd_segment(w1); break;
        case 8: cmd_setbuff(w0, w1); break;
        case 9: cmd_setvol(w0, w1); break;
        case 10: cmd_dmemmove(w0, w1); break;
        case 11: cmd_loadadpcm(rdram, w0, w1); break;
        case 12: cmd_mixer(w0, w1); break;
        case 13: cmd_interleave(w1); break;
        case 14: cmd_polef(rdram, w0, w1); break;
        case 15: cmd_setloop(w1); break;
        default: {
            // The microcode would jump through its constants: count it and
            // name each opcode once in the log.
            g_stats.unknown_opcodes++;
            static uint32_t seen[4] = {};
            if (!(seen[op >> 5] & (1u << (op & 31)))) {
                seen[op >> 5] |= 1u << (op & 31);
                fprintf(stderr, "aspmain-hle: unknown command %u (%08lx %08lx)\n", (unsigned)op, (unsigned long)w0, (unsigned long)w1);
            }
            break;
        }
    }
}

// The microcode's main loop: the list in chunks of up to 0x140 bytes at
// DMEM 0x380 (the first before segment 0 is cleared), a command at a time.
void run_task(uint8_t* rdram, const OSTask& task) {
    dma_read(rdram, 0, task.t.ucode_data, 0xF80);
    memcpy(&g_dm[0xFC0], &task, sizeof(OSTask));
    uint32_t ptr = dw(0xFF0);
    int32_t left = (int32_t)dw(0xFF4);
    auto load_chunk = [&]() -> int32_t {
        const int32_t n = left - (int32_t)kChunk > 0 ? (int32_t)kChunk : left;
        if (n > 0) dma_read(rdram, kList, ptr, (uint32_t)n);
        return n;
    };
    int32_t chunk = load_chunk();
    dww(kSeg, 0);
    uint32_t pos = kList;
    g_sample_task = (g_stats.tasks % kSampleEvery) == 0;
    check_constants(0);
    for (;;) {
        const uint32_t w0 = dw(pos), w1 = dw(pos + 4);
        ptr += 8; left -= 8; pos += 8; chunk -= 8;
        run_command(rdram, w0, w1);
        if (chunk > 0) continue;
        if (left <= 0) break;
        chunk = load_chunk();
        pos = kList;
    }
    g_stats.tasks++;
}

// Differential testing (audio_hle_diff.cpp): a task of one command, as the
// microcode runs it: the command is the whole list chunk, segment 0 cleared.
void lockstep_command(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    dww(kList, w0);
    dww(kList + 4, w1);
    dww(kSeg, 0);
    run_command(rdram, w0, w1);
}

// The RDRAM a command writes (segment 0 reads as cleared, as in a task of one command).
bool command_range(uint32_t w0, uint32_t w1, uint32_t* addr, uint32_t* len) {
    const uint32_t s = w1 >> 24;
    const uint32_t a = ((w1 & 0xFFFFFF) + (s == 0 ? 0 : dw(kSeg + s * 4))) & 0xFFFFF8;
    switch ((w0 >> 24) & 0x7F) {
        case 1: *addr = a; *len = 32; return true;
        case 3: *addr = a; *len = 0x50; return true;
        case 5: *addr = a; *len = 32; return true;
        case 6: *addr = a; *len = dhu(kCount); return *len != 0;
        case 14: *addr = a; *len = 8; return dhu(kCount) != 0;
        default: return false;
    }
}

recomp3ds::AudioHleFamily g_family = {
    "aspmain-hle", g_dm, &g_stats, lockstep_command, command_range, nullptr, nullptr, {},
};

}   // namespace

RspExitReason recomp3ds::aspmain_hle_run(uint8_t* rdram, uint32_t ucode_addr) {
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

void recomp3ds::aspmain_hle_set_reference(RspUcodeFunc* reference) {
    g_reference = reference;
}

void recomp3ds::aspmain_hle_set_low_quality(bool on) {
    g_low = on;
}

void recomp3ds::aspmain_hle_set_no_reverb(bool on) {
    g_nofx = on;
}

bool recomp3ds::aspmain_hle_low_quality() {
    return g_low;
}

void recomp3ds::aspmain_hle_set_diff_verbose(int mismatches) {
    g_diff_verbose = mismatches;
}

const recomp3ds::AudioHleStats& recomp3ds::aspmain_hle_stats() {
    return g_stats;
}

const uint8_t* recomp3ds::aspmain_hle_dmem() {
    return g_dm;
}

void recomp3ds::aspmain_hle_reset_dmem() {
    memset(g_dm, 0, sizeof(g_dm));
}
