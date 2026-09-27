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
uint8_t g_dm[0x1000];

inline uint8_t db(uint32_t a) { return g_dm[(a & 0xFFF) ^ 3]; }
inline void dbw(uint32_t a, uint8_t v) { g_dm[(a & 0xFFF) ^ 3] = v; }
inline int16_t dh(uint32_t a) { return (int16_t)((db(a) << 8) | db(a + 1)); }
inline void dhw(uint32_t a, int32_t v) { dbw(a, (uint8_t)(v >> 8)); dbw(a + 1, (uint8_t)v); }
inline uint32_t dw(uint32_t a) { return ((uint32_t)db(a) << 24) | ((uint32_t)db(a + 1) << 16) | ((uint32_t)db(a + 2) << 8) | db(a + 3); }
inline void dww(uint32_t a, uint32_t v) { dbw(a, v >> 24); dbw(a + 1, v >> 16); dbw(a + 2, v >> 8); dbw(a + 3, v); }

inline uint8_t rb(const uint8_t* rdram, uint32_t p) { return rdram[(p & 0xFFFFFF) ^ 3]; }
inline void rbw(uint8_t* rdram, uint32_t p, uint8_t v) { rdram[(p & 0xFFFFFF) ^ 3] = v; }

inline int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }

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

recomp3ds::NaudioHleStats g_stats{};
RspUcodeFunc* g_reference = nullptr;

// ---------------------------------------------------------------- commands

void cmd_clearbuff(uint32_t w0, uint32_t w1) {
    uint32_t a = (w0 & 0xFFFF) + kBufBase;
    int32_t n = w1 & 0xFFFF;
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
                for (int i = 0; i < 8; i++) {
                    int32_t acc = c1[i] * p2 + c2[i] * p1 + (s[i] << 11);
                    for (int k = 0; k < i; k++) acc += c2[i - 1 - k] * s[k];
                    o[i] = clamp16(acc >> 11);
                }
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
    store_tables();

    for (int block = 0; block < 23; block++) {
        int16_t o[8];
        for (int k = 0; k < 8; k++) {
            uint32_t sa = (uint16_t)dh(kState + 0x10 + 2 * k);
            uint32_t la = (uint16_t)dh(kState + 0x20 + 2 * k);
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
        store_tables();
        for (int k = 0; k < 8; k++) dhw(out + 2 * k, o[k]);
        out += 16;
    }

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
    int32_t maskL = (dry & 1) ? -1 : 0;     // v9: dry LSB inverts the left input
    int32_t maskR = (wet & 1) ? -1 : 0;     // v8 ^ v9 ^ v9: wet LSB inverts the right input

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
    auto apply = [&]() {
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
    while (blocks_done < 23) {
        for (int k = 0; k < 8; k++) { addL(k); clampL(k); addR(k); clampR(k); }
        save_left();
        apply();
    }
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
        default: g_stats.unknown_opcodes++; break;
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

// Differential testing: the RDRAM range one command writes.
struct Range { uint32_t addr, len; };

bool command_range(uint32_t w0, uint32_t w1, Range& r) {
    switch (w0 >> 24) {
        case 1: r = { w0 & 0xFFFFF8, 32 }; return true;
        case 3: r = { w1 & 0xFFFFF8, 0x50 }; return true;
        case 5: r = { w0 & 0xFFFFF8, 16 }; return true;
        case 6: {
            uint32_t count = (w0 >> 12) & 0xFFF;
            if (count == 0) return false;
            r = { w1 & 0xFFFFF8, count };
            return true;
        }
        default: return false;
    }
}

int g_reports_by_op[16];

// Every command runs through the reference (the recompiled microcode, on a
// one-command task) and this implementation from the same DMEM state; DMEM
// and the command's RDRAM range are compared afterwards and the reference's
// results are kept so one bug does not hide the next.
void run_task_lockstep(uint8_t* rdram, const OSTask& task, uint32_t ucode_addr) {
    dma_read(rdram, 0, task.t.ucode_data, 0xF80);
    memcpy(&g_dm[0xFC0], &task, sizeof(OSTask));
    uint32_t list = task.t.data_ptr & 0xFFFFFF;
    uint32_t n = task.t.data_size / 8;
    static uint8_t ref_dm[0x1000];
    static uint8_t before[0x1000], ref[0x1000];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w0 = rw(rdram, list + i * 8), w1 = rw(rdram, list + i * 8 + 4);
        uint32_t op = w0 >> 24;
        Range r{};
        bool has_range = command_range(w0, w1, r);

        // Reference on a one-command task, from this implementation's DMEM.
        memcpy(dmem, g_dm, 0x1000);
        uint32_t one_ptr = task.t.data_ptr + i * 8, one_size = 8;
        memcpy(&dmem[0xFC0 + 0x30], &one_ptr, 4);
        memcpy(&dmem[0xFC0 + 0x34], &one_size, 4);
        memcpy(&g_dm[0xFC0 + 0x30], &one_ptr, 4);
        memcpy(&g_dm[0xFC0 + 0x34], &one_size, 4);
        if (has_range) for (uint32_t k = 0; k < r.len; k++) before[k] = rb(rdram, r.addr + k);
        g_reference(rdram, ucode_addr);
        memcpy(ref_dm, dmem, 0x1000);
        if (has_range) {
            for (uint32_t k = 0; k < r.len; k++) { ref[k] = rb(rdram, r.addr + k); rbw(rdram, r.addr + k, before[k]); }
        }

        run_command(rdram, w0, w1);

        // Compare (the list chunk at 0x2B0 and the task copy at 0xFC0 differ by construction).
        int bad_dmem = -1, bad_rdram = -1;
        for (uint32_t a = 0; a < 0x1000; a++) {
            if (a >= 0x2B0 && a < 0x4F0) continue;
            if (a >= 0xFC0) continue;
            if (g_dm[a ^ 3] != ref_dm[a ^ 3]) { bad_dmem = (int)a; break; }
        }
        if (has_range) {
            for (uint32_t k = 0; k < r.len; k++) {
                if (rb(rdram, r.addr + k) != ref[k]) { bad_rdram = (int)k; break; }
            }
        }
        if ((bad_dmem >= 0 || bad_rdram >= 0) && op < 16) {
            g_stats.diff_mismatches++;
            if (g_reports_by_op[op] < 3) {
                g_reports_by_op[op]++;
                if (bad_dmem >= 0) {
                    uint32_t a = (uint32_t)bad_dmem & ~1u;
                    fprintf(stderr, "naudio-hle: task %u cmd %u op %u %08x %08x: DMEM differs at %03x (mine %04x ref %04x)\n",
                            g_stats.diff_tasks + 1, i, op, w0, w1, a, (uint16_t)dh(a),
                            (uint16_t)((ref_dm[a ^ 3] << 8) | ref_dm[(a + 1) ^ 3]));
                }
                if (bad_rdram >= 0) {
                    uint32_t k = (uint32_t)bad_rdram & ~1u;
                    fprintf(stderr, "naudio-hle: task %u cmd %u op %u %08x %08x: RDRAM differs at %06x+%u (mine %02x%02x ref %02x%02x)\n",
                            g_stats.diff_tasks + 1, i, op, w0, w1, r.addr, k,
                            rb(rdram, r.addr + k), rb(rdram, r.addr + k + 1), ref[k], ref[k + 1]);
                }
            }
        }
        // Continue from the reference's state.
        memcpy(g_dm, ref_dm, 0x1000);
        if (has_range) for (uint32_t k = 0; k < r.len; k++) rbw(rdram, r.addr + k, ref[k]);
    }
    g_stats.tasks++;
    g_stats.diff_tasks++;
}

}   // namespace

// Capture support: AUDIO_CAPTURE.TXT next to the executable makes tasks
// 200-207 write the whole RDRAM plus their OSTask to audio_task_N.bin, for
// the host-side test (tools/naudio_test).
const char* g_capture_dir = nullptr;
void recomp3ds_naudio_capture_dir(const char* dir);

void recomp3ds_naudio_capture_dir(const char* dir) { g_capture_dir = dir; }

void maybe_capture(const uint8_t* rdram, const OSTask& task) {
    static int seen = 0;
    if (g_capture_dir == nullptr) return;
    seen++;
    if (seen < 200 || seen >= 208) return;
    char path[192];
    snprintf(path, sizeof(path), "%s/audio_task_%d.bin", g_capture_dir, seen - 200);
    FILE* f = fopen(path, "wb");
    if (f == nullptr) { fprintf(stderr, "naudio-hle: capture failed to open %s\n", path); return; }
    fwrite(&task, 1, sizeof(OSTask), f);
    for (uint32_t off = 0; off < 16u * 1024 * 1024; off += 65536) fwrite(rdram + off, 1, 65536, f);
    fclose(f);
    fprintf(stderr, "naudio-hle: captured task %d to %s\n", seen, path);
}

RspExitReason recomp3ds::naudio_hle_run(uint8_t* rdram, uint32_t ucode_addr) {
    OSTask task;
    memcpy(&task, &dmem[0xFC0], sizeof(OSTask));
    maybe_capture(rdram, task);
    if (g_reference == nullptr) {
        run_task(rdram, task);
    }
    else {
        run_task_lockstep(rdram, task, ucode_addr);
    }
    return RspExitReason::Broke;
}

void recomp3ds::naudio_hle_set_reference(RspUcodeFunc* reference) {
    g_reference = reference;
}

const recomp3ds::NaudioHleStats& recomp3ds::naudio_hle_stats() {
    return g_stats;
}
