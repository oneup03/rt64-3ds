// The citro3d renderer: interprets each display list into a FrameRecord and
// replays it onto the top screen (mono for now; the stereo pair follows the
// same replay with per-draw shifts).
#include <3ds.h>
#include <citro3d.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rt64_3ds.h"
#include "rt64_3ds_dl.h"
#include "rt64_3ds_record.h"
#include "rt64_3ds_texture.h"
#include "rt64_3ds_tev.h"
#include "rt64_3ds_shbin.h"

extern "C" void C3Di_UpdateContext(void);

namespace {

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

constexpr uint32_t kVboVerts = 65536;

using GpuVertex = rt64_3ds::Vtx3ds;   // the record's vertex is the PICA's attribute layout

#define g_stats rt64_3ds::mutable_stats()
int g_debug_gfx_delay_ms = 0;
volatile bool g_quitting = false;
rt64_3ds::Progress g_progress{};
const rt64_3ds::DrawRecord* g_progress_draw = nullptr;

class C3dRenderContext final : public ultramodern::renderer::RendererContext {
public:
    C3dRenderContext(uint8_t* rdram) : rdram_(rdram), interp_(rdram), textures_(rdram) {
        setup_result = ultramodern::renderer::SetupResult::Success;
        init();
    }
    ~C3dRenderContext() override {}

    bool valid() override { return ok_; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
    void enable_instant_present() override {}
    void send_dummy_workload(uint32_t fb_address) { (void)fb_address; }
    void update_screen() override {}
    void shutdown() override {}
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }

    void send_dl(const OSTask* task) override {
        if (!ok_ || g_quitting) return;
        u64 t0 = svcGetSystemTick();
        frame_.clear();
        vbo_idx_ ^= 1;
        frame_.verts.set_storage((rt64_3ds::Vtx3ds*)vbo_[vbo_idx_], kVboVerts);
        g_progress.phase = 1;
        maybe_capture(task);
        bool present = interp_.run(task->t.data_ptr, frame_);
        u64 t1 = svcGetSystemTick();
        dl_count_++;
        if (cam_log_now_) {
            cam_log_now_ = false;
            const float* c = frame_.cam;
            u64 now = svcGetSystemTick();
            static u64 last = 0;
            // DK64 (debug only): the camera eye/target from character_change_array (0x807FC924) + 0x210 / 0x228.
            auto rf = [&](uint32_t va) { float f; uint32_t w = *(const uint32_t*)(rdram_ + ((va - 0x80000000u) & 0xFFFFFC)); memcpy(&f, &w, 4); return f; };
            uint32_t cc = *(const uint32_t*)(rdram_ + (0x807FC924u - 0x80000000u));
            float ex = 0, ey = 0, ez = 0, ax = 0, ay = 0, az = 0;
            if (cc >= 0x80000000u && cc < 0x80800000u) { ex = rf(cc + 0x210); ey = rf(cc + 0x214); ez = rf(cc + 0x218); ax = rf(cc + 0x228); ay = rf(cc + 0x22C); az = rf(cc + 0x230); }
            (void)c;
            fprintf(stderr, "cam %u: dt %.1f ms eye %.2f %.2f %.2f at %.2f %.2f %.2f\n", frames_,
                    last ? (double)(now - last) * 1000.0 / SYSCLOCK_ARM11 : 0.0, ex, ey, ez, ax, ay, az);
            last = now;
        }
        if (present) replay_and_present();
        if (g_debug_gfx_delay_ms > 0) svcSleepThread((s64)g_debug_gfx_delay_ms * 1000000);
        g_progress.phase = 0;
        u64 t2 = svcGetSystemTick();
        acc_gfx_ += t1 - t0;
        acc_replay_ += t2 - t1;
        if (window_start_ == 0) window_start_ = t0;
        else if (t2 - window_start_ >= SYSCLOCK_ARM11) {
            g_stats.dl_per_sec = (int)dl_count_;
            g_stats.gfx_ms = (float)acc_gfx_ * 1000.0f / (float)SYSCLOCK_ARM11 / (float)(dl_count_ ? dl_count_ : 1);
            g_stats.replay_ms = (float)acc_replay_ * 1000.0f / (float)SYSCLOCK_ARM11 / (float)(dl_count_ ? dl_count_ : 1);
            wait_ms_ = (float)acc_wait_ * 1000.0f / (float)SYSCLOCK_ARM11 / (float)(dl_count_ ? dl_count_ : 1);
            end_ms_ = (float)acc_end_ * 1000.0f / (float)SYSCLOCK_ARM11 / (float)(dl_count_ ? dl_count_ : 1);
            depth_ms_ = (float)acc_depth_ * 1000.0f / (float)SYSCLOCK_ARM11 / (float)(dl_count_ ? dl_count_ : 1);
            dl_count_ = 0;
            acc_gfx_ = acc_replay_ = acc_wait_ = acc_end_ = acc_depth_ = 0;
            window_start_ = t2;
            const rt64_3ds::InterpreterStats& is = interp_.stats();
            frames_in_window_ = (float)(frames_ - frames_at_window_ > 0 ? frames_ - frames_at_window_ : 1);
            frames_at_window_ = frames_;
            if (++report_ % 5 == 1) {
                float fpw = (float)(frames_ - frames_at_profile_ > 0 ? frames_ - frames_at_profile_ : 1);
                frames_at_profile_ = frames_;
                if (prof_on_) fprintf(stderr, "rt64-3ds:   replay profile per frame: textures %.1f tev %.1f state %.1f flush %.1f draw %.1f ms\n",
                        (float)prof_[0] * 1000.0f / SYSCLOCK_ARM11 / fpw, (float)prof_[1] * 1000.0f / SYSCLOCK_ARM11 / fpw,
                        (float)prof_[2] * 1000.0f / SYSCLOCK_ARM11 / fpw, (float)prof_[4] * 1000.0f / SYSCLOCK_ARM11 / fpw,
                        (float)prof_[3] * 1000.0f / SYSCLOCK_ARM11 / fpw);
                fprintf(stderr, "rt64-3ds: frame %u: %u draws %u verts, interp %.1f ms replay %.1f ms (gpu wait %.1f, end %.1f, depth %.1f), tex live %u (%u KB) %u uploads/frame, linear free %u KB, unknown ops %u ex %u, tev fallbacks %d, cimg %06x w%u, snapshots %u\n",
                        frames_, (unsigned)last_draws_, (unsigned)last_verts_, g_stats.gfx_ms, g_stats.replay_ms, wait_ms_, end_ms_, depth_ms_, textures_.live(), textures_.bytes() / 1024, last_uploads_,
                        (unsigned)(linearSpaceFree() / 1024), is.unknown, is.ex_unknown, g_stats.combiner_fallbacks, frame_.color_image, frame_.color_width, snapshots_);
                // Where the geometry lands: screen-space bounds of the last frame.
                float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f, minz = 1e9f, maxz = -1e9f;
                int behind = 0, tris = 0, rects = 0;
                for (const rt64_3ds::DrawRecord& d : frame_.draws) {
                    if (d.kind == rt64_3ds::DrawRecord::Tris) tris++; else rects++;
                    for (uint32_t i = d.first; i < d.first + d.count && i < frame_.verts.size(); i++) {
                        const rt64_3ds::Vtx3ds& v = frame_.verts[i];
                        if (v.w <= 0.0f) { behind++; continue; }
                        float x = v.x / v.w, y = v.y / v.w, z = v.z / v.w;
                        if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
                        if (z < minz) minz = z; if (z > maxz) maxz = z;
                    }
                }
                int textured = 0, untextured = 0;
                for (const rt64_3ds::DrawRecord& d : frame_.draws) { if (d.tex[0].valid) textured++; else untextured++; }
                fprintf(stderr, "rt64-3ds:   %d tri draws %d rect draws (%d textured, %d not, %u unresolved so far), x %.0f..%.0f y %.0f..%.0f z %.2f..%.2f, %d verts behind\n",
                        tris, rects, textured, untextured, is.tex_unresolved, minx, maxx, miny, maxy, minz, maxz, behind);
                if (!frame_.draws.empty()) {
                    const rt64_3ds::DrawRecord& d = frame_.draws[frame_.draws.size() / 2];
                    prof_[0] = prof_[1] = prof_[2] = prof_[3] = prof_[4] = 0;
                fprintf(stderr, "rt64-3ds:   mid draw: kind %d cc %06x %08x omh %06x oml %08x gm %06x tex0 %d %ux%u fmt %u/%u sc %d,%d-%d,%d\n",
                            (int)d.kind, d.cc_w0, d.cc_w1, d.othermode_h, d.othermode_l, d.geometry_mode, d.tex[0].valid,
                            d.tex[0].width, d.tex[0].height, d.tex[0].fmt, d.tex[0].siz, d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3]);
                }
            }
        }
    }

private:
    void init() {
        if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2)) {
            fprintf(stderr, "rt64-3ds: C3D_Init failed\n");
            return;
        }
        top_ = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        if (top_ == nullptr) {
            fprintf(stderr, "rt64-3ds: render target creation failed\n");
            return;
        }
        C3D_RenderTargetSetOutput(top_, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
        gfxSet3D(false);

        dvlb_ = DVLB_ParseFile((u32*)rt64_3ds_shbin, rt64_3ds_shbin_size);
        shaderProgramInit(&prog_);
        shaderProgramSetVsh(&prog_, &dvlb_->DVLE[0]);
        C3D_BindProgram(&prog_);
        u_xform_ = shaderInstanceGetUniformLocation(prog_.vertexShader, "xform");
        u_stereo_ = shaderInstanceGetUniformLocation(prog_.vertexShader, "stereo");
        u_uvscale_ = shaderInstanceGetUniformLocation(prog_.vertexShader, "uvscale");
        u_uv1_ = shaderInstanceGetUniformLocation(prog_.vertexShader, "uv1");

        C3D_AttrInfo* ai = C3D_GetAttrInfo();
        AttrInfo_Init(ai);
        AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
        AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
        AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);

        for (int i = 0; i < 2; i++) vbo_[i] = (GpuVertex*)linearAlloc(kVboVerts * sizeof(GpuVertex));
        if (vbo_[0] == nullptr || vbo_[1] == nullptr) {
            fprintf(stderr, "rt64-3ds: vertex buffer allocation failed\n");
            return;
        }

        // A white 8x8 texture for any unit a combiner samples without a
        // resolved texture: the PICA hangs on an unconfigured texture unit
        // (the emulator does not).
        if (C3D_TexInit(&white_, 8, 8, GPU_RGBA5551)) {
            memset(white_.data, 0xFF, 8 * 8 * 2);
            GSPGPU_FlushDataCache(white_.data, 8 * 8 * 2);
            C3D_TexSetFilter(&white_, GPU_NEAREST, GPU_NEAREST);
        }

        // The snapshot texture: the 240x400 colour buffer copied whole into
        // a 256x512 RGBA8 texture (same tiling, rows padded), for the game's
        // framebuffer effects.
        if (!C3D_TexInitVRAM(&snapshot_, 256, 512, GPU_RGBA8)) {
            if (!C3D_TexInit(&snapshot_, 256, 512, GPU_RGBA8)) fprintf(stderr, "rt64-3ds: snapshot texture allocation failed\n");
        }
        C3D_TexSetFilter(&snapshot_, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&snapshot_, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        interp_.set_snapshot_layout(40);

        // The N64's 320x240 sits centred on the 400 px wide screen for now.
        // Everything arrives in N64 screen pixels with y growing downward:
        // bottom edge = 240, top edge = 0.
        Mtx_OrthoTilt(&proj_, -40.0f, 360.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
        C3D_DepthMap(true, -1.0f, 0.0f);
        C3D_CullFace(GPU_CULL_NONE);
        ok_ = true;
        // GPU_DEBUG.TXT: "<start frame> <frame count> <sync 0/1> <every N frames>" (defaults 0, all, 1, 1).
        if (FILE* f = fopen("sdmc:/3ds/DK64/GPU_DEBUG.TXT", "r")) {
            int a = 0, b = 1000000000, c = 1, d = 1;
            if (fscanf(f, "%d %d %d %d", &a, &b, &c, &d) < 1) { a = 0; }
            fclose(f);
            gpu_debug_ = true;
            debug_from_ = (uint32_t)a; debug_count_ = (uint32_t)b; debug_sync_ = c != 0; debug_step_ = d > 0 ? (uint32_t)d : 1;
            fprintf(stderr, "rt64-3ds: GPU debug mode from frame %u for %u frames, sync %d, every %u\n", debug_from_, debug_count_, (int)debug_sync_, debug_step_);
        }
        if (FILE* f = fopen("sdmc:/3ds/DK64/PROFILE.TXT", "r")) { prof_on_ = true; fclose(f); }
        fprintf(stderr, "rt64-3ds: citro3d renderer up (linear free %u KB)\n", (unsigned)(linearSpaceFree() / 1024));
    }

    void replay_and_present() {
        last_draws_ = frame_.draws.size();
        last_verts_ = frame_.verts.size();
        frames_++;
        g_progress.frames = frames_;
        g_progress.draw_count = (uint32_t)frame_.draws.size();
        g_progress.draw_index = 0;
        g_progress.phase = 2;
        bool debug_frame = gpu_debug_ && frames_ >= debug_from_ && frames_ < debug_from_ + debug_count_ && ((frames_ - debug_from_) % debug_step_) == 0;
        if (debug_frame) {
            fprintf(stderr, "rt64-3ds: === frame %u: %u draws, cimg %06x\n", frames_, (unsigned)frame_.draws.size(), frame_.color_image);
            // Every draw of the frame goes to the log before the GPU sees it,
            // with its first vertices.
            for (size_t i = 0; i < frame_.draws.size(); i++) {
                const rt64_3ds::DrawRecord& d = frame_.draws[i];
                describe_draw((uint32_t)i, d);
                for (uint32_t k = d.first; k < d.first + 4 && k < d.first + d.count && k < frame_.verts.size(); k++) {
                    const rt64_3ds::Vtx3ds& v = frame_.verts[k];
                    fprintf(stderr, "rt64-3ds:    v%u x %.1f y %.1f z %.3f w %.3f u %.2f v %.2f rgba %02x%02x%02x%02x\n", k - d.first,
                            v.x / v.w, v.y / v.w, v.z / v.w, v.w, v.u, v.v, v.r, v.g, v.b, v.a);
                }
            }
        }
        u64 tb0 = svcGetSystemTick();
        C3D_FrameBegin(0);
        acc_wait_ += svcGetSystemTick() - tb0;
        maybe_capture_framebuffer();
        if (rt64_3ds::render_desc().depth_to_rdram && prev_depth_image_ != 0) {
            u64 td = svcGetSystemTick();
            depth_to_rdram(prev_depth_image_);
            acc_depth_ += svcGetSystemTick() - td;
        }
        g_progress.phase = 3;
        for (const rt64_3ds::DrawRecord& d : frame_.draws) {
            if (d.tex[0].valid && d.tex[0].snapshot) {
                if (capture_snapshot_frames_ > 0 && !fb_capture_pending_) { capture_snapshot_frames_--; fb_capture_pending_ = true; }
                // Ask the emulator harness for a screenshot of this frame
                // (rate-limited: one per second).
                static u64 last = 0;
                u64 now = svcGetSystemTick();
                if (now - last > SYSCLOCK_ARM11) { last = now; fprintf(stderr, "AUTOTEST shot snapshot-draw\n"); }
                break;
            }
        }
        if (frame_.snapshot_request && snapshot_.data != nullptr) {
            // The target still holds the last presented frame: copy it, one
            // 8x8-tile row at a time, into the wider texture. Sizes are in
            // bytes; a tile row of the 240-wide buffer is 30 tiles.
            // TextureCopy line widths and gaps are in 16-byte units.
            const u32 row = 240 * 8 * 4, gap = (256 - 240) * 8 * 4;
            C3D_SyncTextureCopy((u32*)top_->frameBuf.colorBuf, GX_BUFFER_DIM(row / 16, 0),
                                (u32*)snapshot_.data, GX_BUFFER_DIM(row / 16, gap / 16), 240 * 400 * 4, 0);
            snapshots_++;
        }
        C3D_RenderTargetClear(top_, C3D_CLEAR_ALL, 0x000000FF, 0);
        C3D_FrameDrawOn(top_);
        C3D_BindProgram(&prog_);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_xform_, &proj_);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo_, 0.0f, 0.0f, 0.0f, 0.0f);
        C3D_TexBind(0, &white_);
        C3D_TexBind(1, &white_);
        C3D_TexBind(2, &white_);
        last_tex_[0] = last_tex_[1] = &white_;
        last_tex_param_[0] = last_tex_param_[1] = 0xFFFFFFFFu;
        last_cc_[0] = last_cc_[1] = 0xFFFFFFFFu;
        last_misc_ = ~0ull;
        last_us_ = last_vs_ = -1.0f;
        last_uv1_[0] = -1.0f;
        last_plan_.stages = -1;

        // The interpreter wrote the frame's vertices straight into this
        // frame's linear buffer (the other one may still be read by the GPU).
        GpuVertex* vb = vbo_[vbo_idx_];
        size_t n = frame_.verts.size();
        GSPGPU_FlushDataCache(vb, n * sizeof(GpuVertex));
        C3D_BufInfo* bi = C3D_GetBufInfo();
        BufInfo_Init(bi);
        BufInfo_Add(bi, vb, sizeof(GpuVertex), 3, 0x210);

        int fallbacks = 0;
        for (size_t i = 0; i < frame_.draws.size(); i++) {
            const rt64_3ds::DrawRecord& d = frame_.draws[i];
            g_progress.draw_index = (uint32_t)i;
            g_progress_draw = &d;
            if (d.count < 3 || d.first + d.count > n) continue;
            apply_state(d, fallbacks);
            u64 td = tick();
            C3Di_UpdateContext();          // the state flush, timed apart from the draw itself
            u64 tf = tick();
            prof_[4] += tf - td;
            C3D_DrawArrays(GPU_TRIANGLES, d.first, d.count);
            prof_[3] += tick() - tf;
            if (debug_frame && debug_sync_) {
                // Finish the frame here and start it again: FrameBegin waits
                // for the GPU, so a hang stops with draw_index naming the
                // draw that caused it (phase 2 = waiting after that draw).
                g_progress.phase = 4;
                C3D_FrameEnd(0);
                g_progress.phase = 2;
                C3D_FrameBegin(0);
                C3D_FrameDrawOn(top_);
                g_progress.phase = 3;
                C3D_BindProgram(&prog_);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_xform_, &proj_);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo_, 0.0f, 0.0f, 0.0f, 0.0f);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_uvscale_, last_us_, last_vs_, 0.0f, 0.0f);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_uv1_, last_uv1_[0], last_uv1_[1], last_uv1_[2], last_uv1_[3]);
                bi = C3D_GetBufInfo();
                BufInfo_Init(bi);
                BufInfo_Add(bi, vb, sizeof(GpuVertex), 3, 0x210);
            }
        }
        g_progress.phase = 4;
        g_stats.combiner_fallbacks = fallbacks;
        g_stats.draws = (int)frame_.draws.size();
        g_stats.tris = (int)(frame_.verts.size() / 3);
        u64 te0 = svcGetSystemTick();
        C3D_FrameEnd(0);
        acc_end_ += svcGetSystemTick() - te0;
        interp_.note_presented_framebuffer(frame_.color_image);
        prev_depth_image_ = (frame_.color_width == 320) ? frame_.depth_image : 0;
        last_uploads_ = textures_.uploads_this_frame();
        textures_.end_frame();
    }

    void apply_state(const rt64_3ds::DrawRecord& d, int& fallbacks) {
        u64 tp0 = tick();
        // Textures.
        float us = 1.0f, vs = 1.0f, us1 = 1.0f, vs1 = 1.0f;
        for (int t = 0; t < 2; t++) {
            bool bound = false;
            if (d.tex[t].valid && d.tex[t].snapshot && snapshot_.data != nullptr) {
                if (last_tex_[t] != &snapshot_) {
                    C3D_TexBind(t, &snapshot_);
                    last_tex_[t] = &snapshot_; last_tex_param_[t] = 0xFFFFFFFFu;
                }
                if (t == 0) { us = 1.0f / 256.0f; vs = 1.0f / 512.0f; }
                else { us1 = 1.0f / 256.0f; vs1 = 1.0f / 512.0f; }
                bound = true;
            }
            else if (d.tex[t].valid) {
                const uint16_t* pal = (d.tex[t].fmt == 2 && d.tex[t].tlut_index + 256 <= frame_.tlut.size()) ? &frame_.tlut[d.tex[t].tlut_index] : nullptr;
                rt64_3ds::BoundTex b = textures_.get(d.tex[t], pal);
                if (b.tex != nullptr) {
                    GPU_TEXTURE_FILTER_PARAM f = d.tex[t].bilerp ? GPU_LINEAR : GPU_NEAREST;
                    GPU_TEXTURE_WRAP_PARAM ws = wrap_mode(d.tex[t].cms, d.tex[t].masks, d.tex[t].width);
                    GPU_TEXTURE_WRAP_PARAM wt = wrap_mode(d.tex[t].cmt, d.tex[t].maskt, d.tex[t].height);
                    uint32_t param = (uint32_t)f | ((uint32_t)ws << 4) | ((uint32_t)wt << 8);
                    if (b.tex != last_tex_[t] || param != last_tex_param_[t] || b.fresh) {
                        C3D_TexSetFilter(b.tex, f, f);
                        C3D_TexSetWrap(b.tex, ws, wt);
                        C3D_TexBind(t, b.tex);
                        last_tex_[t] = b.tex; last_tex_param_[t] = param;
                    }
                    if (t == 0) { us = b.uscale; vs = b.vscale; }
                    else { us1 = b.uscale; vs1 = b.vscale; }
                    bound = true;
                }
            }
            if (!bound && last_tex_[t] != &white_) {
                C3D_TexBind(t, &white_);
                last_tex_[t] = &white_; last_tex_param_[t] = 0xFFFFFFFFu;
            }
        }
        if (us != last_us_ || vs != last_vs_) {
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_uvscale_, us, vs, 0.0f, 0.0f);
            last_us_ = us; last_vs_ = vs;
        }
        {
            // TEXEL1: its own tile's shift and origin, its own texture size.
            float uv1[4];
            if (d.tex[1].valid && !d.tex[1].snapshot) { uv1[0] = d.uv1[0] * us1; uv1[1] = d.uv1[1] * vs1; uv1[2] = d.uv1[2] * us1; uv1[3] = d.uv1[3] * vs1; }
            else { uv1[0] = us; uv1[1] = vs; uv1[2] = 0.0f; uv1[3] = 0.0f; }
            if (memcmp(uv1, last_uv1_, sizeof(uv1)) != 0) {
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_uv1_, uv1[0], uv1[1], uv1[2], uv1[3]);
                memcpy(last_uv1_, uv1, sizeof(uv1));
            }
        }
        u64 tp1 = tick();
        prof_[0] += tp1 - tp0;

        // Combiner: only re-sent when the plan changes (every C3D_TexEnv call
        // dirties the whole stage set).
        if (d.cc_w0 != last_cc_[0] || d.cc_w1 != last_cc_[1] || d.othermode_h != last_omh_ || d.othermode_l != last_oml_ ||
            memcmp(d.prim, last_prim_, 4) != 0 || memcmp(d.env, last_env_, 4) != 0 || memcmp(d.fog, last_fog_, 4) != 0 ||
            d.prim_lod_frac != last_plf_ || d.kind != last_kind_ || d.geometry_mode != last_gm_) {
            // Plans are cached: the same combiner/mode/colour set recurs across
            // draws and frames.
            // Exact key: every input plan_tev reads.
            PlanKey pk{};
            pk.w[0] = d.cc_w0; pk.w[1] = d.cc_w1; pk.w[2] = d.othermode_h; pk.w[3] = d.othermode_l;
            memcpy(&pk.w[4], d.prim, 4); memcpy(&pk.w[5], d.env, 4); memcpy(&pk.w[6], d.fog, 4);
            pk.w[7] = (uint32_t)d.prim_lod_frac | ((uint32_t)d.kind << 8) | (d.geometry_mode & 0x10000);
            uint32_t h = 2166136261u;
            for (uint32_t v : pk.w) { h ^= v; h *= 16777619u; h ^= h >> 15; }
            uint32_t slot = h & (kPlanCache - 1);
            const rt64_3ds::TevPlan* plan;
            if (plan_valid_[slot] && memcmp(&plan_keys_[slot], &pk, sizeof(pk)) == 0) {
                plan = &plan_cache_[slot];
            }
            else {
                rt64_3ds::plan_tev(d, plan_cache_[slot]);
                plan_keys_[slot] = pk; plan_valid_[slot] = true;
                plan = &plan_cache_[slot];
                fallbacks += plan->fallbacks;
            }
            if (plan->stages != last_plan_.stages || memcmp(plan->stage, last_plan_.stage, sizeof(rt64_3ds::TevStage) * plan->stages) != 0) {
                rt64_3ds::apply_tev(*plan);
                last_plan_ = *plan;
            }
            last_cc_[0] = d.cc_w0; last_cc_[1] = d.cc_w1; last_omh_ = d.othermode_h; last_oml_ = d.othermode_l;
            memcpy(last_prim_, d.prim, 4); memcpy(last_env_, d.env, 4); memcpy(last_fog_, d.fog, 4);
            last_plf_ = d.prim_lod_frac; last_kind_ = d.kind; last_gm_ = d.geometry_mode;
        }
        u64 tp2 = tick();
        prof_[1] += tp2 - tp1;

        // Depth: reversed map, so "less" becomes GEQUAL. Rects ignore depth
        // unless the RDP compares (they carry z = 0 = near).
        uint32_t l = d.othermode_l;
        uint64_t misc_key = ((uint64_t)l << 32) | ((uint64_t)(d.othermode_h & 0x300000) << 8) | ((uint64_t)d.kind << 28) | (uint64_t)d.blend[3] |
                            ((uint64_t)(uint16_t)d.scissor[0] << 8) ^ ((uint64_t)(uint16_t)d.scissor[1] << 16) ^ ((uint64_t)(uint16_t)d.scissor[2] << 12) ^ ((uint64_t)(uint16_t)d.scissor[3] << 20);
        if (misc_key == last_misc_) { prof_[2] += tick() - tp2; return; }
        last_misc_ = misc_key;
        bool zcmp = (l & 0x10) != 0 && d.kind == rt64_3ds::DrawRecord::Tris;
        bool zupd = (l & 0x20) != 0 && d.kind == rt64_3ds::DrawRecord::Tris;
        uint32_t zmode = (l >> 10) & 3;
        C3D_DepthTest(zcmp, zcmp ? GPU_GEQUAL : GPU_ALWAYS, zupd ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
        // Decals sit on coplanar surfaces: nudge them toward the viewer
        // (reversed depth: nearer is larger).
        C3D_DepthMap(true, -1.0f, zmode == 3 ? 0.0004f : 0.0f);

        // Blending: force-blend with memory colour by source alpha.
        uint32_t cyc = (d.othermode_h >> 20) & 3;
        uint32_t bl = (cyc == 1) ? (l >> 16) & 0xFFFF : (l >> 16) & 0xFFFF;   // both cycles' fields
        bool force_bl = (l & 0x4000) != 0;
        uint32_t m2 = (cyc == 1) ? ((l >> 20) & 3) : ((l >> 22) & 3);   // second cycle in 2-cycle mode
        uint32_t b2 = (cyc == 1) ? ((l >> 16) & 3) : ((l >> 18) & 3);
        (void)bl;
        bool blend = force_bl && m2 == 1 && (b2 == 0 || b2 == 1);
        if (blend) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        else C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

        // Alpha compare, and the coverage-from-alpha "texture edge" modes.
        uint32_t ac = l & 3;
        if (ac == 1) C3D_AlphaTest(true, GPU_GEQUAL, d.blend[3] ? d.blend[3] : 1);
        else if (ac == 3) C3D_AlphaTest(true, GPU_GREATER, 0);
        else if ((l & 0x3000) == 0x3000) C3D_AlphaTest(true, GPU_GEQUAL, 0x80);
        else C3D_AlphaTest(false, GPU_ALWAYS, 0);

        // Culling.
        // Face culling is done in the interpreter (screen-space winding).
        C3D_CullFace(GPU_CULL_NONE);

        // Scissor in N64 pixels -> target pixels (rotated framebuffer).
        // NO_SCISSOR.TXT disables it (to see draws a wrong clip would hide).
        static int no_scissor = -1;
        if (no_scissor < 0) { FILE* f = fopen("sdmc:/3ds/DK64/NO_SCISSOR.TXT", "r"); no_scissor = f ? 1 : 0; if (f) fclose(f); }
        int x0 = d.scissor[0] + 40, y0 = d.scissor[1], x1 = d.scissor[2] + 40, y1 = d.scissor[3];
        if (no_scissor) { x0 = 0; y0 = 0; x1 = 400; y1 = 240; }
        // The original game's own 320-wide scissor is stretched to the wide
        // screen: the patched projection already fills it.
        if (d.scissor[0] <= 0 && d.scissor[2] >= 320) { x0 = 0; x1 = 400; }
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > 400) x1 = 400; if (y1 > 240) y1 = 240;
        // Mtx_OrthoTilt maps screen y = 0 to framebuffer x = 240 and the
        // screen's left edge to framebuffer y = 400: both axes run backwards.
        if (x1 <= x0 || y1 <= y0) C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 0, 0);
        else C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - y1), (u32)(400 - x1), (u32)(240 - y0), (u32)(400 - x0));
        prof_[2] += tick() - tp2;
    }

public:
    // GFX_CAPTURE.TXT holds a frame number: that frame's RDRAM and OSTask go
    // to gfx_task.bin for tools/dl_test (host-side interpreter profiling).
    void maybe_capture(const OSTask* task) {
        static int want = -2;
        if (want == -2) {
            want = -1;
            if (FILE* f = fopen("sdmc:/3ds/DK64/GFX_CAPTURE.TXT", "r")) {
                char word[16] = {};
                if (fscanf(f, "%15s", word) == 1) {
                    if (strcmp(word, "snap") == 0) capture_snapshot_frames_ = 3;   // the first three frames drawing from a snapshot
                    else if (strcmp(word, "cam") == 0) {
                        // "cam <first frame> <count>": log the camera projection per frame
                        if (fscanf(f, "%d %d", &cam_log_first_, &cam_log_count_) != 2) cam_log_count_ = 0;
                    }
                    else if (strcmp(word, "seq") == 0) {
                        // "seq <first frame> <count>": the rendered frames only, one file each
                        if (fscanf(f, "%d %d", &seq_first_, &seq_count_) != 2) seq_count_ = 0;
                    }
                    else want = atoi(word);
                }
                fclose(f);
            }
        }
        if (seq_count_ > 0 && (int)frames_ >= seq_first_ && (int)frames_ < seq_first_ + seq_count_) fb_capture_pending_ = true;
        if (cam_log_count_ > 0 && (int)frames_ >= cam_log_first_ && (int)frames_ < cam_log_first_ + cam_log_count_) cam_log_now_ = true;
        if (want < 0 || (int)frames_ != want) return;
        want = -1;
        // Snapshot RDRAM and the task now; the slow SD write happens after
        // this frame is drawn (the game thread keeps changing RDRAM).
        capture_copy_ = (uint8_t*)malloc(16u * 1024 * 1024);
        if (capture_copy_ == nullptr) { fprintf(stderr, "rt64-3ds: no memory for an RDRAM snapshot\n"); return; }
        memcpy(capture_copy_, rdram_, 16u * 1024 * 1024);
        capture_task_ = *task;
        fprintf(stderr, "rt64-3ds: captured gfx task of frame %u\n", frames_);
        fb_capture_pending_ = true;   // the next frame begin saves what this task rendered
    }

    // After C3D_FrameBegin the top target still holds the previous frame:
    // untile it into linear memory and save it as gfx_frame.ppm (400x240),
    // to compare with the host reference render of gfx_task.bin.
    void maybe_capture_framebuffer() {
        if (!fb_capture_pending_) return;
        fb_capture_pending_ = false;
        if (capture_copy_ != nullptr) {
            if (FILE* f = fopen("sdmc:/3ds/DK64/gfx_task.bin", "wb")) {
                fwrite(&capture_task_, 1, sizeof(OSTask), f);
                for (uint32_t off = 0; off < 16u * 1024 * 1024; off += 65536) fwrite(capture_copy_ + off, 1, 65536, f);
                fclose(f);
            }
            free(capture_copy_);
            capture_copy_ = nullptr;
        }
        if (snapshot_.data != nullptr && seq_count_ == 0) {
            // The snapshot texture as stored (256x512 RGBA8, tiled), raw.
            if (FILE* f = fopen("sdmc:/3ds/DK64/gfx_snapshot.raw", "wb")) { fwrite(snapshot_.data, 1, 256 * 512 * 4, f); fclose(f); }
        }
        static int frame_captures = 0;
        char fname[64];
        snprintf(fname, sizeof(fname), frame_captures == 0 ? "sdmc:/3ds/DK64/gfx_frame.ppm" : "sdmc:/3ds/DK64/gfx_frame%d.ppm", frame_captures);
        frame_captures++;
        // Read the tiled target straight from VRAM (the CPU can read it; the
        // emulator flushes its cached copy on such reads): 8x8 tiles, 30 per
        // tile row, Morton order inside a tile.
        const uint32_t* lin = (const uint32_t*)top_->frameBuf.colorBuf;
        if (FILE* f = fopen(fname, "wb")) {
            // The target is 240 wide and 400 tall (rotated): written as is;
            // the host rotates it to the 400x240 screen.
            fprintf(f, "P6\n240 400\n255\n");
            static uint8_t row[240 * 3];
            for (int r = 0; r < 400; r++) {
                for (int c = 0; c < 240; c++) {
                    uint32_t tile = (uint32_t)(r >> 3) * 30 + (uint32_t)(c >> 3);
                    uint32_t m = (c & 1) | ((r & 1) << 1) | ((c & 2) << 1) | ((r & 2) << 2) | ((c & 4) << 2) | ((r & 4) << 3);
                    uint32_t p = lin[tile * 64 + m];
                    row[c * 3 + 0] = (uint8_t)(p >> 24); row[c * 3 + 1] = (uint8_t)(p >> 16); row[c * 3 + 2] = (uint8_t)(p >> 8);
                }
                fwrite(row, 1, sizeof(row), f);
            }
            fclose(f);
        }
        fprintf(stderr, "rt64-3ds: captured gfx frame\n");
    }

    static void describe_draw(uint32_t i, const rt64_3ds::DrawRecord& d) {
        fprintf(stderr, "rt64-3ds: draw %u: kind %d first %u count %u cc %06x %08x omh %06x oml %08x gm %06x prim %02x%02x%02x%02x env %02x%02x%02x%02x sc %d,%d-%d,%d",
                i, (int)d.kind, d.first, d.count, d.cc_w0, d.cc_w1, d.othermode_h, d.othermode_l, d.geometry_mode,
                d.prim[0], d.prim[1], d.prim[2], d.prim[3], d.env[0], d.env[1], d.env[2], d.env[3],
                d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3]);
        for (int t = 0; t < 2; t++) {
            if (d.tex[t].valid) {
                fprintf(stderr, " tex%d %06x/%u fmt %u siz %u %ux%u m%u/%u c%u/%u%s", t, d.tex[t].addr, d.tex[t].pitch, d.tex[t].fmt, d.tex[t].siz,
                        d.tex[t].width, d.tex[t].height, d.tex[t].masks, d.tex[t].maskt, d.tex[t].cms, d.tex[t].cmt, d.tex[t].snapshot ? " SNAP" : "");
            }
        }
        fprintf(stderr, " vp %.1f %.1f %.1f %.1f proj %.3f %.3f %.3f %.3f%s\n", d.dbg_vp[0], d.dbg_vp[1], d.dbg_vp[2], d.dbg_vp[3],
                d.dbg_proj[0], d.dbg_proj[1], d.dbg_proj[2], d.dbg_proj[3], d.perspective ? "" : " ortho");
    }

    static GPU_TEXTURE_WRAP_PARAM wrap_mode(uint8_t cm, uint8_t mask, uint16_t size) {
        if (mask == 0 || (cm & 1)) return GPU_CLAMP_TO_EDGE;
        if ((1u << mask) != size) return GPU_CLAMP_TO_EDGE;
        return (cm & 2) ? GPU_MIRRORED_REPEAT : GPU_REPEAT;
    }

    uint8_t* rdram_;
    rt64_3ds::Interpreter interp_;
    rt64_3ds::TextureCache textures_;
    rt64_3ds::FrameRecord frame_;
    bool ok_ = false;
    C3D_RenderTarget* top_ = nullptr;
    bool fb_capture_pending_ = false;
    uint32_t prev_depth_image_ = 0;

    // The finished frame's depth (the GPU is idle after C3D_FrameBegin) into
    // the game's 320x240 RDRAM depth buffer in the RDP's format: one sample
    // per 4x4 block, read straight from VRAM (8x8 Morton tiles, the target
    // rotated: row = screen x, column = 239 - screen y). Stored depth is
    // 1 - z' with z' = (clip z / w + 1) / 2; the RDP's 18-bit z is
    // z' * 0x3FE00 (viewport z scale and offset 0x1FF, x32, 15.3), stored
    // as 3-bit exponent (leading ones) and 11-bit mantissa, dz 0.
    static uint16_t n64_depth(float zp) {
        if (zp < 0.0f) zp = 0.0f;
        if (zp > 1.0f) zp = 1.0f;
        uint32_t z = (uint32_t)(zp * 261632.0f);
        if (z > 0x3FFFF) z = 0x3FFFF;
        uint32_t e, m;
        if (z < 0x20000) { e = 0; m = z >> 6; }
        else if (z < 0x30000) { e = 1; m = z >> 5; }
        else if (z < 0x38000) { e = 2; m = z >> 4; }
        else if (z < 0x3C000) { e = 3; m = z >> 3; }
        else if (z < 0x3E000) { e = 4; m = z >> 2; }
        else if (z < 0x3F000) { e = 5; m = z >> 1; }
        else if (z < 0x3F800) { e = 6; m = z; }
        else { e = 7; m = z; }
        return (uint16_t)((e << 13) | ((m & 0x7FF) << 2));
    }
    void depth_to_rdram(uint32_t zimg) {
        const uint32_t* db = (const uint32_t*)top_->frameBuf.depthBuf;
        if (db == nullptr || (zimg & 0xFFFFFF) + 320 * 240 * 2 > 16u * 1024 * 1024) return;
        uint8_t* z = rdram_ + (zimg & 0xFFFFFF);
        constexpr int B = 8;   // block size: VRAM reads by the CPU are slow on the console
        for (int by = 0; by < 240; by += B) {
            for (int bx = 0; bx < 320; bx += B) {
                int sx = bx + B / 2 + 40, sy = by + B / 2;   // block centre on the 400-wide screen
                int r = sx, c = 239 - sy;
                uint32_t tile = (uint32_t)(r >> 3) * 30 + (uint32_t)(c >> 3);
                uint32_t mo = (c & 1) | ((r & 1) << 1) | ((c & 2) << 1) | ((r & 2) << 2) | ((c & 4) << 2) | ((r & 4) << 3);
                uint32_t d = db[tile * 64 + mo] & 0xFFFFFF;
                uint16_t v = n64_depth(1.0f - (float)d * (1.0f / 16777215.0f));
                for (int y = by; y < by + B; y++) {
                    uint8_t* row = z + (uint32_t)y * 640;
                    for (int x = bx; x < bx + B; x++) *(uint16_t*)(row + ((x * 2) ^ 2)) = v;
                }
            }
        }
    }
    int capture_snapshot_frames_ = 0;
    int seq_first_ = 0, seq_count_ = 0;
    int cam_log_first_ = 0, cam_log_count_ = 0;
    bool cam_log_now_ = false;
    uint8_t* capture_copy_ = nullptr;
    OSTask capture_task_{};
    C3D_Tex white_{};
    C3D_Tex snapshot_{};
    uint32_t snapshots_ = 0;
    bool gpu_debug_ = false, debug_sync_ = true;
    uint32_t debug_from_ = 0, debug_count_ = 0, debug_step_ = 1;
    DVLB_s* dvlb_ = nullptr;
    shaderProgram_s prog_{};
    int u_xform_ = -1, u_stereo_ = -1, u_uvscale_ = -1, u_uv1_ = -1;
    float last_uv1_[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    C3D_Mtx proj_{};
    GpuVertex* vbo_[2] = { nullptr, nullptr };
    int vbo_idx_ = 0;
    uint32_t frames_ = 0, report_ = 0;
    size_t last_draws_ = 0, last_verts_ = 0;
    uint32_t last_uploads_ = 0;
    uint32_t dl_count_ = 0;
    u64 window_start_ = 0, acc_gfx_ = 0, acc_replay_ = 0, acc_wait_ = 0, acc_end_ = 0, acc_depth_ = 0;
    float wait_ms_ = 0, end_ms_ = 0, depth_ms_ = 0;
    u64 prof_[5] = {};
    bool prof_on_ = false;   // PROFILE.TXT: time the replay's sections (a syscall per sample)
    u64 tick() const { return prof_on_ ? svcGetSystemTick() : 0; }
    uint32_t frames_at_profile_ = 0;
    C3D_Tex* last_tex_[2] = { nullptr, nullptr };
    uint32_t last_tex_param_[2] = { 0, 0 };
    uint32_t last_cc_[2] = { 0, 0 }, last_omh_ = 0, last_oml_ = 0, last_gm_ = 0;
    uint8_t last_prim_[4] = {}, last_env_[4] = {}, last_fog_[4] = {}, last_plf_ = 0, last_kind_ = 0;
    rt64_3ds::TevPlan last_plan_{};
    static constexpr uint32_t kPlanCache = 256;
    rt64_3ds::TevPlan plan_cache_[kPlanCache]{};
    struct PlanKey { uint32_t w[8]; };
    PlanKey plan_keys_[kPlanCache]{};
    bool plan_valid_[kPlanCache]{};
    uint64_t last_misc_ = ~0ull;
    float last_us_ = -1.0f, last_vs_ = -1.0f;
    float frames_in_window_ = 1.0f;
    uint32_t frames_at_window_ = 0;
};

}   // namespace

std::unique_ptr<ultramodern::renderer::RendererContext>
rt64_3ds::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    (void)window_handle; (void)developer_mode;
    return std::make_unique<C3dRenderContext>(rdram);
}


const rt64_3ds::Progress& rt64_3ds::progress() { return g_progress; }

void rt64_3ds::dump_progress() {
    fprintf(stderr, "rt64-3ds: progress: frame %u phase %u draw %u of %u\n", g_progress.frames, g_progress.phase, g_progress.draw_index, g_progress.draw_count);
    if (g_progress.phase == 3 && g_progress_draw != nullptr) C3dRenderContext::describe_draw(g_progress.draw_index, *g_progress_draw);
}

void rt64_3ds::set_debug_gfx_delay_ms(int ms) { g_debug_gfx_delay_ms = ms; }
void rt64_3ds::set_quitting() { g_quitting = true; }
