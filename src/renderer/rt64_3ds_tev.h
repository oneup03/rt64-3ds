// RDP colour combiner -> PICA texture environment stages.
#ifndef RT64_3DS_TEV_H
#define RT64_3DS_TEV_H

#include <citro3d.h>
#include <cstdint>

#include "rt64_3ds_record.h"

namespace rt64_3ds {

struct TevStage {
    GPU_TEVSRC src_rgb[3], src_a[3];
    GPU_TEVOP_RGB op_rgb[3];
    GPU_TEVOP_A op_a[3];
    GPU_COMBINEFUNC func_rgb, func_a;
    uint32_t constant;      // RGBA8 as citro3d expects (0xAABBGGRR)
};

struct TevPlan {
    int stages = 0;
    TevStage stage[6];
    int combiner_stages = 0;    // the combiner's own; fog and blend-factor stages follow
    uint32_t buffer_color = 0;  // the second constant, read as PREVIOUS_BUFFER
    int fallbacks = 0;          // constants that could not be expressed exactly
};

// Plans the combiner of a draw (both cycles when in 2-cycle mode), plus the
// fog blend stage when the blender asks for it.
void plan_tev(const DrawRecord& d, TevPlan& plan);
void apply_tev(const TevPlan& plan);

}   // namespace rt64_3ds

#endif
