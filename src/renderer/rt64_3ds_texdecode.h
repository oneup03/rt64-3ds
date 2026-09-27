// RDP texel decoding straight from RDRAM, shared by the texture cache and
// the host reference renderer. RDRAM is stored as 32-bit little-endian words
// (byte address a lives at a ^ 3, halfwords at a ^ 2).
#ifndef RT64_3DS_TEXDECODE_H
#define RT64_3DS_TEXDECODE_H

#include <cstdint>

#include "rt64_3ds_record.h"

namespace rt64_3ds {

inline uint8_t tex_rb(const uint8_t* rdram, uint32_t a) { return rdram[(a & 0xFFFFFF) ^ 3]; }
inline uint16_t tex_rh(const uint8_t* rdram, uint32_t a) { return *(const uint16_t*)(rdram + ((a & 0xFFFFFE) ^ 2)); }
inline uint32_t tex_rw(const uint8_t* rdram, uint32_t a) { return *(const uint32_t*)(rdram + (a & 0xFFFFFC)); }

// Calls sink(x, y, have16, p16, r, g, b, a) for every texel of the tile:
// have16 = the texel is an exact RGBA5551 value (RGBA16, CI with an RGBA16
// palette) in p16; otherwise r, g, b, a hold 8-bit channels.
template <class Sink>
void decode_texels(const uint8_t* rdram, const TexDesc& d, const uint16_t* pal, Sink&& sink) {
    const uint32_t row_words = d.pitch / 8;               // load iterations per texel row
    const uint32_t swap_bytes = (d.siz == 3) ? 8 : 4;     // 32-bit texels sit in two TMEM halves
    for (uint32_t y = 0; y < d.height; y++) {
        const uint32_t row = d.addr + y * d.pitch;
        // Odd-row interleave: the sampler swaps the 32-bit halves of each
        // TMEM word on odd tile rows; a block load swapped the rows its own
        // dxt counter called odd (sampled at the row's middle word). Rows
        // where the two disagree read swapped (dxt 0: pre-interleaved data).
        uint32_t sw = 0;
        if (d.block) {
            uint32_t load_odd = (((uint32_t)d.load_word + y * row_words + row_words / 2) * d.dxt >> 11) & 1;
            if (load_odd != (y & 1)) sw = swap_bytes;
        }
        auto at = [&](uint32_t off) { return row + (off ^ sw); };
        for (uint32_t xt = 0; xt < d.width; xt++) {
            uint32_t x = xt + d.nibble;   // 4-bit formats may start mid-byte
            uint32_t r = 0, g = 0, bl = 0, a = 255;
            uint16_t p16 = 0;
            bool have16 = false;
            switch ((d.fmt << 2) | d.siz) {
                case (0 << 2) | 2: p16 = tex_rh(rdram, at(x * 2)); have16 = true; break;                 // RGBA16
                case (0 << 2) | 3: { uint32_t v = tex_rw(rdram, at(x * 4)); r = v >> 24; g = (v >> 16) & 0xFF; bl = (v >> 8) & 0xFF; a = v & 0xFF; break; }  // RGBA32
                case (2 << 2) | 0: {                                                                     // CI4
                    uint8_t byte = tex_rb(rdram, at(x / 2));
                    uint8_t idx = (x & 1) ? (byte & 0xF) : (byte >> 4);
                    idx |= (d.palette & 0xF) << 4;
                    p16 = pal ? pal[idx] : 0; have16 = true;
                    if (d.tlut_mode == 3) { have16 = false; r = g = bl = p16 >> 8; a = p16 & 0xFF; }
                    break;
                }
                case (2 << 2) | 1: {                                                                     // CI8
                    uint8_t idx = tex_rb(rdram, at(x));
                    p16 = pal ? pal[idx] : 0; have16 = true;
                    if (d.tlut_mode == 3) { have16 = false; r = g = bl = p16 >> 8; a = p16 & 0xFF; }
                    break;
                }
                case (3 << 2) | 0: {                                                                     // IA4
                    uint8_t byte = tex_rb(rdram, at(x / 2));
                    uint8_t v = (x & 1) ? (byte & 0xF) : (byte >> 4);
                    uint8_t i = (v >> 1) * 255 / 7;
                    r = g = bl = i; a = (v & 1) ? 255 : 0;
                    break;
                }
                case (3 << 2) | 1: {                                                                     // IA8
                    uint8_t v = tex_rb(rdram, at(x));
                    r = g = bl = (v >> 4) * 17; a = (v & 0xF) * 17;
                    break;
                }
                case (3 << 2) | 2: {                                                                     // IA16
                    uint16_t v = tex_rh(rdram, at(x * 2));
                    r = g = bl = v >> 8; a = v & 0xFF;
                    break;
                }
                case (4 << 2) | 0: {                                                                     // I4
                    uint8_t byte = tex_rb(rdram, at(x / 2));
                    uint8_t v = (x & 1) ? (byte & 0xF) : (byte >> 4);
                    r = g = bl = a = v * 17;
                    break;
                }
                case (4 << 2) | 1: {                                                                     // I8
                    uint8_t v = tex_rb(rdram, at(x));
                    r = g = bl = a = v;
                    break;
                }
                default:
                    p16 = 0xFFFF; have16 = true;
                    break;
            }
            sink(xt, y, have16, p16, r, g, bl, a);
        }
    }
}

inline uint32_t rgba5551_to_rgba8(uint16_t p) {   // 0xRRGGBBAA
    uint32_t r = ((p >> 11) & 31) * 255 / 31, g = ((p >> 6) & 31) * 255 / 31, b = ((p >> 1) & 31) * 255 / 31, a = (p & 1) ? 255 : 0;
    return (r << 24) | (g << 16) | (b << 8) | a;
}

// Whole tile to 0xRRGGBBAA texels (width * height).
inline void decode_rgba8(const uint8_t* rdram, const TexDesc& d, const uint16_t* pal, uint32_t* out) {
    decode_texels(rdram, d, pal, [&](uint32_t x, uint32_t y, bool have16, uint16_t p16, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
        out[y * d.width + x] = have16 ? rgba5551_to_rgba8(p16) : ((r << 24) | (g << 16) | (b << 8) | a);
    });
}

}   // namespace rt64_3ds

#endif
