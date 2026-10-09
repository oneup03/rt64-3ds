// F3DEX2 or F3DEX (+ RT64 extended GBI) display-list interpreter producing a
// FrameRecord. CPU only; the vertex transform, lighting and RDP state
// tracking happen here so the backend only replays draws.
#ifndef RT64_3DS_DL_H
#define RT64_3DS_DL_H

#include <cstdint>

#include "rt64_3ds_interp.h"
#include "rt64_3ds_record.h"
#include "rt64_3ds_types.h"

namespace rt64_3ds {

struct InterpreterStats {
    uint32_t commands = 0, tris = 0, rects = 0, unknown = 0, ex_unknown = 0;
    uint32_t dl_depth_overflow = 0;
    uint32_t tex_unresolved = 0;   // draws that wanted a texture no load covered
    uint32_t snapshots = 0;        // frames that copied the screen
    uint32_t frame_samples = 0;    // draws that sample the frame drawn so far (sample_drawn_frame)
    uint32_t op_hist[256] = {};    // commands run per opcode
    uint32_t mtx_loads = 0, vtx_loaded = 0, draws_merged = 0, probes = 0, clipped = 0;
    uint32_t rects_merged = 0;     // rectangles stretched as slices of a full-width row (merge_rect_slices)
    uint32_t branch_z_taken = 0, branch_z_not = 0, cull_dl = 0;   // level-of-detail branches, cull tests
    uint32_t refresh_rate = 0;     // the game's intended frame rate (gEXSetRefreshRate), 0 = not given
    uint32_t image_tris = 0;       // triangles sampled from a whole image (merge_image_tiles)
    uint32_t image_fallbacks = 0;  // ... and those of such tiles that kept their tile (coordinates outside it)
    uint32_t image_rects = 0;      // rectangles sampled from a whole image
    uint32_t rects_compacted = 0;  // ... merged into the record before
    // Interpreter::set_profile: run's time by kind of command (host ticks:
    // svcGetSystemTick on the console), the triangles' own included in theirs.
    enum { kProfVtx, kProfTri, kProfMtx, kProfTex, kProfRect, kProfFlow, kProfEx, kProfOther, kProfKinds };
    uint64_t prof[kProfKinds] = {};
};

class Interpreter {
public:
    explicit Interpreter(uint8_t* rdram);

    // Runs a gfx task's display list into `out`. Returns true when a
    // G_RDPFULLSYNC was reached (a frame to present).
    bool run(uint32_t data_ptr, FrameRecord& out);
    // After run: the list was one whole frame drawn into the screen alone
    // (a G_RDPFULLSYNC; every draw into the colour image it ends on, a
    // presented framebuffer, or fills of the depth buffer; nothing into
    // off-screen images, frame copies or snapshots).
    bool last_list_plain() const;
    // Time run's commands by kind into stats().prof (costs a tick read a
    // command: for profiling only).
    void set_profile(bool on);
    void clear_profile() { for (uint64_t& v : stats_.prof) v = 0; }
    // The frame cap (Settings::frame_cap): runs a list for its state alone
    // (no vertices, triangles or rectangles: a fraction of run's cost) and
    // returns whether it was such a whole frame on the screen, which can go
    // undrawn. Then the interpreter holds the state running it would have
    // left; otherwise it is put back as it was before, and the list must
    // be run. `scratch` receives what the state commands record.
    bool pass_over(uint32_t data_ptr, FrameRecord& scratch);
    const InterpreterStats& stats() const { return stats_; }
    // The colour image a presented frame was drawn to: reads from it by a
    // later list are the game copying the screen (framebuffer effects).
    void note_presented_framebuffer(uint32_t addr);
    // Stereo support: the projection group id of the world camera (its
    // projection is recorded in FrameRecord::cam), and whether orthographic
    // draws with an untagged modelview start a new record at every modelview
    // load, so a reticle made of quads stays one quad per record.
    void set_world_proj_ids(const uint32_t* ids, int count);   // up to 4
    void set_split_untagged_ortho(bool on);
    // Always take G_BRANCH_Z (the near model), as the recomp frontends do.
    void set_force_branch_z(bool on);
    // The display-list encoding the game's lists use (F3DEX2 by default).
    void set_gbi(Gbi gbi);
    // Draw off-screen colour images into textures of their own instead of
    // dropping them (see FrameRecord::rtts).
    void set_render_to_texture(bool on);
    // RenderDesc::sample_drawn_frame: loads from the colour image being
    // drawn, after draws went into it, sample the frame drawn so far
    // (TexDesc::frame, FrameRecord::frame_sample_last), and images of
    // another width than the frame's are drawn into textures of their own.
    void set_sample_drawn_frame(bool on);
    // RenderDesc::primary_world_instance: number the projection instances
    // (DrawRecord::proj_inst) and keep the HUD camera's projection per
    // instance (FrameRecord::inst_cams).
    void set_primary_world_instance(bool on);
    // RenderDesc::unify_world_depth.
    void set_unify_world_depth(bool on);
    // Where the game's frame sits on the top screen (RenderDesc::vi_width
    // and vi_height): origins, stretching, clipping and frame copies follow it.
    void set_screen(const ScreenGeometry& geo);
    // RenderDesc::fix_rect_lr and merge_rect_slices (rectangle edges and
    // widescreen detection as the desktop fork has them; both off by default).
    void set_fix_rect_lr(bool on);
    // RenderDesc::scissor_edge_slack (0 = scissors as the list sets them).
    void set_scissor_edge_slack(int px);
    void set_merge_rect_slices(bool on);
    // RenderDesc::hud_camera_id: the first perspective projection loaded under
    // this group is kept in FrameRecord::hud_cam (0 = none).
    void set_hud_camera_id(uint32_t id);
    // RenderDesc::merge_image_tiles: triangles textured from LOADTILE cuts of
    // a wider image sample one texture of the image (TexDesc of its whole
    // width), so draws that differ only in the part they loaded merge.
    void set_merge_image_tiles(bool on);
    // Deferred triangles: run records each triangle of a record whose texel
    // coordinates are the vertices' own as the vertices it was loaded with,
    // and finish_triangles, called after it, does the rest (culling, texel
    // coordinates, clipping, the vertices). For a reader on one core and
    // the drawing on another: run is what the game waits for. Not with
    // merge_image_tiles (its compaction renumbers the records). `vertices`:
    // the loads' transforms, lighting and fog wait too, each vertex done
    // when first used (G_BRANCH_Z computes its depth alone); not while
    // frame interpolation records the list. `notify(arg)` is called on
    // run's thread each time run publishes triangles for drain_triangles.
    void set_defer_triangles(bool on, bool vertices = true, void (*notify)(void*) = nullptr, void* notify_arg = nullptr);
    // On another thread while run is running (the one that then calls
    // finish_triangles): the output of the triangles run has published so
    // far, so little is left once it returns.
    void drain_triangles();
    // After run: the deferred triangles' vertices into `out` (run's), the
    // records' first and count, and unify_world_depth. False when run left
    // none (deferral off, a pass_over, an in-between run).
    bool finish_triangles(FrameRecord& out);

    // Frame interpolation (RenderDesc::frame_interpolation, Settings::
    // frame_cap 2). On, every run records the transforms the list draws
    // with, under the gEXMatrixGroup tags in force (rt64_3ds_interp.h), and
    // keeps the state the list started from. Off, nothing changes.
    void set_interpolation(bool on);
    // After run: pairs the list's transforms with the list's before as RT64
    // does and computes the in-between frame's at `weight` (0..1 from the
    // previous list to this one; < 0 matches without computing them).
    // set_world_proj_ids names the world cameras for the cut test.
    interp::MatchResult match_previous(float weight);
    // The next in-between of the same pair (a game frame of three or four
    // refreshes has two or three): the matches match_previous made, at a
    // new weight, for build_inbetween. Returns the interpolation counts.
    interp::MatchResult reweight_previous(float weight);
    // Runs the list run last a second time, from the state it started from,
    // drawing with the interpolated transforms (and tile scrolls and
    // vertices where the tags ask), into `out`; the state the first run
    // left is put back. False when the second run did not follow the first
    // (the frame must not be shown). `tris` receives its triangle count.
    bool run_inbetween(uint32_t data_ptr, FrameRecord& out, uint32_t* tris = nullptr);
    // The same frame without running the list again (what the renderer
    // uses): `real`'s records (the frame run() drew) with every triangle's
    // vertices transformed anew by the interpolated matrices, culled and
    // clipped; rectangles, lighting, texture generation and state are the
    // game frame's. `tris` receives the triangles drawn.
    bool build_inbetween(const FrameRecord& real, FrameRecord& out, uint32_t* tris = nullptr);
    // A second thread for build_inbetween: with one set, a list of enough
    // triangles is split at a record near its middle, the records after it
    // built on the helper into a buffer of its own, which is then copied
    // after the calling thread's (the same frame as one thread builds).
    // start(job, arg) runs job(arg) there and returns at once; finish()
    // returns once it has. nullptr: one thread.
    struct Helper {
        virtual void start(void (*job)(void*), void* arg) = 0;
        virtual void finish() = 0;
        virtual ~Helper() = default;
    };
    void set_inbetween_helper(Helper* helper) { helper_ = helper; }
    // For the log: what interpolates record `draw` of the list run last (its
    // first triangle's transform and projection, their group ids, what they
    // were paired with and their scale and offset before, now and in the
    // in-between), as text into out.
    void describe_inbetween_draw(uint32_t draw, char* out, size_t n) const;

private:
    struct Impl;
    struct Interp;
    struct Defer;
    Impl* impl_;
    Defer* dfr_ = nullptr;      // set_defer_triangles
    Impl* saved_ = nullptr;     // pass_over's copy of the state, to put back
    Impl* before_ = nullptr;    // frame interpolation: the state the last run started from
    Impl* after_ = nullptr;     // ... and the state it left
    Interp* ip_ = nullptr;
    Helper* helper_ = nullptr;  // set_inbetween_helper
    InterpreterStats stats_;
};

}   // namespace rt64_3ds

#endif
