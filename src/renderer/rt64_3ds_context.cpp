// The citro3d renderer: interprets each display list into a FrameRecord and
// replays it onto the top screen (mono for now; the stereo pair follows the
// same replay with per-draw shifts).
#include <3ds.h>
#include <citro3d.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "rt64_3ds.h"
#include "rt64_3ds_dl.h"
#include "rt64_3ds_record.h"
#include "rt64_3ds_texture.h"
#include "rt64_3ds_tev.h"
#include "rt64_3ds_shbin.h"

namespace {

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

constexpr uint32_t kVboVerts = 65536;

struct GpuVertex { float x, y, z, w; float u, v; uint8_t r, g, b, a; };

#define g_stats rt64_3ds::mutable_stats()

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
        if (!ok_) return;
        u64 t0 = svcGetSystemTick();
        frame_.clear();
        bool present = interp_.run(task->t.data_ptr, frame_);
        u64 t1 = svcGetSystemTick();
        dl_count_++;
        if (present) replay_and_present();
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
            dl_count_ = 0;
            acc_gfx_ = acc_replay_ = acc_wait_ = acc_end_ = 0;
            window_start_ = t2;
            const rt64_3ds::InterpreterStats& is = interp_.stats();
            frames_in_window_ = (float)(frames_ - frames_at_window_ > 0 ? frames_ - frames_at_window_ : 1);
            frames_at_window_ = frames_;
            if (++report_ % 5 == 1) {
                fprintf(stderr, "rt64-3ds:   replay profile per frame: textures %.1f tev %.1f state %.1f draw %.1f ms\n",
                        (float)prof_[0] * 1000.0f / SYSCLOCK_ARM11 / frames_in_window_, (float)prof_[1] * 1000.0f / SYSCLOCK_ARM11 / frames_in_window_,
                        (float)prof_[2] * 1000.0f / SYSCLOCK_ARM11 / frames_in_window_, (float)prof_[3] * 1000.0f / SYSCLOCK_ARM11 / frames_in_window_);
                fprintf(stderr, "rt64-3ds: frame %u: %u draws %u verts, interp %.1f ms replay %.1f ms (gpu wait %.1f, end %.1f), tex live %u (%u KB) %u uploads/frame, linear free %u KB, unknown ops %u ex %u, tev fallbacks %d, cimg %06x w%u\n",
                        frames_, (unsigned)last_draws_, (unsigned)last_verts_, g_stats.gfx_ms, g_stats.replay_ms, wait_ms_, end_ms_, textures_.live(), textures_.bytes() / 1024, last_uploads_,
                        (unsigned)(linearSpaceFree() / 1024), is.unknown, is.ex_unknown, g_stats.combiner_fallbacks, frame_.color_image, frame_.color_width);
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
                    prof_[0] = prof_[1] = prof_[2] = prof_[3] = 0;
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

        // The N64's 320x240 sits centred on the 400 px wide screen for now.
        // N64 y grows downward; this ordering puts row 0 at the top.
        Mtx_OrthoTilt(&proj_, -40.0f, 360.0f, 0.0f, 240.0f, 0.0f, 1.0f, true);
        C3D_DepthMap(true, -1.0f, 0.0f);
        C3D_CullFace(GPU_CULL_NONE);
        ok_ = true;
        fprintf(stderr, "rt64-3ds: citro3d renderer up (linear free %u KB)\n", (unsigned)(linearSpaceFree() / 1024));
    }

    void replay_and_present() {
        last_draws_ = frame_.draws.size();
        last_verts_ = frame_.verts.size();
        frames_++;
        u64 tb0 = svcGetSystemTick();
        C3D_FrameBegin(0);
        acc_wait_ += svcGetSystemTick() - tb0;
        C3D_RenderTargetClear(top_, C3D_CLEAR_ALL, 0x000000FF, 0);
        C3D_FrameDrawOn(top_);
        C3D_BindProgram(&prog_);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_xform_, &proj_);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo_, 0.0f, 0.0f, 0.0f, 0.0f);
        last_tex_[0] = last_tex_[1] = nullptr;
        last_cc_[0] = last_cc_[1] = 0xFFFFFFFFu;
        last_misc_ = ~0ull;
        last_us_ = last_vs_ = -1.0f;
        last_plan_.stages = -1;

        // Vertices for the whole frame in one buffer.
        vbo_idx_ ^= 1;
        GpuVertex* vb = vbo_[vbo_idx_];
        size_t n = frame_.verts.size();
        if (n > kVboVerts) n = kVboVerts;
        for (size_t i = 0; i < n; i++) {
            const rt64_3ds::Vtx3ds& s = frame_.verts[i];
            vb[i] = { s.x, s.y, s.z, s.w, s.u, s.v, s.r, s.g, s.b, s.a };
        }
        GSPGPU_FlushDataCache(vb, n * sizeof(GpuVertex));
        C3D_BufInfo* bi = C3D_GetBufInfo();
        BufInfo_Init(bi);
        BufInfo_Add(bi, vb, sizeof(GpuVertex), 3, 0x210);

        int fallbacks = 0;
        for (const rt64_3ds::DrawRecord& d : frame_.draws) {
            if (d.first + d.count > n) continue;
            apply_state(d, fallbacks);
            u64 td = svcGetSystemTick();
            C3D_DrawArrays(GPU_TRIANGLES, d.first, d.count);
            prof_[3] += svcGetSystemTick() - td;
        }
        g_stats.combiner_fallbacks = fallbacks;
        g_stats.draws = (int)frame_.draws.size();
        g_stats.tris = (int)(frame_.verts.size() / 3);
        u64 te0 = svcGetSystemTick();
        C3D_FrameEnd(0);
        acc_end_ += svcGetSystemTick() - te0;
        last_uploads_ = textures_.uploads_this_frame();
        textures_.end_frame();
    }

    void apply_state(const rt64_3ds::DrawRecord& d, int& fallbacks) {
        u64 tp0 = svcGetSystemTick();
        // Textures.
        float us = 1.0f, vs = 1.0f;
        for (int t = 0; t < 2; t++) {
            if (d.tex[t].valid) {
                const uint16_t* pal = (d.tex[t].fmt == 2 && d.tex[t].tlut_index + 256 <= frame_.tlut.size()) ? &frame_.tlut[d.tex[t].tlut_index] : nullptr;
                rt64_3ds::BoundTex b = textures_.get(d.tex[t], pal);
                if (b.tex != nullptr) {
                    GPU_TEXTURE_FILTER_PARAM f = d.tex[t].bilerp ? GPU_LINEAR : GPU_NEAREST;
                    GPU_TEXTURE_WRAP_PARAM ws = wrap_mode(d.tex[t].cms, d.tex[t].masks, d.tex[t].width);
                    GPU_TEXTURE_WRAP_PARAM wt = wrap_mode(d.tex[t].cmt, d.tex[t].maskt, d.tex[t].height);
                    uint32_t param = (uint32_t)f | ((uint32_t)ws << 4) | ((uint32_t)wt << 8);
                    if (b.tex != last_tex_[t] || param != last_tex_param_[t]) {
                        C3D_TexSetFilter(b.tex, f, f);
                        C3D_TexSetWrap(b.tex, ws, wt);
                        C3D_TexBind(t, b.tex);
                        last_tex_[t] = b.tex; last_tex_param_[t] = param;
                    }
                    if (t == 0) { us = b.uscale; vs = b.vscale; }
                }
            }
        }
        if (us != last_us_ || vs != last_vs_) {
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_uvscale_, us, vs, 0.0f, 0.0f);
            last_us_ = us; last_vs_ = vs;
        }
        u64 tp1 = svcGetSystemTick();
        prof_[0] += tp1 - tp0;

        // Combiner: only re-sent when the plan changes (every C3D_TexEnv call
        // dirties the whole stage set).
        if (d.cc_w0 != last_cc_[0] || d.cc_w1 != last_cc_[1] || d.othermode_h != last_omh_ || d.othermode_l != last_oml_ ||
            memcmp(d.prim, last_prim_, 4) != 0 || memcmp(d.env, last_env_, 4) != 0 || memcmp(d.fog, last_fog_, 4) != 0 ||
            d.prim_lod_frac != last_plf_ || d.kind != last_kind_ || d.geometry_mode != last_gm_) {
            rt64_3ds::TevPlan plan;
            rt64_3ds::plan_tev(d, plan);
            fallbacks += plan.fallbacks;
            if (plan.stages != last_plan_.stages || memcmp(plan.stage, last_plan_.stage, sizeof(rt64_3ds::TevStage) * plan.stages) != 0) {
                rt64_3ds::apply_tev(plan);
                last_plan_ = plan;
            }
            last_cc_[0] = d.cc_w0; last_cc_[1] = d.cc_w1; last_omh_ = d.othermode_h; last_oml_ = d.othermode_l;
            memcpy(last_prim_, d.prim, 4); memcpy(last_env_, d.env, 4); memcpy(last_fog_, d.fog, 4);
            last_plf_ = d.prim_lod_frac; last_kind_ = d.kind; last_gm_ = d.geometry_mode;
        }
        u64 tp2 = svcGetSystemTick();
        prof_[1] += tp2 - tp1;

        // Depth: reversed map, so "less" becomes GEQUAL. Rects ignore depth
        // unless the RDP compares (they carry z = 0 = near).
        uint32_t l = d.othermode_l;
        uint64_t misc_key = ((uint64_t)l << 32) | ((uint64_t)(d.othermode_h & 0x300000) << 8) | ((uint64_t)d.kind << 28) | (uint64_t)d.blend[3] |
                            ((uint64_t)(uint16_t)d.scissor[0] << 8) ^ ((uint64_t)(uint16_t)d.scissor[1] << 16) ^ ((uint64_t)(uint16_t)d.scissor[2] << 12) ^ ((uint64_t)(uint16_t)d.scissor[3] << 20);
        if (misc_key == last_misc_) { prof_[2] += svcGetSystemTick() - tp2; return; }
        last_misc_ = misc_key;
        bool zcmp = (l & 0x10) != 0 && d.kind == rt64_3ds::DrawRecord::Tris;
        bool zupd = (l & 0x20) != 0 && d.kind == rt64_3ds::DrawRecord::Tris;
        uint32_t zmode = (l >> 10) & 3;
        C3D_DepthTest(zcmp, zcmp ? GPU_GEQUAL : GPU_ALWAYS, zupd ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
        C3D_DepthMap(true, -1.0f, zmode == 3 ? -0.0004f : 0.0f);

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
        int x0 = d.scissor[0] + 40, y0 = d.scissor[1], x1 = d.scissor[2] + 40, y1 = d.scissor[3];
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > 400) x1 = 400; if (y1 > 240) y1 = 240;
        if (x1 <= x0 || y1 <= y0) C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 0, 0);
        else C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - y1), (u32)x0, (u32)(240 - y0), (u32)x1);
        prof_[2] += svcGetSystemTick() - tp2;
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
    DVLB_s* dvlb_ = nullptr;
    shaderProgram_s prog_{};
    int u_xform_ = -1, u_stereo_ = -1, u_uvscale_ = -1;
    C3D_Mtx proj_{};
    GpuVertex* vbo_[2] = { nullptr, nullptr };
    int vbo_idx_ = 0;
    uint32_t frames_ = 0, report_ = 0;
    size_t last_draws_ = 0, last_verts_ = 0;
    uint32_t last_uploads_ = 0;
    uint32_t dl_count_ = 0;
    u64 window_start_ = 0, acc_gfx_ = 0, acc_replay_ = 0, acc_wait_ = 0, acc_end_ = 0;
    float wait_ms_ = 0, end_ms_ = 0;
    u64 prof_[4] = {};
    C3D_Tex* last_tex_[2] = { nullptr, nullptr };
    uint32_t last_tex_param_[2] = { 0, 0 };
    uint32_t last_cc_[2] = { 0, 0 }, last_omh_ = 0, last_oml_ = 0, last_gm_ = 0;
    uint8_t last_prim_[4] = {}, last_env_[4] = {}, last_fog_[4] = {}, last_plf_ = 0, last_kind_ = 0;
    rt64_3ds::TevPlan last_plan_{};
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

