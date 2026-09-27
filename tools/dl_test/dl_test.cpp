// Host-side harness for the display-list interpreter: replays a captured
// gfx task (GFX_CAPTURE.TXT on the console -> gfx_task.bin) and reports what
// it produced and how long it takes, for profiling with perf.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    rt64_3ds::FrameRecord frame;
    bool present = interp.run(task.t.data_ptr, frame);
    const rt64_3ds::InterpreterStats& st = interp.stats();
    printf("task data_ptr %08x: fullsync %d, %zu draws, %zu verts, %u commands, %u tris, %u rects, unknown %u ex %u, unresolved tex %u\n",
           task.t.data_ptr, (int)present, frame.draws.size(), frame.verts.size(), st.commands, st.tris, st.rects, st.unknown, st.ex_unknown, st.tex_unresolved);
    size_t textured = 0;
    for (const auto& d : frame.draws) if (d.tex[0].valid) textured++;
    printf("%zu textured draws, %u merged, %u probes\n", textured, st.draws_merged, st.probes);
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
            fprintf(o, "draw %zu kind %d first %u count %u cc %06x %08x om %06x %08x gm %06x prim %02x%02x%02x%02x env %02x%02x%02x%02x blend %02x%02x%02x%02x fog %02x%02x%02x%02x plf %u proj %x persp %d tex0 %d %x fmt %u/%u %ux%u tlut %u tex1 %d %x fmt %u/%u %ux%u vtx %06x | x %.0f..%.0f y %.0f..%.0f\n",
                    i, (int)d.kind, d.first, d.count, d.cc_w0, d.cc_w1, d.othermode_h, d.othermode_l, d.geometry_mode,
                    d.prim[0], d.prim[1], d.prim[2], d.prim[3], d.env[0], d.env[1], d.env[2], d.env[3],
                    d.blend[0], d.blend[1], d.blend[2], d.blend[3], d.fog[0], d.fog[1], d.fog[2], d.fog[3], d.prim_lod_frac, d.proj_id, (int)d.perspective,
                    (int)d.tex[0].valid, d.tex[0].addr, d.tex[0].fmt, d.tex[0].siz, d.tex[0].width, d.tex[0].height, d.tex[0].tlut_mode,
                    (int)d.tex[1].valid, d.tex[1].addr, d.tex[1].fmt, d.tex[1].siz, d.tex[1].width, d.tex[1].height, d.dbg_vtx, minx, maxx, miny, maxy);
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

    if (const char* out = getenv("DL_RENDER")) {
        // Reference render; DL_MAX / DL_ONLY limit the draws, DL_LOD sets LOD_FRACTION.
        RefOptions o;
        if (const char* e = getenv("DL_MAX")) o.max_draw = atoi(e);
        if (const char* e = getenv("DL_ONLY")) o.only_draw = atoi(e);
        if (const char* e = getenv("DL_LOD")) o.lod_fraction = atoi(e);
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
        interp.run(task.t.data_ptr, frame);
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iterations;
    printf("%.3f ms per interpretation on this host (%d iterations)\n", ms, iterations);
    return 0;
}
