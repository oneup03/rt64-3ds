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
    // Write the rendered depth back into the game's RDRAM depth buffer each
    // frame (N64 compressed format, sampled on a 4x4 grid): for games that
    // read depth on the CPU (DK64's camera wall avoidance).
    bool depth_to_rdram = false;
    // World-class draws that neither test nor write depth are overlays drawn
    // in front of the camera (DK64's fairy-camera film card): give them the
    // HUD's depth instead of their very near world parallax.
    bool depthless_world_is_hud = false;
    // Always take gSPBranchLessZ's branch, i.e. always the near model of a
    // level-of-detail pair: what the recomp frontends set (RT64's
    // enhancementConfig.f3dex.forceBranch).
    bool force_lod_branch = true;
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

// The desktop's stereo settings and units (DK64 defaults), plus the 3DS
// panel's gain: separation at full 3D slider = sep_slider * 0.002 * gain of
// the half screen width per eye (25 * 0.002 * 0.5 = 5 px each way).
struct Settings {
    int sep_slider = 25;              // 0..100 (the desktop stops at 50), x0.002 = per-eye separation (fraction of half width)
    int convergence_hundredths = 600; // 10..2000, x0.2 = world units
    int hud_depth = 50;               // 0..100, 50 = screen plane
    bool auto_convergence = true;
    int comfort_target = 0;           // auto-convergence pop-out budget, thousandths of width (-20..30)
    bool hud_on_bottom = true;
    bool show_fps = true;
    // Ghost reduction, the desktop's anti-crosstalk range compression (only
    // while 3D is on): contrast squeezes each eye towards mid-grey, the black
    // floor lifts blacks. 100 and 0 are off.
    int ghost_contrast = 100;         // 50..100 percent
    int ghost_black_floor = 0;        // 0..20 percent
    float slider_gain = 0.5f;         // scales the desktop separation range down for the 3DS panel
};
Settings& settings();

// Fed by the game's patches through host functions.
void set_first_person(bool on);
void set_low_convergence_scene(bool on);
bool first_person_scene();
bool low_convergence_scene();

// Copy a region of the last finished frame (the left eye) into the game's
// RGBA16 framebuffer in RDRAM, for game code that reads pixels (DK64's fairy
// camera). The rectangle is in N64 pixels (the 4:3 area; the wide screen
// adds 40 px either side); fb_width is the framebuffer's width in pixels.
// Returns immediately; false when no frame is available - frames are kept
// only while first_person_scene() is set, where the games that need this
// take their pictures.
bool read_back_frame(uint8_t* rdram, uint32_t fb_addr, uint32_t fb_width, int x, int y, int w, int h);

// Live progress of the gfx thread, for a watchdog on another thread: which
// frame and draw it is on. `dump_progress` writes the current draw's state
// to stderr (read racily; only meant for a hung GPU).
struct Progress {
    volatile uint32_t frames = 0;
    volatile uint32_t draw_index = 0, draw_count = 0;
    volatile uint32_t phase = 0;      // 0 idle, 1 interpreting, 2 frame begin, 3 replaying, 4 frame end
};
const Progress& progress();
void dump_progress();

struct FrameStats {
    float game_ms = 0, gfx_ms = 0, replay_ms = 0, audio_ms = 0;
    int draws = 0, tris = 0, tex_uploads = 0, combiner_fallbacks = 0;
    int dl_per_sec = 0;               // display lists received per second
    int late_frames = 0;              // gaps of more than 50 ms between display lists, last second
    float max_gap_ms = 0;             // the longest such gap, last second
};
const FrameStats& stats();
FrameStats& mutable_stats();   // for the renderer implementations

// Debugging: sleep this long after every display list (emulating console
// timing in the emulator).
void set_debug_gfx_delay_ms(int ms);

// The process is about to end: stop submitting GPU work.
void set_quitting();

// Hold the game: the renderer takes no display list while paused, so the
// game waits on its current one (a modal prompt). The top screen keeps the
// last frame.
void set_paused(bool paused);

// Save the next frame for the host tools: its gfx task and RDRAM
// (gfx_task.bin) and what it rendered (gfx_frame.ppm), in the game's folder.
void request_capture();
int capture_state();    // 0 none, 1 being written, 2 written

}   // namespace rt64_3ds

#endif
