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
    // CI formats (256 entries) or nullptr.
    BoundTex get(const TexDesc& d, const uint16_t* tlut);
    void end_frame();       // ages entries, frees retired textures
    uint32_t uploads_this_frame() const { return uploads_; }
    uint32_t live() const { return live_; }
    uint32_t bytes() const { return bytes_; }

private:
    struct Impl;
    Impl* impl_;
    const uint8_t* rdram_;
    uint32_t uploads_ = 0, live_ = 0, bytes_ = 0;
};

}   // namespace rt64_3ds

#endif
