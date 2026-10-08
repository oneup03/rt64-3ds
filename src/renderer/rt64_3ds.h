// rt64-3ds public API: the renderer a game hands to N64ModernRuntime.
#ifndef RT64_3DS_H
#define RT64_3DS_H

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"
#include "rt64_3ds_types.h"

namespace rt64_3ds {

// How a draw is placed in depth for the stereo pair. Mirrors the owner's RT64
// classification: world geometry gets the full shear + convergence shift,
// "infinity" draws (skies) only the shear, screen overlays none, HUD draws a
// fixed screen-relative offset.
enum class StereoClass : uint8_t { None, World, Infinity, ScreenOverlay, Hud, HudBubble, Rect };
// Rect matches texture/fill rectangles drawn while the rule's group id is in
// force; Any, Perspective and Ortho keep matching triangles only, so existing
// rule tables are unchanged.
enum class ProjKind : uint8_t { Any, Perspective, Ortho, Rect };


// A rule matches a projection-group id range (gEXMatrixGroup ids) and a
// projection kind. The first matching rule wins.
struct StereoRule {
    uint32_t id_lo, id_hi;
    ProjKind kind;
    StereoClass cls;
};

// How auto-convergence finds and follows the nearest object. The defaults
// are DK64's: a chase camera always has ground close under it at the bottom
// of the frame, so that strip is left out, and the answer is smoothed hard.
struct AutoConvTuning {
    float region_bottom = 0.75f;    // the near statistic samples the frame down to here (fraction of its height)
    int history = 9;                // frames in the median filter (1..9)
    float approach = 0.25f;         // per-frame pull toward a nearer depth (the retreat is slower)
    float retreat = 0.05f;          // ... and toward a farther one
    float cut_ratio = 2.5f;         // this much nearer for 2 frames is a camera cut: jump there
    float ease_in = 0.14f, ease_out = 0.06f;   // convergence easing toward the target, nearer / farther
    float near_clamp = 4.0f;        // convergence never comes nearer than this (world units)
};

struct RenderDesc {
    const char* game_name = "";
    Gbi gbi = Gbi::F3DEX2;
    const StereoRule* rules = nullptr;
    size_t rule_count = 0;
    StereoClass unclassified_persp = StereoClass::None;
    StereoClass unclassified_ortho = StereoClass::Hud;
    uint32_t frame_head_proj_id = 0;        // the projection group set at every frame head
    uint32_t hud_id_lo = 0, hud_id_hi = 0;  // perspective draws in this id range are HUD
    uint32_t bubble_id_lo = 0;              // ... and from here up, the "bubble" HUD variant
    float hud_k_ortho = 2.75f, hud_k_bubble = 1.3f, hud_k_persp = 1.35799f;
    // The game's frame, in N64 pixels (its framebuffer's size: DK64 320x240,
    // Banjo 292x216). It is placed as RT64 places it: see ScreenGeometry.
    uint32_t vi_width = 320, vi_height = 240;
    bool hud_on_bottom_supported = false;
    const uint32_t* snapshot_buffers = nullptr;   // RDRAM addresses the game copies frames into
    size_t snapshot_count = 0;
    // Write the rendered depth back into the game's RDRAM depth buffer each
    // frame (N64 compressed format, sampled on a 4x4 grid): for games that
    // read depth on the CPU (DK64's camera wall avoidance).
    bool depth_to_rdram = false;
    // Opaque world-class draws that neither test nor write depth are
    // overlays drawn in front of the camera (DK64's fairy-camera film card):
    // give them the HUD's depth instead of their very near world parallax.
    // Blended depthless draws (DK64's blob shadows) stay in the world.
    bool depthless_world_is_hud = false;
    // Always take gSPBranchLessZ's branch, i.e. always the near model of a
    // level-of-detail pair: what the recomp frontends set (RT64's
    // enhancementConfig.f3dex.forceBranch).
    bool force_lod_branch = true;
    // Draw colour images the game renders off screen (neither a presented
    // framebuffer nor the depth buffer) into GPU textures, and sample those
    // where the game loads textures from them: Banjo's jiggy transition and
    // picture puzzles render the world into such an image. Off, off-screen
    // draws are dropped (copies of the presented frame use the snapshot).
    bool render_to_texture = false;
    // The game copies parts of the frame it is still drawing into small
    // images of its own (texture rectangles out of the colour image, after
    // drawing into it) and draws with those copies in the same frame
    // (Goemon's menu boxes: their rims are strips of the backdrop drawn just
    // before). On the N64 the copies read the frame from RDRAM, which
    // rt64-3ds never writes. On: images of another width than the frame are
    // drawn into GPU textures (as render_to_texture would), loads from the
    // frame being drawn sample a copy of it, and the frames that do this
    // draw each eye's draws up to the last such load a second time, into a
    // 256x512 texture (about 1 MB of VRAM, allocated at start). Frames that
    // don't pay nothing.
    bool sample_drawn_frame = false;
    AutoConvTuning autoconv{};

    // ---- Parity with the desktop fork's renderer for games that ask for it
    // (Goemon). Every default keeps the behaviour DK64 and Banjo were tuned on.
    //
    // RT64's rect.fixRectLR (on by default there): a rectangle whose right or
    // bottom edge ends within one pixel of the scissor's is extended to it.
    // Games draw full-screen rectangles to x = 319 under a 320-wide scissor;
    // extended, they cover the width and are stretched over the wide screen.
    bool fix_rect_lr = false;
    // Plain-origin rectangles of one run of rectangles (no triangles between
    // them, one colour image) that share their rows (uly, lry) and together
    // reach both edges of the frame are stretched over the wide screen like
    // one full-width rectangle: backdrops drawn as slices, or split where
    // their texture wraps (the desktop fork's merged widescreen detection).
    bool merge_rect_slices = false;
    // Infinity-class rectangles (backdrops at infinity) are scaled about the
    // frame's centre by 1 + their per-eye shift as a fraction of the half
    // width, so the shifted image still covers the screen in both eyes
    // instead of uncovering a strip at one edge (the desktop's skybox-rect
    // cover zoom).
    bool infinity_rect_cover_zoom = false;
    // HUD-class rectangles take the orthographic HUD's offset (hud_offset *
    // hud_k_ortho: scaled by the separation, clamped like it) instead of the
    // fixed offset scaled by the 3D slider alone, so 2D text and panels sit
    // at the same depth as the rest of the HUD.
    bool rect_hud_follows_separation = false;
    // Hud, HudBubble and HUD-rectangle shifts are rounded to whole screen
    // pixels (each eye's, symmetrically), so text samples the same texels in
    // both eyes.
    bool snap_hud_shift = false;
    // The projection group (gEXMatrixGroup id) of a HUD camera: perspective
    // HUD items drawn by a private camera the game's rules place as Hud.
    // When its full-width perspective draws outnumber the frame's full-width
    // world draws (a cutscene shot through a camera that passes the game's
    // HUD-camera test), it is the frame's world: its draws are placed as
    // World and its projection gives the depth terms. 0 = none.
    uint32_t hud_camera_id = 0;
    // How hud_camera_id becomes the world, as the desktop fork picks its
    // primary world (rt64_projection_processor.cpp): off, every draw of the
    // HUD camera is the world when its full-width draws outnumber the
    // world's. On, one projection instance is: of the full-width perspective
    // instances (DrawRecord::proj_inst) under the HUD camera or placed as
    // World, the one with the most triangles; if it is a HUD-camera instance,
    // only its draws are the world (and its projection the depth camera),
    // and the HUD camera's other instances (Goemon's menu panels next to the
    // curtain) stay HUD.
    bool primary_world_instance = false;
    // One depth scale for the world. A game may draw part of the world under
    // a second projection with a nearer near plane (Goemon's own model in
    // the closest camera zoom: near 15 against the world's 35, so the
    // model, nearer than 35, is not cut away), which leaves the depth buffer
    // holding two scales: the ground a little ahead of the camera, drawn
    // first, then hides the nearer model below a straight line. On, every
    // triangle draw under a World projection id (perspective, not
    // prim-depth) takes the depth of the frame's World projection with the
    // nearest near plane, from its distance (w): exact, as clipping already
    // happened at each draw's own near plane. The depth camera's terms
    // follow.
    bool unify_world_depth = false;
    // Rectangles that cover their scissor stay on the screen plane (off), or
    // take the HUD rectangles' shift like any other (on: the desktop gives
    // every non-skybox rectangle that shift; Goemon's panels drawn inside a
    // scissor of their own). Rectangles covering the whole frame stay on the
    // screen plane either way: a uniform full-screen tint or fade shows no
    // depth, and shifted it would bare a strip at one edge of each eye.
    bool covering_rects_hud = false;

    // ---- Performance.
    //
    // Triangles textured from tiles that LOADTILE cut out of one wider image
    // sample one texture of the whole image instead (its full width, and the
    // rows the last list that used it loaded), their texel coordinates moved
    // into it, so draws that differ only in which part of the image they
    // loaded become one draw. Goemon's sky cylinder loads a 32-texel column of
    // its panorama for each of up to 400 quads: the 280 sky draws of the
    // heaviest opening frame become one. Only where that samples the same
    // texels: TEXEL0 alone, 8- or 16-bit texels, the tile starting where the
    // load put it and every texel coordinate of the triangle inside the
    // loaded rectangle (no clamp, mask or mirror comes into play); anything
    // else samples its tile as before. Bilinear filtering at a tile's edge
    // then reads the image's true neighbour instead of the clamped edge.
    bool merge_image_tiles = false;
    // The GPU's share of a frame (the defaults are what DK64 and Banjo were
    // tuned with; each is also a settings.ini key, read at start, for A/B
    // tests on the console, where the frame line logs GPU ms per frame):
    // eye_color16 (gpu_color16): the eyes' colour buffers RGB565 instead of
    // RGBA8 (the N64's own framebuffer is 16-bit): half the colour traffic
    // of every draw, blend, clear, ghost pass and display transfer.
    // Snapshots follow the eye's format and so lose their alpha (sampled as
    // 1): games that copy the frame and need it (DK64) keep RGBA8.
    bool eye_color16 = false;
    // eye_depth16 (gpu_depth16): 16-bit depth buffers for the eyes instead
    // of 24-bit depth + stencil (nothing uses stencil): half the depth
    // traffic. 65536 steps of z/w, against the N64's own 18-bit z.
    bool eye_depth16 = false;
    // intensity_textures16 (gpu_textures16): I and IA textures (and CI ones
    // with an IA16 palette) as the PICA's 16-bit LA8 instead of RGBA8: the
    // same texels (L replicated into RGB, 8-bit alpha), half the size.
    bool intensity_textures16 = false;
    // fill_clear (gpu_fill_clear): a frame whose first draw into the eye is
    // a fill-mode rectangle covering it (the game's own clear, Goemon's
    // every frame) clears the eyes to its colour instead of drawing it: one
    // full screen of fragments less per eye, the same pixels.
    bool fill_clear = false;
    // double_eyes (gpu_double_eyes), with frame_interpolation: the
    // in-between frames get eye targets of their own (two more 240x400
    // colour + depth buffers: 1125 KB of VRAM at RGBA8/D16, taken last and
    // skipped when the VRAM is not there), and the game's frame of each
    // interpolated pair is recorded into a command buffer of its own while
    // the GPU still draws the in-between. Without it that frame's
    // C3D_FrameBegin waits for all of the in-between's GPU work (citro3d
    // has one command buffer and one GX queue run per frame) before a
    // single command is recorded: about one eye pair of GPU time a list on
    // the game's path. The in-between's draws go to the GPU as soon as they
    // are recorded (its targets are its own); only its display transfer
    // waits for the frame before it to be on the screen. The presenter
    // thread then queues the recorded frame (its draws run while the
    // in-between waits for its refresh) and its display transfer once the
    // in-between is on the screen.
    bool double_eyes = false;

    // early_drawn: the game hears a display list drawn (the RDP's
    // completion) as soon as the renderer has read it from RDRAM (when it
    // hears it parsed), not when the renderer is done with it. With frame
    // interpolation the renderer's waits for the screen (each frame of a
    // pair on a refresh of its own) then stay off the game's frame time: a
    // game that waits for the RDP before its next frame (Banjo's frame
    // limiter does, every frame) keeps its own pace, and its cutscenes keep
    // to their music. With depth_to_rdram the game hears it once the last
    // game frame's depth is in RDRAM, after the read and once the presenter
    // is done with that frame (the game must not run while it is written).
    bool early_drawn = false;

    // ---- Frame rate.
    //
    // Offer "60 interpolated" on the Frame rate row (Settings::frame_cap 2):
    // while the game asks for 30 frames a second (gEXSetRefreshRate), each
    // display list is drawn twice, first as an in-between frame halfway from
    // the list before (the desktop's high frame rate: RT64's matching and
    // interpolation of the transforms the game's patches tag with
    // gEXMatrixGroup), then as itself one refresh later. For games whose
    // patches tag their transforms for RT64's interpolation (Goemon).
    bool frame_interpolation = false;
};

void set_render_desc(const RenderDesc& desc);
const RenderDesc& render_desc();
// ScreenGeometry::make of render_desc()'s vi_width x vi_height.
ScreenGeometry screen_geometry();

// The folder the renderer's debug switches (*.TXT) and captures live in:
// the game's folder on the SD card, which the platform sets at start.
void set_data_dir(const char* dir);
void data_path(char* out, size_t size, const char* name);    // dir/name
FILE* data_fopen(const char* name, const char* mode);

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
    // Set by the platform before the renderer starts: core 2 is not used
    // (an Old 3DS, or the Cores row testing one), so no core-1 helpers.
    bool no_core2 = false;
    // Set by the platform before the renderer starts: the core of the thread
    // that reads each display list (up to "parsed"), -1 the gfx thread. For
    // a game that waits on "parsed" every frame on a core faster than the
    // renderer's: the read runs there while the game waits, the rest of the
    // list on the gfx thread (send_dl).
    int read_core = -1;
    int read_priority = 0;            // the reader's thread priority, 0 the gfx thread's
    int sep_slider = 25;             // 0..100 (the desktop stops at 50), x0.002 = per-eye separation (fraction of half width)
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
    // Frame cap: 0 draws every frame the game sends; 1 draws at most 30 a
    // second while the game asks for more (gEXSetRefreshRate): every other
    // display list is passed over (see send_dl), the game and its audio run
    // on at their own rate and the screen keeps the last frame drawn.
    // 2 (RenderDesc::frame_interpolation only): 60 a second while the game
    // asks for 30, an interpolated frame between each two of the game's.
    int frame_cap = 0;
    // RenderDesc::eye_color16 / eye_depth16 / intensity_textures16 /
    // fill_clear / double_eyes, as the game sets them unless settings.ini
    // says otherwise (read at start).
    int gpu_color16 = 0, gpu_depth16 = 0, gpu_textures16 = 0, gpu_fill_clear = 0, gpu_double_eyes = 0;
};
Settings& settings();

// Fed by the game's patches through host functions.
void set_first_person(bool on);
bool first_person_scene();

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
    int game_fps = 0;                 // the rate the game asks for (gEXSetRefreshRate), 0 = not given
    int dl_skipped_per_sec = 0;       // of dl_per_sec, the lists the frame cap passed over (Settings::frame_cap)
    int presented_per_sec = 0;        // frames put on the screen, last second
    int inbetween_per_sec = 0;        // of those, interpolated in-between frames (Settings::frame_cap 2)
    int inbetween_dropped_per_sec = 0;// lists the game asked 30 for that got no in-between frame (late, unsafe, a cut)
    char inbetween_reasons[112] = ""; // ... by reason ("budget 12 cut 1")
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
// A writer thread writes it while the game plays on; asked for while an
// earlier one is still being written, it is refused (capture_message says so).
void request_capture();
int capture_state();    // 0 none, 1 being written, 2 written, 3 failed, 4 refused
// The last capture event as one line for the touch screen ("Capture saved:
// gfx_task3.bin (11 s)", "Capture refused: ..."); *seq changes with each.
const char* capture_message(int* seq);
// Quitting: stop the capture writer between two requests, waiting up to
// timeout_ms for it; false if it is still busy (a stuck card).
bool capture_stop(int timeout_ms);
// The next `count` frames shown, as images (gfx_frame*.ppm), and this
// display list and the next with their RDRAM (gfx_task*.bin): the test
// scripts' CAPTURE command.
void request_frame_sequence(int count);
void request_capture_pair();

// A screenshot of what is on the screens: the top screen's eyes (the right
// one only when the frame was drawn in 3D) and the touch screen, as BMPs in
// the game's folder under screenshots/, named by date and time.
void request_screenshot();
int screenshot_state(); // 0 none, 1 being written, 2 written, 3 failed
const char* screenshot_name();      // the last one's name, without _top.bmp etc.

}   // namespace rt64_3ds

#endif
