// Host-side harness for the display-list interpreter: replays a captured
// gfx task (GFX_CAPTURE.TXT on the console -> gfx_task.bin) and reports what
// it produced and how long it takes, for profiling with perf.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rt64_3ds_dl.h"
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
    printf("%zu textured draws, %u merged\n", textured, st.draws_merged);
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
            fprintf(o, "draw %zu kind %d first %u count %u cc %08x %08x om %08x %08x gm %08x prim %02x%02x%02x%02x env %02x%02x%02x%02x tex0 %d %x %u %ux%u tex1 %d\n",
                    i, (int)d.kind, d.first, d.count, d.cc_w0, d.cc_w1, d.othermode_h, d.othermode_l, d.geometry_mode,
                    d.prim[0], d.prim[1], d.prim[2], d.prim[3], d.env[0], d.env[1], d.env[2], d.env[3],
                    (int)d.tex[0].valid, d.tex[0].addr, d.tex[0].pitch, d.tex[0].width, d.tex[0].height, (int)d.tex[1].valid);
        }
        for (size_t i = 0; i < frame.verts.size(); i++) {
            const auto& v = frame.verts[i];
            fprintf(o, "v %zu %.3f %.3f %.4f %.4f %.3f %.3f %u %u %u %u\n", i, v.x, v.y, v.z, v.w, v.u, v.v, v.r, v.g, v.b, v.a);
        }
        fclose(o);
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
