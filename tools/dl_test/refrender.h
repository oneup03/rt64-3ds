// Host reference renderer for captured frames (tools only).
#ifndef RT64_3DS_REFRENDER_H
#define RT64_3DS_REFRENDER_H

#include <cstdint>

#include "rt64_3ds_record.h"

struct RefOptions {
    int only_draw = -1;          // render just this draw
    int max_draw = -1;           // stop after this draw
    int lod_fraction = 0;        // LOD_FRACTION combiner input (mipmap blend)
    bool rect_shade_white = false;
};

bool ref_render(const uint8_t* rdram, const rt64_3ds::FrameRecord& frame, const RefOptions& opt, const char* ppm_path);
bool dump_texture(const uint8_t* rdram, const rt64_3ds::FrameRecord& frame, const rt64_3ds::TexDesc& d, const char* ppm_path);

#endif
