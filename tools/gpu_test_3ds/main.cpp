// The renderer's vertex shader on its own, both of its paths (see CMakeLists.txt).
#include <3ds.h>
#include <citro3d.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>
#include <sys/stat.h>

#include "rt64_3ds_shbin.h"
#include "test_min_shbin.h"

// The shader's one layout (Vtx3ds): a CPU-path vertex has w 1 and pal 0.
struct CpuV { float x, y, z, w; float u, v; uint8_t r, g, b, a; uint8_t pal, pad[3]; };
struct GpuV { float x, y, z, w; float s, t; uint8_t rgba[4]; uint8_t pal; uint8_t pad[3]; };

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    svcOutputDebugString(buf, strlen(buf));
    mkdir("sdmc:/3ds/gpu_test", 0777);
    if (FILE* f = fopen("sdmc:/3ds/gpu_test/result.txt", "a")) { fputs(buf, f); fclose(f); }
}

int main() {
    gfxInitDefault();
    gfxSet3D(false);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    const u32 tf = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                   GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, tf);
    say("gpu_test: start\n");

    DVLB_s* d0 = DVLB_ParseFile((u32*)rt64_3ds_shbin, rt64_3ds_shbin_size);
    shaderProgram_s p0;
    shaderProgramInit(&p0); shaderProgramSetVsh(&p0, &d0->DVLE[0]);
    const int u_xform = shaderInstanceGetUniformLocation(p0.vertexShader, "xform");
    const int u_stereo = shaderInstanceGetUniformLocation(p0.vertexShader, "stereo");
    const int u_eye = shaderInstanceGetUniformLocation(p0.vertexShader, "eye");
    const int u_uvscale = shaderInstanceGetUniformLocation(p0.vertexShader, "uvscale");
    const int u_uv1 = shaderInstanceGetUniformLocation(p0.vertexShader, "uv1");
    // The GPU path's program, once with an indexed draw and once with a
    // plain one, and the smallest shader of its layout (test_min.v.pica: the
    // position straight from the attribute): where to start bisecting when
    // the GPU path draws nothing.
    struct Prog { const char* name; shaderProgram_s p; int xform, uvraw, lcol, amb, fogp, pal, pre, lit, fogged, texgen; uint8_t rgb[3]; float ox, oy; bool arrays, pixels; };
    static constexpr int kProgs = 3;
    Prog progs[kProgs] = { { "gpu", {}, 0,0,0,0,0,0,0,0,0,0, { 0, 255, 0 }, 300.0f, 70.0f, false, false },
                           { "gpu-arrays", {}, 0,0,0,0,0,0,0,0,0,0, { 255, 255, 255 }, 200.0f, 70.0f, true, false },
                           { "min", {}, 0,0,0,0,0,0,0,0,0,0, { 0, 255, 255 }, 70.0f, 190.0f, false, true } };
    const u32* bins[kProgs] = { (const u32*)rt64_3ds_shbin, (const u32*)rt64_3ds_shbin, (const u32*)test_min_shbin };
    const u32 sizes[kProgs] = { rt64_3ds_shbin_size, rt64_3ds_shbin_size, test_min_shbin_size };
    // sdmc:/3ds/gpu_test/mode.txt: a bitmask of the programs to draw (default all), bit 3 the CPU triangle off, bit 4 indexed and arrays draws swapped.
    unsigned mode = 0x7;
    if (FILE* mf = fopen("sdmc:/3ds/gpu_test/mode.txt", "r")) { if (fscanf(mf, "%x", &mode) != 1) mode = 0x7; fclose(mf); }
    say("gpu_test: mode %02x\n", mode);
    // Bit 4: every program drawn the other way (indexed <-> arrays).
    if (mode & 0x10) for (int i = 0; i < kProgs; i++) progs[i].arrays = !progs[i].arrays;
    // Bit 5: the first two programs' colours and places swapped; bit 6: their
    // colours only.
    if (mode & 0x20) { std::swap(progs[0].ox, progs[1].ox); std::swap(progs[0].oy, progs[1].oy); }
    if (mode & 0x60) for (int c = 0; c < 3; c++) std::swap(progs[0].rgb[c], progs[1].rgb[c]);
    for (int i = 0; i < kProgs; i++) {
        Prog& g = progs[i];
        DVLB_s* d = DVLB_ParseFile((u32*)bins[i], sizes[i]);
        shaderProgramInit(&g.p); shaderProgramSetVsh(&g.p, &d->DVLE[0]);
        shaderInstance_s* vs = g.p.vertexShader;
        g.xform = shaderInstanceGetUniformLocation(vs, "xform"); g.uvraw = shaderInstanceGetUniformLocation(vs, "uvraw");
        g.lcol = shaderInstanceGetUniformLocation(vs, "lcol"); g.amb = shaderInstanceGetUniformLocation(vs, "amb");
        g.fogp = shaderInstanceGetUniformLocation(vs, "fogp"); g.pal = shaderInstanceGetUniformLocation(vs, "pal");
        g.pre = shaderInstanceGetUniformLocation(vs, "pre"); g.lit = shaderInstanceGetUniformLocation(vs, "lit"); g.fogged = shaderInstanceGetUniformLocation(vs, "fogged"); g.texgen = shaderInstanceGetUniformLocation(vs, "texgen");
        say("gpu_test: %s: xform %d uvraw %d lcol %d amb %d fogp %d pal %d bools %d %d %d %d\n", g.name, g.xform, g.uvraw, g.lcol, g.amb, g.fogp, g.pal, g.pre, g.lit, g.fogged, g.texgen);
    }
    const int u_pre = shaderInstanceGetUniformLocation(p0.vertexShader, "pre");
    say("gpu_test: cpu xform %d stereo %d eye %d uvscale %d uv1 %d pre %d\n", u_xform, u_stereo, u_eye, u_uvscale, u_uv1, u_pre);

    C3D_AttrInfo ai1;
    AttrInfo_Init(&ai1);
    AttrInfo_AddLoader(&ai1, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(&ai1, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&ai1, 2, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&ai1, 3, GPU_UNSIGNED_BYTE, 4);

    // The CPU path's triangle: red, left half of the screen, N64 screen
    // homogeneous coordinates (w = 1).
    CpuV* vb0 = (CpuV*)linearAlloc(3 * sizeof(CpuV));
    const float px0[3][2] = { { 20, 40 }, { 140, 40 }, { 80, 140 } };
    for (int i = 0; i < 3; i++) { vb0[i] = { px0[i][0], px0[i][1], 0.5f, 1.0f, 0.0f, 0.0f, 255, 0, 0, 255, 0, { 0, 0, 0 } }; }
    // The GPU path's: green, right half, model coordinates the palette's
    // matrix scales by 0.1 and offsets (x + 300, y + 120).
    GpuV* vb1 = (GpuV*)linearAlloc(kProgs * 3 * sizeof(GpuV) + kProgs * 8);
    uint16_t* idx = (uint16_t*)(vb1 + kProgs * 3);
    for (int k = 0; k < kProgs; k++) { idx[k * 4] = (uint16_t)(k * 3); idx[k * 4 + 1] = (uint16_t)(k * 3 + 1); idx[k * 4 + 2] = (uint16_t)(k * 3 + 2); idx[k * 4 + 3] = 0; }
    const float ob[3][3] = { { -600, -600, 0 }, { 600, -600, 0 }, { 0, 600, 0 } };
    for (int k = 0; k < kProgs; k++)
        for (int i = 0; i < 3; i++) {
            // "min" draws screen coordinates directly (z' 0.5, w 1 in the shader).
            const float sx = progs[k].pixels ? progs[k].ox + 0.1f * ob[i][0] : ob[i][0];
            const float sy = progs[k].pixels ? progs[k].oy + 0.1f * ob[i][1] : ob[i][1];
            vb1[k * 3 + i] = { sx, sy, progs[k].pixels ? 0.5f : ob[i][2], 1.0f, 0.0f, 0.0f, { progs[k].rgb[0], progs[k].rgb[1], progs[k].rgb[2], 255 }, 0, { 0, 0, 0 } };
        }
    C3D_BufInfo bi0, bi1;
    BufInfo_Init(&bi0); BufInfo_Add(&bi0, vb0, sizeof(CpuV), 4, 0x3210);
    BufInfo_Init(&bi1); BufInfo_Add(&bi1, vb1, sizeof(GpuV), 4, 0x3210);
    GSPGPU_FlushDataCache(vb0, 3 * sizeof(CpuV));
    GSPGPU_FlushDataCache(vb1, kProgs * 3 * sizeof(GpuV) + kProgs * 8);

    C3D_Mtx proj;
    Mtx_OrthoTilt(&proj, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);

    for (int frame = 0; frame < 900 && aptMainLoop(); frame++) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(top, C3D_CLEAR_ALL, 0x202040FF, 0);
        C3D_FrameDrawOn(top);
        // The CPU path's draw.
        if (!(mode & 0x8)) {
        C3D_BindProgram(&p0);
        C3D_SetAttrInfo(&ai1);
        C3D_SetBufInfo(&bi0);
        if (u_pre >= 0) C3D_BoolUnifSet(GPU_VERTEX_SHADER, u_pre, true);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_xform, &proj);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0.0f, 0.0f, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_eye, 1.0f, 0.0f, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_uvscale, 1.0f, 1.0f, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_uv1, 1.0f, 1.0f, 0.0f, 0.0f);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        }
        // The GPU path's draws: each program its own triangle (an indexed
        // draw of three of the buffer's nine vertices).
        for (int k = 0; k < kProgs; k++) {
            if (!(mode & (1u << k))) continue;
            Prog& g = progs[k];
            C3D_BindProgram(&g.p);
            C3D_SetAttrInfo(&ai1);
            C3D_SetBufInfo(&bi1);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.xform, &proj);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uvraw, 1.0f, 1.0f, 0.0f, 0.0f);
            C3D_FVec* lc = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.lcol, 4);
            for (int l = 0; l < 4; l++) lc[l].x = lc[l].y = lc[l].z = lc[l].w = 0.0f;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g.amb, 0.0f, 0.0f, 0.0f, 0.0f);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g.fogp, 0.0f, 0.0f, 0.0f, 0.0f);
            if (g.pre >= 0) C3D_BoolUnifSet(GPU_VERTEX_SHADER, g.pre, false);
            if (g.lit >= 0) { C3D_BoolUnifSet(GPU_VERTEX_SHADER, g.lit, false); C3D_BoolUnifSet(GPU_VERTEX_SHADER, g.fogged, false); C3D_BoolUnifSet(GPU_VERTEX_SHADER, g.texgen, false); }
            const int npal = g.pixels ? 4 : 10;
            C3D_FVec* pal = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.pal, npal);
            for (int v = 0; v < npal; v++) pal[v].x = pal[v].y = pal[v].z = pal[v].w = 0.0f;
            pal[0].x = 0.1f; pal[0].w = g.ox;
            pal[1].y = 0.1f; pal[1].w = g.oy;
            pal[2].w = 0.5f;
            pal[3].w = 1.0f;
            if (g.arrays) C3D_DrawArrays(GPU_TRIANGLES, k * 3, 3);
            else C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, idx + k * 4);
        }
        C3D_FrameEnd(0);
        if (frame == 30) say("gpu_test: 30 frames drawn\n");
    }
    say("gpu_test: done\n");
    C3D_Fini();
    gfxExit();
    return 0;
}
