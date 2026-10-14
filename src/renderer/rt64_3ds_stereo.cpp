// Stereo classification and per-eye shifts; see rt64_3ds_stereo.h.
#include "rt64_3ds_stereo.h"

#include <algorithm>
#include <cmath>

namespace rt64_3ds {

namespace {

constexpr float kHudMaxOffset = 0.04f;
constexpr float kHudReferenceSeparation = 0.10f;   // the desktop's tuning anchor (slider 50)

// Pop-out is not divergence: behind the screen plane the limit is physical,
// in front of it only comfort, so the two directions clamp differently.
// `unsigned_offset` is positive when the element belongs behind the screen.
float clamp_ndc(float ndc, float unsigned_offset) {
    const float limit = unsigned_offset > 0.0f ? 0.10f : 0.30f;
    return std::max(-limit, std::min(limit, ndc));
}

bool in_rule(uint32_t id, uint32_t lo, uint32_t hi) { return id >= lo && id <= hi; }

// RenderDesc::snap_hud_shift: a shift (N64 units) onto whole screen pixels.
// roundf is symmetric, so the two eyes' opposite shifts stay opposite.
float snap_shift(float v, float px_scale) {
    return px_scale > 0.0f ? roundf(v * px_scale) / px_scale : v;
}

}   // namespace

StereoClass classify_draw(const DrawRecord& d, const RenderDesc& desc) {
    if (d.kind != DrawRecord::Tris) {
        // Only Rect rules place rectangles (Goemon's backdrops at infinity).
        for (size_t i = 0; i < desc.rule_count; i++) {
            const StereoRule& r = desc.rules[i];
            if (r.kind == ProjKind::Rect && in_rule(d.proj_id, r.id_lo, r.id_hi)) return r.cls;
        }
        return StereoClass::Rect;
    }
    for (size_t i = 0; i < desc.rule_count; i++) {
        const StereoRule& r = desc.rules[i];
        if (r.kind == ProjKind::Rect || !in_rule(d.proj_id, r.id_lo, r.id_hi)) continue;
        if (r.kind == ProjKind::Any || (r.kind == ProjKind::Perspective) == d.perspective) {
            // Z_CMP and Z_UPD (othermode L bits 4, 5) clear and FORCE_BL
            // (bit 14) too: an opaque surface with no depth. Blob shadows are
            // depthless as well but blended, and belong on the ground.
            if (r.cls == StereoClass::World && desc.depthless_world_is_hud && (d.othermode_l & 0x4030) == 0) return StereoClass::Hud;
            return r.cls;
        }
    }
    if (d.perspective && desc.hud_id_hi != 0 && in_rule(d.proj_id, desc.hud_id_lo, desc.hud_id_hi)) {
        return (desc.bubble_id_lo != 0 && d.proj_id >= desc.bubble_id_lo) ? StereoClass::HudBubble : StereoClass::Hud;
    }
    return d.perspective ? desc.unclassified_persp : desc.unclassified_ortho;
}

void stereo_shift(const StereoFrame& sf, StereoClass cls, const DrawRecord& d, int eye, float* a, float* b) {
    *a = 0.0f;
    *b = 0.0f;
    if (!sf.on) return;
    const float e = (float)eye;
    const float s_px = sf.sep * kPxPerNdc;
    const RenderDesc& desc = render_desc();
    switch (cls) {
        case StereoClass::World:
            // Off-axis shear plus the view shift, in clip space:
            // x += e * sep * (conv - w): zero parallax at the convergence
            // distance, the full separation at infinity.
            *a = -e * s_px;
            *b = e * s_px * sf.conv;
            break;
        case StereoClass::Infinity:
            *a = -e * s_px;
            break;
        case StereoClass::Hud:
        case StereoClass::HudBubble: {
            if (sf.hud_offset == 0.0f) break;
            const float k = !d.perspective ? desc.hud_k_ortho
                          : cls == StereoClass::HudBubble ? desc.hud_k_bubble : desc.hud_k_persp;
            *a = snap_shift(-e * clamp_ndc(sf.hud_offset * k, sf.hud_offset) * kPxPerNdc, sf.px_scale);
            break;
        }
        case StereoClass::Rect:
            if (desc.rect_hud_follows_separation) {
                // Where the orthographic HUD goes (the desktop fork gives
                // texture rectangles the same offset as its ortho HUD).
                if (sf.hud_offset == 0.0f) break;
                *b = snap_shift(-e * clamp_ndc(sf.hud_offset * desc.hud_k_ortho, sf.hud_offset) * kPxPerNdc, sf.px_scale);
            }
            else {
                *b = snap_shift(-e * sf.rect_ndc * kPxPerNdc, sf.px_scale);
            }
            break;
        default:
            break;
    }
}

bool is_reticle_quad(const DrawRecord& d, const Vtx3ds* verts, float* minx, float* maxx, float* miny, float* maxy) {
    if (d.kind != DrawRecord::Tris || d.perspective || !d.mv_auto || d.count != 6) return false;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (uint32_t i = d.first; i < d.first + d.count; i++) {
        const Vtx3ds& v = verts[i];
        if (!(v.w > 0.0f)) return false;
        const float x = v.x / v.w, y = v.y / v.w;
        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
    }
    const float w = x1 - x0, h = y1 - y0;
    if (w < 8.0f || h < 8.0f || w > 64.0f || h > 64.0f) return false;
    if (w > h * 1.6f || h > w * 1.6f) return false;
    const float sw = (float)(d.scissor[2] - d.scissor[0]), sh = (float)(d.scissor[3] - d.scissor[1]);
    if (sw <= 0.0f || sh <= 0.0f) return false;
    const float cx = d.scissor[0] + sw * 0.5f, cy = d.scissor[1] + sh * 0.5f;
    if (!(std::fabs((x0 + x1) * 0.5f - cx) < sw * 0.2f && std::fabs((y0 + y1) * 0.5f - cy) < sh * 0.2f)) return false;
    *minx = x0; *maxx = x1; *miny = y0; *maxy = y1;
    return true;
}

bool rect_covers_scissor(const DrawRecord& d, const Vtx3ds* verts) {
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (uint32_t i = d.first; i < d.first + d.count; i++) {
        const Vtx3ds& v = verts[i];
        x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
    }
    return x0 <= d.scissor[0] && x1 >= d.scissor[2] && y0 <= d.scissor[1] && y1 >= d.scissor[3];
}

void set_hud_terms(StereoFrame& sf, int hud_depth, float slider, float slider_gain) {
    // 50 is the screen plane; above pops the HUD out, below pushes it back.
    // Triangles scale with separation (so the HUD flattens with the world);
    // rectangles carry the desktop's fixed offset, here scaled by the 3D
    // slider (unless RenderDesc::rect_hud_follows_separation).
    const float centered = ((float)hud_depth - 50.0f) / 50.0f;
    sf.hud_offset = -centered * kHudMaxOffset * (sf.sep / kHudReferenceSeparation);
    // The orthographic HUD's factor (RenderDesc::hud_k_ortho, 2.75 by default)
    // for the rectangles' fixed offset too, so a game that changes it keeps
    // its rectangles and its orthographic HUD at one depth.
    sf.rect_ndc = -centered * kHudMaxOffset * render_desc().hud_k_ortho * slider * slider_gain;
}

float cover_zoom_factor(float sep, const ScreenGeometry& geo) {
    return 1.0f + sep * kPxPerNdc / (geo.wide_w * 0.5f);
}

void cover_zoom(const DrawRecord& d, Vtx3ds* verts, float factor, const ScreenGeometry& geo) {
    const float cx = (float)geo.w * 0.5f, cy = (float)geo.h * 0.5f;
    for (uint32_t k = d.first; k < d.first + d.count; k++) {
        Vtx3ds& v = verts[k];      // rectangles: w = 1
        v.x = cx + (v.x - cx) * factor;
        v.y = cy + (v.y - cy) * factor;
    }
}

bool hud_camera_is_world(const DrawRecord* draws, size_t count, const RenderDesc& desc, int frame_w) {
    if (desc.hud_camera_id == 0) return false;
    uint32_t hud_cam = 0, world = 0;
    for (size_t i = 0; i < count; i++) {
        const DrawRecord& d = draws[i];
        if (d.kind != DrawRecord::Tris || !d.perspective || d.rtt >= 0) continue;
        if (!(d.scissor[0] <= 0 && d.scissor[2] >= frame_w)) continue;      // full width
        if (d.proj_id == desc.hud_camera_id) hud_cam++;
        else if (classify_draw(d, desc) == StereoClass::World) world++;
    }
    return hud_cam > world;
}

uint32_t primary_world_instance(const DrawRecord* draws, size_t count, const RenderDesc& desc, int frame_w, bool* hud_cam,
                                InstanceCount* table, int table_max, int* table_count) {
    // Instances are numbered in draw order; a frame has a few dozen.
    constexpr int kMax = 64;
    InstanceCount inst[kMax];
    int n = 0;
    // Triangles the rules place as World under any scissor: a world scene in
    // a window (Goemon's pause map) is no candidate, but a HUD camera with no
    // more triangles than it (that screen's two 3D icons) doesn't become the
    // world over it.
    uint32_t world_tris = 0;
    for (size_t i = 0; i < count; i++) {
        const DrawRecord& d = draws[i];
        if (d.kind != DrawRecord::Tris || !d.perspective || d.rtt >= 0) continue;
        const bool hud = desc.hud_camera_id != 0 && d.proj_id == desc.hud_camera_id;
        const bool world = !hud && classify_draw(d, desc) == StereoClass::World;
        if (world) world_tris += d.count / 3;
        if (d.proj_inst == 0) continue;
        if (!(d.scissor[0] <= 0 && d.scissor[2] >= frame_w)) continue;      // full width
        if (!hud && !world) continue;
        int k = n - 1;
        while (k >= 0 && inst[k].inst != d.proj_inst) k--;
        if (k < 0) {
            if (n == kMax) continue;
            k = n++;
            inst[k].inst = d.proj_inst;
            inst[k].proj_id = d.proj_id;
        }
        inst[k].records++;
        inst[k].tris += d.count / 3;
    }
    // By triangles, not records: a record is a run of triangles in one state
    // (Goemon's menu curtain, 392 triangles, is one, as it is one RT64 call),
    // so counting records would make the first menu panel's rims (4 records
    // of 2 triangles, one per texture) the world and put the curtain at the
    // HUD's depth. In play the world camera has far more of both.
    int best = -1;
    for (int k = 0; k < n; k++) {
        if (desc.hud_camera_id != 0 && inst[k].proj_id == desc.hud_camera_id && inst[k].tris <= world_tris) continue;
        if (best < 0 || inst[k].tris > inst[best].tris) best = k;
    }
    if (table != nullptr) {
        const int m = n < table_max ? n : table_max;
        for (int k = 0; k < m; k++) table[k] = inst[k];
        if (table_count != nullptr) *table_count = n;
    }
    *hud_cam = best >= 0 && desc.hud_camera_id != 0 && inst[best].proj_id == desc.hud_camera_id;
    return best >= 0 ? inst[best].inst : 0;
}

// A texture that repeats in both directions (wrapped or mirrored by a mask,
// no clamp): an overlay pattern rather than a picture.
static bool tiles_both_ways(const TexDesc& t) {
    return !t.valid || (t.masks != 0 && t.maskt != 0 && !(t.cms & 2) && !(t.cmt & 2));
}

bool stretch_tint_quad(const DrawRecord& d, Vtx3ds* verts, const ScreenGeometry& geo) {
    if (d.kind != DrawRecord::Tris || d.count == 0 || d.count > 6 || d.rtt >= 0) return false;
    const bool textured = d.tex[0].valid || d.tex[1].valid;
    if (!tiles_both_ways(d.tex[0]) || !tiles_both_ways(d.tex[1])) return false;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (uint32_t i = d.first; i < d.first + d.count; i++) {
        const Vtx3ds& v = verts[i];
        if (!(v.w > 0.0f)) return false;
        const float x = v.x / v.w, y = v.y / v.w;
        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
    }
    const float w = (float)geo.w, h = (float)geo.h;
    if (x0 > kTintQuadMargin || x1 < w - kTintQuadMargin || y0 > kTintQuadMargin || y1 < h - kTintQuadMargin) return false;
    if (x0 <= -geo.x0 && x1 >= w + geo.x0) return false;      // covers the wide screen already
    // A screen-aligned rectangle: each vertex on a corner of the bounds.
    for (uint32_t i = d.first; i < d.first + d.count; i++) {
        const Vtx3ds& v = verts[i];
        const float x = v.x / v.w, y = v.y / v.w;
        if (std::min(std::fabs(x - x0), std::fabs(x - x1)) > 1.0f || std::min(std::fabs(y - y0), std::fabs(y - y1)) > 1.0f) return false;
    }
    // A textured quad keeps its texel size, its pattern repeating into the new
    // area: the texture coordinates as an affine function of the screen
    // position (u = ux x + uy y + u0), from the corners at (x0, y0), (x1, y0)
    // and (x0, y1), extend with it.
    float ux = 0.0f, uy = 0.0f, vx = 0.0f, vy = 0.0f;
    if (textured) {
        const Vtx3ds* c[3] = {};
        const float cx[3] = { x0, x1, x0 }, cy[3] = { y0, y0, y1 };
        for (uint32_t i = d.first; i < d.first + d.count; i++) {
            const Vtx3ds& v = verts[i];
            const float x = v.x / v.w, y = v.y / v.w;
            for (int k = 0; k < 3; k++) if (c[k] == nullptr && std::fabs(x - cx[k]) <= 1.0f && std::fabs(y - cy[k]) <= 1.0f) c[k] = &v;
        }
        if (c[0] == nullptr || c[1] == nullptr || c[2] == nullptr) return false;
        ux = (c[1]->u - c[0]->u) / (x1 - x0); vx = (c[1]->v - c[0]->v) / (x1 - x0);
        uy = (c[2]->u - c[0]->u) / (y1 - y0); vy = (c[2]->v - c[0]->v) / (y1 - y0);
    }
    // The bounds mapped onto the target in screen terms, x' = a x + b, on the
    // homogeneous coordinates (x' w = a x w + b w).
    const float tx0 = -geo.x0 - 1.0f, tx1 = w + geo.x0 + 1.0f;
    const float ty0 = std::min(y0, -1.0f), ty1 = std::max(y1, h + 1.0f);
    const float ax = (tx1 - tx0) / (x1 - x0), bx = tx0 - x0 * ax;
    const float ay = (ty1 - ty0) / (y1 - y0), by = ty0 - y0 * ay;
    for (uint32_t i = d.first; i < d.first + d.count; i++) {
        Vtx3ds& v = verts[i];
        const float x = v.x / v.w, y = v.y / v.w;
        const float dx = (x * ax + bx) - x, dy = (y * ay + by) - y;
        v.x = v.x * ax + bx * v.w;
        v.y = v.y * ay + by * v.w;
        v.u += ux * dx + uy * dy;
        v.v += vx * dx + vy * dy;
    }
    return true;
}

bool rect_covers_frame(const DrawRecord& d, const Vtx3ds* verts, int frame_w, int frame_h) {
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (uint32_t i = d.first; i < d.first + d.count; i++) {
        const Vtx3ds& v = verts[i];
        x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
    }
    return x0 <= 0.0f && x1 >= (float)frame_w && y0 <= 0.0f && y1 >= (float)frame_h;
}

float view_depth(float zp, float m22, float m32) {
    if (!(zp > 0.0f) || !(zp < 1.0f)) return -1.0f;
    const float ndc = 2.0f * zp - 1.0f;
    const float denom = -ndc - m22;
    if (std::fabs(denom) < 1e-6f) return -1.0f;
    const float z = m32 / denom;
    return z > 0.0f ? z : -z;
}

float AutoConvergence::update(float nearest_z, float manual_conv, float sep, int comfort_thousandths, const AutoConvTuning& tune) {
    if (sep <= 0.0f || manual_conv <= 0.0f) return manual_conv;
    // No near depth this frame (a sky, a menu): keep the last convergence.
    // Returning the manual value instead made a sky with a few birds in it
    // flap between the two, frame by frame, as the birds came and went.
    if (nearest_z <= 0.0f) return last_conv > 0.0f ? std::min(last_conv, manual_conv) : manual_conv;

    const int len = std::max(1, std::min(kHistory, tune.history));
    history[cursor] = nearest_z;
    cursor = (cursor + 1) % kHistory;
    count = count < len ? count + 1 : len;
    float sorted[kHistory];
    for (int i = 0; i < count; i++) sorted[i] = history[(cursor + kHistory - 1 - i) % kHistory];   // the newest `count`
    std::sort(sorted, sorted + count);
    const float z_median = sorted[count / 2];

    if (z_ema <= 0.0f) {
        z_ema = z_median;
    }
    else {
        const float rel = std::fabs(z_median - z_ema) / z_ema;
        // A camera cut stays changed; a small object flickering in and out of
        // a patch does not, so the snap waits for two frames of it.
        const float approach = nearest_z < z_ema ? z_ema / nearest_z : 1.0f;
        cut_frames = approach > tune.cut_ratio ? cut_frames + 1 : 0;
        if (cut_frames >= 2) {
            float recent[3];
            for (int i = 0; i < 3; i++) recent[i] = history[(cursor + kHistory - 1 - i) % kHistory];
            std::sort(recent, recent + 3);
            z_ema = recent[1];
            inv_conv = -1.0f;
            cut_frames = 0;
        }
        else if (rel > 0.02f) {
            // Quick to pull in as something approaches, slow to let go.
            const float alpha = z_median < z_ema ? tune.approach : tune.retreat;
            z_ema += (z_median - z_ema) * alpha;
        }
    }

    float target = (float)std::max(-50, std::min(60, comfort_thousandths)) * 0.001f;
    const float at_manual = sep * 0.5f * (manual_conv / z_ema - 1.0f);
    float conv = manual_conv;
    if (at_manual > target) {
        conv = std::min(z_ema * (1.0f + 2.0f * target / sep), manual_conv);
    }
    conv = std::max(conv, z_ema * 0.25f);
    conv = std::max(conv, tune.near_clamp);

    const float target_inv = 1.0f / conv;
    if (inv_conv <= 0.0f) inv_conv = target_inv;
    else inv_conv += (target_inv - inv_conv) * (target_inv > inv_conv ? tune.ease_in : tune.ease_out);
    last_conv = std::min(1.0f / inv_conv, manual_conv);
    return last_conv;
}

float near_depth_statistic(const float* grid, int cols, int rows, float region_bottom) {
    constexpr int kCols = 9, kRows = 5;
    const float x0 = cols * 0.05f, x1 = cols * 0.95f, y0 = rows * 0.05f, y1 = rows * std::max(0.3f, std::min(1.0f, region_bottom));
    float nears[kCols * kRows];
    int n = 0;
    float samples[64];
    for (int py = 0; py < kRows; py++) {
        const int ya = (int)(y0 + (y1 - y0) * py / kRows), yb = (int)(y0 + (y1 - y0) * (py + 1) / kRows);
        for (int px = 0; px < kCols; px++) {
            const int xa = (int)(x0 + (x1 - x0) * px / kCols), xb = (int)(x0 + (x1 - x0) * (px + 1) / kCols);
            int m = 0;
            for (int y = ya; y < yb && y < rows; y++) {
                for (int x = xa; x < xb && x < cols; x++) {
                    const float z = grid[y * cols + x];
                    if (z > 0.0f && z < 1.0f && m < 64) samples[m++] = z;
                }
            }
            if (m < 4) continue;
            // A low percentile, so one stray texel cannot pull the value.
            std::nth_element(samples, samples + m / 4, samples + m);
            nears[n++] = samples[m / 4];
        }
    }
    // Fewer than 3 patches with depth is no scene to converge on: a sky with
    // a bird or two in it. (A 3DS patch is about 16 samples of 8x8 pixels,
    // so a small object fills one easily; the desktop's are finer.)
    if (n < 3) return -1.0f;
    // The second nearest patch, so one small object is not the whole story.
    const int k = n >= 4 ? 1 : 0;
    std::nth_element(nears, nears + k, nears + n);
    return nears[k];
}

}   // namespace rt64_3ds

float rt64_3ds::snap_to_pixels(float v, float px_scale) { return snap_shift(v, px_scale); }
