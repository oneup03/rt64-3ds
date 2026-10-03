// The arithmetic the audio microcode interpreters spend their time in, two
// 16-bit samples at a time on the ARM11's DSP extensions (ARMv6: SMLAD and
// the SMLAxy 16x16 multiply-accumulates, SSAT), shared by every interpreter:
// naudio_hle.cpp (the n_aspMain family: DK64, Banjo-Kazooie) and
// aspmain_hle.cpp (libaudio's ABI 1 aspMain: Mystical Ninja Starring Goemon).
// Each kernel computes exactly what the scalar code it replaces computed:
// the microcode's rounding, its saturation points and its 32-bit wrap are
// all kept, so the interpreters stay bit-exact. Off the ARM (the host tools)
// the primitives are plain C with the same semantics, so tools/naudio_test
// exercises the same kernels.
//
// DMEM is kept in RDRAM byte order (byte a at [a ^ 3]); from a 4-byte
// aligned address, the 32-bit word holds two halfwords, the sample at the
// higher address in its low half (sample index j sits at raw index j ^ 1).
// "Raw" below means that order: a word's low half is the odd sample.
#ifndef RECOMP3DS_AUDIO_HLE_SIMD_H
#define RECOMP3DS_AUDIO_HLE_SIMD_H

#include <cstdint>

#if defined(__ARM_FEATURE_DSP) && defined(__ARM_FEATURE_SIMD32)
#include <arm_acle.h>
#define RECOMP3DS_AHLE_ARM 1
#else
#define RECOMP3DS_AHLE_ARM 0
#endif

namespace recomp3ds::ahle {

typedef uint32_t __attribute__((may_alias)) word_t;

inline int32_t lo16(uint32_t w) { return (int16_t)(w & 0xFFFFu); }
inline int32_t hi16(uint32_t w) { return (int16_t)(w >> 16); }
// (l, h) in one word. GCC does not form PKHBT from the C expression on its
// own (it emits two or three instructions): the hot paths use pkhbt().
inline uint32_t pack(int32_t l, int32_t h) { return ((uint32_t)l & 0xFFFFu) | ((uint32_t)h << 16); }

// The products are 16 x 16 bits signed; the sums wrap at 32 bits as the
// instructions' do (they set the Q flag on overflow, which nothing reads).
#if RECOMP3DS_AHLE_ARM
inline int32_t smlabb(uint32_t a, uint32_t b, int32_t acc) { return __smlabb((int32_t)a, (int32_t)b, acc); }
inline int32_t smlabt(uint32_t a, uint32_t b, int32_t acc) { return __smlabt((int32_t)a, (int32_t)b, acc); }
inline int32_t smlatb(uint32_t a, uint32_t b, int32_t acc) { return __smlatb((int32_t)a, (int32_t)b, acc); }
inline int32_t smlatt(uint32_t a, uint32_t b, int32_t acc) { return __smlatt((int32_t)a, (int32_t)b, acc); }
inline int32_t smlad(uint32_t a, uint32_t b, int32_t acc) { return __smlad((int32_t)a, (int32_t)b, acc); }
inline int32_t sat16(int32_t v) { return __ssat(v, 16); }
// (a's low half, b's low half): one PKHBT.
inline uint32_t pkhbt(int32_t a, int32_t b) {
    uint32_t r;
    __asm__("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
#else
inline int32_t smlabb(uint32_t a, uint32_t b, int32_t acc) { return (int32_t)((uint32_t)(lo16(a) * lo16(b)) + (uint32_t)acc); }
inline int32_t smlabt(uint32_t a, uint32_t b, int32_t acc) { return (int32_t)((uint32_t)(lo16(a) * hi16(b)) + (uint32_t)acc); }
inline int32_t smlatb(uint32_t a, uint32_t b, int32_t acc) { return (int32_t)((uint32_t)(hi16(a) * lo16(b)) + (uint32_t)acc); }
inline int32_t smlatt(uint32_t a, uint32_t b, int32_t acc) { return (int32_t)((uint32_t)(hi16(a) * hi16(b)) + (uint32_t)acc); }
inline uint32_t pkhbt(int32_t a, int32_t b) { return pack(a, b); }
inline int32_t smlad(uint32_t a, uint32_t b, int32_t acc) {
    return (int32_t)((uint32_t)(lo16(a) * lo16(b)) + (uint32_t)(hi16(a) * hi16(b)) + (uint32_t)acc);
}
inline int32_t sat16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }
#endif

// ------------------------------------------------------------------ mixing

// The mixing step of MIXER and the envelope mixers (VMULF by 0x7FFF, then
// VMACF of the input times the gain): out = sat16((out * 0x7FFF + in * g +
// 0x4000) >> 15), the two products summed before the rounding, as the
// accumulator does. Two 16x16 multiply-accumulates a sample, on the halves
// of the words as they are (no unpacking); the sum cannot leave 32 bits
// (|out * 0x7FFF + in * g| < 2^31 for 16-bit values). A gain word holds the
// gains of a word's two samples: low half for its low half.
constexpr uint32_t kK = 0x7FFF;

inline uint32_t mix2(uint32_t o, uint32_t x, uint32_t g) {
    const int32_t l = smlabb(x, g, smlabb(o, kK, 0x4000));
    const int32_t h = smlatt(x, g, smlatb(o, kK, 0x4000));
    return pkhbt(sat16(l >> 15), sat16(h >> 15));
}

// One gain for both samples of a word.
inline uint32_t gain_pair(int32_t g) { return pack(g, g); }

// MIXER: `words` words of out and in (the same or disjoint), one gain.
inline void mix_fixed(word_t* out, const word_t* in, int32_t gain, uint32_t words) {
    const uint32_t g = gain_pair(gain);
    for (uint32_t i = 0; i < words; i++) out[i] = mix2(out[i], in[i], g);
}

// An 8-sample block's gain words (4) from gains by sample (lane) index:
// raw position r is lane r ^ 1, so word m holds lanes 2m + 1 (low half) and
// 2m (high half). Returns whether all eight are the same (a settled ramp),
// when mix_block_uniform can keep the one gain in a register.
inline bool lane_gain_words(const int32_t* by_lane, uint32_t* gw) {
    for (int m = 0; m < 4; m++) gw[m] = pack(by_lane[2 * m + 1], by_lane[2 * m]);
    return gw[0] == gw[1] && gw[1] == gw[2] && gw[2] == gw[3] && lo16(gw[0]) == hi16(gw[0]);
}
// ... from gains already in raw order.
inline bool raw_gain_words(const int32_t* by_raw, uint32_t* gw) {
    for (int m = 0; m < 4; m++) gw[m] = pack(by_raw[2 * m], by_raw[2 * m + 1]);
    return gw[0] == gw[1] && gw[1] == gw[2] && gw[2] == gw[3] && lo16(gw[0]) == hi16(gw[0]);
}

// One 8-sample block (4 words) of a bus: out[m] mixed with x[m] by gw[m].
inline void mix_block(word_t* out, const uint32_t* x, const uint32_t* gw) {
    out[0] = mix2(out[0], x[0], gw[0]);
    out[1] = mix2(out[1], x[1], gw[1]);
    out[2] = mix2(out[2], x[2], gw[2]);
    out[3] = mix2(out[3], x[3], gw[3]);
}
// ... with one gain word for the whole block.
inline void mix_block_uniform(word_t* out, const uint32_t* x, uint32_t g) {
    out[0] = mix2(out[0], x[0], g);
    out[1] = mix2(out[1], x[1], g);
    out[2] = mix2(out[2], x[2], g);
    out[3] = mix2(out[3], x[3], g);
}
// ... either, by the flag lane_gain_words returned.
inline void mix_block_any(word_t* out, const uint32_t* x, const uint32_t* gw, bool uniform) {
    if (uniform) mix_block_uniform(out, x, gw[0]);
    else mix_block(out, x, gw);
}

// --------------------------------------------------------------- resampling

// One output of the 4-tap table resampler: the four input samples x0..x3
// against a coefficient row as DMEM holds it, w0 = (tap 1, tap 0) and
// w1 = (tap 3, tap 2) (raw order). Each product rounds and saturates on its
// own (VMULF: (x * c + 0x4000) >> 15), then the sums saturate pairwise.
// SMLAxy multiplies a register's low half: x0..x3 are sign-extended
// samples.
inline int32_t tap4(int32_t x0, int32_t x1, int32_t x2, int32_t x3, uint32_t w0, uint32_t w1) {
    const int32_t m0 = sat16(smlabt((uint32_t)x0, w0, 0x4000) >> 15);
    const int32_t m1 = sat16(smlabb((uint32_t)x1, w0, 0x4000) >> 15);
    const int32_t m2 = sat16(smlabt((uint32_t)x2, w1, 0x4000) >> 15);
    const int32_t m3 = sat16(smlabb((uint32_t)x3, w1, 0x4000) >> 15);
    return sat16(sat16(m0 + m1) + sat16(m2 + m3));
}

// ------------------------------------------------------------------ VADPCM

// A frame's codebook in the pairs the half-frame decoder multiplies:
// ck[k] = (c1[k], c2[k]) against (older, newer) of the previous outputs,
// r[i] = (c2[i], c2[i - 1]) (r[0] = (c2[0], 0)) against input pairs.
struct AdpcmBook {
    uint32_t ck[8];
    uint32_t r[8];
};

inline void adpcm_book(AdpcmBook& b, const int32_t* c1, const int32_t* c2) {
    for (int k = 0; k < 8; k++) b.ck[k] = pack(c1[k], c2[k]);
    b.r[0] = pack(c2[0], 0);
    for (int i = 1; i < 8; i++) b.r[i] = pack(c2[i], c2[i - 1]);
}

// Eight outputs of a half frame: acc_k = c1[k] * older + c2[k] * newer +
// (s[k] << 11) + sum over j < k of c2[k - 1 - j] * s[j], wrapped to 32 bits,
// then out_k = sat16(acc_k >> 11). The cascade pairs s[j], s[j + 1] against
// (c2[k - 1 - j], c2[k - 2 - j]); its last pair's second coefficient is
// c2[-1] = 0 when k is odd. 24 SMLADs for the 52 products.
inline void adpcm_half(const AdpcmBook& b, const int32_t* s, int32_t older, int32_t newer, int32_t* out) {
    const uint32_t p = pack(older, newer);
    const uint32_t s0 = pack(s[0], s[1]), s2 = pack(s[2], s[3]), s4 = pack(s[4], s[5]), s6 = pack(s[6], s[7]);
    int32_t a[8];
    for (int k = 0; k < 8; k++) a[k] = smlad(p, b.ck[k], (int32_t)((uint32_t)s[k] << 11));
    a[1] = smlad(s0, b.r[0], a[1]);
    a[2] = smlad(s0, b.r[1], a[2]);
    a[3] = smlad(s2, b.r[0], smlad(s0, b.r[2], a[3]));
    a[4] = smlad(s2, b.r[1], smlad(s0, b.r[3], a[4]));
    a[5] = smlad(s4, b.r[0], smlad(s2, b.r[2], smlad(s0, b.r[4], a[5])));
    a[6] = smlad(s4, b.r[1], smlad(s2, b.r[3], smlad(s0, b.r[5], a[6])));
    a[7] = smlad(s6, b.r[0], smlad(s4, b.r[2], smlad(s2, b.r[4], smlad(s0, b.r[6], a[7]))));
    for (int k = 0; k < 8; k++) out[k] = sat16(a[k] >> 11);
}

}   // namespace recomp3ds::ahle

#endif
