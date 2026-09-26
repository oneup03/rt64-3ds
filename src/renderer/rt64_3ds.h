// rt64-3ds public API: the renderer a game hands to N64ModernRuntime.
#ifndef RT64_3DS_H
#define RT64_3DS_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace rt64_3ds {

// How a draw is placed in depth for the stereo pair. Mirrors the owner's RT64
// classification: world geometry gets the full shear + convergence shift,
// "infinity" draws (skies) only the shear, screen overlays none, HUD draws a
// fixed screen-relative offset.
enum class StereoClass : uint8_t { None, World, Infinity, ScreenOverlay, Hud, HudBubble, Rect };
enum class ProjKind : uint8_t { Any, Perspective, Ortho };

// A rule matches a projection-group id range (gEXMatrixGroup ids) and a
// projection kind. The first matching rule wins.
struct StereoRule {
    uint32_t id_lo, id_hi;
    ProjKind kind;
    StereoClass cls;
};

struct RenderDesc {
    const char* game_name = "";
    const StereoRule* rules = nullptr;
    size_t rule_count = 0;
    StereoClass unclassified_persp = StereoClass::None;
    StereoClass unclassified_ortho = StereoClass::Hud;
    uint32_t frame_head_proj_id = 0;        // the projection group set at every frame head
    uint32_t hud_id_lo = 0, hud_id_hi = 0;  // perspective draws in this id range are HUD
    uint32_t bubble_id_lo = 0;              // ... and from here up, the "bubble" HUD variant
    float hud_k_ortho = 2.75f, hud_k_bubble = 1.3f, hud_k_persp = 1.35799f;
    uint32_t vi_width = 320, vi_height = 240;
    bool hud_on_bottom_supported = false;
    const uint32_t* snapshot_buffers = nullptr;   // RDRAM addresses the game copies frames into
    size_t snapshot_count = 0;
};

void set_render_desc(const RenderDesc& desc);
const RenderDesc& render_desc();

// The ultramodern renderer callback. Runs on the runtime's gfx thread.
std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

// Build-time choice for bring-up: a renderer that accepts every display list
// and draws nothing, for measuring the game's CPU cost without any GPU work.
std::unique_ptr<ultramodern::renderer::RendererContext>
create_null_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

struct Settings {
    int sep_slider = 10;              // 0..50, x0.002 = separation as a fraction of width
    int convergence_hundredths = 2000;// 10..2000, x0.2 = world units
    int hud_depth = 35;               // 0..100, 50 = screen plane
    bool auto_convergence = true;
    bool hud_on_bottom = true;
    bool show_fps = true;
    bool ghost_reduction = false;
    float slider_gain = 0.5f;         // scales the desktop separation range down for the 3DS panel
};
Settings& settings();

// Fed by the game's patches through host functions.
void set_first_person(bool on);
void set_low_convergence_scene(bool on);

// Copy a region of the last presented frame back into the game's framebuffer
// in RDRAM (for effects whose CPU code reads pixels). Synchronous.
void request_fb_readback(uint32_t fb_addr, int x, int y, int w, int h);

struct FrameStats {
    float game_ms = 0, gfx_ms = 0, replay_ms = 0, audio_ms = 0;
    int draws = 0, tris = 0, tex_uploads = 0, combiner_fallbacks = 0;
    int dl_per_sec = 0;               // display lists received per second
};
const FrameStats& stats();

}   // namespace rt64_3ds

#endif
