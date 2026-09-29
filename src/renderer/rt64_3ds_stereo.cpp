// Stereo classification and per-eye shifts; see rt64_3ds_stereo.h.
#include "rt64_3ds_stereo.h"

#include <algorithm>
#include <cmath>

namespace rt64_3ds {

namespace {

constexpr float kPxPerNdc = 200.0f;          // half the 400 px top screen
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

}   // namespace

StereoClass classify_draw(const DrawRecord& d, const RenderDesc& desc) {
    if (d.kind != DrawRecord::Tris) return StereoClass::Rect;
    for (size_t i = 0; i < desc.rule_count; i++) {
        const StereoRule& r = desc.rules[i];
        if (!in_rule(d.proj_id, r.id_lo, r.id_hi)) continue;
        if (r.kind == ProjKind::Any || (r.kind == ProjKind::Perspective) == d.perspective) {
            // Z_CMP and Z_UPD (othermode L bits 4, 5) both clear.
            if (r.cls == StereoClass::World && desc.depthless_world_is_hud && (d.othermode_l & 0x30) == 0) return StereoClass::Hud;
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
            *a = -e * clamp_ndc(sf.hud_offset * k, sf.hud_offset) * kPxPerNdc;
            break;
        }
        case StereoClass::Rect:
            *b = -e * sf.rect_ndc * kPxPerNdc;
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

float view_depth(float zp, float m22, float m32) {
    if (!(zp > 0.0f) || !(zp < 1.0f)) return -1.0f;
    const float ndc = 2.0f * zp - 1.0f;
    const float denom = -ndc - m22;
    if (std::fabs(denom) < 1e-6f) return -1.0f;
    const float z = m32 / denom;
    return z > 0.0f ? z : -z;
}

float AutoConvergence::update(float nearest_z, float manual_conv, float sep, int comfort_thousandths, bool low_convergence_scene) {
    if (nearest_z <= 0.0f || sep <= 0.0f || manual_conv <= 0.0f) return manual_conv;

    history[cursor] = nearest_z;
    cursor = (cursor + 1) % kHistory;
    count = count < kHistory ? count + 1 : kHistory;
    float sorted[kHistory];
    for (int i = 0; i < count; i++) sorted[i] = history[i];
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
        cut_frames = approach > 2.5f ? cut_frames + 1 : 0;
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
            const float alpha = z_median < z_ema ? 0.25f : 0.05f;
            z_ema += (z_median - z_ema) * alpha;
        }
    }

    float target = (float)std::max(-50, std::min(60, comfort_thousandths)) * 0.001f;
    if (low_convergence_scene) target *= 0.45f;
    constexpr float kNearClamp = 4.0f;
    const float at_manual = sep * 0.5f * (manual_conv / z_ema - 1.0f);
    float conv = manual_conv;
    if (at_manual > target) {
        conv = std::min(z_ema * (1.0f + 2.0f * target / sep), manual_conv);
    }
    conv = std::max(conv, z_ema * 0.25f);
    conv = std::max(conv, kNearClamp);

    const float target_inv = 1.0f / conv;
    if (inv_conv <= 0.0f) inv_conv = target_inv;
    else inv_conv += (target_inv - inv_conv) * (target_inv > inv_conv ? 0.14f : 0.06f);
    return std::min(1.0f / inv_conv, manual_conv);
}

float near_depth_statistic(const float* grid, int cols, int rows) {
    constexpr int kCols = 9, kRows = 5;
    const float x0 = cols * 0.05f, x1 = cols * 0.95f, y0 = rows * 0.05f, y1 = rows * 0.75f;
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
    if (n == 0) return -1.0f;
    // The second nearest patch, so one small object is not the whole story.
    const int k = n >= 4 ? 1 : 0;
    std::nth_element(nears, nears + k, nears + n);
    return nears[k];
}

}   // namespace rt64_3ds
