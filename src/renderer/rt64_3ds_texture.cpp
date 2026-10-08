// Texture cache. Texels are read straight from RDRAM through the load
// parameters the interpreter recorded (no TMEM copy), decoded to a PICA
// format, tiled (8x8 Morton, rows flipped so v = 0 is the top texel row) and
// kept in linear memory keyed by their source and content.
#include "rt64_3ds_texture.h"
#include "rt64_3ds_texdecode.h"
#include "rt64_3ds.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rt64_3ds {

namespace {

constexpr uint32_t kMaxLive = 320;
constexpr uint32_t kMaxBytes = 8u * 1024 * 1024;

}   // namespace

struct Entry {
    C3D_Tex tex{};
    uint32_t potw = 0, poth = 0, bytes = 0;
    uint32_t last_frame = 0, checked_frame = 0;
    uint32_t content = 0;
    bool vram = false;          // tex.data is in VRAM
    bool vram_pending = false;  // a copy to VRAM waits for upload_pending
};

namespace {

// TextureCache::set_vram: how many copies go a frame.
constexpr size_t kMaxUploadsPerFrame = 8;

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
    // set_vram: copies waiting for upload_pending (the entry's key, its
    // linear texels and the VRAM block for them), and the linear copies of
    // those queued at the last upload_pending.
    struct Upload { uint64_t key; void* src; void* dst; uint32_t bytes; };
    bool vram = false;
    uint32_t vram_reserve = 0;
    std::vector<Upload> pending;
    std::vector<void*> queued_src;
    uint32_t swept_frame = ~0u;
    std::vector<uint16_t> stage16;
    std::vector<uint32_t> stage32;
};

TextureCache::TextureCache(const uint8_t* rdram) : impl_(new Impl()), rdram_(rdram) {}

BoundTex TextureCache::get(const TexDesc& d, const uint16_t* tlut, const TexDesc* mips) {
    BoundTex b;
    if (!d.valid || d.width == 0 || d.height == 0) return b;
    const uint32_t levels = (mipmaps_ && mips != nullptr && d.levels > 1) ? d.levels : 1;

    uint32_t bpp_shift = d.siz;                          // bytes per row = width << siz >> 1
    uint32_t row_bytes = ((uint32_t)d.width << bpp_shift) >> 1;
    if (row_bytes == 0) row_bytes = 1;
    // Key on where and how the texels are read, plus a fingerprint of 16
    // words from the texture's interior: games load textures into heap
    // blocks they free and reuse (DK64's animated torch flames move between
    // 4 KB blocks every few frames), so one address and format can hold a
    // different texture from one frame to the next. The fuller content hash
    // below still catches in-place edits the fingerprint misses, every
    // fourth frame.
    auto mix = [](uint64_t h, uint64_t v) {
        h ^= v;
        h *= 0xFF51AFD7ED558CCDull;
        return h ^ (h >> 33);
    };
    // The rows really read from RDRAM (TMEM may wrap: TexDesc::wrap_rows).
    const uint32_t rows_read = (d.wrap_rows != 0 && d.wrap_rows < d.height) ? d.wrap_rows : d.height;
    uint32_t fp = 2166136261u;
    for (uint32_t j = 0; j < 4; j++) {
        const uint32_t row = d.addr + (2 * j + 1) * rows_read / 8 * d.pitch;
        for (uint32_t i = 0; i < 4; i++) {
            fp ^= rw(rdram_, row + (2 * i + 1) * row_bytes / 8);
            fp *= 16777619u;
        }
    }
    uint64_t key = mix(0, d.addr);
    key = mix(key, (uint64_t)d.fmt | ((uint64_t)d.siz << 4) | ((uint64_t)d.width << 8) | ((uint64_t)d.height << 24) | ((uint64_t)d.pitch << 40));
    key = mix(key, (uint64_t)d.tlut_hash | ((uint64_t)d.palette << 32) | ((uint64_t)d.nibble << 40) | ((uint64_t)d.block << 48));
    if (d.block) key = mix(key, (uint64_t)d.dxt | ((uint64_t)d.load_word << 32));
    if (d.wrap_rows != 0) key = mix(key, d.wrap_rows);
    for (uint32_t k = 1; k < levels; k++) key = mix(key, (uint64_t)mips[k - 1].addr | ((uint64_t)mips[k - 1].load_word << 32) | ((uint64_t)k << 48));
    key = mix(key, fp);
    auto content_hash = [&]() {
        // A fixed sample: up to 8 rows spread over the texture, up to 8 words
        // spread over each, plus the row's last word. A texture reloaded into
        // reused memory or animated in place changes far more than that;
        // hashing every fourth word cost up to ~9 ms a frame on the console.
        uint32_t h = 2166136261u;
        const uint32_t rows = rows_read < 8 ? rows_read : 8;
        const uint32_t words = (row_bytes + 3) / 4;
        const uint32_t cols = words < 8 ? words : 8;
        for (uint32_t i = 0; i < rows; i++) {
            uint32_t y = rows > 1 ? i * (rows_read - 1) / (rows - 1) : 0;
            uint32_t a = (d.addr + y * d.pitch) & ~3u;
            for (uint32_t j = 0; j < cols; j++) {
                uint32_t w = cols > 1 ? j * (words - 1) / (cols - 1) : 0;
                h ^= rw(rdram_, a + w * 4); h *= 16777619u;
            }
            h ^= rw(rdram_, (d.addr + y * d.pitch + row_bytes - 4) & ~3u); h *= 16777619u;
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
                if (e.vram) vram_bytes_ -= e.bytes;
                bytes_ -= e.bytes;
                live_--;
                impl_->map.erase(it);
                it = impl_->map.end();
            }
        }
        if (it != impl_->map.end()) {
            e.last_frame = impl_->frame;
            if (impl_->vram && !e.vram && !e.vram_pending) to_vram(key, e);
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
    // Intensity texels (r = g = b from the decoder) as LA8: L in the high
    // byte, A in the low one; the PICA samples (L, L, L, A), exactly what
    // RGBA8 held (set_compact_intensity).
    const bool la8 = compact_ && ((d.fmt == 3 && d.siz <= 2) || (d.fmt == 4 && d.siz <= 1) || (d.fmt == 2 && d.tlut_mode == 3 && d.siz <= 1));
    if (la8) wide = false;
    GPU_TEXCOLOR fmt = wide ? GPU_RGBA8 : la8 ? GPU_LA8 : GPU_RGBA5551;
    // Decodes a level into the stage, rows `stride` texels apart.
    auto decode_level = [&](const TexDesc& ld, uint32_t stride) {
        decode_texels(rdram_, ld, tlut, [&](uint32_t xt, uint32_t y, bool have16, uint16_t p16, uint32_t r, uint32_t g, uint32_t bl, uint32_t a) {
            if (wide) impl_->stage32[y * stride + xt] = have16 ? rgba5551_to_rgba8(p16) : ((r << 24) | (g << 16) | (bl << 8) | a);
            else if (la8) impl_->stage16[y * stride + xt] = have16 ? (uint16_t)0xFFFF : (uint16_t)((r << 8) | a);
            else impl_->stage16[y * stride + xt] = have16 ? p16 : ia_to_rgba5551((uint8_t)r, (uint8_t)a);
        });
    };
    if (wide) impl_->stage32.assign(potw * poth, 0); else impl_->stage16.assign(potw * poth, 0);
    decode_level(d, potw);
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
    // The file holds the frame number to start at (0 = from boot), and
    // optionally an RDRAM address (hex) to dump only that texture's decodes.
    static int dump_state = -1, dumped = 0, dump_from = 0;
    static unsigned dump_addr = 0;
    if (dump_state < 0) {
        FILE* f = rt64_3ds::data_fopen("TEX_DUMP.TXT", "r");
        dump_state = f ? 1 : 0;
        if (f) { if (fscanf(f, "%d %x", &dump_from, &dump_addr) < 1) dump_from = 0; fclose(f); }
    }
    if (dump_state == 1 && dumped < 40 && (int)impl_->frame >= dump_from && (dump_addr == 0 || (d.addr & 0xFFFFFF) == (dump_addr & 0xFFFFFF))) {
        char path[96];
        char name[24];
        snprintf(name, sizeof(name), "tex_%02d.ppm", dumped);
        rt64_3ds::data_path(path, sizeof(path), name);
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", (unsigned)d.width, (unsigned)d.height);
            for (uint32_t y = 0; y < d.height; y++) for (uint32_t x = 0; x < d.width; x++) {
                uint8_t px[3];
                if (wide) { uint32_t v = impl_->stage32[y * potw + x]; px[0] = v >> 24; px[1] = v >> 16; px[2] = v >> 8; }
                else if (la8) { uint16_t v = impl_->stage16[y * potw + x]; px[0] = px[1] = px[2] = (uint8_t)(v >> 8); }
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
    if (!C3D_TexInitWithParams(&e.tex, nullptr, C3D_TexInitParams{ (u16)potw, (u16)poth, (u8)(levels - 1), fmt, GPU_TEX_2D, false })) {
        fprintf(stderr, "rt64-3ds: C3D_TexInit %ux%u (%u levels) failed\n", (unsigned)potw, (unsigned)poth, (unsigned)levels);
        return b;
    }
    const uint32_t bytes = C3D_TexCalcTotalSize(e.tex.size, levels - 1);
    if (wide) swizzle32((uint32_t*)e.tex.data, impl_->stage32.data(), (int)potw, (int)poth);
    else swizzle16((uint16_t*)e.tex.data, impl_->stage16.data(), (int)potw, (int)poth);
    // Mipmaps: each level from the game's own (TexDesc::levels; the
    // interpreter checked each is exactly half the one before, so none
    // needs padding), after the base in the texture's memory.
    for (uint32_t k = 1; k < levels; k++) {
        const uint32_t lw = potw >> k, lh = poth >> k;
        decode_level(mips[k - 1], lw);
        void* dst = C3D_TexGetImagePtr(&e.tex, e.tex.data, (int)k, nullptr);
        if (wide) swizzle32((uint32_t*)dst, impl_->stage32.data(), (int)lw, (int)lh);
        else swizzle16((uint16_t*)dst, impl_->stage16.data(), (int)lw, (int)lh);
    }
    // The kernel's flush on this core (GSPGPU_FlushDataCache is a request to
    // the GSP module on core 1, which waits for that core).
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)e.tex.data, bytes);
    e.potw = potw; e.poth = poth; e.bytes = bytes; e.last_frame = impl_->frame; e.checked_frame = impl_->frame; e.content = h;
    uploads_++;
    live_++;
    bytes_ += bytes;
    auto ins = impl_->map.emplace(key, e);
    if (impl_->vram) to_vram(key, ins.first->second);
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
                if (it->second.vram) vram_bytes_ -= it->second.bytes;
                bytes_ -= it->second.bytes;
                live_--;
                it = impl_->map.erase(it);
            }
            else ++it;
        }
    }
}

uint32_t TextureCache::mipmapped() const {
    uint32_t n = 0;
    for (const auto& [key, e] : impl_->map) if (e.tex.maxLevel > 0) n++;
    return n;
}

void TextureCache::set_vram(bool on, uint32_t reserve) { impl_->vram = on; impl_->vram_reserve = reserve; }

// A texture's copy to VRAM, for the next upload_pending (texels stay where
// they are until it is queued). Short of room: textures in VRAM unused for
// a second make way (retired: freed three frames on), for a later turn.
void TextureCache::to_vram(uint64_t key, Entry& e) {
    if (vramSpaceFree() < impl_->vram_reserve + e.bytes) { sweep_vram(); return; }
    void* dst = vramAlloc(e.bytes);
    if (dst == nullptr) return;
    impl_->pending.push_back(Impl::Upload{ key, e.tex.data, dst, e.bytes });
    e.vram_pending = true;
}

void TextureCache::sweep_vram() {
    if (impl_->swept_frame == impl_->frame) return;
    impl_->swept_frame = impl_->frame;
    const int slot = impl_->frame % 3;
    for (auto it = impl_->map.begin(); it != impl_->map.end();) {
        Entry& e = it->second;
        if (e.vram && e.last_frame + 30 < impl_->frame) {
            impl_->retired[slot].push_back(e.tex);
            vram_bytes_ -= e.bytes;
            bytes_ -= e.bytes;
            live_--;
            it = impl_->map.erase(it);
        }
        else ++it;
    }
}

void TextureCache::make_vram_room(uint32_t bytes) {
    if (impl_->vram_reserve < bytes + 128u * 1024) impl_->vram_reserve = bytes + 128u * 1024;
    std::vector<std::pair<uint32_t, uint64_t>> old;     // last use, key
    for (const auto& [key, e] : impl_->map) {
        if (e.vram) old.push_back({ e.last_frame, key });    // retired textures live 3 frames on
    }
    std::sort(old.begin(), old.end());
    const int slot = impl_->frame % 3;
    uint32_t freed = 0;
    for (const auto& [last, key] : old) {
        if (freed >= bytes) break;
        auto it = impl_->map.find(key);
        Entry& e = it->second;
        impl_->retired[slot].push_back(e.tex);
        freed += e.bytes;
        vram_bytes_ -= e.bytes;
        bytes_ -= e.bytes;
        live_--;
        impl_->map.erase(it);
    }
    fprintf(stderr, "rt64-3ds: VRAM for a render target: %u KB of textures leave VRAM, %u KB kept free from now on\n",
            (unsigned)(freed / 1024), (unsigned)(impl_->vram_reserve / 1024));
}

void TextureCache::upload_pending() {
    for (void* p : impl_->queued_src) linearFree(p);
    impl_->queued_src.clear();
    size_t n = 0;
    for (; n < impl_->pending.size() && n < kMaxUploadsPerFrame; n++) {
        const Impl::Upload& u = impl_->pending[n];
        auto it = impl_->map.find(u.key);
        if (it == impl_->map.end() || it->second.tex.data != u.src) {
            // Retired meanwhile (its retired copy frees the linear texels).
            vramFree(u.dst);
            continue;
        }
        // Raw copy (texture-copy mode, flags 8), as C3D_TexLoadImage does.
        C3D_SyncTextureCopy((u32*)u.src, 0, (u32*)u.dst, 0, u.bytes, 8);
        Entry& e = it->second;
        e.tex.data = u.dst;
        e.vram = true;
        e.vram_pending = false;
        vram_bytes_ += e.bytes;
        impl_->queued_src.push_back(u.src);
    }
    impl_->pending.erase(impl_->pending.begin(), impl_->pending.begin() + (ptrdiff_t)n);
}

}   // namespace rt64_3ds
