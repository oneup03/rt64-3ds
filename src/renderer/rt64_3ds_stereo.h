// Stereo for the top screen: which depth treatment each draw gets and the
// per-eye horizontal shift that places it there. A port of the owner's RT64
// stereo (rt64-3D DK64-3D branch: rt64_projection_processor.cpp,
// rt64_framebuffer_renderer.cpp, rt64_workload_queue.cpp,
// rt64_stereo_depth_sampler.cpp), recast from per-eye matrix edits into the
// clip-space form the vertex shader applies: x' = x + a*w + b, with x and w
// in the record's N64-screen homogeneous space, so a and b are in pixels.
//
// Desktop NDC offsets are fractions of the half screen width; the 3DS top
// screen is 400 px wide, so one NDC unit is 200 px here.
#ifndef RT64_3DS_STEREO_H
#define RT64_3DS_STEREO_H

#include <cstdint>

#include "rt64_3ds.h"
#include "rt64_3ds_record.h"

namespace rt64_3ds {

// The first matching rule of the game's table, then the HUD id range, then
// the game's defaults by projection type. Rectangles are their own class.
StereoClass classify_draw(const DrawRecord& d, const RenderDesc& desc);

// Everything one frame's shifts depend on.
struct StereoFrame {
    bool on = false;
    float sep = 0;           // per-eye separation, NDC (fraction of half width) at infinity
    float conv = 0;          // convergence distance, world units (clip w)
    float hud_offset = 0;    // the eye-independent HUD term before its per-kind scale
    float rect_ndc = 0;      // texture/fill rectangles (eye-independent, NDC)
    float aim_ndc = 0;       // the reticle at the aimed depth (eye-independent, NDC)
    bool reticle = false;    // look for the first-person reticle this frame
};

// The (a, b) for one draw and eye (+1 left, -1 right).
void stereo_shift(const StereoFrame& sf, StereoClass cls, const DrawRecord& d, int eye, float* a, float* b);

// DK64's first-person reticle: an untagged, roughly square orthographic quad
// of 8..64 px near the middle of its scissor. Returns its bounds.
bool is_reticle_quad(const DrawRecord& d, const Vtx3ds* verts, float* minx, float* maxx, float* miny, float* maxy);

// A full-screen rectangle (tints, glare) stays on the screen plane.
bool rect_covers_scissor(const DrawRecord& d, const Vtx3ds* verts);

// Positive view distance of a depth sample z' (= (ndc z + 1) / 2) under the
// world projection's m[2][2] and m[3][2]; <= 0 when invalid.
float view_depth(float zp, float m22, float m32);

// Depth-driven auto-convergence (the desktop's StereoAutoConvergence): keeps
// the nearest significant object's pop-out within the comfort budget by
// pulling convergence in, never beyond the manual value.
struct AutoConvergence {
    static constexpr int kHistory = 9;
    float history[kHistory] = {};
    int count = 0, cursor = 0;
    float z_ema = -1.0f;
    float inv_conv = -1.0f;
    int cut_frames = 0;
    // Returns the convergence to apply, world units.
    float update(float nearest_z, float manual_conv, float sep, int comfort_thousandths, bool low_convergence_scene);
    void reset() { count = cursor = cut_frames = 0; z_ema = inv_conv = -1.0f; }
};

// The near statistic over a grid of depth samples z' (0 < z' < 1 valid): a
// 9x5 patch layout inside the frame minus margins (5% sides and top, 25%
// bottom), the 25th percentile within each patch, then the second nearest
// patch. Returns z' or -1.
float near_depth_statistic(const float* grid, int cols, int rows);

}   // namespace rt64_3ds

#endif
