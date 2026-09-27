// Texture cache. Texels are read straight from RDRAM through the load
// parameters the interpreter recorded (no TMEM copy), decoded to a PICA
// format, tiled (8x8 Morton, rows flipped so v = 0 is the top texel row) and
// kept in linear memory keyed by their source and content.
#include "rt64_3ds_texture.h"
#include "rt64_3ds_texdecode.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rt64_3ds {

namespace {

constexpr uint32_t kMaxLive = 320;
constexpr uint32_t kMaxBytes = 8u * 1024 * 1024;

struct Entry {
    C3D_Tex tex{};
    uint32_t potw = 0, poth = 0, bytes = 0;
    uint32_t last_frame = 0, checked_frame = 0;
    uint32_t content = 0;
};

inline uint8_t rb(const uint8_t* rdram, uint32_t a) { return rdram[(a & 0xFFFFFF) ^ 3]; }
inline uint16_t rh(const uint8_t* rdram, uint32_t a) { return *(const uint16_t*)(rdram + ((a & 0xFFFFFE) ^ 2)); }
inline uint32_t rw(const uint8_t* rdram, uint32_t a) { return *(const uint32_t*)(rdram + (a & 0xFFFFFC)); }

uint32_t pot(uint32_t v) {
    uint32_t p = 8;
    while (p < v) p <<= 1;
    return p;
}

inline uint16_t ia_to_rgba5551(uint8_t i, uint8_t a) {
    uint16_t c = (uint16_t)(i >> 3);
    return (uint16_t)((c << 11) | (c << 6) | (c << 1) | (a ? 1 : 0));
}

// 8x8 Morton tiling of 16-bit texels, with the row flip.
void swizzle16(uint16_t* dst, const uint16_t* src, int w, int h) {
    int tilesX = w >> 3;
    for (int yd = 0; yd < h; yd++) {
        const uint16_t* row = src + (size_t)(h - 1 - yd) * w;
        uint16_t* tileRow = dst + (size_t)(yd >> 3) * tilesX * 64;
        unsigned my = ((yd & 1) << 1) | ((yd & 2) << 2) | ((yd & 4) << 3);
        for (int x = 0; x < w; x++) {
            tileRow[(x >> 3) * 64 + (my | (x & 1) | ((x & 2) << 1) | ((x & 4) << 2))] = row[x];
        }
    }
}

void swizzle32(uint32_t* dst, const uint32_t* src, int w, int h) {
    int tilesX = w >> 3;
    for (int yd = 0; yd < h; yd++) {
        const uint32_t* row = src + (size_t)(h - 1 - yd) * w;
        uint32_t* tileRow = dst + (size_t)(yd >> 3) * tilesX * 64;
        unsigned my = ((yd & 1) << 1) | ((yd & 2) << 2) | ((yd & 4) << 3);
        for (int x = 0; x < w; x++) {
            tileRow[(x >> 3) * 64 + (my | (x & 1) | ((x & 2) << 1) | ((x & 4) << 2))] = row[x];
        }
    }
}

}   // namespace

struct TextureCache::Impl {
    std::unordered_map<uint64_t, Entry> map;
    std::vector<C3D_Tex> retired[3];
    uint32_t frame = 0;
    std::vector<uint16_t> stage16;
    std::vector<uint32_t> stage32;
};

TextureCache::TextureCache(const uint8_t* rdram) : impl_(new Impl()), rdram_(rdram) {}

BoundTex TextureCache::get(const TexDesc& d, const uint16_t* tlut) {
    BoundTex b;
    if (!d.valid || d.width == 0 || d.height == 0) return b;

    uint32_t bpp_shift = d.siz;                          // bytes per row = width << siz >> 1
    uint32_t row_bytes = ((uint32_t)d.width << bpp_shift) >> 1;
    if (row_bytes == 0) row_bytes = 1;
    // Key on where and how the texels are read; the content hash is checked
    // once per frame per key so a texture the game rewrites in place is
    // picked up without hashing every draw.
    uint64_t key = ((uint64_t)d.addr * 2654435761u) ^ ((uint64_t)d.fmt << 3) ^ ((uint64_t)d.siz << 6) ^
                   ((uint64_t)d.width << 10) ^ ((uint64_t)d.height << 21) ^ ((uint64_t)d.tlut_hash << 40) ^ ((uint64_t)d.palette << 56) ^
                   ((uint64_t)d.pitch << 44) ^ ((uint64_t)d.nibble << 63) ^
                   (d.block ? (((uint64_t)d.dxt << 27) ^ ((uint64_t)d.load_word << 50) ^ (1ull << 62)) : 0);
    auto content_hash = [&]() {
        uint32_t h = 2166136261u;
        for (uint32_t y = 0; y < d.height; y++) {
            uint32_t a = d.addr + y * d.pitch;
            uint32_t end = a + row_bytes;
            // Every fourth word: enough to catch a rewritten texture cheaply.
            for (uint32_t p = a & ~3u; p < end; p += 16) { h ^= rw(rdram_, p); h *= 16777619u; }
            h ^= rw(rdram_, (end - 4) & ~3u); h *= 16777619u;
        }
        return h;
    };

    auto it = impl_->map.find(key);
    if (it != impl_->map.end()) {
        Entry& e = it->second;
        // Re-hash every fourth frame, staggered by key, so a texture the game
        // rewrites in place is noticed within ~130 ms at a quarter of the cost.
        if (e.checked_frame != impl_->frame && ((impl_->frame + (uint32_t)(key >> 12)) & 3) == 0) {
            e.checked_frame = impl_->frame;
            uint32_t h = content_hash();
            if (h != e.content) {
                // Rewritten in place: retire the old texture and decode again.
                impl_->retired[impl_->frame % 3].push_back(e.tex);
                bytes_ -= e.bytes;
                live_--;
                impl_->map.erase(it);
                it = impl_->map.end();
            }
        }
        if (it != impl_->map.end()) {
            e.last_frame = impl_->frame;
            b.tex = &e.tex;
            b.uscale = 1.0f / (float)e.potw;
            b.vscale = 1.0f / (float)e.poth;
            return b;
        }
    }
    uint32_t h = content_hash();

    // Decode.
    uint32_t potw = pot(d.width), poth = pot(d.height);
    bool wide = (d.fmt == 0 && d.siz == 3) || d.fmt == 3 || d.fmt == 4 || (d.fmt == 2 && d.tlut_mode == 3);
    GPU_TEXCOLOR fmt = wide ? GPU_RGBA8 : GPU_RGBA5551;
    uint32_t texel_bytes = wide ? 4 : 2;
    uint32_t bytes = potw * poth * texel_bytes;
    if (wide) impl_->stage32.assign(potw * poth, 0); else impl_->stage16.assign(potw * poth, 0);

    decode_texels(rdram_, d, tlut, [&](uint32_t xt, uint32_t y, bool have16, uint16_t p16, uint32_t r, uint32_t g, uint32_t bl, uint32_t a) {
        if (wide) impl_->stage32[y * potw + xt] = have16 ? rgba5551_to_rgba8(p16) : ((r << 24) | (g << 16) | (bl << 8) | a);
        else impl_->stage16[y * potw + xt] = have16 ? p16 : ia_to_rgba5551((uint8_t)r, (uint8_t)a);
    });
    for (uint32_t y = 0; y < d.height; y++) {
        // Pad the row to the POT width with the last texel (clamping looks right).
        for (uint32_t x = d.width; x < potw; x++) {
            if (wide) impl_->stage32[y * potw + x] = impl_->stage32[y * potw + d.width - 1];
            else impl_->stage16[y * potw + x] = impl_->stage16[y * potw + d.width - 1];
        }
    }
    for (uint32_t y = d.height; y < poth; y++) {
        if (wide) memcpy(&impl_->stage32[y * potw], &impl_->stage32[(d.height - 1) * potw], potw * 4);
        else memcpy(&impl_->stage16[y * potw], &impl_->stage16[(d.height - 1) * potw], potw * 2);
    }

    // TEX_DUMP.TXT next to the executable: write the first decoded textures
    // as PPM files with their descriptions, to check the decoder by eye.
    // The file holds the frame number to start at (0 = from boot).
    static int dump_state = -1, dumped = 0, dump_from = 0;
    if (dump_state < 0) {
        FILE* f = fopen("sdmc:/3ds/DK64/TEX_DUMP.TXT", "r");
        dump_state = f ? 1 : 0;
        if (f) { if (fscanf(f, "%d", &dump_from) != 1) dump_from = 0; fclose(f); }
    }
    if (dump_state == 1 && dumped < 40 && (int)impl_->frame >= dump_from) {
        char path[96];
        snprintf(path, sizeof(path), "sdmc:/3ds/DK64/tex_%02d.ppm", dumped);
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", (unsigned)d.width, (unsigned)d.height);
            for (uint32_t y = 0; y < d.height; y++) for (uint32_t x = 0; x < d.width; x++) {
                uint8_t px[3];
                if (wide) { uint32_t v = impl_->stage32[y * potw + x]; px[0] = v >> 24; px[1] = v >> 16; px[2] = v >> 8; }
                else { uint16_t v = impl_->stage16[y * potw + x]; px[0] = ((v >> 11) & 31) * 8; px[1] = ((v >> 6) & 31) * 8; px[2] = ((v >> 1) & 31) * 8; }
                fwrite(px, 1, 3, f);
            }
            fclose(f);
        }
        fprintf(stderr, "texdump %02d: addr %06x pitch %u fmt %u siz %u %ux%u masks %u/%u cm %u/%u pal %u tlut %u\n", dumped,
                d.addr, d.pitch, d.fmt, d.siz, d.width, d.height, d.masks, d.maskt, d.cms, d.cmt, d.palette, d.tlut_mode);
        dumped++;
    }

    Entry e;
    if (!C3D_TexInit(&e.tex, (u16)potw, (u16)poth, fmt)) {
        fprintf(stderr, "rt64-3ds: C3D_TexInit %ux%u failed\n", (unsigned)potw, (unsigned)poth);
        return b;
    }
    if (wide) swizzle32((uint32_t*)e.tex.data, impl_->stage32.data(), (int)potw, (int)poth);
    else swizzle16((uint16_t*)e.tex.data, impl_->stage16.data(), (int)potw, (int)poth);
    GSPGPU_FlushDataCache(e.tex.data, bytes);
    e.potw = potw; e.poth = poth; e.bytes = bytes; e.last_frame = impl_->frame; e.checked_frame = impl_->frame; e.content = h;
    uploads_++;
    live_++;
    bytes_ += bytes;
    auto ins = impl_->map.emplace(key, e);
    b.tex = &ins.first->second.tex;
    b.fresh = true;
    b.uscale = 1.0f / (float)potw;
    b.vscale = 1.0f / (float)poth;
    return b;
}

void TextureCache::end_frame() {
    uploads_ = 0;
    impl_->frame++;
    // Free textures retired three frames ago (the GPU is done with them).
    int slot = impl_->frame % 3;
    for (C3D_Tex& t : impl_->retired[slot]) C3D_TexDelete(&t);
    impl_->retired[slot].clear();
    if (live_ > kMaxLive || bytes_ > kMaxBytes) {
        // Retire everything not used in the last 2 frames.
        for (auto it = impl_->map.begin(); it != impl_->map.end();) {
            if (it->second.last_frame + 2 < impl_->frame) {
                impl_->retired[slot].push_back(it->second.tex);
                bytes_ -= it->second.bytes;
                live_--;
                it = impl_->map.erase(it);
            }
            else ++it;
        }
    }
}

}   // namespace rt64_3ds
