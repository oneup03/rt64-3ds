// Texture cache: N64 texel data in RDRAM -> PICA textures.
#ifndef RT64_3DS_TEXTURE_H
#define RT64_3DS_TEXTURE_H

#include <3ds.h>
#include <citro3d.h>
#include <cstdint>

#include "rt64_3ds_record.h"

namespace rt64_3ds {

struct BoundTex {
    C3D_Tex* tex = nullptr;
    float uscale = 1.0f, vscale = 1.0f;   // texels -> normalized
    // Created by this call: the C3D_Tex may sit at the address of one freed
    // earlier this frame, so a pointer comparison cannot skip its bind.
    bool fresh = false;
};

class TextureCache {
public:
    explicit TextureCache(const uint8_t* rdram);
    // Finds or decodes the texture a draw samples; `tlut` is the palette for
    // CI formats (256 entries) or nullptr; `mips` the sources of its levels
    // past the base (d.levels - 1 of them, FrameRecord::mips) or nullptr.
    BoundTex get(const TexDesc& d, const uint16_t* tlut, const TexDesc* mips = nullptr);
    // Mipmaps (TexDesc::levels): on by default; off, a mipmapped draw's
    // texture is its base level alone.
    void set_mipmaps(bool on) { mipmaps_ = on; }
    bool mipmaps() const { return mipmaps_; }
    uint32_t mipmapped() const;     // live textures with more than one level
    void end_frame();       // ages entries, frees retired textures
    uint32_t uploads_this_frame() const { return uploads_; }
    // RenderDesc::intensity_textures16: I and IA textures (and CI ones with
    // an IA16 palette) as LA8, 16 bits a texel, instead of RGBA8. Set once,
    // before the first texture.
    void set_compact_intensity(bool on) { compact_ = on; }
    bool compact_intensity() const { return compact_; }
    uint32_t live() const { return live_; }
    uint32_t bytes() const { return bytes_; }
    // Textures in VRAM, where the GPU reads them faster than from the
    // linear heap: decoded there as ever, then copied by the GPU. A texture
    // goes over when its copy is queued (upload_pending), as room allows
    // (reserve: VRAM left for render targets made later; textures unused
    // for a while make way); until then it is read where it is.
    void set_vram(bool on, uint32_t reserve);
    // On the gfx thread, out of a frame of citro3d's (before C3D_FrameBegin):
    // runs a few of the waiting copies (C3D_SyncTextureCopy, each on a queue
    // of its own: a frame's GX queue takes 32 entries, and the frames that
    // render to textures come near that), and frees the linear copies of
    // those run at the last call (a frame has begun since: the GPU is done
    // with them, and with what was recorded reading them).
    void upload_pending();
    // A render target could not be made for want of VRAM: keep `bytes` more
    // free from now on, and retire textures in VRAM, least recently used
    // first, to give it back (freed three frames on, as any retired texture).
    void make_vram_room(uint32_t bytes);
    uint32_t vram_bytes() const { return vram_bytes_; }

private:
    struct Impl;
    Impl* impl_;
    const uint8_t* rdram_;
    uint32_t uploads_ = 0, live_ = 0, bytes_ = 0, vram_bytes_ = 0;
    void to_vram(uint64_t key, struct Entry& e);
    void sweep_vram();
    bool compact_ = false;
    bool mipmaps_ = true;
};

}   // namespace rt64_3ds

#endif
