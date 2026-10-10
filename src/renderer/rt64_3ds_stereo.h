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

#include <cstddef>
#include <cstdint>

#include "rt64_3ds.h"
#include "rt64_3ds_record.h"

namespace rt64_3ds {

// The first matching rule of the game's table, then the HUD id range, then
// the game's defaults by projection type. Rectangles are their own class
// unless a ProjKind::Rect rule names their projection group.
StereoClass classify_draw(const DrawRecord& d, const RenderDesc& desc);

// Everything one frame's shifts depend on.
struct StereoFrame {
    bool on = false;
    float sep = 0;           // per-eye separation, NDC (fraction of half width) at infinity
    float conv = 0;          // convergence distance, world units (clip w)
    float hud_offset = 0;    // the eye-independent HUD term before its per-kind scale
    float rect_ndc = 0;      // texture/fill rectangles (eye-independent, NDC)
    float aim_ndc = 0;       // the reticle at the aimed depth (eye-independent, NDC)
    float px_scale = 0;      // screen pixels per N64 unit when HUD shifts snap to pixels (RenderDesc::snap_hud_shift), else 0
    bool reticle = false;    // look for the first-person reticle this frame
    // The world wrote no depth last frame (menus drawn from rectangles,
    // such as DK64's file select): its stray world draws, like a shadow
    // under the camera, would otherwise float out of the screen.
    bool world_empty = false;
};

// The (a, b) for one draw and eye (+1 left, -1 right).
void stereo_shift(const StereoFrame& sf, StereoClass cls, const DrawRecord& d, int eye, float* a, float* b);

// DK64's first-person reticle: an untagged, roughly square orthographic quad
// of 8..64 px near the middle of its scissor. Returns its bounds.
bool is_reticle_quad(const DrawRecord& d, const Vtx3ds* verts, float* minx, float* maxx, float* miny, float* maxy);

// A full-screen rectangle (tints, glare) stays on the screen plane.
bool rect_covers_scissor(const DrawRecord& d, const Vtx3ds* verts);

// A frame's eye-independent HUD terms (StereoFrame::hud_offset, rect_ndc)
// for a HUD depth other than 50, the 3D slider's position and the panel gain.
void set_hud_terms(StereoFrame& sf, int hud_depth, float slider, float slider_gain);

// RenderDesc::infinity_rect_cover_zoom: the scale that keeps a backdrop
// covering the screen when it is shifted by sep (per eye, NDC) - 1 + the
// shift over the half width - and its application to one rectangle's
// vertices, about the frame's centre (so the slices of one backdrop stay
// together) and in both axes (the scissor clips the excess height).
float cover_zoom_factor(float sep, const ScreenGeometry& geo);
void cover_zoom(const DrawRecord& d, Vtx3ds* verts, float factor, const ScreenGeometry& geo);

// RenderDesc::hud_camera_id: whether the HUD camera is this frame's world,
// i.e. its full-width perspective triangle draws outnumber the full-width
// perspective draws placed as World (the desktop fork never takes the
// frame's main world projection, the full-width perspective with the most
// draws, for a HUD camera). False when no HUD camera is named.
bool hud_camera_is_world(const DrawRecord* draws, size_t count, const RenderDesc& desc, int frame_w);

// RenderDesc::primary_world_instance: the frame's primary world projection
// instance (DrawRecord::proj_inst), 0 = none: of the full-width perspective
// triangle records outside images that are placed as World or drawn under
// the HUD camera, the instance with the most triangles (the first one on a
// tie; the desktop counts draw calls, see the .cpp). A HUD-camera instance
// counts only with more triangles than the frame's World draws under any
// scissor. *hud_cam says whether it is a HUD-camera instance. The table, when
// given, receives up to table_max instances for logging.
struct InstanceCount { uint32_t inst = 0, proj_id = 0, records = 0, tris = 0; };
uint32_t primary_world_instance(const DrawRecord* draws, size_t count, const RenderDesc& desc, int frame_w, bool* hud_cam,
                                InstanceCount* table = nullptr, int table_max = 0, int* table_count = nullptr);

// A rectangle covering the whole frame (N64 units, either axis).
bool rect_covers_frame(const DrawRecord& d, const Vtx3ds* verts, int frame_w, int frame_h);

// RenderDesc::stretch_tint_quads: a screen tint drawn as triangles (Goemon's
// Impact battle flashes and smoke: one quad under a perspective camera,
// sized to the 4:3 frame, its edges up to 10 pixels inside it where the TV's
// overscan hid them) is stretched to cover the wide screen. The draw
// qualifies when it is at most two triangles, untextured, on the screen, a
// screen-aligned rectangle (every vertex at a corner of its bounds) and
// covers the frame to within kTintQuadMargin N64 pixels on every side, but
// not yet the wide screen (so a second call leaves it be). A textured one
// qualifies when its textures repeat both ways (an overlay pattern, such as
// the battles' smoke, not a picture), and keeps its texel size: the pattern
// continues into the added area. Its bounds then go one pixel past the wide
// screen's sides, and an edge inside the frame's top or bottom one past that
// edge. Returns whether it was stretched.
constexpr float kTintQuadMargin = 12.0f;
bool stretch_tint_quad(const DrawRecord& d, Vtx3ds* verts, const ScreenGeometry& geo);

// Positive view distance of a depth sample z' (= (ndc z + 1) / 2) under the
// world projection's m[2][2] and m[3][2]; <= 0 when invalid.
float view_depth(float zp, float m22, float m32);

// Depth-driven auto-convergence (the desktop's StereoAutoConvergence): keeps
// the nearest significant object's pop-out within the comfort budget by
// pulling convergence in, never beyond the manual value. A frame without a
// near depth (nearest_z <= 0) keeps the last convergence, as the desktop
// does by not updating then.
struct AutoConvergence {
    static constexpr int kHistory = 9;
    float history[kHistory] = {};
    int count = 0, cursor = 0;
    float z_ema = -1.0f;
    float inv_conv = -1.0f;
    int cut_frames = 0;
    float last_conv = -1.0f;
    // Returns the convergence to apply, world units.
    float update(float nearest_z, float manual_conv, float sep, int comfort_thousandths, const AutoConvTuning& tune);
    void reset() { count = cursor = cut_frames = 0; z_ema = inv_conv = last_conv = -1.0f; }
};

// The near statistic over a grid of depth samples z' (0 < z' < 1 valid): a
// 9x5 patch layout inside the frame minus margins (5% sides and top, the
// frame below region_bottom), the 25th percentile within each patch, then
// the second nearest patch. Returns z' or -1, also when fewer than 3
// patches have depth.
float near_depth_statistic(const float* grid, int cols, int rows, float region_bottom = 0.75f);

}   // namespace rt64_3ds

#endif
