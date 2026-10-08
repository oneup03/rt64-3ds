// Host-side harness for the display-list interpreter: replays a captured
// gfx task (GFX_CAPTURE.TXT on the console -> gfx_task.bin) and reports what
// it produced and how long it takes, for profiling with perf.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

#include "rt64_3ds_dl.h"
#include "refrender.h"
#include "ultramodern/ultra64.h"

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: dl_test gfx_task.bin [iterations]\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    OSTask task;
    if (fread(&task, 1, sizeof(OSTask), f) != sizeof(OSTask)) { fprintf(stderr, "short file\n"); return 2; }
    const size_t mem = 16u * 1024 * 1024;
    std::vector<uint8_t> rdram(mem);
    if (fread(rdram.data(), 1, mem, f) != mem) { fprintf(stderr, "short rdram\n"); return 2; }
    fclose(f);
    int iterations = argc > 2 ? atoi(argv[2]) : 20;

    rt64_3ds::Interpreter interp(rdram.data());
    // The game's display-list encoding and frame size, as its RenderDesc has
    // them: DL_GBI=f3dex (default F3DEX2), DL_SCREEN=292x216 (default 320x240).
    if (const char* e = getenv("DL_GBI")) if (strcmp(e, "f3dex") == 0) interp.set_gbi(rt64_3ds::Gbi::F3DEX);
    rt64_3ds::ScreenGeometry geo = rt64_3ds::ScreenGeometry::make(320, 240);
    if (const char* e = getenv("DL_SCREEN")) { int w = 0, h = 0; if (sscanf(e, "%dx%d", &w, &h) == 2) geo = rt64_3ds::ScreenGeometry::make(w, h); }
    interp.set_screen(geo);
    if (getenv("DL_RTT")) interp.set_render_to_texture(true);
    // RenderDesc::force_lod_branch = false (Banjo): G_BRANCH_Z tests the depth.
    if (getenv("DL_LOD_BRANCH")) interp.set_force_branch_z(false);
    // RenderDesc::sample_drawn_frame (Goemon's menu box rims).
    if (getenv("DL_FRAME_COPY")) interp.set_sample_drawn_frame(true);
    // RenderDesc::primary_world_instance (the dump then lists each record's projection instance).
    if (getenv("DL_PROJ_INST")) interp.set_primary_world_instance(true);
    // DL_UNIFY_DEPTH: RenderDesc::unify_world_depth, with Goemon's World ids
    // (0x1000 and untagged), which the game's rules give on the console.
    if (getenv("DL_UNIFY_DEPTH")) {
        static const uint32_t ids[] = { 0x1000, 0 };
        interp.set_world_proj_ids(ids, 2);
        interp.set_unify_world_depth(true);
    }
    // RenderDesc::fix_rect_lr and merge_rect_slices (Goemon's settings).
    if (getenv("DL_RECT_PARITY")) { interp.set_fix_rect_lr(true); interp.set_merge_rect_slices(true); }
    // RenderDesc::merge_image_tiles (Goemon's sky). The rows of an image
    // texture come from the list before, so the task is run once first, as
    // the console runs the frame before it, and its frame presented
    // (DL_PREROLL=0: not; DL_PREROLL=1 without image tiles too).
    const bool image_tiles = getenv("DL_IMAGE_TILES") != nullptr;
    if (image_tiles) interp.set_merge_image_tiles(true);
    // DL_DEFER: deferred triangles (Interpreter::set_defer_triangles, the
    // console's reader on another core): every run is followed by
    // finish_triangles, as the renderer does.
    // DL_DEFER=tris: the triangles alone, the vertices transformed in the read.
    const bool defer = getenv("DL_DEFER") != nullptr;
    if (defer) interp.set_defer_triangles(true, strcmp(getenv("DL_DEFER"), "tris") != 0);
    // DL_DEFER_THREAD (with DL_DEFER): run on a second thread while this one
    // drains the triangles it publishes, as the console's reader and gfx
    // thread do.
    const bool defer_thread = defer && getenv("DL_DEFER_THREAD") != nullptr;
    auto run_list = [&](uint32_t data_ptr, rt64_3ds::FrameRecord& f) {
        bool r = false;
        if (defer_thread) {
            std::atomic<bool> ran{ false };
            std::thread reader([&] { r = interp.run(data_ptr, f); ran.store(true, std::memory_order_release); });
            while (!ran.load(std::memory_order_acquire)) interp.drain_triangles();
            reader.join();
        }
        else r = interp.run(data_ptr, f);
        interp.finish_triangles(f);
        return r;
    };
    rt64_3ds::FrameRecord frame;
    const char* preroll_env = getenv("DL_PREROLL");
    const bool preroll = preroll_env != nullptr ? atoi(preroll_env) != 0 : image_tiles;
    if (preroll) {
        run_list(task.t.data_ptr, frame);
        interp.note_presented_framebuffer(frame.color_image);
        frame.clear();
    }
    // DL_PASS_OVER: the frame cap's state-only run of the list first, timed.
    if (getenv("DL_PASS_OVER")) {
        rt64_3ds::FrameRecord scratch;
        const int n = 200;
        bool plain = false;
        auto p0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++) { scratch.clear(); plain = interp.pass_over(task.t.data_ptr, scratch); }
        auto p1 = std::chrono::steady_clock::now();
        printf("pass_over: %s, %.3f ms per pass on this host\n", plain ? "a whole frame on the screen, can go undrawn" : "must be drawn (state put back)",
               std::chrono::duration<double, std::milli>(p1 - p0).count() / n);
    }
    // DL_EXTRA_RUNS=n: run the list n more times first (to compare the state
    // a pass_over leaves with what full runs leave).
    if (const char* e = getenv("DL_EXTRA_RUNS")) for (int i = 0; i < atoi(e); i++) { frame.clear(); run_list(task.t.data_ptr, frame); }
    frame.clear();
    // DL_INTERP: frame interpolation (RenderDesc::frame_interpolation). The
    // list is run once as the frame before, then as this one, and then as
    // the in-between frame, which against an identical frame before must
    // draw what the frame draws (DL_INTERP_DUMP=file writes its draws).
    const bool interp_test = getenv("DL_INTERP") != nullptr;
    // DL_PREV=<capture of the list before> (GFX_CAPTURE.TXT "pair"): the
    // frame before is that list, run on its own RDRAM; DL_PREV_RENDER=file
    // renders it with the reference renderer.
    std::vector<uint8_t> cur_rdram;
    rt64_3ds::FrameRecord before;
    if (interp_test) {
        interp.set_interpolation(true);
        if (const char* prev = getenv("DL_PREV")) {
            FILE* pf = fopen(prev, "rb");
            OSTask ptask;
            if (pf == nullptr || fread(&ptask, 1, sizeof(OSTask), pf) != sizeof(OSTask)) { fprintf(stderr, "can't read %s\n", prev); return 2; }
            cur_rdram = rdram;
            if (fread(rdram.data(), 1, mem, pf) != mem) { fprintf(stderr, "short rdram in %s\n", prev); return 2; }
            fclose(pf);
            run_list(ptask.t.data_ptr, before);
            if (const char* out = getenv("DL_PREV_RENDER")) {
                RefOptions o;
                o.geo = geo;
                o.model_images = getenv("DL_FRAME_COPY") != nullptr;
                printf("reference render of the list before -> %s: %s\n", out, ref_render(rdram.data(), before, o, out) ? "ok" : "failed");
            }
            memcpy(rdram.data(), cur_rdram.data(), mem);
        }
        else run_list(task.t.data_ptr, before);
        interp.note_presented_framebuffer(before.color_image);
    }
    bool present = run_list(task.t.data_ptr, frame);
    // DL_INTERP_WEIGHT: the in-between's weight (default 0.5, a 30 fps
    // game frame's one in-between; 1/3 and 2/3 at 20 fps).
    const float interp_weight = getenv("DL_INTERP_WEIGHT") ? (float)atof(getenv("DL_INTERP_WEIGHT")) : 0.5f;
    if (interp_test) {
        auto t0 = std::chrono::steady_clock::now();
        const rt64_3ds::interp::MatchResult mr = interp.match_previous(interp_weight);
        auto t1 = std::chrono::steady_clock::now();
        rt64_3ds::FrameRecord between;
        uint32_t tris = 0;
        const bool ok = interp.run_inbetween(task.t.data_ptr, between, &tris);
        auto t2 = std::chrono::steady_clock::now();
        printf("interpolation: %u transforms (%u matched, %u interpolated, %u with vertices), %u projections (%u matched, %u interpolated), %u tile entries (%u interpolated), camera cut %d; "
               "match %.3f ms, in-between run %.3f ms; followed %d, %zu draws %zu verts (frame: %zu draws %zu verts)\n",
               mr.xf, mr.xf_matched, mr.xf_interp, mr.vert_entries, mr.pj, mr.pj_matched, mr.pj_interp, mr.tiles, mr.tiles_interp, (int)mr.camera_cut,
               std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count(),
               (int)ok, between.draws.size(), between.verts.size(), frame.draws.size(), frame.verts.size());
        float worst = 0.0f;
        size_t differ = 0;
        if (between.verts.size() == frame.verts.size()) {
            for (size_t i = 0; i < frame.verts.size(); i++) {
                const auto& a = frame.verts[i];
                const auto& b = between.verts[i];
                const float d = std::max({ std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z), std::fabs(a.w - b.w), std::fabs(a.u - b.u), std::fabs(a.v - b.v) });
                if (d > worst) worst = d;
                if (d > 1e-3f || a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) differ++;
            }
        }
        printf("in-between against the frame: worst vertex difference %.6f, %zu vertices differ by more than 0.001\n", worst, differ);
        // The renderer's way: the in-between rebuilt from the frame's
        // records, against the second run's.
        rt64_3ds::FrameRecord rebuilt;
        uint32_t rtris = 0;
        auto t3 = std::chrono::steady_clock::now();
        const bool rok = interp.build_inbetween(frame, rebuilt, &rtris);
        auto t4 = std::chrono::steady_clock::now();
        float rworst = 0.0f;
        size_t rdiffer = 0;
        bool same_layout = rebuilt.draws.size() == between.draws.size() && rebuilt.verts.size() == between.verts.size();
        for (size_t i = 0; same_layout && i < rebuilt.draws.size(); i++) same_layout = rebuilt.draws[i].first == between.draws[i].first && rebuilt.draws[i].count == between.draws[i].count;
        if (same_layout) {
            for (size_t i = 0; i < rebuilt.verts.size(); i++) {
                const auto& a = rebuilt.verts[i];
                const auto& b = between.verts[i];
                const float d = std::max({ std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z), std::fabs(a.w - b.w), std::fabs(a.u - b.u), std::fabs(a.v - b.v) });
                if (d > rworst) rworst = d;
                if (d > 1e-3f || a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) rdiffer++;
            }
        }
        printf("rebuilt in-between: ok %d, %.3f ms, %u triangles, %zu draws %zu verts; same layout as the second run %d, worst difference %.6f, %zu vertices differ\n",
               (int)rok, std::chrono::duration<double, std::milli>(t4 - t3).count(), rtris, rebuilt.draws.size(), rebuilt.verts.size(), (int)same_layout, rworst, rdiffer);
        if (!same_layout) {
            // Per draw: where the two in-betweens part.
            int shown = 0;
            for (size_t i = 0; i < std::min(rebuilt.draws.size(), between.draws.size()) && shown < 8; i++) {
                if (rebuilt.draws[i].count != between.draws[i].count) {
                    printf("  draw %zu: rebuilt %u verts, second run %u (kind %d)\n", i, rebuilt.draws[i].count, between.draws[i].count, (int)rebuilt.draws[i].kind);
                    shown++;
                }
            }
        }
        {
            // How far the in-between moved the frame's vertices (same layout only).
            if (rebuilt.verts.size() == frame.verts.size()) {
                double sum = 0.0; float mx = 0.0f; size_t moved = 0;
                for (size_t i = 0; i < frame.verts.size(); i++) {
                    const auto& a = frame.verts[i]; const auto& b = rebuilt.verts[i];
                    if (a.w <= 0.0f || b.w <= 0.0f) continue;
                    const float dx = a.x / a.w - b.x / b.w, dy = a.y / a.w - b.y / b.w;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (d > 0.01f) moved++;
                    sum += d; if (d > mx) mx = d;
                }
                printf("in-between against the frame on screen: %zu of %zu vertices moved, mean %.2f px, max %.2f px\n", moved, frame.verts.size(), sum / std::max<size_t>(1, frame.verts.size()), mx);
            }
        }
        if (getenv("DL_PREV") != nullptr) {
            // Where the in-between puts things between the two frames: per
            // draw drawn alike in all three (same vertex count), its
            // vertices' mean screen position, and the fraction of the way
            // from the frame before to this one (0.5 = halfway).
            std::vector<float> fr;
            double moved_px = 0.0;
            int alike = 0;
            const size_t nd = std::min({ before.draws.size(), frame.draws.size(), rebuilt.draws.size() });
            for (size_t i = 0; i < nd; i++) {
                const auto& a = before.draws[i]; const auto& b = rebuilt.draws[i]; const auto& d = frame.draws[i];
                if (a.count == 0 || a.count != b.count || a.count != d.count || a.kind != rt64_3ds::DrawRecord::Tris) continue;
                auto mean = [](const rt64_3ds::FrameRecord& f, const rt64_3ds::DrawRecord& r, float& x, float& y) {
                    double sx = 0, sy = 0; int n = 0;
                    for (uint32_t k = r.first; k < r.first + r.count; k++) { const auto& v = f.verts[k]; if (v.w <= 0) continue; sx += v.x / v.w; sy += v.y / v.w; n++; }
                    x = n ? (float)(sx / n) : 0; y = n ? (float)(sy / n) : 0; return n > 0;
                };
                float ax, ay, bx, by, dx, dy;
                if (!mean(before, a, ax, ay) || !mean(rebuilt, b, bx, by) || !mean(frame, d, dx, dy)) continue;
                const float mx = dx - ax, my = dy - ay, len2 = mx * mx + my * my;
                if (len2 < 0.25f) continue;           // moved less than half a pixel
                fr.push_back(((bx - ax) * mx + (by - ay) * my) / len2);
                moved_px += std::sqrt(len2);
                alike++;
            }
            std::sort(fr.begin(), fr.end());
            if (!fr.empty()) printf("in-between placement: %d draws moved between the frames (mean %.1f px); fraction of the way: median %.3f, 10%% %.3f, 90%% %.3f\n",
                                    alike, moved_px / alike, fr[fr.size() / 2], fr[fr.size() / 10], fr[fr.size() * 9 / 10]);
            else printf("in-between placement: no draw moved by half a pixel or more\n");
        }
        if (const char* out = getenv("DL_INTERP_RENDER")) {
            RefOptions o;
            o.geo = geo;
            o.model_images = getenv("DL_FRAME_COPY") != nullptr;
            printf("reference render of the in-between -> %s: %s\n", out, ref_render(rdram.data(), rebuilt, o, out) ? "ok" : "failed");
        }
        if (const char* e = getenv("DL_REWEIGHT")) {
            // The renderer's next in-between of the same pair: the matches
            // re-weighted, against a fresh match at that weight (must agree).
            const float w2 = (float)atof(e);
            interp.reweight_previous(w2);
            rt64_3ds::FrameRecord a, b;
            interp.build_inbetween(frame, a);
            interp.match_previous(w2);
            interp.build_inbetween(frame, b);
            bool same = a.draws.size() == b.draws.size() && a.verts.size() == b.verts.size();
            float worst = 0.0f;
            for (size_t i = 0; same && i < a.verts.size(); i++) {
                const auto& x = a.verts[i]; const auto& y = b.verts[i];
                worst = std::max({ worst, std::fabs(x.x - y.x), std::fabs(x.y - y.y), std::fabs(x.z - y.z), std::fabs(x.w - y.w), std::fabs(x.u - y.u), std::fabs(x.v - y.v) });
            }
            // Where it puts each vertex: built at weight 0 (the frame
            // before, as the in-between sees it) and 1 (this frame), the
            // fraction of the way from one to the other, over the vertices
            // that move half a pixel or more (draws laid out alike).
            rt64_3ds::FrameRecord a0, a1;
            interp.reweight_previous(0.0f);
            interp.build_inbetween(frame, a0);
            interp.reweight_previous(1.0f);
            interp.build_inbetween(frame, a1);
            std::vector<float> fr;
            const size_t nd = std::min({ a0.draws.size(), a1.draws.size(), a.draws.size() });
            for (size_t i = 0; i < nd; i++) {
                const auto& p = a0.draws[i]; const auto& q = a.draws[i]; const auto& d = a1.draws[i];
                if (p.count == 0 || p.count != q.count || p.count != d.count) continue;
                for (uint32_t k = 0; k < p.count; k++) {
                    const auto& vp = a0.verts[p.first + k]; const auto& vq = a.verts[q.first + k]; const auto& vd = a1.verts[d.first + k];
                    if (vp.w <= 0 || vq.w <= 0 || vd.w <= 0) continue;
                    const double mx = vd.x / vd.w - vp.x / vp.w, my = vd.y / vd.w - vp.y / vp.w, len2 = mx * mx + my * my;
                    if (len2 < 0.25) continue;
                    fr.push_back((float)(((vq.x / vq.w - vp.x / vp.w) * mx + (vq.y / vq.w - vp.y / vp.w) * my) / len2));
                }
            }
            std::sort(fr.begin(), fr.end());
            printf("reweighted to %.3f: same layout as a fresh match %d, worst difference %.6f; fraction of the way from weight 0 to 1: median %.3f, 10%% %.3f, 90%% %.3f (%zu vertices)\n",
                   w2, (int)same, worst, fr.empty() ? -1.0f : fr[fr.size() / 2], fr.empty() ? -1.0f : fr[fr.size() / 10], fr.empty() ? -1.0f : fr[fr.size() * 9 / 10], fr.size());
            if (const char* out = getenv("DL_REWEIGHT_RENDER")) {
                RefOptions o;
                o.geo = geo;
                printf("reference render of the reweighted in-between -> %s: %s\n", out, ref_render(rdram.data(), a, o, out) ? "ok" : "failed");
            }
        }
        if (getenv("DL_INTERP_BENCH")) {
            const int n = 50;
            auto b0 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; i++) { rt64_3ds::FrameRecord r2; interp.build_inbetween(frame, r2); }
            auto b1 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; i++) interp.match_previous(0.5f);
            auto b2 = std::chrono::steady_clock::now();
            printf("bench: build_inbetween %.3f ms, match %.3f ms (host)\n", std::chrono::duration<double, std::milli>(b1 - b0).count() / n,
                   std::chrono::duration<double, std::milli>(b2 - b1).count() / n);
        }
    }
    // DL_RUN_BENCH: the list's run on its own (what the game waits for, to
    // "parsed"), with the interpolation records (DL_INTERP) or without.
    if (getenv("DL_RUN_BENCH")) {
        const int n = 50;
        rt64_3ds::FrameRecord r;
        auto b0 = std::chrono::steady_clock::now();
        double fin = 0.0;
        for (int i = 0; i < n; i++) {
            r.clear();
            interp.run(task.t.data_ptr, r);
            auto f0 = std::chrono::steady_clock::now();
            interp.finish_triangles(r);
            fin += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
        }
        auto b1 = std::chrono::steady_clock::now();
        printf("bench: run %.3f ms, of it finish_triangles %.3f ms (host, interpolation %s, deferred %s)\n",
               std::chrono::duration<double, std::milli>(b1 - b0).count() / n, fin / n, interp_test ? "on" : "off", defer ? "on" : "off");
        // DL_PROFILE: the runs' time by kind of command (Interpreter::set_profile).
        if (getenv("DL_PROFILE")) {
            interp.clear_profile();
            interp.set_profile(true);
            for (int i = 0; i < n; i++) { r.clear(); interp.run(task.t.data_ptr, r); interp.finish_triangles(r); }
            interp.set_profile(false);
            static const char* const kinds[] = { "vertices", "triangles", "matrices", "texture loads", "rects", "list flow", "extended", "other" };
            const uint64_t* pr = interp.stats().prof;
            printf("profile per run (host):");
            for (int k = 0; k < rt64_3ds::InterpreterStats::kProfKinds; k++) printf(" %s %.3f", kinds[k], (double)pr[k] / 1e6 / n);
            printf(" ms\n");
        }
    }
    const rt64_3ds::InterpreterStats& st = interp.stats();
    printf("task data_ptr %08x: fullsync %d, %zu draws, %zu verts, %u commands, %u tris, %u rects, unknown %u ex %u, unresolved tex %u\n",
           task.t.data_ptr, (int)present, frame.draws.size(), frame.verts.size(), st.commands, st.tris, st.rects, st.unknown, st.ex_unknown, st.tex_unresolved);
    size_t textured = 0;
    for (const auto& d : frame.draws) if (d.tex[0].valid) textured++;
    printf("%zu textured draws, %u merged, %u probes, %u triangles clipped, %u rectangles stretched as slices of a row\n", textured, st.draws_merged, st.probes, st.clipped, st.rects_merged);
    if (image_tiles) printf("image tiles: %u triangles sampled their whole image, %u kept their tile; %u rectangles, %u merged into the one before; list plain %d\n",
                            st.image_tris, st.image_fallbacks, st.image_rects, st.rects_compacted, (int)interp.last_list_plain());
    {
        size_t wrapped = 0, rtt_draws = 0;
        for (const auto& d : frame.draws) {
            if ((d.tex[0].valid && d.tex[0].wrap_rows) || (d.tex[1].valid && d.tex[1].wrap_rows)) wrapped++;
            if (d.rtt >= 0) rtt_draws++;
        }
        printf("%zu draws read a tile taller than TMEM holds (rows wrap), %zu draws into off-screen images, %u sample the frame drawn so far (last draw %d)\n",
               wrapped, rtt_draws, st.frame_samples, frame.frame_sample_last);
    }
    static const char* names[256] = {};
    names[0x00] = "NOOP"; names[0x01] = "VTX"; names[0x02] = "MODIFYVTX"; names[0x03] = "CULLDL"; names[0x04] = "BRANCH_Z"; names[0x05] = "TRI1"; names[0x06] = "TRI2"; names[0x07] = "QUAD";
    names[0xD7] = "TEXTURE"; names[0xD8] = "POPMTX"; names[0xD9] = "GEOMETRYMODE"; names[0xDA] = "MTX"; names[0xDB] = "MOVEWORD"; names[0xDC] = "MOVEMEM"; names[0xDE] = "DL"; names[0xDF] = "ENDDL";
    names[0xE0] = "SPNOOP"; names[0xE1] = "RDPHALF_1"; names[0xE2] = "SETOTHERMODE_L"; names[0xE3] = "SETOTHERMODE_H"; names[0xE4] = "TEXRECT"; names[0xE5] = "TEXRECTFLIP"; names[0xE6] = "RDPLOADSYNC"; names[0xE7] = "RDPPIPESYNC";
    names[0xE8] = "RDPTILESYNC"; names[0xE9] = "RDPFULLSYNC"; names[0xEA] = "SETKEYGB"; names[0xEB] = "SETKEYR"; names[0xEC] = "SETCONVERT"; names[0xED] = "SETSCISSOR"; names[0xEE] = "SETPRIMDEPTH"; names[0xEF] = "RDPSETOTHERMODE";
    names[0xF0] = "LOADTLUT"; names[0xF1] = "RDPHALF_2"; names[0xF2] = "SETTILESIZE"; names[0xF3] = "LOADBLOCK"; names[0xF4] = "LOADTILE"; names[0xF5] = "SETTILE"; names[0xF6] = "FILLRECT"; names[0xF7] = "SETFILLCOLOR";
    names[0xF8] = "SETFOGCOLOR"; names[0xF9] = "SETBLENDCOLOR"; names[0xFA] = "SETPRIMCOLOR"; names[0xFB] = "SETENVCOLOR"; names[0xFC] = "SETCOMBINE"; names[0xFD] = "SETTIMG"; names[0xFE] = "SETZIMG"; names[0xFF] = "SETCIMG"; names[0x64] = "EXTENDED";
    for (int op = 0; op < 256; op++) if (st.op_hist[op]) printf("  %02x %-16s %u\n", op, names[op] ? names[op] : "?", st.op_hist[op]);

    if (const char* dump = getenv("DL_DUMP")) {
        // Every vertex and draw in text form, for diffing two builds.
        FILE* o = fopen(dump, "w");
        for (size_t i = 0; i < frame.draws.size(); i++) {
            const auto& d = frame.draws[i];
            float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
            for (uint32_t k = d.first; k < d.first + d.count; k++) {
                const auto& v = frame.verts[k];
                if (v.w <= 0) continue;
                float x = v.x / v.w, y = v.y / v.w;
                if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
            }
            fprintf(o, "draw %zu kind %d first %u count %u cc %06x %08x om %06x %08x gm %06x prim %02x%02x%02x%02x env %02x%02x%02x%02x blend %02x%02x%02x%02x fog %02x%02x%02x%02x plf %u proj %x persp %d tex0 %d %x fmt %u/%u %ux%u tlut %u tex1 %d %x fmt %u/%u %ux%u rtt %d/%d vtx %06x snap %d pitch %u blk %d cm %u/%u mask %u/%u | x %.0f..%.0f y %.0f..%.0f\n",
                    i, (int)d.kind, d.first, d.count, d.cc_w0, d.cc_w1, d.othermode_h, d.othermode_l, d.geometry_mode,
                    d.prim[0], d.prim[1], d.prim[2], d.prim[3], d.env[0], d.env[1], d.env[2], d.env[3],
                    d.blend[0], d.blend[1], d.blend[2], d.blend[3], d.fog[0], d.fog[1], d.fog[2], d.fog[3], d.prim_lod_frac, d.proj_id, (int)d.perspective,
                    (int)d.tex[0].valid, d.tex[0].addr, d.tex[0].fmt, d.tex[0].siz, d.tex[0].width, d.tex[0].height, d.tex[0].tlut_mode,
                    (int)d.tex[1].valid, d.tex[1].addr, d.tex[1].fmt, d.tex[1].siz, d.tex[1].width, d.tex[1].height, (int)d.rtt, (int)d.tex[0].rtt, d.dbg_vtx,
                    (int)d.tex[0].snapshot, d.tex[0].pitch, (int)d.tex[0].block, d.tex[0].cms, d.tex[0].cmt, d.tex[0].masks, d.tex[0].maskt, minx, maxx, miny, maxy);
            // Only when set, so dumps of games that never use them don't change.
            if (d.tex[0].frame || d.tex[1].frame) fprintf(o, "  frame %d/%d\n", (int)d.tex[0].frame, (int)d.tex[1].frame);
            if (d.tex[0].wrap_rows || d.tex[1].wrap_rows) fprintf(o, "  wrap rows %u/%u\n", d.tex[0].wrap_rows, d.tex[1].wrap_rows);
            if (d.proj_inst) fprintf(o, "  projection instance %u\n", d.proj_inst);
            if (d.tex[0].levels > 1) {
                fprintf(o, "  mip levels %u:", d.tex[0].levels);
                for (uint32_t k = 1; k < d.tex[0].levels; k++) {
                    const auto& m = frame.mips[d.tex[0].mip_index + k - 1];
                    fprintf(o, " %x %ux%u", m.addr, m.width, m.height);
                }
                fprintf(o, "\n");
            }
        }
        for (size_t i = 0; i < frame.verts.size(); i++) {
            const auto& v = frame.verts[i];
            fprintf(o, "v %zu %.3f %.3f %.4f %.4f %.3f %.3f %u %u %u %u\n", i, v.x, v.y, v.z, v.w, v.u, v.v, v.r, v.g, v.b, v.a);
        }
        fclose(o);
    }

    {
        // Block loads whose row step disagrees with the sampler's odd rows.
        size_t block = 0, swapped = 0;
        for (const auto& d : frame.draws) {
            const auto& t = d.tex[0];
            if (!t.valid || !t.block) continue;
            block++;
            uint32_t rw = t.pitch / 8, bad = 0;
            for (uint32_t y = 0; y < t.height; y++) if (((((uint32_t)t.load_word + y * rw + rw / 2) * t.dxt >> 11) & 1) != (y & 1)) bad++;
            if (bad) {
                swapped++;
                if (swapped <= 12) printf("  swap: tex %06x fmt %u siz %u %ux%u pitch %u dxt %u word %u: %u rows\n", t.addr, t.fmt, t.siz, t.width, t.height, t.pitch, t.dxt, t.load_word, bad);
            }
        }
        printf("%zu block-loaded draws, %zu need odd-row swaps\n", block, swapped);
    }

    if (getenv("DL_ZRANGE")) {
        // Draws with vertices outside the depth range (z' = z/w beyond 0..1).
        for (size_t i = 0; i < frame.draws.size(); i++) {
            const auto& d = frame.draws[i];
            float zmin = 1e9f, zmax = -1e9f;
            for (uint32_t k = d.first; k < d.first + d.count; k++) {
                const auto& v = frame.verts[k];
                if (v.w <= 0) continue;
                float z = v.z / v.w;
                if (z < zmin) zmin = z; if (z > zmax) zmax = z;
            }
            if (d.count && (zmin < 0.0f || zmax > 0.999f)) printf("draw %zu: z' %.3f..%.3f (%u verts)\n", i, zmin, zmax, d.count);
        }
    }
    if (const char* out = getenv("DL_RENDER")) {
        // Reference render; DL_MAX / DL_ONLY limit the draws, DL_LOD sets LOD_FRACTION.
        RefOptions o;
        o.geo = geo;
        if (const char* e = getenv("DL_MAX")) o.max_draw = atoi(e);
        if (const char* e = getenv("DL_ONLY")) o.only_draw = atoi(e);
        if (const char* e = getenv("DL_LOD")) o.lod_fraction = atoi(e);
        // Draw off-screen images into images of their own and sample them,
        // and the frame drawn so far where a draw reads it (DL_REF_RTT, or
        // with DL_FRAME_COPY); otherwise everything lands on the screen.
        o.model_images = getenv("DL_REF_RTT") != nullptr || getenv("DL_FRAME_COPY") != nullptr;
        printf("reference render -> %s: %s\n", out, ref_render(rdram.data(), frame, o, out) ? "ok" : "failed");
    }
    if (const char* list = getenv("DL_TEXDUMP")) {
        // "draw[,draw...]": write both textures of those draws as PPM.
        const char* dir = getenv("DL_TEXDIR") ? getenv("DL_TEXDIR") : ".";
        for (const char* p = list; *p;) {
            int i = atoi(p);
            if (i >= 0 && (size_t)i < frame.draws.size()) {
                for (int t = 0; t < 2; t++) {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/draw%03d_tex%d.ppm", dir, i, t);
                    if (dump_texture(rdram.data(), frame, frame.draws[i].tex[t], path)) printf("wrote %s\n", path);
                }
            }
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; i++) {
        frame.clear();
        run_list(task.t.data_ptr, frame);
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iterations;
    printf("%.3f ms per interpretation on this host (%d iterations)\n", ms, iterations);
    return 0;
}
