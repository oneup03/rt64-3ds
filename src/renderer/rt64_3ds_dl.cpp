// F3DEX2 / F3DEX + extended GBI interpreter. See rt64_3ds_dl.h.
//
// RDRAM is the recompiled runtime's byte-swapped image: byte a lives at
// [a ^ 3], halfword a at [a ^ 2], aligned words in host order.
#include "rt64_3ds_dl.h"
#include "rt64_3ds_texdecode.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#ifdef __3DS__
#include <3ds.h>
static inline uint64_t prof_tick() { return svcGetSystemTick(); }
#else
#include <chrono>
static inline uint64_t prof_tick() { return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count(); }

#endif

namespace rt64_3ds {

namespace {

// ---- F3DEX2 opcodes
enum : uint8_t {
    G_NOOP = 0x00, G_VTX = 0x01, G_MODIFYVTX = 0x02, G_CULLDL = 0x03, G_BRANCH_Z = 0x04,
    G_TRI1 = 0x05, G_TRI2 = 0x06, G_QUAD = 0x07,
    G_EXTENDED = 0x64,
    G_DMA_IO = 0xD6, G_TEXTURE = 0xD7, G_POPMTX = 0xD8, G_GEOMETRYMODE = 0xD9, G_MTX = 0xDA,
    G_MOVEWORD = 0xDB, G_MOVEMEM = 0xDC, G_LOAD_UCODE = 0xDD, G_DL = 0xDE, G_ENDDL = 0xDF,
    G_SPNOOP = 0xE0, G_RDPHALF_1 = 0xE1, G_SETOTHERMODE_L = 0xE2, G_SETOTHERMODE_H = 0xE3,
    G_TEXRECT = 0xE4, G_TEXRECTFLIP = 0xE5, G_RDPLOADSYNC = 0xE6, G_RDPPIPESYNC = 0xE7,
    G_RDPTILESYNC = 0xE8, G_RDPFULLSYNC = 0xE9, G_SETKEYGB = 0xEA, G_SETKEYR = 0xEB,
    G_SETCONVERT = 0xEC, G_SETSCISSOR = 0xED, G_SETPRIMDEPTH = 0xEE, G_RDPSETOTHERMODE = 0xEF,
    G_LOADTLUT = 0xF0, G_RDPHALF_2 = 0xF1, G_SETTILESIZE = 0xF2, G_LOADBLOCK = 0xF3,
    G_LOADTILE = 0xF4, G_SETTILE = 0xF5, G_FILLRECT = 0xF6, G_SETFILLCOLOR = 0xF7,
    G_SETFOGCOLOR = 0xF8, G_SETBLENDCOLOR = 0xF9, G_SETPRIMCOLOR = 0xFA, G_SETENVCOLOR = 0xFB,
    G_SETCOMBINE = 0xFC, G_SETTIMG = 0xFD, G_SETZIMG = 0xFE, G_SETCIMG = 0xFF,
};

// ---- F3DEX 1.x (GBI1) RSP opcodes, the ones whose number or encoding
// differs from F3DEX2's. The main loop runs them in its one dispatch, in
// kF3dexOp's numbering: each is put into its F3DEX2 form there (or run) and
// goes on as that; the RDP opcodes are shared.
enum : uint8_t {
    F3DEX_G_SPNOOP = 0x00, F3DEX_G_MTX = 0x01, F3DEX_G_MOVEMEM = 0x03, F3DEX_G_VTX = 0x04, F3DEX_G_DL = 0x06,
    F3DEX_G_SPRITE2D_BASE = 0x09,
    F3DEX_G_LOAD_UCODE = 0xAF, F3DEX_G_BRANCH_Z = 0xB0, F3DEX_G_TRI2 = 0xB1, F3DEX_G_MODIFYVTX = 0xB2,
    F3DEX_G_RDPHALF_2 = 0xB3, F3DEX_G_RDPHALF_1 = 0xB4, F3DEX_G_QUAD = 0xB5, F3DEX_G_CLEARGEOMETRYMODE = 0xB6,
    F3DEX_G_SETGEOMETRYMODE = 0xB7, F3DEX_G_ENDDL = 0xB8, F3DEX_G_SETOTHERMODE_L = 0xB9, F3DEX_G_SETOTHERMODE_H = 0xBA,
    F3DEX_G_TEXTURE = 0xBB, F3DEX_G_MOVEWORD = 0xBC, F3DEX_G_POPMTX = 0xBD, F3DEX_G_CULLDL = 0xBE, F3DEX_G_TRI1 = 0xBF,
    G_RDPNOOP = 0xC0,
};
// GBI1 G_MOVEMEM indices and G_MOVEWORD's vertex-modify index.
enum : uint8_t {
    F3DEX_G_MV_VIEWPORT = 0x80, F3DEX_G_MV_LOOKATY = 0x82, F3DEX_G_MV_LOOKATX = 0x84, F3DEX_G_MV_L0 = 0x86,
    F3DEX_G_MV_L7 = 0x94, F3DEX_G_MV_MATRIX_1 = 0x9E,
    F3DEX_G_MW_POINTS = 0x0C,
};
// The main loop's opcode numbering. An F3DEX list's low opcodes, which
// F3DEX2 uses for others, move up to kF3dexLow + op; its 0xAF..0xBF and the
// RDP's keep their numbers; anything a GBI1 list never holds (F3DEX2's own
// commands among them) is kOpUnknown. An F3DEX2 list keeps its numbers but
// for those F3DEX's take.
constexpr uint8_t kF3dexLow = 0x80, kOpUnknown = 0x7F;
struct OpTable {
    uint8_t v[256];
    constexpr explicit OpTable(bool f3dex) : v{} {
        for (int i = 0; i < 256; i++) v[i] = (uint8_t)i;
        for (int i = kF3dexLow; i <= F3DEX_G_TRI1; i++) v[i] = kOpUnknown;
        if (!f3dex) return;
        for (int i = 0; i < 0xC0; i++) v[i] = kOpUnknown;
        for (int i = 0; i < 0x10; i++) v[i] = (uint8_t)(kF3dexLow | i);
        for (int i = F3DEX_G_LOAD_UCODE; i <= F3DEX_G_TRI1; i++) v[i] = (uint8_t)i;
        v[G_EXTENDED] = G_EXTENDED;           // RT64_EXTENDED_OPCODE is the same in both
        for (int i = G_DMA_IO; i <= G_SETOTHERMODE_H; i++) v[i] = kOpUnknown;
        v[G_RDPNOOP] = G_NOOP;
    }
};
constexpr OpTable kF3dex2Op{ false }, kF3dexOp{ true };
// GBI1 geometry-mode bits that sit elsewhere in F3DEX2 (the rest agree).
enum : uint32_t {
    F3DEX_G_SHADING_SMOOTH = 0x00000200, F3DEX_G_CULL_FRONT = 0x00001000, F3DEX_G_CULL_BACK = 0x00002000,
    G_SHADING_SMOOTH = 0x00200000,
};

// othermode L: the depth source (pixel or the primitive depth)
constexpr uint32_t G_ZS_PRIM = 0x00000004;

enum : uint32_t {
    G_ZBUFFER = 0x00000001, G_SHADE = 0x00000004, G_CULL_FRONT = 0x00000200, G_CULL_BACK = 0x00000400,
    G_FOG = 0x00010000, G_LIGHTING = 0x00020000, G_TEXTURE_GEN = 0x00040000, G_TEXTURE_GEN_LINEAR = 0x00080000,
};

// Extended GBI (rt64_extended_gbi.h)
enum : uint32_t {
    EX_TEXRECT = 0x02, EX_FILLRECT = 0x03, EX_SETVIEWPORT = 0x04, EX_SETSCISSOR = 0x05,
    EX_SETRECTALIGN = 0x06, EX_SETVIEWPORTALIGN = 0x07, EX_SETSCISSORALIGN = 0x08, EX_SETREFRESHRATE = 0x09,
    EX_VERTEXZTEST = 0x0A, EX_ENDVERTEXZTEST = 0x0B, EX_MATRIXGROUP = 0x0C, EX_POPMATRIXGROUP = 0x0D,
    EX_FORCEUPSCALE2D = 0x0E, EX_FORCETRUEBILERP = 0x0F, EX_FORCESCALELOD = 0x10, EX_FORCEBRANCH = 0x11,
    EX_SETRENDERTORAM = 0x12, EX_EDITGROUPBYADDRESS = 0x13, EX_VERTEX = 0x14,
    EX_PUSHVIEWPORT = 0x15, EX_POPVIEWPORT = 0x16, EX_PUSHSCISSOR = 0x17, EX_POPSCISSOR = 0x18,
    EX_PUSHOTHERMODE = 0x19, EX_POPOTHERMODE = 0x1A, EX_PUSHCOMBINE = 0x1B, EX_POPCOMBINE = 0x1C,
    EX_PUSHPROJMATRIX = 0x1D, EX_POPPROJMATRIX = 0x1E, EX_PUSHENVCOLOR = 0x1F, EX_POPENVCOLOR = 0x20,
    EX_PUSHBLENDCOLOR = 0x21, EX_POPBLENDCOLOR = 0x22, EX_PUSHFOGCOLOR = 0x23, EX_POPFOGCOLOR = 0x24,
    EX_PUSHFILLCOLOR = 0x25, EX_POPFILLCOLOR = 0x26, EX_PUSHPRIMCOLOR = 0x27, EX_POPPRIMCOLOR = 0x28,
    EX_PUSHGEOMETRYMODE = 0x29, EX_POPGEOMETRYMODE = 0x2A, EX_SETDITHERNOISESTRENGTH = 0x2B,
    EX_SETRDRAMEXTENDED = 0x2C, EX_SETPROJMATRIXFLOAT = 0x2D, EX_SETVIEWMATRIXFLOAT = 0x2E,
    EX_SETNEARCLIPPING = 0x2F, EX_MATRIX_FLOAT = 0x30, EX_SETVERTEXSEGMENT = 0x31,
    EX_SETTEXCOORDWRAPPOINT = 0x32, EX_SETRECTASPECT = 0x33,
};
constexpr uint32_t kHookMagic = 0x525464;

struct Tile {
    uint8_t fmt = 0, siz = 0, palette = 0, cms = 0, cmt = 0, masks = 0, maskt = 0, shifts = 0, shiftt = 0;
    uint16_t line = 0, tmem = 0;
    uint16_t uls = 0, ult = 0, lrs = 0, lrt = 0;  // 10.2
};

// Where a LOADBLOCK/LOADTILE put its texels: enough to read them back from
// RDRAM at draw time (the texel data is not copied).
struct TmemLoad {
    bool valid = false;
    uint32_t tmem = 0;       // 64-bit words
    uint32_t words = 0;      // size in 64-bit words
    uint32_t addr = 0;       // RDRAM source (first texel)
    uint32_t pitch = 0;      // bytes per row in RDRAM
    uint8_t siz = 0;
    bool block = false;      // LOADBLOCK: texels contiguous, rows are the tile's line
    uint32_t seq = 0;        // load order: a later load overwrites the TMEM range
    uint8_t nibble = 0;      // 4-bit tile loads that start on an odd texel
    uint16_t dxt = 0;        // LOADBLOCK row step (1.11 per 64-bit word)
    // LOADTILE: the image it cut its rectangle from and where
    // (RenderDesc::merge_image_tiles).
    uint32_t img_addr = 0;   // SETTIMG's address: the image's first row
    uint16_t img_width = 0;  // texels per image row
    uint16_t s0 = 0, t0 = 0, s1 = 0, t1 = 0;   // the rectangle in image texels, inclusive
    uint16_t line = 0;       // the load tile's line (64-bit words per TMEM row)
};

struct Light { float dir[3]; uint8_t col[3]; };

struct Mtx { float m[4][4]; };

// A projection divides by w when its w column is anything but (0,0,0,s).
// Testing m[2][3] alone misses a projection the game multiplied by its view
// matrix (Goemon loads P*V on the projection stack): its w column is the
// view's forward axis, whose z part is 0 whenever the camera looks along
// the world's X axis, and the whole world then counted as orthographic.
inline bool is_perspective(const Mtx& p) {
    return p.m[0][3] != 0.0f || p.m[1][3] != 0.0f || p.m[2][3] != 0.0f || p.m[3][3] == 0.0f;
}

// The projection a depth camera is kept as: the context turns the depth
// grid into distances with its [2][2] and [3][2] (z_clip = a*z_eye + b,
// w = -z_eye). A projection times a view matrix V keeps that relation,
// z_clip = -a*w + b, but spreads a and b over the matrix: column 2 is
// a*V[.][2] + b*V[.][3] and column 3 is -V[.][2], so a comes from the two
// columns' ratio and b from the bottom row. A plain projection gives back
// its own [2][2] and [3][2].
inline Mtx depth_camera(const Mtx& p) {
    Mtx c = p;
    const float ww = p.m[0][3] * p.m[0][3] + p.m[1][3] * p.m[1][3] + p.m[2][3] * p.m[2][3];
    if (ww <= 0.0f) return c;
    const float a = -(p.m[0][2] * p.m[0][3] + p.m[1][2] * p.m[1][3] + p.m[2][2] * p.m[2][3]) / ww;
    c.m[2][2] = a;
    c.m[3][2] = p.m[3][2] + a * p.m[3][3];
    return c;
}

constexpr int kMaxDlDepth = 18;

// 1 for opcodes that may change the state a draw snapshots.
struct StateOpTable {
    uint8_t v[256];
    constexpr StateOpTable() : v{} {
        for (int i = 0; i < 256; i++) v[i] = 1;
        for (int op : std::initializer_list<int>{ G_TRI1, G_TRI2, G_QUAD, G_VTX, G_MODIFYVTX, G_DL, G_ENDDL, G_CULLDL, G_BRANCH_Z, G_NOOP,
                        G_RDPPIPESYNC, G_RDPLOADSYNC, G_RDPTILESYNC, G_MTX, G_POPMTX, G_MOVEMEM, G_RDPHALF_1, G_RDPHALF_2,
                        F3DEX_G_TRI1, F3DEX_G_TRI2, F3DEX_G_QUAD, kF3dexLow | F3DEX_G_VTX, F3DEX_G_MODIFYVTX, kF3dexLow | F3DEX_G_DL,
                        F3DEX_G_ENDDL, F3DEX_G_CULLDL, F3DEX_G_BRANCH_Z, kF3dexLow | F3DEX_G_MTX, F3DEX_G_POPMTX,
                        kF3dexLow | F3DEX_G_MOVEMEM, F3DEX_G_RDPHALF_1, F3DEX_G_RDPHALF_2 })
            v[op] = 0;
    }
};
constexpr StateOpTable kStateOpTable{};
#define kStateOp kStateOpTable.v

// 1 for opcodes that may change what describe_tile returns (tiles, loads,
// palettes, the texture tile index, texture filter/TLUT modes, and the
// combiner inputs that decide which texels are used).
struct TexOpTable {
    uint8_t v[256];
    constexpr TexOpTable() : v{} {
        for (int op : std::initializer_list<int>{ G_SETTILE, G_SETTILESIZE, G_LOADBLOCK, G_LOADTILE, G_LOADTLUT, G_SETOTHERMODE_H, G_RDPSETOTHERMODE,
                        G_TEXTURE, G_SETCOMBINE, F3DEX_G_SETOTHERMODE_H, F3DEX_G_TEXTURE })
            v[op] = 1;
    }
};
constexpr TexOpTable kTexOpTable{};
constexpr int kMaxMtxStack = 32;
constexpr int kMaxVerts = 64;

// A loaded vertex, already in N64 screen homogeneous space (the RSP applies
// the viewport at load time): x/w, y/w are pixels (y down), z/w in [0, 1];
// u/v are texel coordinates after the G_TEXTURE scale.
struct LoadedVertex { float x, y, z, w; float u, v; uint8_t r, g, b, a; };

// The RSP's fog factor for a vertex (G_FOG puts it in the shade alpha): the
// clip-space z clamped at zero, over w, times the fog multiplier plus the
// offset (RT64's RSPProcessCS: max(z, 0) / w * mul + offset). The clamp
// decides vertices behind the eye (z and w both negative): unclamped, their
// z / w is positive and they take full fog, which the near-plane clip then
// spreads over the near ground and water as a wash of the fog colour
// (Goemon's plaza and sunset water). z is the vertex's z' = (z + w) / 2.
inline uint8_t rsp_fog(float z, float w, float mul, float off) {
    const float zc = 2.0f * z - w;
    float f = (zc > 0.0f ? zc : 0.0f) / (w != 0.0f ? w : 1e-6f) * mul + off;
    if (!(f > 0.0f)) f = 0.0f;
    if (f > 255.0f) f = 255.0f;
    return (uint8_t)f;
}

// What a vertex load's vertices are transformed, lit and fogged with
// (load_vertices; with deferred vertices also kept per load, for
// finish_triangles). The lighting state of a lit load: the RSP's integer
// lighting terms and texgen's look-at directions and scales.
struct VtxShade {
    int32_t amb[3];
    int32_t n;                      // lights
    int32_t dir[8][3];              // their directions in 16.16, rotated by the modelview
    int32_t col[8][3];
    float lk[2][3];                 // look-at directions (texgen)
    float scale_s, scale_t;         // G_TEXTURE's scales (texgen)
    uint32_t linear;                // G_TEXTURE_GEN_LINEAR
};
struct VtxCtx {
    float m[4][4];                  // model -> N64 screen homogeneous
    float uscale, vscale, fm, fo;   // G_TEXTURE's scales, the fog terms
    bool lit, texgen, fogged;
    uint32_t shade;                 // Interpreter::Defer::shades (lit, deferred)
};
// One vertex: transform, texel coordinates, lighting, texgen and fog, as the
// RSP does them (sh: the load's lighting, for a lit one).
__attribute__((always_inline)) inline void shade_vertex(LoadedVertex& v, const VtxCtx& x, const VtxShade* sh, float ob0, float ob1, float ob2, float tu, float tv,
                         uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3) {
    const float (*m)[4] = x.m;
    v.x = ob0 * m[0][0] + ob1 * m[1][0] + ob2 * m[2][0] + m[3][0];
    v.y = ob0 * m[0][1] + ob1 * m[1][1] + ob2 * m[2][1] + m[3][1];
    v.z = ob0 * m[0][2] + ob1 * m[1][2] + ob2 * m[2][2] + m[3][2];
    v.w = ob0 * m[0][3] + ob1 * m[1][3] + ob2 * m[2][3] + m[3][3];
    v.u = tu * x.uscale;
    v.v = tv * x.vscale;
    if (x.lit) {
        // Integer lighting, as the RSP does it: the unnormalised
        // signed normal dotted with 16.16 unit directions.
        const VtxShade& s = *sh;
        const int32_t nx = (int8_t)c0, ny = (int8_t)c1, nz = (int8_t)c2;
        int32_t r = s.amb[0], g = s.amb[1], b = s.amb[2];
        for (int l = 0; l < s.n; l++) {
            int32_t d = nx * s.dir[l][0] + ny * s.dir[l][1] + nz * s.dir[l][2];
            if (d > 0) { r += d * s.col[l][0]; g += d * s.col[l][1]; b += d * s.col[l][2]; }
        }
        r >>= 16; g >>= 16; b >>= 16;
        v.r = (uint8_t)(r > 255 ? 255 : r);
        v.g = (uint8_t)(g > 255 ? 255 : g);
        v.b = (uint8_t)(b > 255 ? 255 : b);
        v.a = c3;
        if (x.texgen) {
            float fx = (float)nx, fy = (float)ny, fz = (float)nz;
            float dx = fx * s.lk[0][0] + fy * s.lk[0][1] + fz * s.lk[0][2];
            float dy = fx * s.lk[1][0] + fy * s.lk[1][1] + fz * s.lk[1][2];
            if (dx < -1) dx = -1; if (dx > 1) dx = 1;
            if (dy < -1) dy = -1; if (dy > 1) dy = 1;
            if (s.linear) {
                dx = acosf(-dx) / 3.14159265f;
                dy = acosf(-dy) / 3.14159265f;
            }
            else {
                dx = (dx + 1.0f) * 0.25f;
                dy = (dy + 1.0f) * 0.25f;
            }
            v.u = dx * s.scale_s / 32.0f;
            v.v = dy * s.scale_t / 32.0f;
        }
    }
    else {
        v.r = c0; v.g = c1; v.b = c2; v.a = c3;
    }
    if (x.fogged) v.a = rsp_fog(v.z, v.w, x.fm, x.fo);   // fog factor into alpha
}

// Where a triangle's output goes (output_tri, clip_tri): a vertex array (the
// frame's, or Interpreter::Defer's for deferred triangles), the vertex count
// of its record (or run), the counters, and the game's frame size.
struct TriSink {
    VtxArray* verts;
    uint32_t* count;
    uint32_t* tris;                 // triangles output
    uint32_t* clipped;              // ... clipped
    int screen_w, screen_h;
};

// The PICA rasterises in fixed point over a limited range: a triangle
// with a corner far off screen (or behind the eye) is dropped on the
// console, where emulators draw it. Such triangles are clipped here to a
// guard band around the screen and to the near plane (z' >= 0), in
// homogeneous screen space, then drawn as a fan.
constexpr float kGuard = 1024.0f;   // pixels beyond the screen centre, each axis
inline bool in_guard_band(const Vtx3ds& v, int screen_w, int screen_h) {
    if (!(v.w > 0.0f) || v.z < 0.0f) return false;
    const float gx = kGuard * v.w;
    const float dx = v.x - (float)screen_w * 0.5f * v.w, dy = v.y - (float)screen_h * 0.5f * v.w;
    return dx <= gx && dx >= -gx && dy <= gx && dy >= -gx;
}
inline Vtx3ds lerp_vtx(const Vtx3ds& p, const Vtx3ds& q, float t) {
    Vtx3ds o;
    o.x = p.x + (q.x - p.x) * t; o.y = p.y + (q.y - p.y) * t; o.z = p.z + (q.z - p.z) * t; o.w = p.w + (q.w - p.w) * t;
    o.u = p.u + (q.u - p.u) * t; o.v = p.v + (q.v - p.v) * t;
    auto mix = [t](uint8_t a, uint8_t b) { float f = (float)a + ((float)b - (float)a) * t; return (uint8_t)(f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f + 0.5f)); };
    o.r = mix(p.r, q.r); o.g = mix(p.g, q.g); o.b = mix(p.b, q.b); o.a = mix(p.a, q.a);
    return o;
}
void clip_tri(const TriSink& s, const Vtx3ds* tri) {
    const int screen_w = s.screen_w, screen_h = s.screen_h;
    {
        // Wholly outside one plane: nothing is left (what the clipping
        // below finds, without the clipping). Skies and models behind
        // the camera are most of the triangles that come here. Exact:
        // the guard band planes are tested only while no earlier plane
        // cut the triangle, i.e. on the vertices the loop would see.
        const float cx = (float)screen_w * 0.5f, cy = (float)screen_h * 0.5f;
        int out0 = 0;
        for (int i = 0; i < 3; i++) out0 += (tri[i].z - 1e-5f * tri[i].w) < 0.0f;
        if (out0 == 3) return;
        if (out0 == 0) {
            for (int plane = 1; plane < 5; plane++) {
                int outs = 0;
                for (int i = 0; i < 3; i++) {
                    const Vtx3ds& v = tri[i];
                    const float g = kGuard * v.w;
                    const float f = plane == 1 ? g - (v.x - cx * v.w) : plane == 2 ? g + (v.x - cx * v.w) :
                                    plane == 3 ? g - (v.y - cy * v.w) : g + (v.y - cy * v.w);
                    outs += f < 0.0f;
                }
                if (outs == 3) return;
                if (outs != 0) break;
            }
        }
    }
    Vtx3ds buf_a[12], buf_b[12];
    int n = 3;
    for (int i = 0; i < 3; i++) buf_a[i] = tri[i];
    Vtx3ds* in = buf_a;
    Vtx3ds* outp = buf_b;
    // Planes as f(v) >= 0: near (z' >= 0), then the four guard band edges.
    for (int plane = 0; plane < 5 && n >= 3; plane++) {
        const float cx = (float)screen_w * 0.5f, cy = (float)screen_h * 0.5f;
        auto f = [plane, cx, cy](const Vtx3ds& v) {
            const float g = kGuard * v.w;
            switch (plane) {
                case 0: return v.z - 1e-5f * v.w;
                case 1: return g - (v.x - cx * v.w);
                case 2: return g + (v.x - cx * v.w);
                case 3: return g - (v.y - cy * v.w);
                default: return g + (v.y - cy * v.w);
            }
        };
        int m = 0;
        for (int i = 0; i < n && m < 11; i++) {
            const Vtx3ds& p = in[i];
            const Vtx3ds& q = in[(i + 1) % n];
            const float fp = f(p), fq = f(q);
            if (fp >= 0.0f) outp[m++] = p;
            if ((fp >= 0.0f) != (fq >= 0.0f) && m < 11) outp[m++] = lerp_vtx(p, q, fp / (fp - fq));
        }
        n = m;
        Vtx3ds* t = in; in = outp; outp = t;
    }
    if (n < 3) return;
    (*s.clipped)++;
    Vtx3ds* o = s.verts->append((uint32_t)(n - 2) * 3);
    if (o == nullptr) return;
    for (int i = 1; i + 1 < n; i++) { *o++ = in[0]; *o++ = in[i]; *o++ = in[i + 1]; }
    *s.count += (uint32_t)(n - 2) * 3;
}

// A triangle's output (emit_tri's, or the deferred output's): culled by its
// winding, its texel coordinates relative to the tile and then mapped by
// remap, its depth the primitive's where zprim >= 0, clipped to the guard
// band where a corner falls outside it.
template <typename Remap>
inline void output_tri(const TriSink& s, const LoadedVertex& a, const LoadedVertex& b, const LoadedVertex& c, uint32_t cull,
                       float ss, float st, float uls, float ult, float zprim, Remap remap) {
    if (cull) {
        // Winding in screen space (y down), each vertex scaled by the
        // other two w's so the comparison holds without dividing; a
        // vertex behind the eye flips it, hence the sign of the product.
        const float ab = a.w * b.w, bc = b.w * c.w, ac = a.w * c.w;
        const float ax = a.x * bc, ay = a.y * bc;
        const float bx = b.x * ac, by = b.y * ac;
        const float cx = c.x * ab, cy = c.y * ab;
        float cross = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (ab * c.w < 0.0f) cross = -cross;
        // Front faces wind counter-clockwise on the y-down screen (cross < 0).
        if ((cull & G_CULL_BACK) && cross > 0.0f) return;
        if ((cull & G_CULL_FRONT) && cross < 0.0f) return;
    }
    Vtx3ds tri[3];
    for (int k = 0; k < 3; k++) {
        const LoadedVertex& v = k == 0 ? a : (k == 1 ? b : c);
        Vtx3ds& t = tri[k];
        t.x = v.x; t.y = v.y; t.z = zprim >= 0.0f ? zprim * v.w : v.z; t.w = v.w;
        t.u = v.u * ss - uls;
        t.v = v.v * st - ult;
        remap(t.u, t.v);
        t.r = v.r; t.g = v.g; t.b = v.b; t.a = v.a;
    }
    (*s.tris)++;
    if (!in_guard_band(tri[0], s.screen_w, s.screen_h) || !in_guard_band(tri[1], s.screen_w, s.screen_h) || !in_guard_band(tri[2], s.screen_w, s.screen_h)) {
        clip_tri(s, tri);
        return;
    }
    Vtx3ds* o = s.verts->append(3);
    if (o == nullptr) return;
    o[0] = tri[0]; o[1] = tri[1]; o[2] = tri[2];
    *s.count += 3;
}

}   // namespace

// Frame interpolation (Interpreter::set_interpolation): this list's
// transforms as the run records them, the list before's, and what the
// in-between run draws with.
struct Interpreter::Interp {
    interp::FrameLog logs[2];
    int cur = 0;                    // logs[cur]: the list run last (or being run)
    bool recording = false;         // run(): record into now()
    bool applying = false;          // run_inbetween(): draw with now()'s interpolated transforms
    bool diverged = false;          // run_inbetween() did not follow run()
    float weight = 1.0f;
    interp::FrameLog& now() { return logs[cur]; }
    interp::FrameLog& before() { return logs[cur ^ 1]; }

    // What build_inbetween needs of the list run last to draw it again with
    // other transforms, without running it: each vertex load's matrix and
    // viewport, each loaded vertex as it came from RDRAM and as it was lit,
    // and each triangle with its draw and its texel coordinates.
    struct LoadCtx {
        float m[4][4];              // model -> N64 screen homogeneous, as loaded
        float sx, sy, tx, ty;       // the viewport (and stretch) part of it
        int32_t xf, pj;             // the transform and projection (FrameLog)
        float uscale, vscale, fm, fo;
        bool texgen;
    };
    struct LoadedVtx {
        int16_t ob[3], tc[2];
        uint32_t ctx;               // ctxs
        uint32_t vi;                // its place among its transform's vertices
        float u, v;                 // texel coordinates as loaded (G_TEXTURE's scale or texgen applied)
        uint8_t rgba[4];            // lit (alpha before fog)
        bool fog;                   // alpha is the fog factor (from z)
    };
    // A triangle: its vertices, and the state it shares with the triangles
    // around it (a run of them, a new one where anything below changed).
    struct TriState {
        uint32_t draw;              // the record (after compact_rects)
        uint32_t first_tri;         // its first triangle (tris)
        uint32_t uv_first;          // not plain: the first triangle's texel coordinates in tri_uv (6 floats a triangle)
        float ss, st;               // the tile's shift scale (vertex texcoord interpolation)
        float uls, ult;             // the tile's origin, in texels
        float zprim;                // < 0: the vertices' depth, else the primitive depth (z')
        int32_t tile;               // its tile entry (FrameLog::tiles), -1 none
        uint32_t cull;              // the cull bits in force
        // Texel coordinates the vertices' own, relative to the tile (else
        // mapped onto an image, a frame or a snapshot: stored in tri_uv).
        bool plain;
    };
    struct TriRec {
        uint32_t src[3];            // lv
        uint32_t state;             // tri_states
    };
    std::vector<LoadCtx> ctxs;
    std::vector<LoadedVtx> lv;
    std::vector<TriRec> tris;
    std::vector<TriState> tri_states;
    std::vector<float> tri_uv;
    std::vector<uint32_t> draw_remap;   // compact_rects: old record index -> new
    // The rectangles' vertices as the list left them (rect_first: each
    // record's first in rect_verts, ~0u for triangles). The renderer changes
    // the game frame's in place (the cover zoom of backdrops at infinity)
    // while the builder copies them, and once more for each in-between: the
    // in-betweens take them from here.
    std::vector<Vtx3ds> rect_verts;
    std::vector<uint32_t> rect_first;
    std::vector<interp::M4> ctx_m;      // build_inbetween: each load's matrix in the in-between frame
    // ... and its loaded vertices as transformed (when a triangle first uses
    // them), one cache for each thread building (the helper's second).
    struct VtxCache {
        std::vector<uint32_t> stamp;    // == cur: lv_out holds it
        std::vector<Vtx3ds> out;
        uint32_t cur = 0;
    };
    VtxCache cache[2];
    VtxArray split_out;                 // the helper's share of the in-between (set_inbetween_helper)
};

// Deferred triangles (Interpreter::set_defer_triangles): the list's loaded
// vertices, its triangles as three of them each, and runs of triangles of
// one record under one state (a new run where the record or the state
// changed: every state command bumps state_seq).
struct Interpreter::Defer {
    struct Run {
        uint32_t draw;              // the record
        uint32_t first;             // its first triangle (tris)
        float ss, st, uls, ult;     // the tile's shift scales and origin (texels)
        float zprim;                // < 0: the vertices' depth, else the primitive depth (z')
        uint32_t cull;              // the cull bits in force
        uint32_t out_first, out_count;   // drain's: its triangles' vertices in out
    };
    struct Tri { uint32_t v[3]; };
    // The log: verts[e] once raw[e] is done (kDone), else the vertex as in
    // RDRAM and its load (ctxs), transformed when first used (resolve):
    // on the reader for G_BRANCH_Z (its depth alone), records output in the
    // read and vertex edits; the rest by drain.
    static constexpr uint32_t kDone = 0x80000000u;
    struct Raw { uint8_t b[16]; uint32_t ctx; };
    // Fixed room, made once: drain reads the log on another thread while
    // run writes on, so nothing may move. Full: a load is transformed in
    // the read (no context or lighting left) or not loaded (no entries
    // left), a triangle is dropped (as a full vertex buffer drops it).
    static constexpr uint32_t kVerts = 65536, kTris = 32768, kRuns = 16384, kCtxs = 16384, kShades = 4096, kOut = 65536;
    std::vector<LoadedVertex> verts;
    std::vector<Raw> raw;
    std::vector<VtxCtx> ctxs;
    std::vector<VtxShade> shades;
    std::vector<Run> runs;
    std::vector<Tri> tris;
    std::vector<Vtx3ds> out_store;
    VtxArray out;                   // drain's output, copied after the frame's own vertices by finish
    Defer() : verts(kVerts), raw(kVerts), ctxs(kCtxs), shades(kShades), runs(kRuns), tris(kTris), out_store(kOut) {
        out.set_storage(out_store.data(), kOut);
    }
    // run's counts (the reader's).
    uint32_t used = 0, nctxs = 0, nshades = 0, nruns = 0, ntris = 0, last_pub = 0;
    // What run has published for drain: the runs and triangles written
    // (release on pub_tris, acquire in drain), and its call to say so.
    uint32_t pub_runs = 0, pub_tris = 0;
    void (*notify)(void*) = nullptr;
    void* notify_arg = nullptr;
    static constexpr uint32_t kPublishEvery = 128;   // triangles
    // drain's.
    uint32_t done_tris = 0, started_runs = 0, out_tris = 0, out_clipped = 0;
    int screen_w = 320, screen_h = 240;
    bool pending = false;           // run() left triangles for finish_triangles
    bool vertices = true;           // the vertices' transforms too (set_defer_triangles)

    // A list's start (run, before it publishes anything; drain's side was
    // put back by the last finish).
    void begin(int sw, int sh) {
        used = nctxs = nshades = nruns = ntris = last_pub = 0;
        screen_w = sw; screen_h = sh;
    }
    void publish() {
        last_pub = ntris;
        __atomic_store_n(&pub_runs, nruns, __ATOMIC_RELAXED);
        __atomic_store_n(&pub_tris, ntris, __ATOMIC_RELEASE);
        if (notify != nullptr) notify(notify_arg);
    }
    // A fresh entry, done (the caller writes verts[e]). Full: the last one
    // again (65536 vertices a list: never seen).
    uint32_t add_done() {
        const uint32_t e = used < kVerts ? used++ : kVerts - 1;
        raw[e].ctx = kDone;
        return e;
    }
    // The load's lighting: the last one again if the same, else a new one
    // (~0u: full).
    uint32_t shade_index(const VtxShade& s) {
        if (nshades > 0 && memcmp(&shades[nshades - 1], &s, sizeof(s)) == 0) return nshades - 1;
        if (nshades >= kShades) return ~0u;
        shades[nshades] = s;
        return nshades++;
    }
    static void unpack(const uint8_t* src, float& ob0, float& ob1, float& ob2) {
        // RDRAM is stored in 32-bit little-endian words: halves are at ^2, bytes at ^3.
        ob0 = (float)*(const int16_t*)(src + 2); ob1 = (float)*(const int16_t*)(src + 0); ob2 = (float)*(const int16_t*)(src + 6);
    }
    // Both threads may resolve one entry at once: each writes the same
    // values, and the flag goes up after them.
    LoadedVertex& resolve(uint32_t e) {
        Raw& r = raw[e];
        const uint32_t c = __atomic_load_n(&r.ctx, __ATOMIC_ACQUIRE);
        if (!(c & kDone)) {
            const VtxCtx& x = ctxs[c];
            const uint8_t* src = r.b;
            float ob0, ob1, ob2;
            unpack(src, ob0, ob1, ob2);
            shade_vertex(verts[e], x, x.lit ? &shades[x.shade] : nullptr, ob0, ob1, ob2,
                         (float)*(const int16_t*)(src + 10), (float)*(const int16_t*)(src + 8), src[15], src[14], src[13], src[12]);
            __atomic_store_n(&r.ctx, c | kDone, __ATOMIC_RELEASE);
        }
        return verts[e];
    }
    // The vertex's z and w alone, as resolve computes them.
    void depth(uint32_t e, float& z, float& w) const {
        const Raw& r = raw[e];
        const uint32_t c = __atomic_load_n(&r.ctx, __ATOMIC_ACQUIRE);
        if (c & kDone) { z = verts[e].z; w = verts[e].w; return; }
        const float (*m)[4] = ctxs[c].m;
        float ob0, ob1, ob2;
        unpack(r.b, ob0, ob1, ob2);
        z = ob0 * m[0][2] + ob1 * m[1][2] + ob2 * m[2][2] + m[3][2];
        w = ob0 * m[0][3] + ob1 * m[1][3] + ob2 * m[2][3] + m[3][3];
    }
    // The published triangles' output into out, in order (each run's
    // vertices one stretch of it, and each record's runs consecutive).
    // On the thread that calls finish, while run goes on elsewhere or after.
    void drain() {
        const uint32_t nt = __atomic_load_n(&pub_tris, __ATOMIC_ACQUIRE);
        const uint32_t nr = __atomic_load_n(&pub_runs, __ATOMIC_RELAXED);
        for (; done_tris < nt; done_tris++) {
            while (started_runs < nr && runs[started_runs].first <= done_tris) {
                runs[started_runs].out_first = out.size();
                runs[started_runs].out_count = 0;
                started_runs++;
            }
            if (started_runs == 0) continue;
            Run& R = runs[started_runs - 1];
            const Tri& T = tris[done_tris];
            output_tri(TriSink{ &out, &R.out_count, &out_tris, &out_clipped, screen_w, screen_h },
                       resolve(T.v[0]), resolve(T.v[1]), resolve(T.v[2]), R.cull, R.ss, R.st, R.uls, R.ult, R.zprim,
                       [](float&, float&) {});
        }
    }
    // After run: the rest, then the output after the frame's own vertices
    // and the records' first and count; drain's side put back.
    void finish(FrameRecord& f, InterpreterStats& st) {
        drain();
        const uint32_t n = out.size();
        const uint32_t base = f.verts.size();
        Vtx3ds* dst = n != 0 ? f.verts.append(n) : nullptr;
        if (dst != nullptr) memcpy(dst, out.data(), n * sizeof(Vtx3ds));
        uint32_t cur = ~0u;
        DrawRecord* r = nullptr;
        for (uint32_t k = 0; k < started_runs; k++) {
            const Run& R = runs[k];
            if (R.draw != cur) {
                cur = R.draw;
                r = R.draw < f.draws.size() ? &f.draws[R.draw] : nullptr;
                if (r != nullptr) { r->first = dst != nullptr ? base + R.out_first : base; r->count = 0; }
            }
            if (r != nullptr && dst != nullptr) r->count += R.out_count;
        }
        st.tris += out_tris;
        st.clipped += out_clipped;
        done_tris = started_runs = out_tris = out_clipped = 0;
        out.clear();
        __atomic_store_n(&pub_runs, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&pub_tris, 0u, __ATOMIC_RELEASE);
    }
};

struct Interpreter::Impl {
    uint8_t* rdram;
    InterpreterStats* stats;
    FrameRecord* out = nullptr;

    // RSP
    uint32_t segments[16] = {};
    bool extended_rdram = false;
    bool ex_enabled = false;
    Mtx proj{}, mv_stack[kMaxMtxStack]{};
    int mv_depth = 0;
    Mtx mvp{};
    bool mvp_dirty = true;
    uint32_t geometry_mode = 0;
    // trans_stretch: the x translate without the origin's shift to the wide
    // screen's edge, for STRETCH content (see stretch_x).
    // The game's frame on the top screen (RenderDesc::vi_width/vi_height,
    // see ScreenGeometry). Everything here is in its N64 units; the
    // backend's projection scales them to the screen.
    ScreenGeometry geo = ScreenGeometry::make(320, 240);
    int screen_w = 320, screen_h = 240;
    struct Viewport { float scale[3], trans[3]; float trans_stretch = 160.0f; };
    Viewport vp{ { 160, -120, 511 }, { 160, 120, 511 }, 160.0f };
    Viewport vp_stack[8]{};
    int vp_depth = 0;
    int16_t vp_align_x = 0, vp_align_y = 0;
    uint16_t vp_origin = 0x800;
    struct { uint16_t scaleS, scaleT; uint8_t tile, level; bool on; } texture{ 0xFFFF, 0xFFFF, 0, 0, false };
    Light lights[8]{};
    uint8_t ambient[3]{};
    int num_lights = 0;
    float lookat[2][3]{};
    float ldir[8][3]{}, lk[2][3]{};   // light/lookat directions rotated by the current modelview
    bool ldir_dirty = true;
    int16_t fog_mul = 0, fog_off = 0;
    uint32_t proj_id = 0;
    // gEXMatrixGroup id stacks as RT64 keeps them: a push adds a level, the
    // group sets the top. gEXPopMatrixGroup pops the stack its flag names
    // (w1 bit 8). RT64 reads that flag from the opcode word, where it is
    // always clear; the desktop fork reads w1, as here, for Goemon's sky,
    // which pops a projection group. DK64 and Banjo only pop modelview ones.
    static constexpr int kGroupStack = 16;
    static constexpr uint32_t kIdAuto = 0xFFFFFFFFu;
    uint32_t proj_groups[kGroupStack] = {}, mv_groups[kGroupStack] = {};
    uint8_t proj_aspects[kGroupStack] = {};   // G_EX_ASPECT_* of each projection group
    int proj_group_depth = 0, mv_group_depth = 0;

    // ---- Frame interpolation (null when off: nothing below is used then).
    Interpreter::Interp* ip = nullptr;
    // The groups as RT64 keeps them (TransformGroup stacks beside the ids
    // above), and the index of each top in the log's groups (-1: changed
    // since it was last recorded).
    interp::Group mv_tg[kGroupStack], proj_tg[kGroupStack];
    int32_t mv_tg_idx = -1, proj_tg_idx = -1;
    // RT64's split of the projection stack into a view and a projection
    // (RSP::matrixCommon), with gEXPushProjectionMatrix's stack.
    Mtx pv_view{}, pv_proj{};
    Mtx pv_view_stack[8]{}, pv_proj_stack[8]{};
    uint32_t proj_addr_stack[8] = {};
    uint32_t mv_addr[32] = {}, proj_addr = 0, mtx_addr = 0;   // RDRAM addresses of the matrices (gEXEditGroupByAddress)
    uint32_t proj_seq = 1, proj_rec_seq = 0;   // projection changes, and the one last recorded
    int32_t xf_i = -1, pj_i = -1, tl_i = -1;   // the current entries of the log
    uint32_t v_i = 0;                          // vertices loaded under the current transform
    uint32_t slot_src[kMaxVerts] = {};         // the Interp::lv each vertex slot holds (recording)
    int32_t tile_cur = -1;                     // the tile entry of the current tile state, -1 none
    Mtx eff_mv{}, eff_proj{};                  // what the in-between run draws with
    uint32_t tile_rec_seq = 0;                 // tex_seq of the current tile entry
    // emit_tri's last Interp::TriState: a new one when any of these changed.
    uint32_t tri_key_draw = ~0u, tri_key_state = 0, tri_key_tex = 0;
    int32_t tri_key_tile = -2;
    bool tri_key_plain = false;
    int tile_rec_tile = -1, tile_rec_xf = -2;
    float tile_du[2] = {}, tile_dv[2] = {};    // the in-between's tile origin offsets (texels)
    uint32_t mv_seq = 0;
    // The world cameras (Interpreter::set_world_proj_ranges): none until the
    // context sets them from the game's World rules (a game that tags its
    // camera 0x10 + selector gives a range); dl_test sets DL_WORLD_IDS.
    static constexpr int kWorldRanges = 8;
    Interpreter::ProjIdRange world_ranges[kWorldRanges] = {};
    int world_range_count = 0;
    bool is_world_proj(uint32_t id) const {
        for (int i = 0; i < world_range_count; i++) {
            if (id >= world_ranges[i].lo && id <= world_ranges[i].hi) return true;
        }
        return false;
    }
    bool split_untagged_ortho = false;
    bool aim_hash = false;             // Interpreter::set_aim_hash
    // FNV-1a over the bytes the tile load reads (texel_span) and, with the
    // TLUT on, the palette entries it indexes: RenderDesc::aim_texture_hashes.
    uint64_t hash_texture(const TexDesc& d) const {
        uint32_t lo, hi;
        texel_span(d, lo, hi);
        uint64_t h = 1469598103934665603ull;
        for (uint32_t a = lo; a < hi; a++) h = (h ^ rdram[a]) * 1099511628211ull;
        if (d.tlut_mode != 0) {
            const int first = d.siz == 0 ? (int)d.palette * 16 : 0, n = d.siz == 0 ? 16 : 256;
            for (int i = 0; i < n && first + i < 256; i++) {
                h = (h ^ (tlut[first + i] & 0xFF)) * 1099511628211ull;
                h = (h ^ (tlut[first + i] >> 8)) * 1099511628211ull;
            }
        }
        return h == 0 ? 1 : h;
    }
    bool force_branch_z = true;        // RenderDesc::force_lod_branch, or gEXForceBranch
    bool fix_rect_lr = false;          // RenderDesc::fix_rect_lr
    int scissor_slack = 0;             // RenderDesc::scissor_edge_slack
    bool merge_rect_slices = false;    // RenderDesc::merge_rect_slices
    uint32_t hud_camera_id = 0;        // RenderDesc::hud_camera_id (0 = none)
    Gbi gbi = Gbi::F3DEX2;
    const uint8_t* op_table = kF3dex2Op.v;     // the main loop's numbering (kF3dexOp for F3DEX)
    // The vertex buffer (with deferred triangles: as it stood before the
    // list and after it, the list's own in dfr).
    LoadedVertex vtx[kMaxVerts]{};
    // ---- Interpreter::set_defer_triangles (dfr null when off): run()
    // writes every loaded vertex into dfr->verts (dslot: each slot's entry)
    // and each triangle of a plain record as its three entries, and
    // finish_triangles does the rest of emit_tri.
    Interpreter::Defer* dfr = nullptr;
    bool defer_active = false;             // in run(), with dfr
    int32_t defer_open = -1;               // the record taking deferred triangles (while it is the last)
    uint32_t defer_key_draw = ~0u, defer_key_state = 0;   // the state of dfr's last run
    uint32_t dslot[kMaxVerts] = {};
    LoadedVertex& slot(int i) { return defer_active ? dfr->resolve(dslot[i]) : vtx[i]; }
    // A fresh entry for slot i (edits of a vertex the triangles before it
    // keep, the list's starting buffer): written at once, before the next
    // one is made.
    LoadedVertex& new_slot(int i) {
        const uint32_t e = dfr->add_done();
        dslot[i] = e;
        return dfr->verts[e];
    }
    int32_t ldir_i[8][3]{}, lk_i[2][3]{};   // light directions in 16.16 for the integer lighting loop
    uint32_t rdphalf1 = 0, rdphalf2 = 0;

    // RDP
    Tile tiles[8]{};
    TmemLoad loads[16]{};
    int load_count = 0;
    struct { uint8_t fmt, siz; uint16_t width; uint32_t addr; } timg{};
    uint32_t cimg = 0, cimg_width = 0, zimg = 0;
    uint32_t othermode_h = 0, othermode_l = 0;
    uint32_t cc_w0 = 0, cc_w1 = 0;
    uint8_t prim[4]{}, env[4]{}, blend[4]{}, fog[4]{}, fill[4]{};
    uint8_t prim_lod_frac = 0;
    int16_t scissor[4] = { 0, 0, 320, 240 };
    int16_t scissor_stack[8][4]{};
    int scissor_depth = 0;
    // gEXPush*/gEXPop* state stacks.
    static constexpr int kStateStack = 8;
    struct { uint32_t h, l; } othermode_stack[kStateStack]{};
    struct { uint32_t w0, w1; } combine_stack[kStateStack]{};
    uint32_t geometry_stack[kStateStack]{};
    Mtx proj_stack[kStateStack]{};
    uint8_t color_stack[5][kStateStack][4]{};   // env, blend, fog, fill, prim
    int othermode_depth = 0, combine_depth = 0, geometry_depth = 0, proj_depth = 0, color_depth[5] = {};
    uint16_t prim_depth_z = 0;       // gDPSetPrimDepth (G_ZS_PRIM draws take it as their depth)
    // The primitive depth as z' (0 near .. 1 far): the RDP's 15-bit screen z.
    float prim_z() const { return (float)(prim_depth_z > 0x7FFF ? 0x7FFF : prim_depth_z) / 32767.0f; }
    uint16_t tlut[256]{};
    // Bumped by every LOADTLUT that changes an entry. Not a hash of the
    // load: the same palette loaded into another slot (Banjo's two eyes,
    // palettes 4 and 0) changes TMEM all the same.
    uint32_t tlut_seq = 1;
    uint32_t tlut_frame_seq = 0, tlut_frame_index = 0;
    // Hashes of [p] 4-bit palette p's 16 entries, [16] all 256, as of
    // tlut_seq pal_hash_seq[p].
    uint32_t pal_hash[17]{}, pal_hash_seq[17]{};
    uint32_t palette_hash(int p) {
        if (pal_hash_seq[p] != tlut_seq) {
            const int first = p < 16 ? p * 16 : 0, n = p < 16 ? 16 : 256;
            uint32_t h = 2166136261u;
            for (int i = 0; i < n; i++) h = (h ^ tlut[first + i]) * 16777619u;
            pal_hash[p] = h;
            pal_hash_seq[p] = tlut_seq;
        }
        return pal_hash[p];
    }
    uint32_t state_seq = 1;    // bumped by every command that changes draw state
    // TexDescs of the current texture tile pair, recomputed only when a
    // texture-affecting command ran (tex_seq) or the tile index changed.
    uint32_t tex_seq = 1, cached_tex_seq = 0;
    int cached_tile = -1;
    TexDesc cached_tex[2];
    const TexDesc* current_textures() {
        int t0 = texture.tile & 7;
        if (cached_tex_seq != tex_seq || cached_tile != t0) {
            cached_tex[0] = TexDesc{};
            cached_tex[1] = TexDesc{};
            tex0_load = -1;
            if (uses_texel(0)) {
                describe_tile(t0, cached_tex[0]);
                tex0_load = last_best;
                if (cached_tex[0].valid && lod_trilerp(cc_w0, cc_w1, othermode_h)) describe_mips(t0, cached_tex[0]);
            }
            if (uses_texel(1)) describe_tile((t0 + 1) & 7, cached_tex[1]);
            cached_tex_seq = tex_seq;
            cached_tile = t0;
            if (image_tiles) img_ok = describe_image(t0);
        }
        return cached_tex;
    }
    uint32_t last_draw_seq = 0;

    // RenderDesc::merge_image_tiles. Per image LOADTILE cuts tiles from, the
    // rows the last list that loaded from it loaded: a texture of the image
    // spans those rows and its full width, which keeps it the same texture
    // from frame to frame (a panorama's visible columns change as the camera
    // turns; its rows do not).
    bool image_tiles = false;
    struct ImageRows {
        uint32_t addr = 0;
        uint16_t width = 0;
        uint8_t siz = 0;
        int16_t lo = -1, hi = -1;           // the rows of the last list that loaded from it
        int16_t cur_lo = -1, cur_hi = -1;   // ... of this list so far
        uint32_t last_list = 0;
    };
    static constexpr int kImages = 16;
    static constexpr uint32_t kImageBytes = 1024 * 1024;   // largest PICA texture of an image
    ImageRows images[kImages];
    void note_image_load(const TmemLoad& L) {
        ImageRows* e = nullptr;
        for (ImageRows& im : images) {
            if (im.addr == L.img_addr && im.width == L.img_width && im.siz == L.siz) { e = &im; break; }
        }
        if (e == nullptr) {
            e = &images[0];
            for (ImageRows& im : images) if (im.last_list < e->last_list) e = &im;   // empty ones have 0
            *e = ImageRows{};
            e->addr = L.img_addr; e->width = L.img_width; e->siz = L.siz;
        }
        if (e->cur_lo < 0 || (int16_t)L.t0 < e->cur_lo) e->cur_lo = (int16_t)L.t0;
        if (e->cur_hi < 0 || (int16_t)L.t1 > e->cur_hi) e->cur_hi = (int16_t)L.t1;
        e->last_list = list_seq;
    }
    // At the start of a list: what the last one loaded becomes the rows;
    // images unused for a while are forgotten.
    void roll_images() {
        for (ImageRows& im : images) {
            if (im.cur_lo >= 0) { im.lo = im.cur_lo; im.hi = im.cur_hi; im.cur_lo = im.cur_hi = -1; }
            else if (im.addr != 0 && list_seq - im.last_list > 16) im = ImageRows{};
        }
    }
    // The current texture state as one texture of its image (img_tex), when
    // that samples what the tile does for coordinates inside the loaded
    // rectangle [0, img_lim_u] x [0, img_lim_v] of the tile: the triangle's
    // coordinates move by img_du, img_dv into the image texture.
    bool img_ok = false;
    TexDesc img_tex[2];
    float img_du = 0.0f, img_dv = 0.0f, img_lim_u = 0.0f, img_lim_v = 0.0f;
    bool last_tri_img = false;          // the last triangle's record samples an image texture
    int last_best = -1;                 // describe_tile's load
    int tex0_load = -1;                 // ... for TEXEL0's tile (current_textures)
    struct ImageMap { float du = 0.0f, dv = 0.0f, lim_u = 0.0f, lim_v = 0.0f; };
    static constexpr float kClampOvershoot = 8.0f;   // texels a rectangle may reach past its tile into the clamp
    bool describe_image(int t0) {
        if (cached_tex[1].valid) return false;
        ImageMap m;
        if (!image_of(t0, cached_tex[0], tex0_load, img_tex[0], m)) return false;
        img_tex[1] = TexDesc{};
        img_du = m.du; img_dv = m.dv; img_lim_u = m.lim_u; img_lim_v = m.lim_v;
        return true;
    }
    // Tile t0, described as d from load `load`, as a texture of its image:
    // `out`, and where the tile's texel coordinates go in it.
    bool image_of(int t0, const TexDesc& d, int load, TexDesc& out, ImageMap& map) const {
        if (!d.valid || d.snapshot || d.frame || d.rtt >= 0 || d.block || d.wrap_rows != 0 || load < 0) return false;
        const TmemLoad& L = loads[load];
        const Tile& tl = tiles[t0 & 7];
        if (L.block || L.img_width == 0 || tl.tmem != L.tmem || tl.siz != L.siz || (tl.siz != 1 && tl.siz != 2) || tl.line != L.line) return false;
        const ImageRows* im = nullptr;
        for (const ImageRows& e : images) {
            if (e.addr == L.img_addr && e.width == L.img_width && e.siz == L.siz) { im = &e; break; }
        }
        if (im == nullptr || im->lo < 0 || (int)L.t0 < im->lo || (int)L.t1 > im->hi) return false;
        const uint32_t rows = (uint32_t)(im->hi - im->lo + 1), w = L.img_width;
        if (w > 1024 || rows > 1024) return false;
        const bool wide = d.fmt == 3 || d.fmt == 4 || (d.fmt == 2 && d.tlut_mode == 3);   // decoded to RGBA8
        if ((uint32_t)pot16(w) * pot16(rows) * (wide ? 4u : 2u) > kImageBytes) return false;
        const uint32_t addr = L.img_addr + (uint32_t)im->lo * L.pitch;
        if ((addr & kRdramMask) + rows * L.pitch > kRdramBytes) return false;
        out = d;
        TexDesc& m = out;
        m.addr = addr;
        m.pitch = L.pitch;
        m.width = (uint16_t)w;
        m.height = (uint16_t)rows;
        m.masks = m.maskt = 0;
        m.cms = m.cmt = 2;           // clamped at the image's edges
        m.nibble = 0;
        m.dxt = 0;
        m.load_word = 0;
        map.du = (float)L.s0;
        map.dv = (float)(L.t0 - im->lo);
        map.lim_u = (float)std::min<int>(d.width, L.s1 - L.s0 + 1);
        map.lim_v = (float)std::min<int>(d.height, L.t1 - L.t0 + 1);
        return true;
    }

    // The frame cap (Interpreter::pass_over): what the list drew into. Plain
    // = only the colour image it draws first (and fills of the depth buffer).
    bool skim = false;                  // pass_over: state only, nothing drawn
    bool plain = true;
    uint32_t plain_cimg = 0;
    void note_target(bool fill) {
        if (cur_rtt >= 0) { plain = false; return; }
        if (cimg_is_depth_memory()) { if (!fill) plain = false; return; }
        if (plain_cimg == 0) plain_cimg = cimg;
        else if (cimg != plain_cimg) plain = false;
    }
    float rect_align[4]{};     // left/top/right/bottom offsets (extended), pixels
    uint32_t rect_lorigin = 0x800, rect_rorigin = 0x800;
    uint8_t rect_aspect = 0;

    // Framebuffer effects: the game copies the front buffer into a storage
    // buffer in RDRAM with texture rectangles, then draws with it. Draws
    // into the storage are dropped; the first one requests a GPU snapshot of
    // the last presented frame, and loads from the storage sample it.
    uint32_t vi_fbs[4] = {};
    int vi_fb_next = 0;
    // The VI origin register's values (note_vi_origin): a framebuffer plus
    // the mode's field offset (a line or two), so a colour image that short
    // distance below one is that framebuffer.
    uint32_t vi_origins[4] = {};
    int vi_origin_next = 0;
    static constexpr uint32_t kOriginSlack = 0x1000;
    bool is_vi_origin_fb(uint32_t a) const {
        for (uint32_t o : vi_origins) if (o != 0 && a <= o && o - a <= kOriginSlack) return true;
        return false;
    }
    // The storage buffers, one GPU copy each (kMaxSnapshots slots keyed by
    // base address; the least recently stored into makes way): Dinosaur
    // Planet's effects keep two snapshots and its pause a third, DK64 one.
    struct SnapSlot { uint32_t base = 0; bool valid = false; uint32_t list = 0; };
    SnapSlot snaps[kMaxSnapshots];
    int dbg_snap_miss = 0;
    int snap_slot_for(uint32_t base) {
        int pick = -1;
        for (int i = 0; i < kMaxSnapshots; i++) {
            if (snaps[i].valid && snaps[i].base == base) return i;
            if (!snaps[i].valid && pick < 0) pick = i;
        }
        if (pick < 0) {
            pick = 0;
            for (int i = 1; i < kMaxSnapshots; i++) if (snaps[i].list < snaps[pick].list) pick = i;
        }
        snaps[pick] = SnapSlot{ base, false, 0 };
        return pick;
    }
    int snap_of_load(uint32_t addr) const {
        for (int i = 0; i < kMaxSnapshots; i++) {
            if (snaps[i].valid && addr >= snaps[i].base && addr < snaps[i].base + (uint32_t)(screen_w * screen_h * 2)) return i;
        }
        return -1;
    }

    // Render to texture (RenderDesc::render_to_texture): a colour image that
    // is neither a presented framebuffer nor the depth buffer is drawn into
    // a GPU texture of its own, which later loads from that image sample.
    bool rtt_enabled = false;
    RttImage rtt_images[kMaxRtt];
    uint32_t rtt_list[kMaxRtt] = {};    // the list (list_seq) that last drew into each image
    int rtt_count = 0, rtt_next = 0;
    int cur_rtt = -1;                   // the image the current colour image draws into, or -1
    uint32_t list_seq = 0;              // display lists run so far

    // RenderDesc::sample_drawn_frame: the game copies parts of the frame it
    // is drawing into small images of its own, mid-list, and draws with them
    // (Goemon's menu box rims). Loads from the colour image being drawn,
    // after draws went into it, sample the frame as drawn so far
    // (TexDesc::frame); images narrower or wider than the frame are drawn
    // into GPU textures as render_to_texture would (a framebuffer always has
    // the frame's width).
    bool frame_copy = false;
    // RenderDesc::primary_world_instance: projection instances
    // (DrawRecord::proj_inst). A triangle record starts a new one when the
    // projection or the viewport differs from the instance's, or a rectangle
    // or a colour image change came in between (RT64 starts a new Projection
    // then); records are not merged across instances.
    bool inst_track = false;
    // RenderDesc::unify_world_depth, and the current projection's depth
    // terms (DrawRecord::depth_a/b), kept while the option is on.
    bool unify_depth = false;
    float cur_depth_a = 0.0f, cur_depth_b = 0.0f;
    void update_depth_terms() {
        if (!unify_depth || !is_perspective(proj)) { cur_depth_a = cur_depth_b = 0.0f; return; }
        const Mtx dc = depth_camera(proj);
        cur_depth_a = dc.m[2][2];
        cur_depth_b = dc.m[3][2];
    }
    bool world_depth_draw(const DrawRecord& d) const {
        return d.kind == DrawRecord::Tris && d.perspective && d.rtt < 0 && (d.othermode_l & G_ZS_PRIM) == 0 &&
               is_world_proj(d.proj_id) && d.depth_b != 0.0f;
    }
    // RenderDesc::unify_world_depth, once the frame's draws are recorded:
    // the World projection with the nearest near plane (n = b / (a - 1))
    // gives every World draw its depth, z' = ((1 - a) w + b) / 2 from the
    // vertex's w (= -z_eye), clamped to 0..w. The draws keep their own
    // terms, so an in-between frame built from the records does the same.
    void unify_world_depth(FrameRecord& f) const {
        if (!unify_depth) return;
        bool have = false, differ = false;
        float fa = 0.0f, fb = 0.0f, ta = 0.0f, tb = 0.0f, best = 0.0f;
        for (const DrawRecord& d : f.draws) {
            if (!world_depth_draw(d)) continue;
            const float n = d.depth_b / (d.depth_a - 1.0f);
            if (!(n > 0.0f)) continue;
            if (!have) { have = true; fa = ta = d.depth_a; fb = tb = d.depth_b; best = n; continue; }
            if (d.depth_a != fa || d.depth_b != fb) differ = true;
            if (n < best) { best = n; ta = d.depth_a; tb = d.depth_b; }
        }
        if (!have || !differ) return;
        for (const DrawRecord& d : f.draws) {
            if (!world_depth_draw(d) || (d.depth_a == ta && d.depth_b == tb)) continue;
            for (uint32_t k = d.first; k < d.first + d.count && k < f.verts.size(); k++) {
                Vtx3ds& v = f.verts[k];
                if (!(v.w > 0.0f)) continue;
                const float z = 0.5f * ((1.0f - ta) * v.w + tb);
                v.z = z < 0.0f ? 0.0f : (z > v.w ? v.w : z);
            }
        }
        if (f.cam_is_world) { f.cam[10] = ta; f.cam[14] = tb; }
    }
    uint32_t proj_inst = 0;
    bool inst_break = true;
    Mtx inst_proj{};
    Viewport inst_vp{};
    // The projection or the viewport may differ from the instance's: set by
    // everything that writes them, cleared once they compare equal (the
    // comparison per triangle cost ~3 ms a frame on the console).
    bool inst_dirty = true;
    bool new_instance_pending() {
        if (inst_break) return true;
        if (!inst_dirty) return false;
        const bool differs = memcmp(&inst_proj, &proj, sizeof(Mtx)) != 0 ||
                             memcmp(vp.scale, inst_vp.scale, sizeof(vp.scale)) != 0 || memcmp(vp.trans, inst_vp.trans, sizeof(vp.trans)) != 0;
        if (!differs) inst_dirty = false;
        return differs;
    }
    void start_instance(DrawRecord& r) {
        if (new_instance_pending()) {
            proj_inst++;
            inst_proj = proj;
            inst_vp = vp;
            inst_break = false;
            inst_dirty = false;
            if (hud_camera_id != 0 && proj_id == hud_camera_id && r.perspective) {
                FrameRecord::InstanceCam c;
                c.inst = proj_inst;
                const Mtx cam = depth_camera(proj);
                memcpy(c.m, &cam, sizeof(c.m));
                out->inst_cams.push_back(c);
            }
        }
        r.proj_inst = proj_inst;
    }
    uint32_t screen_cimg = 0;           // the on-screen colour image this list draws into
    bool screen_drawn = false;          // ... and something was drawn there

    bool is_presented_fb(uint32_t a) const {
        for (uint32_t fb : vi_fbs) if (fb != 0 && a == fb) return true;
        return is_vi_origin_fb(a);
    }
    // Colour drawn into the depth buffer's memory (Banjo stores its paused
    // frame there) is off screen, unless the game names a presented
    // framebuffer as its depth image (Dinosaur Planet's 640x480 front end
    // has no room for one and points at framebuffer 0): that is the screen.
    bool cimg_is_depth_memory() const {
        return zimg != 0 && cimg == zimg && !is_presented_fb(cimg);
    }
    int rtt_for_cimg(uint32_t a, uint32_t width) {
        if (a == zimg) return -1;
        // A framebuffer the VI presents is on screen wherever it lives
        // (Dinosaur Planet keeps its 640x480 front-end buffers in patch RAM).
        if (is_presented_fb(a)) return -1;
        if (!rtt_enabled) {
            if (!frame_copy || width == (uint32_t)screen_w || width == 0) return -1;
            // A copy target: one texture per address and width (Goemon draws
            // an 8-wide and a 256-wide image at one address, in memory that
            // does not overlap, but would in one texture).
            for (int i = 0; i < rtt_count; i++) {
                if (rtt_images[i].addr == a && rtt_images[i].width == width) { rtt_list[i] = list_seq; return i; }
            }
            int i = rtt_count < kMaxRtt ? rtt_count++ : (rtt_next++ % kMaxRtt);
            rtt_images[i].addr = a;
            rtt_images[i].width = (uint16_t)width;
            rtt_images[i].pot_w = pot16(width);
            rtt_images[i].pot_h = pot16((uint32_t)screen_h);
            rtt_list[i] = list_seq;
            return i;
        }
        // Known once two framebuffers have been presented (a double-buffered
        // game's second one is drawn before it is ever shown).
        const bool known = (vi_fbs[0] != 0 && vi_fbs[1] != 0) || (vi_origins[0] != 0 && vi_origins[1] != 0);
        if (!(a >= 0x800000 || (known && !is_presented_fb(a)))) return -1;
        for (int i = 0; i < rtt_count; i++) {
            if (rtt_images[i].addr == a) {
                if (rtt_images[i].width != width) {   // re-used at another width: a new texture
                    rtt_images[i].width = (uint16_t)width;
                    rtt_images[i].pot_w = pot16(width);
                }
                return i;
            }
        }
        int i = rtt_count < kMaxRtt ? rtt_count++ : (rtt_next++ % kMaxRtt);
        rtt_images[i].addr = a;
        rtt_images[i].width = (uint16_t)width;
        rtt_images[i].pot_w = pot16(width);
        rtt_images[i].pot_h = pot16((uint32_t)screen_h);
        return i;
    }
    static uint16_t pot16(uint32_t v) { uint32_t p = 8; while (p < v && p < 1024) p <<= 1; return (uint16_t)p; }
    // Texel coordinates relative to a tile that reads an off-screen image ->
    // texels of its render texture (rows counted from the texture's bottom,
    // where the PICA puts t = 0).
    void rtt_uv(const TexDesc& d, float& u, float& v) const {
        const RttImage& img = rtt_images[d.rtt];
        uint32_t texel = (d.addr - img.addr) / 2;
        float x0 = (float)(texel % img.width), y0 = (float)(texel / img.width);
        u = x0 + u;
        v = (float)img.pot_h - (y0 + v);
    }

    bool is_vi_framebuffer(uint32_t a) const {
        for (uint32_t fb : vi_fbs) if (fb != 0 && a >= fb && a < fb + (uint32_t)(screen_w * screen_h * 2)) return true;
        for (uint32_t o : vi_origins) if (o != 0 && a + kOriginSlack >= o && a < o + (uint32_t)(screen_w * screen_h * 2)) return true;
        return false;
    }

    // ---- memory
    uint8_t r8(uint32_t a) const { return rdram[(a & kRdramMask) ^ 3]; }
    int16_t r16(uint32_t a) const { return *(const int16_t*)(rdram + ((a & 0xFFFFFE) ^ 2)); }
    uint16_t ru16(uint32_t a) const { return *(const uint16_t*)(rdram + ((a & 0xFFFFFE) ^ 2)); }
    uint32_t r32(uint32_t a) const { return *(const uint32_t*)(rdram + (a & 0xFFFFFC)); }

    uint32_t seg(uint32_t a) const {
        // Extended RDRAM (gEXSetRDRAMExtended): a KSEG0 address as it is, so
        // mod data above the N64's 8 MB is reachable; segment offsets stay
        // the RSP's 24 bits. Both masked to the RDRAM model.
        if (extended_rdram && (a & 0x80000000)) return a & kRdramMask;
        return (segments[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & kRdramMask;
    }

    // ---- matrices
    void load_mtx(Mtx& m, uint32_t addr) {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                int16_t ip = r16(addr + (i * 4 + j) * 2);
                uint16_t fp = ru16(addr + 32 + (i * 4 + j) * 2);
                m.m[i][j] = (float)ip + (float)fp / 65536.0f;
            }
        }
    }
    static void mul(Mtx& out, const Mtx& a, const Mtx& b) {   // out = a * b (row vectors: v * a * b)
        Mtx t;
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
            t.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        }
        out = t;
    }
    static void identity(Mtx& m) {
        memset(&m, 0, sizeof(m));
        m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    }
    void update_mvp(uint32_t vtx_addr) {
        if (mvp_dirty) {
            if (ip != nullptr) {
                interp_point(vtx_addr);
                if (ip->applying) { mul(mvp, eff_mv, eff_proj); mvp_dirty = false; return; }
            }
            mul(mvp, mv_stack[mv_depth], proj);
            mvp_dirty = false;
        }
    }

    // ---- frame interpolation
    static interp::M4 m4(const Mtx& m) { interp::M4 r; memcpy(&r, &m, sizeof(r)); return r; }
    static Mtx mtx(const interp::M4& m) { Mtx r; memcpy(&r, &m, sizeof(r)); return r; }
    // A new transform for the vertices about to load (RT64 records a world
    // transform whenever the matrices changed since the last load, and a
    // projection when the projection did): recorded by run(), and by
    // run_inbetween() replaced with what match_previous() computed.
    void interp_point(uint32_t vtx_addr) {
        interp::FrameLog& L = ip->now();
        if (proj_seq != proj_rec_seq) {
            proj_rec_seq = proj_seq;
            pj_i++;
            if (ip->recording) {
                if (proj_tg_idx < 0) { proj_tg_idx = (int32_t)L.groups.size(); L.groups.push_back(proj_tg[proj_group_depth]); }
                interp::PjEntry e;
                e.view = m4(pv_view);
                e.proj = m4(pv_proj);
                e.full = m4(proj);
                e.group = (uint32_t)proj_tg_idx;
                e.addr = proj_addr;
                e.perspective = is_perspective(proj);
                L.pj.push_back(e);
            }
            if (ip->applying) {
                eff_proj = proj;
                if ((size_t)pj_i < L.pj.size()) {
                    const interp::PjEntry& e = L.pj[pj_i];
                    if (memcmp(&e.full, &proj, sizeof(Mtx)) != 0) ip->diverged = true;
                    else if (e.interp) eff_proj = mtx(e.lerped);
                }
                else ip->diverged = true;
            }
        }
        xf_i++;
        v_i = 0;
        if (ip->recording) {
            if (mv_tg_idx < 0) { mv_tg_idx = (int32_t)L.groups.size(); L.groups.push_back(mv_tg[mv_group_depth]); }
            interp::XfEntry e;
            e.m = m4(mv_stack[mv_depth]);
            e.group = (uint32_t)mv_tg_idx;
            e.key = vtx_addr;
            e.addr = mv_addr[mv_depth];
            e.pj = pj_i;
            e.vfirst = (uint32_t)(L.vdata.size() / 5);
            const interp::Group& g = L.groups[e.group];
            e.vstored = g.vert != interp::kSkip || g.tc != interp::kSkip;
            L.xf.push_back(e);
        }
        if (ip->applying) {
            eff_mv = mv_stack[mv_depth];
            if ((size_t)xf_i < L.xf.size()) {
                const interp::XfEntry& e = L.xf[xf_i];
                if (memcmp(&e.m, &mv_stack[mv_depth], sizeof(Mtx)) != 0) ip->diverged = true;
                else if (e.interp) eff_mv = mtx(e.lerped);
            }
            else ip->diverged = true;
            ldir_dirty = true;     // lighting follows the interpolated modelview
        }
    }
    const Mtx& light_mv() const { return (ip != nullptr && ip->applying) ? eff_mv : mv_stack[mv_depth]; }
    // A projection matrix command, as RT64 splits it into a view and a
    // projection (RSP::matrixCommon).
    void interp_proj_matrix(const Mtx& m, uint8_t p) {
        const interp::M4 f = m4(m);
        if (p & 2) {
            interp::M4 v, pr;
            if (interp::is_view_proj(f)) interp::decompose_view_proj(f, v, pr);
            else { pr = f; v = interp::identity(); }
            pv_view = mtx(v);
            pv_proj = mtx(pr);
        }
        else if (interp::is_affine(f) && !interp::is_identity(f)) mul(pv_view, m, pv_view);
        else mul(pv_proj, m, pv_proj);
        proj_addr = mtx_addr;
        proj_seq++;
    }
    // gEXEditGroupByAddress: the groups of this list's transforms loaded
    // from that address take the new properties where they allow it.
    void interp_edit_group(uint32_t addr, uint32_t flags) {
        interp::FrameLog& L = ip->now();
        const interp::Group g = interp::Group::decode(0, flags);
        const bool proj_group = (flags >> 1) & 1;
        auto edit = [&](uint32_t gi) { if (gi < L.groups.size() && L.groups[gi].edit == 1) L.groups[gi].set_props(g); };
        if (proj_group) { for (const interp::PjEntry& e : L.pj) if (e.addr == addr) edit(e.group); }
        else { for (const interp::XfEntry& e : L.xf) if (e.addr == addr) edit(e.group); }
    }
    // The texture tiles a triangle samples, as RT64 matches them per draw:
    // a new entry when the tiles or the transform changed. Sets tile_du/dv.
    void interp_tiles() {
        if (tex_seq == tile_rec_seq && (int)texture.tile == tile_rec_tile && xf_i == tile_rec_xf) return;
        tile_rec_seq = tex_seq;
        tile_rec_tile = texture.tile;
        tile_rec_xf = xf_i;
        tile_du[0] = tile_du[1] = tile_dv[0] = tile_dv[1] = 0.0f;
        tile_cur = -1;
        interp::FrameLog& L = ip->now();
        if (xf_i < 0 || (size_t)xf_i >= L.xf.size()) return;
        const interp::Group& g = L.groups[L.xf[xf_i].group];
        if (g.tile == interp::kSkip) return;
        const TexDesc* tex = current_textures();
        if (!tex[0].valid || (image_tiles && img_ok)) return;
        tl_i++;
        tile_cur = tl_i;
        if (ip->recording) {
            interp::TileEntry e;
            uint32_t h = 2166136261u;
            auto mixh = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };
            mixh(cc_w0); mixh(cc_w1); mixh(othermode_h); mixh(othermode_l); mixh(geometry_mode); mixh(g.id);
            e.key = h;
            h = 2166136261u;
            mixh(tex[0].addr); mixh((uint32_t)tex[0].fmt | ((uint32_t)tex[0].siz << 8) | ((uint32_t)tex[0].palette << 16)); mixh(tex[0].tlut_hash);
            e.tex = h;
            e.flag = g.tile;
            e.ntiles = tex[1].valid ? 2 : 1;
            for (int t = 0; t < e.ntiles; t++) {
                const Tile& tl = tiles[(texture.tile + t) & 7];
                interp::TileParams& p = e.t[t];
                p.uls = tl.uls; p.ult = tl.ult; p.lrs = tl.lrs; p.lrt = tl.lrt; p.line = tl.line;
                p.fmt = tl.fmt; p.siz = tl.siz; p.masks = tl.masks; p.maskt = tl.maskt; p.shifts = tl.shifts; p.shiftt = tl.shiftt;
                p.cms = tl.cms; p.cmt = tl.cmt;
            }
            L.tiles.push_back(e);
        }
        if (ip->applying) {
            if ((size_t)tl_i >= L.tiles.size()) { ip->diverged = true; return; }
            const interp::TileEntry& e = L.tiles[tl_i];
            if (!e.interp) return;
            tile_du[0] = e.du[0]; tile_dv[0] = e.dv[0];
            tile_du[1] = e.du[1]; tile_dv[1] = e.dv[1];
        }
    }

    // ---- vertices
    void update_light_dirs() {
        // Light and look-at directions rotated by the modelview (transposed
        // product, as the RSP effectively does) so they can be dotted with
        // model-space normals; the vertex normals themselves are used as the
        // RSP uses them, unnormalised, scaled by 1/127.
        const Mtx& mv = light_mv();
        for (int i = 0; i < num_lights; i++) {
            float d[3];
            for (int k = 0; k < 3; k++) d[k] = lights[i].dir[0] * mv.m[k][0] + lights[i].dir[1] * mv.m[k][1] + lights[i].dir[2] * mv.m[k][2];
            float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            float inv = len > 0.0f ? 1.0f / (len * 127.0f) : 0.0f;
            ldir[i][0] = d[0] * inv; ldir[i][1] = d[1] * inv; ldir[i][2] = d[2] * inv;
            for (int k = 0; k < 3; k++) ldir_i[i][k] = (int32_t)(ldir[i][k] * 65536.0f);
        }
        for (int i = 0; i < 2; i++) {
            float d[3];
            for (int k = 0; k < 3; k++) d[k] = lookat[i][0] * mv.m[k][0] + lookat[i][1] * mv.m[k][1] + lookat[i][2] * mv.m[k][2];
            float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            float inv = len > 0.0f ? 1.0f / (len * 127.0f) : 0.0f;
            lk[i][0] = d[0] * inv; lk[i][1] = d[1] * inv; lk[i][2] = d[2] * inv;
        }
        ldir_dirty = false;
    }

    uint32_t last_vtx_addr = 0;
    void load_vertices(uint32_t addr, int n, int v0) {
#ifdef DL_BENCH_SKIP_VTX
        return;
#endif
        // A pass_over needs no vertices, unless G_BRANCH_Z tests their depth
        // (force_lod_branch off): then the list's flow, and so the state it
        // leaves, depends on them.
        if (skim && force_branch_z) return;
        last_vtx_addr = addr;
        update_mvp(addr);
        const bool lit = (geometry_mode & G_LIGHTING) != 0;
        const bool texgen = (geometry_mode & G_TEXTURE_GEN) != 0;
        const bool fogged = (geometry_mode & G_FOG) != 0;
        if ((lit || texgen) && ldir_dirty) update_light_dirs();
        const float uscale = (float)texture.scaleS / (65536.0f * 32.0f), vscale = (float)texture.scaleT / (65536.0f * 32.0f);
        // Model -> clip -> screen in one matrix: the viewport is a scale and
        // translate of x/w and y/w (y negated: screen y grows downward) and
        // z' = (z + w) / 2, all linear in homogeneous coordinates.
        const bool stretch = proj_stretch() && cur_rtt < 0;   // an off-screen image is drawn 1:1
        const float sx = stretch ? vp.scale[0] * stretch_k() : vp.scale[0], sy = -vp.scale[1];
        const float tx = stretch ? stretch_x(vp.trans_stretch) : vp.trans[0], ty = vp.trans[1];
        float m[4][4];
        for (int i = 0; i < 4; i++) {
            const float cx = mvp.m[i][0], cy = mvp.m[i][1], cz = mvp.m[i][2], cw = mvp.m[i][3];
            m[i][0] = cx * sx + cw * tx;
            m[i][1] = cy * sy + cw * ty;
            m[i][2] = 0.5f * (cz + cw);
            m[i][3] = cw;
        }
        const float fm = (float)fog_mul, fo = (float)fog_off;
        const int32_t amb0 = ambient[0] << 16, amb1 = ambient[1] << 16, amb2 = ambient[2] << 16;
        int lo = v0 < 0 ? -v0 : 0, hi = n;
        if (v0 + hi > kMaxVerts) hi = kMaxVerts - v0;
        // Frame interpolation: the transform's vertices are recorded (when
        // its group interpolates vertices or texcoords), or moved between the
        // previous frame's and these.
        interp::XfEntry* xe = nullptr;
        const int16_t* vprev = nullptr;
        float vw = 0.0f;
        uint32_t vi0 = 0;
        if (ip != nullptr && hi > lo) {
            interp::FrameLog& L = ip->now();
            if (xf_i >= 0 && (size_t)xf_i < L.xf.size()) {
                xe = &L.xf[xf_i];
                if (ip->recording) {
                    Interpreter::Interp::LoadCtx lc;
                    memcpy(lc.m, m, sizeof(lc.m));
                    lc.sx = sx; lc.sy = sy; lc.tx = tx; lc.ty = ty;
                    lc.xf = xf_i; lc.pj = pj_i;
                    lc.uscale = uscale; lc.vscale = vscale; lc.fm = fm; lc.fo = fo;
                    lc.texgen = lit && texgen;
                    ip->ctxs.push_back(lc);
                    if (xe->vstored) {
                        const size_t base = L.vdata.size();
                        L.vdata.resize(base + (size_t)(hi - lo) * 5);
                        int16_t* q = &L.vdata[base];
                        for (int i = lo; i < hi; i++, q += 5) {
                            const uint8_t* src = rdram + ((addr + i * 16) & 0xFFFFFC);
                            q[0] = *(const int16_t*)(src + 2); q[1] = *(const int16_t*)(src + 0); q[2] = *(const int16_t*)(src + 6);
                            q[3] = *(const int16_t*)(src + 10); q[4] = *(const int16_t*)(src + 8);
                        }
                    }
                    xe->vcount += (uint32_t)(hi - lo);
                }
                else if (ip->applying && (xe->vpos || xe->vtc) && v_i + (uint32_t)(hi - lo) <= xe->vcount) {
                    vprev = &ip->before().vdata[((size_t)xe->vprev + v_i) * 5];
                    vw = 1.0f - ip->weight;
                }
            }
            vi0 = v_i;
            v_i += (uint32_t)(hi - lo);
        }
        VtxCtx x;
        memcpy(x.m, m, sizeof(x.m));
        x.uscale = uscale; x.vscale = vscale; x.fm = fm; x.fo = fo;
        x.lit = lit; x.texgen = texgen; x.fogged = fogged;
        x.shade = 0;
        VtxShade sh;
        if (lit) {
            memset(&sh, 0, sizeof(sh));
            sh.amb[0] = amb0; sh.amb[1] = amb1; sh.amb[2] = amb2;
            sh.n = num_lights;
            for (int l = 0; l < num_lights; l++) {
                for (int k = 0; k < 3; k++) { sh.dir[l][k] = ldir_i[l][k]; sh.col[l][k] = lights[l].col[k]; }
            }
            memcpy(sh.lk, lk, sizeof(sh.lk));
            sh.scale_s = (float)texture.scaleS; sh.scale_t = (float)texture.scaleT;
            sh.linear = (geometry_mode & G_TEXTURE_GEN_LINEAR) != 0;
        }
        // Deferred triangles: the load's vertices go to fresh entries, as
        // they are in RDRAM (Defer::resolve transforms them when used), or
        // done now while frame interpolation records them.
        LoadedVertex* dlog = nullptr;
        if (defer_active && hi > lo) {
            Interpreter::Defer& D = *dfr;
            if (D.used + (uint32_t)(hi - lo) > Interpreter::Defer::kVerts) return;   // full: not loaded
            bool raw_load = D.vertices && (ip == nullptr || !ip->recording) && vprev == nullptr && D.nctxs < Interpreter::Defer::kCtxs;
            if (raw_load && lit) {
                x.shade = D.shade_index(sh);
                raw_load = x.shade != ~0u;
            }
            if (raw_load) {
                const uint32_t ci = D.nctxs++;
                D.ctxs[ci] = x;
                for (int i = lo; i < hi; i++) {
                    const uint32_t e = D.used++;
                    Interpreter::Defer::Raw& r = D.raw[e];
                    memcpy(r.b, rdram + ((addr + i * 16) & 0xFFFFFC), 16);
                    r.ctx = ci;
                    dslot[v0 + i] = e;
                }
                return;
            }
            dlog = D.verts.data();
        }
        for (int i = lo; i < hi; i++) {
            const uint8_t* src = rdram + ((addr + i * 16) & 0xFFFFFC);
            // RDRAM is stored in 32-bit little-endian words: halves are at ^2, bytes at ^3.
            float ob0 = (float)*(const int16_t*)(src + 2), ob1 = (float)*(const int16_t*)(src + 0), ob2 = (float)*(const int16_t*)(src + 6);
            const int16_t tc0 = *(const int16_t*)(src + 10), tc1 = *(const int16_t*)(src + 8);
            float tu = (float)tc0, tv = (float)tc1;
            if (vprev != nullptr) {
                // RT64: pos - (pos - prev) * (1 - weight), per component.
                const int16_t* p = vprev + (size_t)(i - lo) * 5;
                if (xe->vpos) { ob0 += ((float)p[0] - ob0) * vw; ob1 += ((float)p[1] - ob1) * vw; ob2 += ((float)p[2] - ob2) * vw; }
                if (xe->vtc) { tu += ((float)p[3] - tu) * vw; tv += ((float)p[4] - tv) * vw; }
            }
            const uint8_t c0 = src[15], c1 = src[14], c2 = src[13], c3 = src[12];
            LoadedVertex* vp = &vtx[v0 + i];
            if (dlog != nullptr) { const uint32_t e = dfr->used++; dfr->raw[e].ctx = Interpreter::Defer::kDone; dslot[v0 + i] = e; vp = &dlog[e]; }
            LoadedVertex& v = *vp;
            shade_vertex(v, x, &sh, ob0, ob1, ob2, tu, tv, c0, c1, c2, c3);
            if (xe != nullptr && ip->recording) {
                Interpreter::Interp::LoadedVtx q;
                q.ob[0] = *(const int16_t*)(src + 2); q.ob[1] = *(const int16_t*)(src + 0); q.ob[2] = *(const int16_t*)(src + 6);
                q.tc[0] = tc0; q.tc[1] = tc1;
                q.ctx = (uint32_t)ip->ctxs.size() - 1;
                q.vi = vi0 + (uint32_t)(i - lo);
                q.u = v.u; q.v = v.v;
                q.rgba[0] = v.r; q.rgba[1] = v.g; q.rgba[2] = v.b; q.rgba[3] = v.a;
                q.fog = fogged;
                slot_src[v0 + i] = (uint32_t)ip->lv.size();
                ip->lv.push_back(q);
            }
        }
    }

    // ---- texture description from a tile
    void describe_tile(int t, TexDesc& d) {
        const Tile& tl = tiles[t & 7];
        d.valid = false;
        // Find the load whose TMEM range holds this tile.
        // The most recent load covering the tile's TMEM address wins: the
        // loads of this list sit in the ring in order, newest at
        // load_count - 1, so the first one found looking back is it.
        const TmemLoad* best = nullptr;
        const int oldest = load_count > 16 ? load_count - 16 : 0;
        for (int k = load_count - 1; k >= oldest; k--) {
            const TmemLoad& L = loads[k & 15];
            if (L.valid && tl.tmem >= L.tmem && tl.tmem < L.tmem + L.words) { best = &L; break; }
        }
        last_best = best != nullptr ? (int)(best - loads) : -1;
        if (best == nullptr) {
            stats->tex_unresolved++;
            if (stats->tex_unresolved <= 3) {
                fprintf(stderr, "rt64-3ds: tile %d (fmt %u siz %u tmem %u line %u %u..%u x %u..%u) has no load; %d loads:",
                        t, tl.fmt, tl.siz, tl.tmem, tl.line, tl.uls >> 2, tl.lrs >> 2, tl.ult >> 2, tl.lrt >> 2, load_count);
                for (int i = 0; i < 16; i++) if (loads[i].valid) fprintf(stderr, " [%u+%u]", loads[i].tmem, loads[i].words);
                fprintf(stderr, "\n");
            }
            return;
        }
        // A masked axis repeats with the mask's period whatever the tile
        // size (scrolling tiles set an upper bound below the lower one);
        // with clamping on, a smaller tile clamps inside that period.
        int tw = ((tl.lrs >> 2) - (tl.uls >> 2)) + 1;
        int th = ((tl.lrt >> 2) - (tl.ult >> 2)) + 1;
        int w = tw, h = th;
        if (tl.masks != 0) { int m = 1 << tl.masks; w = ((tl.cms & 2) && tw > 0 && tw < m) ? tw : m; }
        if (tl.maskt != 0) { int m = 1 << tl.maskt; h = ((tl.cmt & 2) && th > 0 && th < m) ? th : m; }
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return;
        // A block load is one contiguous run whose rows are the tile's line;
        // a tile load copies rows out of a wider image in RDRAM.
        // 32-bit texels are split across TMEM's two halves, so their line
        // counts half the row.
        uint32_t pitch = best->block ? tl.line * (tl.siz == 3 ? 16 : 8) : best->pitch;
        if (pitch == 0) pitch = best->pitch;
        d.valid = true;
        d.addr = best->addr + (tl.tmem - best->tmem) * 8;
        d.pitch = pitch;
        // TMEM is 4 KB (2 KB for 32-bit texels, split over its halves, and
        // for the texels when the TLUT holds the upper half); the sampler's
        // address wraps there. A tile taller than that from the start of
        // TMEM reads its first rows again (Goemon's menu curtain: a 64x32
        // load fills TMEM, the tile's mask is 64 rows), not RDRAM past them.
        d.wrap_rows = 0;
        if (tl.tmem == 0 && tl.line != 0 && h > 1) {
            const uint32_t limit = (tl.siz == 3 || ((othermode_h >> 14) & 3) >= 2) ? 256u : 512u;
            if (limit % tl.line == 0 && limit / tl.line < (uint32_t)h) d.wrap_rows = (uint16_t)(limit / tl.line);
        }
        // RenderDesc::sample_drawn_frame: a 16-bit tile load out of the
        // colour image this list is drawing into, after something was drawn
        // there, reads the frame drawn so far (Goemon's menu boxes copy
        // strips of the backdrop for their rims).
        d.frame = frame_copy && screen_drawn && screen_cimg != 0 && !best->block && tl.siz == 2 &&
                  best->pitch == (uint32_t)screen_w * 2 && d.addr >= screen_cimg && d.addr < screen_cimg + (uint32_t)(screen_w * screen_h * 2);
        // Reads of the stored frame are 16-bit tile loads out of a
        // screen-wide image; anything else at that address is ordinary
        // texture data (the storage may be heap memory the game reuses later).
        const int snap = d.frame ? -1 : snap_of_load(d.addr);
        d.snapshot = snap >= 0 && !best->block && best->pitch == (uint32_t)screen_w * 2 && tl.siz == 2;
        d.snap_slot = d.snapshot ? (uint8_t)snap : 0;
        if (!d.snapshot && dbg_snap_miss < 6) {
            for (int i = 0; i < kMaxSnapshots; i++) {
                if (snaps[i].base == 0 || d.addr < snaps[i].base || d.addr >= snaps[i].base + (uint32_t)(screen_w * screen_h * 2)) continue;
                dbg_snap_miss++;
                fprintf(stderr, "rt64-3ds: load at %06x inside snapshot slot %d (base %06x valid %d) is not a snapshot read: block %d pitch %u (screen %d) siz %u frame %d tile %dx%d\n",
                        d.addr, i, snaps[i].base, snaps[i].valid ? 1 : 0, best->block ? 1 : 0, best->pitch, screen_w, tl.siz, d.frame ? 1 : 0, w, h);
            }
        }
        d.rtt = -1;
        // With sample_drawn_frame a block load of whole rows of an image
        // reads it too (the rims load their strips with LOADBLOCK), but only
        // an image drawn by this list: the memory is the game's to reuse.
        if ((rtt_enabled || frame_copy) && !d.snapshot && !d.frame && (!best->block || frame_copy) && tl.siz == 2) {
            const uint32_t row = best->block ? pitch : best->pitch;
            for (int i = 0; i < rtt_count; i++) {
                const RttImage& img = rtt_images[i];
                if (!rtt_enabled && rtt_list[i] != list_seq) continue;
                if (d.addr >= img.addr && d.addr < img.addr + (uint32_t)img.width * 2 * img.pot_h && row == (uint32_t)img.width * 2) {
                    d.rtt = (int8_t)i;
                    break;
                }
            }
        }
        d.tlut_mode = (othermode_h >> 14) & 3;
        // With the TLUT on, the RDP looks every 4- and 8-bit texel up in the
        // palette whatever the tile's format says (Banjo's HUD sprites are
        // RGBA 4-bit tiles with a palette): read them as CI.
        d.fmt = (d.tlut_mode >= 2 && tl.siz <= 1) ? 2 : tl.fmt;
        d.siz = tl.siz;
        d.width = (uint16_t)w;
        d.height = (uint16_t)h;
        d.masks = tl.masks; d.maskt = tl.maskt; d.cms = tl.cms; d.cmt = tl.cmt;
        d.palette = tl.palette;
        d.nibble = (tl.siz == 0) ? best->nibble : 0;
        d.block = best->block;
        d.dxt = best->dxt;
        d.load_word = (uint16_t)(tl.tmem - best->tmem);
        // The entries this draw reads: a 4-bit texture's palette, else all
        // 256 (the texture cache's key).
        d.tlut_hash = (d.fmt == 2) ? palette_hash(d.siz == 0 ? (d.palette & 15) : 16) : 0;
        d.tlut_index = 0;
        if (d.fmt == 2) {
            // Palettes change within a frame: keep the one this draw uses.
            if (tlut_frame_seq != tlut_seq) {
                tlut_frame_seq = tlut_seq;
                tlut_frame_index = (uint32_t)out->tlut.size();
                out->tlut.insert(out->tlut.end(), tlut, tlut + 256);
            }
            d.tlut_index = tlut_frame_index;
        }
        d.bilerp = ((othermode_h >> 12) & 3) != 0;
    }

    // A mipmapped draw's further levels (lod_trilerp): the tiles after the
    // base, up to G_TEXTURE's level count, each the one before at half the
    // size and texel scale, in the same format and palette. The PICA's
    // levels stop at 8x8 (smaller ones are left out: the far distance shows
    // the 8x8 level); a tile that does not fit the chain ends it.
    void describe_mips(int t0, TexDesc& b) {
        if (texture.level == 0 || b.snapshot || b.frame || b.rtt >= 0 || b.wrap_rows != 0) return;
        if ((b.width & (b.width - 1)) != 0 || (b.height & (b.height - 1)) != 0) return;
        const Tile& base = tiles[t0 & 7];
        TexDesc lv[7];
        int n = 0;
        for (int k = 1; k <= (int)texture.level && k <= 7; k++) {
            if ((b.width >> k) < 8 || (b.height >> k) < 8) break;
            const Tile& tk = tiles[(t0 + k) & 7];
            if (tk.fmt != base.fmt || tk.siz != base.siz || tk.palette != base.palette || tk.cms != base.cms || tk.cmt != base.cmt) break;
            if (shift_scale(tk.shifts) * (float)(1 << k) != shift_scale(base.shifts) ||
                shift_scale(tk.shiftt) * (float)(1 << k) != shift_scale(base.shiftt)) break;
            if (((tk.uls >> 2) << k) != (base.uls >> 2) || ((tk.ult >> 2) << k) != (base.ult >> 2)) break;
            TexDesc& d = lv[n];
            describe_tile((t0 + k) & 7, d);
            if (!d.valid || d.width != (b.width >> k) || d.height != (b.height >> k) || d.wrap_rows != 0) break;
            n++;
        }
        if (n == 0) return;
        b.levels = (uint8_t)(n + 1);
        b.mip_index = (uint32_t)out->mips.size();
        out->mips.insert(out->mips.end(), lv, lv + n);
    }

    // TEXEL1's coordinates relative to TEXEL0's (tile t0 and the next one).
    // tile_lerp: a triangle's, which takes the in-between frame's tile
    // scroll (tile_du/dv, else 0).
    void compute_uv1(float* uv1, int t0, bool tile_lerp = false) const {
        const Tile& a = tiles[t0 & 7];
        const Tile& b = tiles[(t0 + 1) & 7];
        float k_u = shift_scale(b.shifts) / shift_scale(a.shifts), k_v = shift_scale(b.shiftt) / shift_scale(a.shiftt);
        uv1[0] = k_u; uv1[1] = k_v;
        if (tile_lerp) {
            uv1[2] = ((float)(a.uls >> 2) + tile_du[0]) * k_u - ((float)(b.uls >> 2) + tile_du[1]);
            uv1[3] = ((float)(a.ult >> 2) + tile_dv[0]) * k_v - ((float)(b.ult >> 2) + tile_dv[1]);
            return;
        }
        uv1[2] = (float)(a.uls >> 2) * k_u - (float)(b.uls >> 2);
        uv1[3] = (float)(a.ult >> 2) * k_v - (float)(b.ult >> 2);
    }
    void set_uv1(DrawRecord& r, int t0, bool tile_lerp = false) const { compute_uv1(r.uv1, t0, tile_lerp); }

    void snapshot(DrawRecord& r) {
        r.cc_w0 = cc_w0; r.cc_w1 = cc_w1;
        r.othermode_h = othermode_h; r.othermode_l = othermode_l;
        r.geometry_mode = geometry_mode;
        memcpy(r.prim, prim, 4); memcpy(r.env, env, 4); memcpy(r.blend, blend, 4); memcpy(r.fog, fog, 4);
        r.prim_lod_frac = prim_lod_frac;
        effective_scissor(r.scissor, r.kind == DrawRecord::Tris && proj_stretch());
        r.proj_id = proj_id;
        r.rtt = (int8_t)cur_rtt;
        r.perspective = is_perspective(proj);
        r.depth_a = cur_depth_a; r.depth_b = cur_depth_b;
        r.mv_auto = mv_groups[mv_group_depth] == kIdAuto;
        r.mv_seq = mv_seq;
        r.dbg_vtx = last_vtx_addr;
        set_uv1(r, texture.tile, r.kind == DrawRecord::Tris && ip != nullptr);
        r.dbg_vp[0] = vp.scale[0]; r.dbg_vp[1] = vp.scale[1]; r.dbg_vp[2] = vp.trans[0]; r.dbg_vp[3] = vp.trans[1];
        r.dbg_proj[0] = proj.m[1][1]; r.dbg_proj[1] = proj.m[3][1]; r.dbg_proj[2] = proj.m[2][3]; r.dbg_proj[3] = proj.m[3][3];
    }

    bool uses_texel(int which) const {
        // Any combiner input referencing TEXEL0/1 in either cycle; a
        // mipmapped draw reads TEXEL1 as TEXEL0 (lod_trilerp).
        if (which == 1 && lod_trilerp(cc_w0, cc_w1, othermode_h)) return false;
        uint32_t a0 = (cc_w0 >> 20) & 0xF, c0 = (cc_w0 >> 15) & 0x1F, Aa0 = (cc_w0 >> 12) & 7, Ac0 = (cc_w0 >> 9) & 7;
        uint32_t a1 = (cc_w0 >> 5) & 0xF, c1 = cc_w0 & 0x1F, b0 = (cc_w1 >> 28) & 0xF, b1 = (cc_w1 >> 24) & 0xF;
        uint32_t Aa1 = (cc_w1 >> 21) & 7, Ac1 = (cc_w1 >> 18) & 7, d0 = (cc_w1 >> 15) & 7, Ab0 = (cc_w1 >> 12) & 7;
        uint32_t Ad0 = (cc_w1 >> 9) & 7, d1 = (cc_w1 >> 6) & 7, Ab1 = (cc_w1 >> 3) & 7, Ad1 = cc_w1 & 7;
        uint32_t t = which == 0 ? 1 : 2;
        uint32_t ct = which == 0 ? 8 : 9;   // TEXEL0_ALPHA / TEXEL1_ALPHA as a C input
        return a0 == t || b0 == t || c0 == t || d0 == t || a1 == t || b1 == t || c1 == t || d1 == t ||
               c0 == ct || c1 == ct ||
               Aa0 == t || Ab0 == t || Ac0 == t || Ad0 == t || Aa1 == t || Ab1 == t || Ac1 == t || Ad1 == t;
    }

    bool offscreen_target() const {
        // Draws into buffers the game keeps in patch RAM (frame copies for
        // effects) are not part of the visible frame; with render to texture,
        // every off-screen image is, and so is colour drawn into the depth
        // buffer's memory (Banjo stores the paused frame there): its copy
        // of the front buffer becomes a snapshot.
        if (rtt_enabled) return cur_rtt >= 0 || cimg_is_depth_memory();
        return (cimg >= 0x800000 && !is_presented_fb(cimg)) || cur_rtt >= 0;
    }

    void emit_tri(int i0, int i1, int i2) {
#ifdef DL_BENCH_SKIP_TRI
        return;
#endif
        if (cimg != plain_cimg || cur_rtt >= 0) note_target(false);
        if (skim) return;
        if (offscreen_target() && cur_rtt < 0) return;
        if (i0 >= kMaxVerts || i1 >= kMaxVerts || i2 >= kMaxVerts) return;
        if (ip != nullptr) interp_tiles();
        // RenderDesc::merge_image_tiles: the triangle samples a texture of its
        // tile's whole image when its coordinates stay inside the tile.
        bool img = false;
        if (image_tiles) {
            current_textures();
            if (img_ok) {
                img = tri_inside_image(i0, i1, i2);
                if (img) stats->image_tris++;
                else stats->image_fallbacks++;
            }
        }
        DrawRecord* r = nullptr;
        if (!out->draws.empty()) {
            DrawRecord& last = out->draws.back();
            // Reticle candidates (see set_split_untagged_ortho) end at a
            // modelview load.
            bool split = split_untagged_ortho && !last.perspective && last.mv_auto && last.mv_seq != mv_seq;
            if (inst_track && new_instance_pending()) split = true;
            // (A record taking deferred triangles has no vertices yet.)
            const bool tail = defer_open == (int32_t)out->draws.size() - 1 || last.first + last.count == out->verts.size();
            if (last.kind == DrawRecord::Tris && tail && !split) {
                if (last_draw_seq == state_seq && last_tri_img == img) {
                    r = &last;
                }
                else {
                    // State commands ran since the last triangle; if they left
                    // the same state (re-sent textures and modes are common)
                    // the record continues, sparing a draw call.
                    stats->probes++;
#ifdef DL_BENCH_NO_PROBE
                    if (false)
#endif
                    if (same_as_last(last, img ? img_tex : current_textures())) {
                        last_draw_seq = state_seq;
                        r = &last;
                        stats->draws_merged++;
                    }
                }
            }
        }
        if (r == nullptr) {
            if (!rect_slices.empty()) flush_rect_slices();   // the run of rectangles ends
            last_draw_seq = state_seq;
            const TexDesc* tex = img ? img_tex : current_textures();
            out->draws.emplace_back();
            r = &out->draws.back();
            r->kind = DrawRecord::Tris;
            r->first = (uint32_t)out->verts.size();
            r->count = 0;
            snapshot(*r);
            r->tex[0] = tex[0];
            r->tex[1] = tex[1];
            if (frame_copy) note_draw(*r);
            if (inst_track) start_instance(*r);
            // Deferred triangles: a record whose texel coordinates are the
            // vertices' own (not mapped onto an image, a frame, a snapshot
            // or a render texture) takes them all.
            if (defer_active && !img && !r->tex[0].snapshot && !r->tex[0].frame && r->tex[0].rtt < 0)
                defer_open = (int32_t)out->draws.size() - 1;
        }
        last_tri_img = img;
#ifdef DL_BENCH_NO_EMIT
        return;
#endif
        // Deferred (finish_triangles): the triangle as its three vertices.
        const bool deferred = defer_open == (int32_t)(r - out->draws.data());
        if (deferred && (ip == nullptr || !ip->recording)) {
            defer_tri(i0, i1, i2);
            return;
        }
        const LoadedVertex& a = slot(i0); const LoadedVertex& b = slot(i1); const LoadedVertex& c = slot(i2);
        if (ip != nullptr && ip->recording) {
            // Frame interpolation: the triangle as build_inbetween draws it
            // again (its texel coordinates as below, culled or not): its
            // vertices, and its state, shared with the triangles before it
            // while nothing it holds can have changed.
            const uint32_t draw = (uint32_t)(r - out->draws.data());
            const bool plain = !img && !r->tex[0].snapshot && !r->tex[0].frame && r->tex[0].rtt < 0;
            if (draw != tri_key_draw || state_seq != tri_key_state || tex_seq != tri_key_tex || tile_cur != tri_key_tile || plain != tri_key_plain) {
                tri_key_draw = draw; tri_key_state = state_seq; tri_key_tex = tex_seq; tri_key_tile = tile_cur; tri_key_plain = plain;
                Interpreter::Interp::TriState s;
                const Tile& tl = tiles[texture.tile & 7];
                s.draw = draw;
                s.first_tri = (uint32_t)ip->tris.size();
                s.uv_first = (uint32_t)ip->tri_uv.size();
                s.ss = shift_scale(tl.shifts); s.st = shift_scale(tl.shiftt);
                s.uls = (float)(tl.uls >> 2); s.ult = (float)(tl.ult >> 2);
                s.zprim = (othermode_l & G_ZS_PRIM) != 0 ? prim_z() : -1.0f;
                s.tile = tile_cur;
                s.cull = geometry_mode & (G_CULL_FRONT | G_CULL_BACK);
                s.plain = plain;
                ip->tri_states.push_back(s);
            }
            const Interpreter::Interp::TriState& s = ip->tri_states.back();
            ip->tris.push_back({ { slot_src[i0], slot_src[i1], slot_src[i2] }, (uint32_t)ip->tri_states.size() - 1 });
            if (!plain) {
                for (int k = 0; k < 3; k++) {
                    const auto& v = k == 0 ? a : (k == 1 ? b : c);
                    float u = v.u * s.ss - s.uls, w = v.v * s.st - s.ult;
                    if (img) { u += img_du; w += img_dv; }
                    else if (r->tex[0].snapshot) snapshot_uv(r->tex[0], u, w, u, w, proj_stretch());
                    else if (r->tex[0].frame) frame_uv(r->tex[0], u, w, u, w, proj_stretch() && cur_rtt < 0);
                    else if (r->tex[0].rtt >= 0) rtt_uv(r->tex[0], u, w);
                    ip->tri_uv.push_back(u); ip->tri_uv.push_back(w);
                }
            }
        }
        if (deferred) {
            defer_tri(i0, i1, i2);
            return;
        }
        const Tile& tl = tiles[texture.tile & 7];
        // (tile_du/dv: the in-between frame's tile scroll, else 0.)
        const float uls = (float)(tl.uls >> 2) + tile_du[0], ult = (float)(tl.ult >> 2) + tile_dv[0];
        const float ss = shift_scale(tl.shifts), st = shift_scale(tl.shiftt);
        const bool snap = r->tex[0].snapshot;
        output_tri(TriSink{ &out->verts, &r->count, &stats->tris, &stats->clipped, screen_w, screen_h },
                   a, b, c, geometry_mode & (G_CULL_FRONT | G_CULL_BACK), ss, st, uls, ult,
                   (othermode_l & G_ZS_PRIM) != 0 ? prim_z() : -1.0f, [&](float& u, float& v) {
            if (img) { u += img_du; v += img_dv; }
            else if (snap) snapshot_uv(r->tex[0], u, v, u, v, proj_stretch());
            else if (r->tex[0].frame) frame_uv(r->tex[0], u, v, u, v, proj_stretch() && cur_rtt < 0);
            else if (r->tex[0].rtt >= 0) rtt_uv(r->tex[0], u, v);
        });
    }

    // A deferred triangle: its three vertices, in the run of the record and
    // state it comes under (a new run where either changed).
    void defer_tri(int i0, int i1, int i2) {
        Interpreter::Defer& D = *dfr;
        if (D.ntris >= Interpreter::Defer::kTris) return;   // full: dropped
        const uint32_t draw = (uint32_t)defer_open;
        if (draw != defer_key_draw || state_seq != defer_key_state) {
            if (D.nruns >= Interpreter::Defer::kRuns) return;
            defer_key_draw = draw;
            defer_key_state = state_seq;
            const Tile& tl = tiles[texture.tile & 7];
            D.runs[D.nruns++] = { draw, D.ntris, shift_scale(tl.shifts), shift_scale(tl.shiftt),
                                  (float)(tl.uls >> 2) + tile_du[0], (float)(tl.ult >> 2) + tile_dv[0],
                                  (othermode_l & G_ZS_PRIM) != 0 ? prim_z() : -1.0f, geometry_mode & (G_CULL_FRONT | G_CULL_BACK), 0, 0 };
        }
        Interpreter::Defer::Tri& T = D.tris[D.ntris++];
        T.v[0] = dslot[i0]; T.v[1] = dslot[i1]; T.v[2] = dslot[i2];
        // Every so many, for drain on the other thread.
        if (D.ntris - D.last_pub >= Interpreter::Defer::kPublishEvery) D.publish();
    }

    // RenderDesc::merge_image_tiles: the triangle's texel coordinates (as
    // emit_tri computes them, relative to the tile) all inside the loaded
    // rectangle, where the image texture holds what the tile would sample.
    bool tri_inside_image(int i0, int i1, int i2) {
        const Tile& tl = tiles[texture.tile & 7];
        const float uls = (float)(tl.uls >> 2), ult = (float)(tl.ult >> 2);
        const float ss = shift_scale(tl.shifts), st = shift_scale(tl.shiftt);
        const int idx[3] = { i0, i1, i2 };
        for (int k = 0; k < 3; k++) {
            const LoadedVertex& lv = slot(idx[k]);
            const float u = lv.u * ss - uls, v = lv.v * st - ult;
            if (!(u >= 0.0f && u <= img_lim_u && v >= 0.0f && v <= img_lim_v)) return false;
        }
        return true;
    }

    // (The guard band and clipping: in_guard_band and clip_tri.)
    bool inside_guard_band(const Vtx3ds& v) const { return in_guard_band(v, screen_w, screen_h); }
    void emit_clipped(DrawRecord& r, const Vtx3ds* tri) {
        clip_tri(TriSink{ &out->verts, &r.count, &stats->tris, &stats->clipped, screen_w, screen_h }, tri);
    }

    // Texel coordinates relative to a tile that reads the stored frame ->
    // snapshot texture texels. The snapshot is the rotated 240x400 colour
    // buffer copied into a 256x512 texture: its u axis runs along screen y
    // (bottom row first), its v axis along screen x.
    // The stored frame covers the 4:3 centre of the screen for squeezed
    // draws and the whole wide screen for stretched ones (what RT64's frame
    // copies are), in N64 pixels scaled to the screen. The copy keeps the
    // colour buffer's memory order, row = screen x, and the PICA puts t = 0
    // at the LAST row (normal textures are stored flipped for that), so t
    // counts back from 512.
    void snapshot_uv(const TexDesc& d, float u, float v, float& su, float& sv, bool stretch) const {
        image_uv(snaps[d.snap_slot < kMaxSnapshots ? d.snap_slot : 0].base, d, u, v, su, sv, stretch);
    }
    // TexDesc::frame: the same layout, from the colour image being drawn
    // (the backend keeps each eye's frame in a 256x512 texture that way).
    void frame_uv(const TexDesc& d, float u, float v, float& su, float& sv, bool stretch) const {
        image_uv(screen_cimg, d, u, v, su, sv, stretch);
    }
    void image_uv(uint32_t base, const TexDesc& d, float u, float v, float& su, float& sv, bool stretch) const {
        uint32_t texel = (d.addr - base) / 2;
        float x0 = (float)(texel % (uint32_t)screen_w), y0 = (float)(texel / (uint32_t)screen_w);
        float nx = stretch ? (x0 + u) * stretch_k() - geo.x0 : x0 + u;   // N64 x on the wide screen
        float sx = geo.screen_x(nx);                                       // screen pixel, 0..400
        float sy = geo.screen_y(y0 + v);
        su = 240.0f - sy;
        sv = 512.0f - sx;
    }

    // RenderDesc::sample_drawn_frame bookkeeping for a new record: what was
    // drawn on screen, and the last draw that samples the frame.
    void note_draw(const DrawRecord& r) {
        if (r.rtt < 0) screen_drawn = true;
        if ((r.tex[0].valid && r.tex[0].frame) || (r.tex[1].valid && r.tex[1].frame)) {
            out->frame_sample_last = (int32_t)(out->draws.size() - 1);
            stats->frame_samples++;
        }
    }

    static bool same_tex(const TexDesc& a, const TexDesc& b) {
        if (a.valid != b.valid) return false;
        if (!a.valid) return true;
        return a.addr == b.addr && a.pitch == b.pitch && a.fmt == b.fmt && a.siz == b.siz && a.width == b.width && a.height == b.height &&
               a.masks == b.masks && a.maskt == b.maskt && a.cms == b.cms && a.cmt == b.cmt && a.palette == b.palette &&
               a.nibble == b.nibble && a.tlut_hash == b.tlut_hash && a.tlut_mode == b.tlut_mode && a.bilerp == b.bilerp && a.snapshot == b.snapshot && a.rtt == b.rtt &&
               a.block == b.block && a.dxt == b.dxt && a.load_word == b.load_word && a.frame == b.frame && a.wrap_rows == b.wrap_rows &&
               a.levels == b.levels;
    }
    static bool same_draw_state(const DrawRecord& a, const DrawRecord& b) {
        return a.cc_w0 == b.cc_w0 && a.cc_w1 == b.cc_w1 && a.othermode_h == b.othermode_h && a.othermode_l == b.othermode_l &&
               a.geometry_mode == b.geometry_mode && memcmp(a.prim, b.prim, 4) == 0 && memcmp(a.env, b.env, 4) == 0 &&
               memcmp(a.blend, b.blend, 4) == 0 && memcmp(a.fog, b.fog, 4) == 0 && a.prim_lod_frac == b.prim_lod_frac &&
               memcmp(a.scissor, b.scissor, sizeof(a.scissor)) == 0 && a.proj_id == b.proj_id && a.perspective == b.perspective && a.rtt == b.rtt &&
               memcmp(a.uv1, b.uv1, sizeof(a.uv1)) == 0 && a.depth_a == b.depth_a && a.depth_b == b.depth_b &&
               same_tex(a.tex[0], b.tex[0]) && same_tex(a.tex[1], b.tex[1]);
    }

    // Bitwise equality of small fixed-size fields, as memcmp has it, in
    // word loads (newlib's memcmp is a call and a loop on the console).
    template <size_t N>
    static bool same_bits(const void* a, const void* b) {
        static_assert(N % 4 == 0, "whole words");
        uint32_t x[N / 4], y[N / 4];
        memcpy(x, a, N);
        memcpy(y, b, N);
        uint32_t d = 0;
        for (size_t i = 0; i < N / 4; i++) d |= x[i] ^ y[i];
        return d == 0;
    }

    // The current state against a recorded draw (what same_draw_state
    // compares, without building a probe record).
    bool same_as_last(const DrawRecord& a, const TexDesc* tex) const {
        if (a.cc_w0 != cc_w0 || a.cc_w1 != cc_w1 || a.othermode_h != othermode_h || a.othermode_l != othermode_l ||
            a.geometry_mode != geometry_mode || a.prim_lod_frac != prim_lod_frac || a.proj_id != proj_id || a.rtt != cur_rtt) return false;
        if (a.mv_auto != (mv_groups[mv_group_depth] == kIdAuto)) return false;
        if (a.depth_a != cur_depth_a || a.depth_b != cur_depth_b) return false;
        if (!same_bits<4>(a.prim, prim) || !same_bits<4>(a.env, env) || !same_bits<4>(a.blend, blend) || !same_bits<4>(a.fog, fog)) return false;
        int16_t sc[4];
        effective_scissor(sc, proj_stretch());
        if (!same_bits<sizeof(sc)>(a.scissor, sc)) return false;
        if (a.perspective != is_perspective(proj)) return false;
        // uv1 places TEXEL1 alone (merge_image_tiles: the sky's column loads
        // move tile 0's origin, which uv1 follows, with no TEXEL1 in use).
        if (!(image_tiles && !a.tex[1].valid && !tex[1].valid)) {
            float uv1[4];
            compute_uv1(uv1, texture.tile, ip != nullptr);
            if (!same_bits<sizeof(uv1)>(a.uv1, uv1)) return false;
        }
        return same_tex(a.tex[0], tex[0]) && same_tex(a.tex[1], tex[1]);
    }

    static float shift_scale(int s) {
        if (s == 0) return 1.0f;
        if (s <= 10) return 1.0f / (float)(1 << s);
        return (float)(1 << (16 - s));
    }

    // Rectangles are emitted in N64 pixels with w = 1.
    void emit_rect(float ulx, float uly, float lrx, float lry, bool textured, int tile, float s, float t, float dsdx, float dtdy, bool flip) {
        emit_rect_aligned(ulx, uly, lrx, lry, textured, tile, s, t, dsdx, dtdy, flip, origin_shift(rect_lorigin), origin_shift(rect_rorigin), rect_align,
                          rect_lorigin == 0x800 && rect_rorigin == 0x800);
    }
    // The edges arrive as the command has them (plus the alignment offsets);
    // lshift/rshift then move each from its origin onto the wide screen.
    // plain_origins: neither edge is anchored to a screen edge (the only
    // rectangles RT64 stretches for covering the width).
    void emit_rect_aligned(float ulx, float uly, float lrx, float lry, bool textured, int tile, float s, float t, float dsdx, float dtdy, bool flip,
                           float lshift, float rshift, const float* align, bool plain_origins) {
        ulx += align[0]; uly += align[1];
        lrx += align[2]; lry += align[3];
        uint32_t cyc = (othermode_h >> 20) & 3;
        if (fix_rect_lr) {
            // RT64's rect.fixRectLR (rt64_rdp.cpp, drawRect): an edge within
            // one pixel of the scissor's, on a rectangle starting before it,
            // is extended to it. As there, judged before the origin moves the
            // rectangle and with fill and copy mode's extra pixel counted
            // (added below; RT64 has it in the coordinates by then).
            const float extra = (cyc == 2 || cyc == 3) ? 1.0f : 0.0f;
            const float sx = (float)scissor[2], sy = (float)scissor[3];
            if (std::fabs(sx - (lrx + extra)) <= 1.0f && ulx < sx) lrx = sx - extra;
            if (std::fabs(sy - (lry + extra)) <= 1.0f && uly < sy) lry = sy - extra;
        }
        ulx += lshift;
        lrx += rshift;
        note_target(!textured);
        if (skim) return;
        if (offscreen_target()) {
            // A copy of the front buffer into the storage: snapshot instead.
            if (textured) {
                TexDesc src;
                describe_tile(tile, src);
                // (A copy of the frame being drawn is drawn instead: frame_copy.)
                if (src.valid && !src.frame && is_vi_framebuffer(src.addr)) {
                    if (!out->snapshot_request) stats->snapshots++;
                    out->snapshot_request = true;
                    const int slot = snap_slot_for(cimg);
                    snaps[slot].valid = true;
                    snaps[slot].list = list_seq;
                    out->snapshot_store |= (uint8_t)(1u << slot);
                    tex_seq++;
                    return;
                }
            }
            if (cur_rtt < 0) return;
        }
        if (!textured && cimg_is_depth_memory()) return;   // depth clear: the target is cleared per frame
        state_seq++;
        out->draws.emplace_back();
        DrawRecord& r = out->draws.back();
        r.kind = textured ? DrawRecord::TexRect : DrawRecord::FillRect;
        r.first = (uint32_t)out->verts.size();
        r.count = 6;
        snapshot(r);
        int rect_load = -1;
        if (textured) {
            describe_tile(tile, r.tex[0]);
            rect_load = last_best;
            if (cyc == 2 && uses_texel(1)) describe_tile((tile + 1) & 7, r.tex[1]);
            set_uv1(r, tile);
        }
        else if (cyc == 3) {
            memcpy(r.prim, fill, 4);   // fill mode: the fill colour rides in prim for the backend
        }
        if (frame_copy) note_draw(r);
        // Fill and copy modes cover the lower-right pixel too.
        if (cyc == 2 || cyc == 3) { lrx += 1.0f; lry += 1.0f; }
        float w = lrx - ulx, h = lry - uly;
        // RenderDesc::aim_texture_hashes: a candidate reticle's texels are
        // hashed here, while RDRAM still holds them (nothing reads RDRAM
        // after "parsed"): a textured rectangle of 8..64 px, roughly square,
        // sampling ordinary texture memory.
        if (aim_hash && textured && r.tex[0].valid && !r.tex[0].snapshot && !r.tex[0].frame && r.tex[0].rtt < 0 &&
            w >= 8.0f && h >= 8.0f && w <= 64.0f && h <= 64.0f && w <= h * 1.6f && h <= w * 1.6f) {
            r.tex_hash = hash_texture(r.tex[0]);
        }
        const Tile& tl = tiles[tile & 7];
        float uls = (float)(tl.uls >> 2), ult = (float)(tl.ult >> 2);
        float u0 = s - uls, v0 = t - ult;
        float u1 = u0 + dsdx * w, v1 = v0 + dtdy * h;
        if (cyc == 2) u1 = u0 + dsdx * w / 4.0f;   // copy mode: dsdx is in 4-texel units
        // RenderDesc::merge_image_tiles: a strip of a wider image (Goemon's
        // flat backdrops, rows of the panorama) samples the image's texture,
        // so the strips' records can merge (compact_rects).
        // A rectangle reaching a few texels past the loaded ones (fix_rect_lr
        // extends Goemon's backdrop rows to the scissor) is drawn as two
        // quads: the loaded texels, and past them the edge texel the tile's
        // clamp repeats (sampled at its centre: the same with bilinear).
        float u_clamp = -1.0f;
        if (image_tiles && textured && !r.tex[1].valid) {
            TexDesc m;
            ImageMap map;
            const Tile& tlr = tiles[tile & 7];
            if (image_of(tile, r.tex[0], rect_load, m, map) && std::fmin(v0, v1) >= 0.0f && std::fmax(v0, v1) <= map.lim_v) {
                const bool u_in = std::fmin(u0, u1) >= 0.0f && std::fmax(u0, u1) <= map.lim_u;
                const bool u_over = !flip && u0 >= 0.0f && u0 < map.lim_u && u1 > map.lim_u && u1 <= map.lim_u + kClampOvershoot &&
                                    ((tlr.cms & 2) || tlr.masks == 0);
                if (u_in || u_over) {
                    r.tex[0] = m;
                    r.image_rect = true;
                    if (u_over) u_clamp = map.lim_u;
                    u0 += map.du; u1 += map.du;
                    v0 += map.dv; v1 += map.dv;
                    if (u_over) u_clamp += map.du;
                    stats->image_rects++;
                }
            }
        }
        Vtx3ds q[4];
        // RT64 spreads a rectangle over the whole wide screen when it reads a
        // frame copy, spans the full width with plain origins (full-screen
        // fills and tints), or is tagged STRETCH, unless it is tagged ADJUST.
        const bool stretch = ((textured && (r.tex[0].snapshot || r.tex[0].frame)) || (plain_origins && ulx <= 0.0f && lrx >= (float)screen_w) ||
                              rect_aspect == kAspectStretch) && rect_aspect != kAspectAdjust && cur_rtt < 0;
        const float ulx_n64 = ulx, lrx_n64 = lrx;     // before any stretch (merge_rect_slices)
        if (stretch) {
            ulx = stretch_x(ulx);
            lrx = stretch_x(lrx);
            effective_scissor(r.scissor, true);
        }
        float xs[4] = { ulx, lrx, lrx, ulx }, ys[4] = { uly, uly, lry, lry };
        float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
        if (flip) { us[1] = u0; vs[1] = v1; us[3] = u1; vs[3] = v0; }
        // The clamped part of an image rectangle (u_clamp >= 0): from where
        // the coordinate reaches the loaded texels' edge, a second quad.
        float x_split = lrx;
        if (u_clamp >= 0.0f) {
            x_split = ulx + (lrx - ulx) * (u_clamp - u0) / (u1 - u0);
            xs[1] = xs[2] = x_split;
            us[1] = us[2] = u_clamp;
        }
        for (int i = 0; i < 4; i++) {
            q[i].x = xs[i]; q[i].y = ys[i]; q[i].z = (othermode_l & G_ZS_PRIM) ? prim_z() : 0.0f; q[i].w = 1.0f;
            q[i].u = us[i]; q[i].v = vs[i];
            if (r.tex[0].snapshot) snapshot_uv(r.tex[0], q[i].u, q[i].v, q[i].u, q[i].v, stretch);
            else if (r.tex[0].frame) frame_uv(r.tex[0], q[i].u, q[i].v, q[i].u, q[i].v, stretch);
            else if (r.tex[0].rtt >= 0) rtt_uv(r.tex[0], q[i].u, q[i].v);
            q[i].r = q[i].g = q[i].b = q[i].a = 0;   // rectangles have no shade: SHADE reads as 0 (as RT64)
        }
        Vtx3ds* o = out->verts.append(u_clamp >= 0.0f ? 12 : 6);
        if (o == nullptr) { out->draws.pop_back(); return; }
        o[0] = q[0]; o[1] = q[1]; o[2] = q[2];
        o[3] = q[0]; o[4] = q[2]; o[5] = q[3];
        if (u_clamp >= 0.0f) {
            Vtx3ds c[4] = { q[1], q[1], q[2], q[2] };
            c[0].x = c[3].x = x_split;
            c[1].x = c[2].x = lrx;
            for (Vtx3ds& v : c) v.u = u_clamp - 0.5f;     // the edge texel's centre
            o[6] = c[0]; o[7] = c[1]; o[8] = c[2];
            o[9] = c[0]; o[10] = c[2]; o[11] = c[3];
            r.count = 12;
        }
        stats->rects++;
        inst_break = true;          // the next triangles are another projection instance
        // Fill-mode rectangles take no part in the merged detection (as in
        // the desktop fork), nor do ones drawn 1:1 into an image or ADJUST.
        if (merge_rect_slices && plain_origins && cyc != 3 && cur_rtt < 0 && rect_aspect != kAspectAdjust) {
            rect_slices.push_back({ (uint32_t)(out->draws.size() - 1), ulx_n64, lrx_n64, uly, lry, stretch });
        }
    }

    // RenderDesc::merge_rect_slices: the rectangles of the current run of
    // rectangles, in N64 units before any stretch. The desktop fork collects
    // them per rectangle projection (rectangles between two triangle draws
    // of one framebuffer pair), keyed by their rows; a row whose rectangles
    // reach both edges of the frame between them is stretched like one
    // rectangle covering the width.
    struct RectSlice { uint32_t draw; float ulx, lrx, uly, lry; bool stretched; };
    std::vector<RectSlice> rect_slices;
    void flush_rect_slices() {
        const size_t n = rect_slices.size();
        if (n >= 2) {
            // Rows together (a run of glyphs can be long: no pairwise scan).
            std::sort(rect_slices.begin(), rect_slices.end(), [](const RectSlice& a, const RectSlice& b) {
                return a.uly != b.uly ? a.uly < b.uly : a.lry != b.lry ? a.lry < b.lry : a.draw < b.draw;
            });
            for (size_t i = 0; i < n;) {
                size_t j = i;
                float lo = rect_slices[i].ulx, hi = rect_slices[i].lrx;
                for (; j < n && rect_slices[j].uly == rect_slices[i].uly && rect_slices[j].lry == rect_slices[i].lry; j++) {
                    lo = std::fmin(lo, rect_slices[j].ulx);
                    hi = std::fmax(hi, rect_slices[j].lrx);
                }
                if (lo <= 0.0f && hi >= (float)screen_w) {
                    for (size_t k = i; k < j; k++) {
                        RectSlice& b = rect_slices[k];
                        if (b.stretched) continue;
                        DrawRecord& r = out->draws[b.draw];
                        for (uint32_t v = r.first; v < r.first + r.count; v++) out->verts[v].x = stretch_x(out->verts[v].x);
                        if (r.scissor[0] >= 0 && r.scissor[2] <= screen_w) {   // as effective_scissor
                            r.scissor[0] = (int16_t)stretch_x((float)r.scissor[0]);
                            r.scissor[2] = (int16_t)stretch_x((float)r.scissor[2]);
                        }
                        b.stretched = true;
                        stats->rects_merged++;
                    }
                }
                i = j;
            }
        }
        rect_slices.clear();
    }

    void set_othermode(bool low, uint32_t w0, uint32_t w1) {
        int len = (w0 & 0xFF) + 1;
        int shift = 32 - ((w0 >> 8) & 0xFF) - len;
        uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
        if (low) othermode_l = (othermode_l & ~mask) | (w1 & mask);
        else othermode_h = (othermode_h & ~mask) | (w1 & mask);
    }

    // ---- extended commands. Returns how many extra 8-byte commands were consumed.
    int extended(uint32_t sub, uint32_t w1, uint32_t pc) {
        switch (sub) {
            case EX_SETRDRAMEXTENDED: extended_rdram = (w1 & 1) != 0; return 0;
            case EX_SETRECTASPECT: rect_aspect = w1 & 3; return 0;
            case EX_SETREFRESHRATE: stats->refresh_rate = w1 & 0xFFFF; return 0;
            case EX_SETNEARCLIPPING: case EX_SETTEXCOORDWRAPPOINT:
            case EX_FORCEUPSCALE2D: case EX_FORCETRUEBILERP: case EX_FORCESCALELOD: case EX_FORCEBRANCH:
            case EX_SETRENDERTORAM: case EX_SETDITHERNOISESTRENGTH: case EX_VERTEXZTEST: case EX_ENDVERTEXZTEST:
                return 0;
            case EX_MATRIXGROUP: {
                uint32_t id = w1;
                uint32_t f = r32(pc + 8);
                bool push = f & 1, is_proj = (f >> 1) & 1;
                uint32_t* stack = is_proj ? proj_groups : mv_groups;
                int& depth = is_proj ? proj_group_depth : mv_group_depth;
                if (push && depth + 1 < kGroupStack) depth++;
                stack[depth] = id;
                if (is_proj) {
                    proj_id = id;
                    proj_aspects[depth] = (f >> 20) & 3;
                }
                if (ip != nullptr) {
                    (is_proj ? proj_tg : mv_tg)[depth] = interp::Group::decode(id, f);
                    (is_proj ? proj_tg_idx : mv_tg_idx) = -1;
                }
                return 1;
            }
            case EX_POPMATRIXGROUP: {
                int count = w1 & 0xFF;
                bool is_proj = (w1 >> 8) & 1;      // gEXPopMatrixGroup's proj, beside the count
                int& depth = is_proj ? proj_group_depth : mv_group_depth;
                depth = depth > count ? depth - count : 0;
                if (is_proj) proj_id = proj_groups[proj_group_depth];
                if (ip != nullptr) (is_proj ? proj_tg_idx : mv_tg_idx) = -1;
                return 0;
            }
            case EX_EDITGROUPBYADDRESS:
                if (ip != nullptr && ip->recording) interp_edit_group(seg(w1), r32(pc + 8));
                return 1;
            case EX_SETRECTALIGN: {
                rect_lorigin = w1 & 0xFFF; rect_rorigin = (w1 >> 12) & 0xFFF;
                uint32_t a = r32(pc + 8), b = r32(pc + 12);
                // The offsets are in the rectangle's own units, quarter pixels
                // (RT64 adds them to the 10.2 coordinates; Dinosaur Planet's
                // front end centres its 640-wide backgrounds with -/+ 320 * 4).
                rect_align[0] = (float)(int16_t)(a >> 16) * 0.25f; rect_align[1] = (float)(int16_t)(a & 0xFFFF) * 0.25f;
                rect_align[2] = (float)(int16_t)(b >> 16) * 0.25f; rect_align[3] = (float)(int16_t)(b & 0xFFFF) * 0.25f;
                return 1;
            }
            case EX_SETVIEWPORTALIGN: {
                // Takes effect at the next viewport load, as in RT64: DK64's
                // rain sets it without reloading, and spreads its drops for
                // the viewport it already has.
                vp_origin = w1 & 0xFFF;
                uint32_t a = r32(pc + 8);
                vp_align_x = (int16_t)(a >> 16); vp_align_y = (int16_t)(a & 0xFFFF);
                return 1;
            }
            case EX_SETSCISSOR: {
                // x edges are relative to an origin each (left/centre/right
                // of the N64 screen, or absolute).
                uint32_t lorigin = (w1 >> 2) & 0xFFF, rorigin = (w1 >> 14) & 0xFFF;
                uint32_t a = r32(pc + 8), b = r32(pc + 12);
                scissor[0] = (int16_t)lroundf((float)((int16_t)(a >> 16) / 4) + origin_x(lorigin)); scissor[1] = (int16_t)(a & 0xFFFF) / 4;
                scissor[2] = (int16_t)lroundf((float)((int16_t)(b >> 16) / 4) + origin_x(rorigin)); scissor[3] = (int16_t)(b & 0xFFFF) / 4;
                return 1;
            }
            case EX_SETSCISSORALIGN: return 2;
            case EX_SETVIEWPORT: {
                uint32_t addr = seg(r32(pc + 12));
                load_viewport(addr);
                return 1;
            }
            case EX_PUSHVIEWPORT:
                if (vp_depth < 8) { vp_stack[vp_depth++] = vp; }
                return 0;
            case EX_POPVIEWPORT:
                if (vp_depth > 0) { vp = vp_stack[--vp_depth]; inst_dirty = true; }
                return 0;
            case EX_PUSHSCISSOR:
                if (scissor_depth < 8) { memcpy(scissor_stack[scissor_depth++], scissor, sizeof(scissor)); }
                return 0;
            case EX_POPSCISSOR:
                if (scissor_depth > 0) { memcpy(scissor, scissor_stack[--scissor_depth], sizeof(scissor)); }
                return 0;
            case EX_TEXRECT: {
                int tile = w1 & 7;
                bool flip = (w1 >> 7) & 1;
                uint32_t lorigin = (w1 >> 3) & 0xFFF, rorigin = (w1 >> 15) & 0xFFF;
                uint32_t a = r32(pc + 8), b = r32(pc + 12), c = r32(pc + 16), d = r32(pc + 20);
                float ulx = (int16_t)(a >> 16) / 4.0f, uly = (int16_t)(a & 0xFFFF) / 4.0f;
                float lrx = (int16_t)(b >> 16) / 4.0f, lry = (int16_t)(b & 0xFFFF) / 4.0f;
                float s = (int16_t)(c >> 16) / 32.0f, t = (int16_t)(c & 0xFFFF) / 32.0f;
                float dsdx = (int16_t)(d >> 16) / 1024.0f, dtdy = (int16_t)(d & 0xFFFF) / 1024.0f;
                static const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                emit_rect_aligned(ulx, uly, lrx, lry, true, tile, s, t, dsdx, dtdy, flip, origin_x(lorigin), origin_x(rorigin), none,
                                  lorigin == 0x800 && rorigin == 0x800);
                return 2;
            }
            case EX_FILLRECT: {
                uint32_t lorigin = w1 & 0xFFF, rorigin = (w1 >> 12) & 0xFFF;
                uint32_t a = r32(pc + 8), b = r32(pc + 12);
                float ulx = (int16_t)(a >> 16) / 4.0f, uly = (int16_t)(a & 0xFFFF) / 4.0f;
                float lrx = (int16_t)(b >> 16) / 4.0f, lry = (int16_t)(b & 0xFFFF) / 4.0f;
                static const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                emit_rect_aligned(ulx, uly, lrx, lry, false, 0, 0, 0, 0, 0, false, origin_x(lorigin), origin_x(rorigin), none,
                                  lorigin == 0x800 && rorigin == 0x800);
                return 1;
            }
            case EX_VERTEX: return 1;
            case EX_MATRIX_FLOAT:
                mtx_addr = seg(r32(pc + 12));
                matrix_float(mtx_addr, w1);
                return 1;
            case EX_SETVERTEXSEGMENT: return 1;
            // State stacks, as RT64 keeps them: a push saves, a pop restores.
            case EX_PUSHOTHERMODE:
                if (othermode_depth < kStateStack) othermode_stack[othermode_depth++] = { othermode_h, othermode_l };
                return 0;
            case EX_POPOTHERMODE:
                if (othermode_depth > 0) { othermode_depth--; othermode_h = othermode_stack[othermode_depth].h; othermode_l = othermode_stack[othermode_depth].l; tex_seq++; }
                return 0;
            case EX_PUSHCOMBINE:
                if (combine_depth < kStateStack) combine_stack[combine_depth++] = { cc_w0, cc_w1 };
                return 0;
            case EX_POPCOMBINE:
                if (combine_depth > 0) { combine_depth--; cc_w0 = combine_stack[combine_depth].w0; cc_w1 = combine_stack[combine_depth].w1; tex_seq++; }
                return 0;
            case EX_PUSHGEOMETRYMODE:
                if (geometry_depth < kStateStack) geometry_stack[geometry_depth++] = geometry_mode;
                return 0;
            case EX_POPGEOMETRYMODE:
                if (geometry_depth > 0) { geometry_mode = geometry_stack[--geometry_depth]; ldir_dirty = true; }
                return 0;
            case EX_PUSHPROJMATRIX:
                if (proj_depth < kStateStack) {
                    if (ip != nullptr) { pv_view_stack[proj_depth] = pv_view; pv_proj_stack[proj_depth] = pv_proj; proj_addr_stack[proj_depth] = proj_addr; }
                    proj_stack[proj_depth++] = proj;
                }
                return 0;
            case EX_POPPROJMATRIX:
                if (proj_depth > 0) {
                    proj = proj_stack[--proj_depth]; mvp_dirty = true; inst_dirty = true;
                    update_depth_terms();
                    if (ip != nullptr) { pv_view = pv_view_stack[proj_depth]; pv_proj = pv_proj_stack[proj_depth]; proj_addr = proj_addr_stack[proj_depth]; proj_seq++; }
                }
                return 0;
            case EX_PUSHENVCOLOR: case EX_PUSHBLENDCOLOR: case EX_PUSHFOGCOLOR: case EX_PUSHFILLCOLOR: case EX_PUSHPRIMCOLOR:
            case EX_POPENVCOLOR: case EX_POPBLENDCOLOR: case EX_POPFOGCOLOR: case EX_POPFILLCOLOR: case EX_POPPRIMCOLOR: {
                const bool push = sub == EX_PUSHENVCOLOR || sub == EX_PUSHBLENDCOLOR || sub == EX_PUSHFOGCOLOR ||
                                  sub == EX_PUSHFILLCOLOR || sub == EX_PUSHPRIMCOLOR;
                const int which = sub == EX_PUSHENVCOLOR || sub == EX_POPENVCOLOR ? 0 :
                                  sub == EX_PUSHBLENDCOLOR || sub == EX_POPBLENDCOLOR ? 1 :
                                  sub == EX_PUSHFOGCOLOR || sub == EX_POPFOGCOLOR ? 2 :
                                  sub == EX_PUSHFILLCOLOR || sub == EX_POPFILLCOLOR ? 3 : 4;
                uint8_t* colors[5] = { env, blend, fog, fill, prim };
                int& d = color_depth[which];
                if (push) { if (d < kStateStack) memcpy(color_stack[which][d++], colors[which], 4); }
                else if (d > 0) memcpy(colors[which], color_stack[which][--d], 4);
                return 0;
            }
            // RT64 keeps these for its own interpolation and stereo; the 3DS
            // shifts the eyes in clip space and needs neither.
            case EX_SETPROJMATRIXFLOAT: case EX_SETVIEWMATRIXFLOAT:
                return 0;
            default:
                stats->ex_unknown++;
                return 0;
        }
    }

    // gEX origins: 0 left, 0x200 centre, 0x400 right, 0x800 none. A
    // coordinate is relative to the origin's N64 position (0, w/2, w), and
    // the origin itself sits on the matching edge of the wide screen, whose
    // N64 x range is -x0..w+x0 (-40..360 for DK64's 320); "none" keeps the
    // 4:3 placement.
    float origin_x(uint32_t origin) const {
        switch (origin) { case 0x000: return -geo.x0; case 0x200: return (float)screen_w * 0.5f; case 0x400: return (float)screen_w + geo.x0; default: return 0.0f; }
    }
    float origin_shift(uint32_t origin) const {
        switch (origin) { case 0x000: return -geo.x0; case 0x400: return geo.x0; default: return 0.0f; }
    }

    float raw_vtrans_x = 640.0f;   // quarter pixels, as loaded
    void load_viewport(uint32_t addr) {
        inst_dirty = true;
        vp.scale[0] = r16(addr + 0) / 4.0f;
        vp.scale[1] = r16(addr + 2) / 4.0f;
        vp.scale[2] = r16(addr + 4);
        raw_vtrans_x = (float)r16(addr + 8);
        vp.trans[1] = r16(addr + 10) / 4.0f;
        vp.trans[2] = r16(addr + 12);
        apply_viewport_align();
    }
    void apply_viewport_align() {
        // gEXSetViewportAlign (the last one before this viewport was loaded):
        // the translate is re-based on an origin and
        // shifted by an offset (quarter pixels); the origin then sits on the
        // wide screen's edge.
        float x = raw_vtrans_x;
        if (vp_origin < 0x800) x += (float)vp_origin * (float)screen_w * 4.0f / 1024.0f;
        vp.trans[0] = (x + (float)vp_align_x) / 4.0f + (float)origin_shift(vp_origin);
        vp.trans_stretch = (x + (float)vp_align_x) / 4.0f;
    }

    // Widescreen, as RT64 does it: its native mapping spreads the N64's
    // 0..w across the whole wide screen and ADJUST squeezes content back to
    // 4:3; here the default is the squeezed placement (0..w centred, the
    // screen spanning -x0..w+x0) and STRETCH content is spread out. Origins
    // only anchor squeezed content, so stretched content ignores them.
    float stretch_k() const { return geo.wide_w / (float)screen_w; }
    float stretch_x(float x) const { return x * stretch_k() - geo.x0; }
    static constexpr uint8_t kAspectStretch = 1, kAspectAdjust = 2;
    bool proj_stretch() const { return proj_aspects[proj_group_depth] == kAspectStretch; }
    // A scissor still in 4:3 space goes wide with stretched content; one set
    // through origins already is.
    void effective_scissor(int16_t* out, bool stretch) const {
        memcpy(out, scissor, sizeof(scissor));
        if (stretch && scissor[0] >= 0 && scissor[2] <= screen_w) {
            out[0] = (int16_t)stretch_x((float)scissor[0]);
            out[2] = (int16_t)stretch_x((float)scissor[2]);
        }
    }

    // A matrix command's effect; p holds F3DEX2's flags with the push bit as
    // meant (4 projection, 2 load, 1 push).
    void apply_matrix(const Mtx& m, uint8_t p) {
        if (p & 4) {
            if (p & 2) proj = m; else mul(proj, m, proj);
            update_depth_terms();
            if (ip != nullptr) interp_proj_matrix(m, p);
            inst_dirty = true;
            if (is_perspective(proj)) {
                bool world = is_world_proj(proj_id);
                const Mtx cam = depth_camera(proj);
                if (!out->has_cam || (world && !out->cam_is_world)) {
                    out->has_cam = true;
                    out->cam_is_world = world;
                    memcpy(out->cam, &cam, sizeof(out->cam));
                }
                if (hud_camera_id != 0 && proj_id == hud_camera_id && !out->has_hud_cam) {
                    out->has_hud_cam = true;
                    memcpy(out->hud_cam, &cam, sizeof(out->hud_cam));
                }
            }
        }
        else {
            mv_seq++;
            if (p & 1) {
                if (mv_depth + 1 < kMaxMtxStack) { mv_stack[mv_depth + 1] = mv_stack[mv_depth]; mv_depth++; }
            }
            if (p & 2) mv_stack[mv_depth] = m; else mul(mv_stack[mv_depth], m, mv_stack[mv_depth]);
            mv_addr[mv_depth] = mtx_addr;
            ldir_dirty = true;
        }
        mvp_dirty = true;
    }

    // gEXMatrixFloat: a matrix of 16 floats, row by row, with the game's own
    // G_MTX flags (not inverted, in its GBI's encoding).
    void matrix_float(uint32_t addr, uint32_t flags) {
        Mtx m;
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t bits = r32(addr + (uint32_t)(i * 4 + j) * 4);
                memcpy(&m.m[i][j], &bits, 4);
            }
        }
        uint8_t p = flags & 0xFF;
        if (gbi == Gbi::F3DEX) p = (uint8_t)(((p & 1) ? 4 : 0) | (p & 2) | ((p & 4) ? 1 : 0));   // GBI1: projection 1, push 4
        apply_matrix(m, p);
    }

    // ---- F3DEX 1.x
    // GBI1 geometry-mode bits -> F3DEX2's (only the shading and culling bits move).
    static uint32_t f3dex_geometry_bits(uint32_t g) {
        uint32_t out = g & ~(F3DEX_G_SHADING_SMOOTH | F3DEX_G_CULL_FRONT | F3DEX_G_CULL_BACK);
        if (g & F3DEX_G_SHADING_SMOOTH) out |= G_SHADING_SMOOTH;
        if (g & F3DEX_G_CULL_FRONT) out |= G_CULL_FRONT;
        if (g & F3DEX_G_CULL_BACK) out |= G_CULL_BACK;
        return out;
    }

    // ---- main loop: one dispatch for both GBIs. F3DEX commands get numbers
    // of their own (kF3dexOp), and each one's case rewrites the command into
    // F3DEX2's form and jumps to that command's case (the RDP commands are
    // shared).
    // Interpreter::set_profile: each command's time goes to its kind when
    // the next one starts (and at the end of the run).
    struct ProfScope {
        uint64_t* prof = nullptr;
        uint64_t t = 0;
        int kind = InterpreterStats::kProfOther;
        void next(int k) {
            const uint64_t now = prof_tick();
            prof[kind] += now - t;
            t = now;
            kind = k;
        }
        ~ProfScope() { if (prof != nullptr) prof[kind] += prof_tick() - t; }
    };
    static int prof_kind(uint8_t op) {
        switch (op) {
            case G_VTX: case kF3dexLow | F3DEX_G_VTX: return InterpreterStats::kProfVtx;
            case G_TRI1: case G_TRI2: case G_QUAD: case F3DEX_G_TRI1: case F3DEX_G_TRI2: case F3DEX_G_QUAD: return InterpreterStats::kProfTri;
            case G_MTX: case G_POPMTX: case kF3dexLow | F3DEX_G_MTX: case F3DEX_G_POPMTX: return InterpreterStats::kProfMtx;
            case G_SETTIMG: case G_LOADBLOCK: case G_LOADTILE: case G_LOADTLUT: case G_SETTILE: case G_SETTILESIZE: case G_TEXTURE:
                return InterpreterStats::kProfTex;
            case G_TEXRECT: case G_TEXRECTFLIP: case G_FILLRECT: return InterpreterStats::kProfRect;
            case G_DL: case G_ENDDL: case G_BRANCH_Z: case G_CULLDL:
            case kF3dexLow | F3DEX_G_DL: case F3DEX_G_ENDDL: case F3DEX_G_BRANCH_Z: case F3DEX_G_CULLDL: return InterpreterStats::kProfFlow;
            case G_EXTENDED: case G_SPNOOP: return InterpreterStats::kProfEx;
            default: return InterpreterStats::kProfOther;
        }
    }
    bool profile = false;
    uint32_t dbg_lists_ = 0, dbg_unknown_ = 0;   // the bring-up logs in run()

    bool run(uint32_t data_ptr) {
        tex_seq++;   // FrameRecord::tlut restarts: cached palette indices are stale
        uint32_t stack[kMaxDlDepth];
        int depth = 0;
        uint32_t pc = seg(data_ptr);
        bool fullsync = false;
        // The first lists, for a port's bring-up: where the list is and how
        // it begins (a wrong pointer or byte order reads as unknown opcodes).
        if (dbg_lists_ < 3) {
            dbg_lists_++;
            fprintf(stderr, "rt64-3ds: list %u at %08x (pc %06x): %08x %08x  %08x %08x  %08x %08x  %08x %08x\n", dbg_lists_, data_ptr, pc,
                    r32(pc), r32(pc + 4), r32(pc + 8), r32(pc + 12), r32(pc + 16), r32(pc + 20), r32(pc + 24), r32(pc + 28));
        }
        ProfScope prof;
        if (profile) { prof.prof = stats->prof; prof.t = prof_tick(); }
        for (uint32_t guard = 0; guard < 400000; guard++) {
            uint32_t w0 = r32(pc), w1 = r32(pc + 4);
            uint8_t op = op_table[w0 >> 24];
            stats->commands++;
            stats->op_hist[w0 >> 24]++;
            if (prof.prof != nullptr) prof.next(prof_kind(op));
#ifndef __3DS__
            // Host tools: DL_TRACE=<first draw>,<last draw> prints the commands
            // that run while the frame has that many draws.
            static int trace_lo = -2, trace_hi = -1;
            if (trace_lo == -2) { trace_lo = -1; if (const char* e = getenv("DL_TRACE")) sscanf(e, "%d,%d", &trace_lo, &trace_hi); }
            if (trace_lo >= 0 && (int)out->draws.size() >= trace_lo && (int)out->draws.size() <= trace_hi) {
                fprintf(stderr, "  [%zu] %08x: %02x %08x %08x\n", out->draws.size(), pc, op, w0, w1);
                if (op == G_VTX) {
                    int n = (w0 >> 12) & 0xFF, v0 = ((w0 >> 1) & 0x7F) - n;
                    fprintf(stderr, "        vtx %d..%d from %06x (mv depth %d)\n", v0, v0 + n - 1, seg(w1), mv_depth);
                    for (int i = 0; i < n && i < 32; i++) {
                        const uint8_t* src = rdram + ((seg(w1) + i * 16) & 0xFFFFFC);
                        fprintf(stderr, "          v%-2d ob %6d %6d %6d\n", v0 + i, *(const int16_t*)(src + 2), *(const int16_t*)(src + 0), *(const int16_t*)(src + 6));
                    }
                }
                if (op == G_MTX) {
                    Mtx m; load_mtx(m, seg(w1));
                    fprintf(stderr, "        mtx %s %s %s from %06x:", ((w0 ^ 1) & 4) ? "proj" : "mv", ((w0 ^ 1) & 2) ? "load" : "mul", ((w0 ^ 1) & 1) ? "push" : "nopush", seg(w1));
                    for (int r = 0; r < 4; r++) fprintf(stderr, " [%.3g %.3g %.3g %.3g]", m.m[r][0], m.m[r][1], m.m[r][2], m.m[r][3]);
                    fprintf(stderr, "\n");
                }
            }
#endif
            uint32_t next = pc + 8;
            // Anything but drawing, vertex loads and list flow may change the
            // state a draw snapshots (rects bump it themselves).
            state_seq += kStateOp[op];
            tex_seq += kTexOpTable.v[op];
            switch (op) {
                case G_NOOP: case G_RDPNOOP: break;
                case G_SPNOOP: op_spnoop:
                    if ((w0 & 0xFFFFFF) == kHookMagic) {
                        uint32_t hop = w1 >> 28;
                        if (hop == 1) ex_enabled = true;
                        else if (hop == 2) ex_enabled = false;
                        else if (hop == 3) {   // DL
                            if (depth < kMaxDlDepth) stack[depth++] = next;
                            next = seg(w1 & 0x0FFFFFFF);
                        }
                        else if (hop == 4) next = seg(w1 & 0x0FFFFFFF);
                    }
                    break;
                case G_EXTENDED: {
                    int extra = extended(w0 & 0xFFFFFF, w1, pc);
                    next += extra * 8;
                    break;
                }
                case G_VTX: {
                    int n = (w0 >> 12) & 0xFF;
                    int v0 = ((w0 >> 1) & 0x7F) - n;
                    load_vertices(seg(w1), n, v0);
                    break;
                }
                case G_MODIFYVTX: op_modifyvtx: {
                    int where = (w0 >> 16) & 0xFF;
                    int v = (w0 & 0xFFFF) / 2;
                    if (v < kMaxVerts) {
                        if (where == 0x10 && ip != nullptr && ip->recording && slot_src[v] < ip->lv.size()) {
                            // The vertex as now coloured, for the triangles that follow.
                            Interpreter::Interp::LoadedVtx q = ip->lv[slot_src[v]];
                            q.rgba[0] = w1 >> 24; q.rgba[1] = w1 >> 16; q.rgba[2] = w1 >> 8; q.rgba[3] = w1;
                            q.fog = false;
                            slot_src[v] = (uint32_t)ip->lv.size();
                            ip->lv.push_back(q);
                        }
                        // Deferred triangles: the triangles before keep the vertex as it was.
                        if (defer_active && (where == 0x10 || where == 0x14)) {
                            const LoadedVertex was = slot(v);
                            new_slot(v) = was;
                        }
                        LoadedVertex& mv = slot(v);
                        if (where == 0x10) { mv.r = w1 >> 24; mv.g = w1 >> 16; mv.b = w1 >> 8; mv.a = w1; }
                        else if (where == 0x14) {
                            // The vertex buffer holds coordinates already
                            // scaled by G_TEXTURE: the new ones go in as given.
                            mv.u = (float)(int16_t)(w1 >> 16) / 32.0f;
                            mv.v = (float)(int16_t)(w1 & 0xFFFF) / 32.0f;
                            if (ip != nullptr && ip->recording && slot_src[v] < ip->lv.size()) {
                                // ... and for the triangles that follow.
                                Interpreter::Interp::LoadedVtx q = ip->lv[slot_src[v]];
                                q.u = mv.u; q.v = mv.v;
                                slot_src[v] = (uint32_t)ip->lv.size();
                                ip->lv.push_back(q);
                            }
                        }
                    }
                    break;
                }
                case G_CULLDL: stats->cull_dl++; break;
                case G_BRANCH_Z: op_branch_z: {
                    // gSPBranchLessZ, the games' level of detail: jump to the
                    // list in RDPHALF_1 when the vertex's screen depth (the
                    // RSP's z/w * vscale + vtrans, 0..1023) is below w1/65536.
                    // The near model sits behind the branch; what follows is
                    // the far one, or nothing for objects that vanish. The
                    // recomp frontends force the branch (RT64's forceBranch,
                    // or gEXForceBranch): always the near model.
                    int vi = (w0 >> 1) & 0x7FF;
                    if (vi < kMaxVerts) {
                        // (Deferred vertices: its depth alone.)
                        float vz, vw;
                        if (defer_active) dfr->depth(dslot[vi], vz, vw);
                        else { vz = vtx[vi].z; vw = vtx[vi].w; }
                        float zw = 2.0f * vz / (vw != 0.0f ? vw : 1e-6f) - 1.0f;
                        if (force_branch_z || zw * vp.scale[2] + vp.trans[2] < (float)w1 / 65536.0f) {
                            next = seg(rdphalf1);
                            stats->branch_z_taken++;
                        }
                        else stats->branch_z_not++;
                    }
                    break;
                }
                case G_TRI1:
                    emit_tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                    break;
                case G_TRI2: case G_QUAD:
                    emit_tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                    emit_tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                    break;
                case G_DMA_IO: case G_LOAD_UCODE: break;
                case G_TEXTURE: op_texture:
                    texture.level = (w0 >> 11) & 7;
                    texture.tile = (w0 >> 8) & 7;
                    texture.on = (w0 & 0xFF) != 0;
                    texture.scaleS = w1 >> 16;
                    texture.scaleT = w1 & 0xFFFF;
                    break;
                case G_POPMTX: op_popmtx: {
                    int n = (int)(w1 / 64);
                    mv_depth -= n;
                    if (mv_depth < 0) mv_depth = 0;
                    mvp_dirty = true;
                    ldir_dirty = true;
                    break;
                }
                case G_GEOMETRYMODE:
                    geometry_mode = (geometry_mode & (w0 & 0xFFFFFF)) | w1;
                    break;
                case G_MTX: op_mtx: {
                    Mtx m;
                    mtx_addr = seg(w1);
                    load_mtx(m, mtx_addr);
                    apply_matrix(m, (w0 & 0xFF) ^ 1);   // F3DEX2 inverts the push bit
                    break;
                }
                case G_MOVEWORD: op_moveword: {
                    uint8_t index = (w0 >> 16) & 0xFF;
                    uint16_t offset = w0 & 0xFFFF;
                    switch (index) {
                        case 0x06: segments[(offset / 4) & 0xF] = w1 & 0xFFFFFF; break;
                        case 0x08: fog_mul = (int16_t)(w1 >> 16); fog_off = (int16_t)(w1 & 0xFFFF); break;
                        case 0x02: num_lights = (int)(w1 / 24); if (num_lights > 7) num_lights = 7; ldir_dirty = true; break;
                        case 0x0A: {
                            int li = offset / 24;
                            if (li < 8) { lights[li].col[0] = w1 >> 24; lights[li].col[1] = w1 >> 16; lights[li].col[2] = w1 >> 8; }
                            break;
                        }
                        default: break;
                    }
                    break;
                }
                case G_MOVEMEM: op_movemem: {
                    uint8_t index = w0 & 0xFF;
                    uint32_t offset = ((w0 >> 8) & 0xFF) * 8;
                    uint32_t addr = seg(w1);
                    if (index == 8) {            // G_MV_VIEWPORT
                        load_viewport(addr);
                    }
                    else if (index == 10) {      // G_MV_LIGHT
                        ldir_dirty = true;
                        if (offset < 0x30) {
                            int k = offset / 0x18;
                            lookat[k][0] = (int8_t)r8(addr + 8); lookat[k][1] = (int8_t)r8(addr + 9); lookat[k][2] = (int8_t)r8(addr + 10);
                        }
                        else {
                            int li = (offset - 0x30) / 0x18;
                            if (li < 8) {
                                lights[li].col[0] = r8(addr); lights[li].col[1] = r8(addr + 1); lights[li].col[2] = r8(addr + 2);
                                lights[li].dir[0] = (int8_t)r8(addr + 8); lights[li].dir[1] = (int8_t)r8(addr + 9); lights[li].dir[2] = (int8_t)r8(addr + 10);
                                if (li == num_lights) { ambient[0] = lights[li].col[0]; ambient[1] = lights[li].col[1]; ambient[2] = lights[li].col[2]; }
                            }
                        }
                    }
                    break;
                }
                case G_DL: op_dl:
                    if (((w0 >> 16) & 0xFF) == 0) {
                        if (depth < kMaxDlDepth) stack[depth++] = next; else stats->dl_depth_overflow++;
                    }
                    next = seg(w1);
                    break;
                case G_ENDDL: op_enddl:
                    if (depth == 0) return fullsync;
                    next = stack[--depth];
                    break;
                case G_RDPHALF_1: rdphalf1 = w1; break;
                case G_RDPHALF_2: rdphalf2 = w1; break;
                case G_SETOTHERMODE_L: set_othermode(true, w0, w1); break;
                case G_SETOTHERMODE_H: set_othermode(false, w0, w1); break;
                case G_RDPSETOTHERMODE: othermode_h = w0 & 0xFFFFFF; othermode_l = w1; break;
                case G_TEXRECT: case G_TEXRECTFLIP: {
                    uint32_t h1 = r32(next + 4), h2 = r32(next + 12);
                    next += 16;
                    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
                    int tile = (w1 >> 24) & 7;
                    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
                    float s = (int16_t)(h1 >> 16) / 32.0f, t = (int16_t)(h1 & 0xFFFF) / 32.0f;
                    float dsdx = (int16_t)(h2 >> 16) / 1024.0f, dtdy = (int16_t)(h2 & 0xFFFF) / 1024.0f;
                    emit_rect(ulx, uly, lrx, lry, true, tile, s, t, dsdx, dtdy, op == G_TEXRECTFLIP);
                    break;
                }
                case G_RDPLOADSYNC: case G_RDPPIPESYNC: case G_RDPTILESYNC: break;
                case G_RDPFULLSYNC: fullsync = true; out->has_fullsync = true; break;
                case G_SETKEYGB: case G_SETKEYR: case G_SETCONVERT: break;
                case G_SETSCISSOR:
                    scissor[0] = ((w0 >> 12) & 0xFFF) / 4; scissor[1] = (w0 & 0xFFF) / 4;
                    scissor[2] = ((w1 >> 12) & 0xFFF) / 4; scissor[3] = (w1 & 0xFFF) / 4;
                    if (scissor_slack > 0) {
                        // RenderDesc::scissor_edge_slack: the frame's edges.
                        if (scissor[0] <= scissor_slack) scissor[0] = 0;
                        if (scissor[1] <= scissor_slack) scissor[1] = 0;
                        if (scissor[2] >= screen_w - scissor_slack && scissor[2] < screen_w) scissor[2] = (int16_t)screen_w;
                        if (scissor[3] >= screen_h - scissor_slack && scissor[3] < screen_h) scissor[3] = (int16_t)screen_h;
                    }
                    break;
                case G_SETPRIMDEPTH: prim_depth_z = (uint16_t)(w1 >> 16); break;
                case G_LOADTLUT: {
                    int tile = (w1 >> 24) & 7;
                    Tile& tl = tiles[tile];
                    // The command's own coordinates, which the RDP (and RT64)
                    // also leave on the tile: the source starts at uls, ult of
                    // the texture image and runs to lrs. The tile's coordinates
                    // from an earlier load have nothing to do with it (Goemon's
                    // sky loads palettes through tile 7 right after column
                    // loads left uls far into its image).
                    tl.uls = (w0 >> 12) & 0xFFF; tl.ult = w0 & 0xFFF; tl.lrs = (w1 >> 12) & 0xFFF; tl.lrt = w1 & 0xFFF;
                    int count = (int)(tl.lrs >> 2) - (int)(tl.uls >> 2) + 1;
                    // Palettes sit in TMEM's upper half, one entry per 64-bit
                    // word (quadrupled): entry = word - 256. A 16-colour
                    // palette p is loaded at word 256 + 16p.
                    int base = (int)tl.tmem - 256;
                    const uint32_t row_bytes = ((uint32_t)timg.width << timg.siz) >> 1;
                    uint32_t src = timg.addr + ((((uint32_t)tl.uls >> 2) << timg.siz) >> 1) + row_bytes * ((uint32_t)tl.ult >> 2);
                    bool changed = false;
                    for (int i = 0; i < count; i++) {
                        int idx = base + i;
                        uint16_t v = ru16(src + i * 2);
                        if (idx >= 0 && idx < 256 && tlut[idx] != v) { tlut[idx] = v; changed = true; }
                    }
                    if (changed) tlut_seq++;
                    break;
                }
                case G_SETTILESIZE: {
                    Tile& tl = tiles[(w1 >> 24) & 7];
                    tl.uls = (w0 >> 12) & 0xFFF; tl.ult = w0 & 0xFFF; tl.lrs = (w1 >> 12) & 0xFFF; tl.lrt = w1 & 0xFFF;
                    break;
                }
                case G_LOADBLOCK: {
                    Tile& tl = tiles[(w1 >> 24) & 7];
                    uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
                    uint32_t lrs = (w1 >> 12) & 0xFFF;
                    uint32_t bpp_shift = timg.siz;   // bytes = texels << siz >> 1
                    uint32_t texels = lrs + 1;
                    uint32_t bytes = (texels << bpp_shift) >> 1;
                    uint32_t row_bytes = ((uint32_t)timg.width << bpp_shift) >> 1;
                    TmemLoad& L = loads[load_count++ & 15];
                    L.valid = true;
                    L.tmem = tl.tmem;
                    L.words = (bytes + 7) / 8;
                    L.addr = timg.addr + ((ult >> 2) * row_bytes) + (((uls >> 2) << bpp_shift) >> 1);
                    L.pitch = row_bytes;
                    L.siz = timg.siz;
                    L.block = true;
                    L.seq = load_count;
                    L.nibble = 0;
                    L.dxt = (uint16_t)(w1 & 0xFFF);
                    L.img_width = 0;
                    tl.uls = uls; tl.ult = ult; tl.lrs = lrs;
                    break;
                }
                case G_LOADTILE: {
                    Tile& tl = tiles[(w1 >> 24) & 7];
                    uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
                    uint32_t lrs = (w1 >> 12) & 0xFFF, lrt = w1 & 0xFFF;
                    uint32_t bpp_shift = timg.siz;
                    uint32_t row_bytes = ((uint32_t)timg.width << bpp_shift) >> 1;
                    uint32_t rows = ((lrt >> 2) - (ult >> 2)) + 1;
                    uint32_t line_words = tl.line ? tl.line : ((((((lrs >> 2) - (uls >> 2)) + 1) << bpp_shift) >> 1) + 7) / 8;
                    TmemLoad& L = loads[load_count++ & 15];
                    L.valid = true;
                    L.tmem = tl.tmem;
                    L.words = rows * line_words;
                    L.addr = timg.addr + ((ult >> 2) * row_bytes) + (((uls >> 2) << bpp_shift) >> 1);
                    L.pitch = row_bytes;
                    L.siz = timg.siz;
                    L.block = false;
                    L.dxt = 0;
                    L.seq = load_count;
                    L.nibble = (timg.siz == 0) ? ((uls >> 2) & 1) : 0;
                    L.img_addr = timg.addr;
                    L.img_width = timg.width;
                    L.s0 = (uint16_t)(uls >> 2); L.t0 = (uint16_t)(ult >> 2);
                    L.s1 = (uint16_t)(lrs >> 2); L.t1 = (uint16_t)(lrt >> 2);
                    L.line = (uint16_t)line_words;
                    // Only cuts narrower than their image are tracked: a list
                    // loads hundreds of whole small images (glyphs), which
                    // would push the panorama out of the table.
                    if (image_tiles && L.s1 >= L.s0 && L.t1 >= L.t0 && (uint32_t)(L.s1 - L.s0 + 1) < L.img_width) note_image_load(L);
                    tl.uls = uls; tl.ult = ult; tl.lrs = lrs; tl.lrt = lrt;
                    break;
                }
                case G_SETTILE: {
                    Tile& tl = tiles[(w1 >> 24) & 7];
                    tl.fmt = (w0 >> 21) & 7; tl.siz = (w0 >> 19) & 3; tl.line = (w0 >> 9) & 0x1FF; tl.tmem = w0 & 0x1FF;
                    tl.palette = (w1 >> 20) & 0xF; tl.cmt = (w1 >> 18) & 3; tl.maskt = (w1 >> 14) & 0xF; tl.shiftt = (w1 >> 10) & 0xF;
                    tl.cms = (w1 >> 8) & 3; tl.masks = (w1 >> 4) & 0xF; tl.shifts = w1 & 0xF;
                    break;
                }
                case G_FILLRECT: {
                    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
                    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
                    emit_rect(ulx, uly, lrx, lry, false, 0, 0, 0, 0, 0, false);
                    break;
                }
                case G_SETFILLCOLOR: {
                    // One RGBA5551 pixel (16-bit framebuffers).
                    uint16_t p = w1 & 0xFFFF;
                    fill[0] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
                    fill[1] = (uint8_t)(((p >> 6) & 31) * 255 / 31);
                    fill[2] = (uint8_t)(((p >> 1) & 31) * 255 / 31);
                    fill[3] = (p & 1) ? 255 : 0;
                    break;
                }
                case G_SETFOGCOLOR: fog[0] = w1 >> 24; fog[1] = w1 >> 16; fog[2] = w1 >> 8; fog[3] = w1; break;
                case G_SETBLENDCOLOR: blend[0] = w1 >> 24; blend[1] = w1 >> 16; blend[2] = w1 >> 8; blend[3] = w1; break;
                case G_SETPRIMCOLOR: prim_lod_frac = w0 & 0xFF; prim[0] = w1 >> 24; prim[1] = w1 >> 16; prim[2] = w1 >> 8; prim[3] = w1; break;
                case G_SETENVCOLOR: env[0] = w1 >> 24; env[1] = w1 >> 16; env[2] = w1 >> 8; env[3] = w1; break;
                case G_SETCOMBINE: cc_w0 = w0 & 0xFFFFFF; cc_w1 = w1; break;
                case G_SETTIMG:
                    timg.fmt = (w0 >> 21) & 7; timg.siz = (w0 >> 19) & 3; timg.width = (w0 & 0xFFF) + 1; timg.addr = seg(w1);
                    break;
                case G_SETZIMG: zimg = seg(w1); break;
                case G_SETCIMG:
                    flush_rect_slices();      // a new framebuffer pair
                    inst_break = true;
                    cimg = seg(w1); cimg_width = (w0 & 0xFFF) + 1;
                    cur_rtt = rtt_for_cimg(cimg, cimg_width);
                    if (cur_rtt >= 0) out->rtts[cur_rtt] = rtt_images[cur_rtt];
                    // Back to the screen after drawing into a copy: what was
                    // drawn there is still there.
                    if (frame_copy && cur_rtt < 0 && cimg != zimg && cimg != screen_cimg) { screen_cimg = cimg; screen_drawn = false; }
                    out->color_image = cimg; out->color_width = cimg_width; out->depth_image = zimg;
                    break;
                // F3DEX (GBI1) commands, in kF3dexOp's numbering: put into
                // F3DEX2's form where it differs, then run as that.
                case kF3dexLow | F3DEX_G_SPNOOP:
                    // The RT64 hook rides on GBI1's no-op (RT64_HOOK_OPCODE 0x00).
                    op = G_SPNOOP; w0 = ((uint32_t)G_SPNOOP << 24) | (w0 & 0xFFFFFF);
                    goto op_spnoop;
                case kF3dexLow | F3DEX_G_MTX: {
                    // GBI1 flags: projection 1, load 2, push 4. F3DEX2 stores
                    // projection 4, load 2, push 1, the push bit inverted.
                    const uint32_t p = (w0 >> 16) & 0xFF;
                    const uint32_t p2 = ((p & 1) ? 4u : 0u) | (p & 2) | ((p & 4) ? 1u : 0u);
                    op = G_MTX; w0 = ((uint32_t)G_MTX << 24) | (p2 ^ 1);
                    goto op_mtx;
                }
                case kF3dexLow | F3DEX_G_MOVEMEM: {
                    const uint8_t index = (w0 >> 16) & 0xFF;
                    uint32_t offset;   // F3DEX2 G_MV_LIGHT offset: look-at X, Y, then the lights
                    if (index == F3DEX_G_MV_VIEWPORT) { op = G_MOVEMEM; w0 = ((uint32_t)G_MOVEMEM << 24) | 8; goto op_movemem; }
                    if (index == F3DEX_G_MV_LOOKATX) offset = 0;
                    else if (index == F3DEX_G_MV_LOOKATY) offset = 0x18;
                    else if (index >= F3DEX_G_MV_L0 && index <= F3DEX_G_MV_L7) offset = 0x30 + (uint32_t)((index - F3DEX_G_MV_L0) / 2) * 0x18;
                    else { stats->unknown++; break; }   // G_MV_MATRIX_* (forced matrices), G_MV_TXTATT
                    op = G_MOVEMEM; w0 = ((uint32_t)G_MOVEMEM << 24) | ((offset / 8) << 8) | 10;
                    goto op_movemem;
                }
                case kF3dexLow | F3DEX_G_VTX: {
                    const uint32_t n = (w0 >> 10) & 0x3F, v0 = (w0 >> 17) & 0x7F;
                    load_vertices(seg(w1), (int)n, (int)((v0 + n) & 0x7F) - (int)n);
                    break;
                }
                case kF3dexLow | F3DEX_G_DL: op = G_DL; goto op_dl;
                case F3DEX_G_LOAD_UCODE: break;
                case F3DEX_G_BRANCH_Z: op = G_BRANCH_Z; goto op_branch_z;
                case F3DEX_G_TRI2: case F3DEX_G_QUAD:
                    // TRI2: two index triples (x2), as F3DEX2's. gSP1Quadrangle:
                    // four indices (x2) in w1, drawn as a, b, c and a, c, d.
                    // (L3DEX's G_LINE3D shares QUAD's number; Banjo draws no lines.)
                    if (op == F3DEX_G_TRI2) {
                        emit_tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                        emit_tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                    }
                    else {
                        emit_tri(((w1 >> 24) & 0xFF) / 2, ((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2);
                        emit_tri(((w1 >> 24) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                    }
                    break;
                case F3DEX_G_TRI1:
                    // GBI1 carries the indices (x2) in w1.
                    emit_tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                    break;
                case F3DEX_G_MODIFYVTX: op = G_MODIFYVTX; goto op_modifyvtx;   // same layout
                case F3DEX_G_RDPHALF_1: rdphalf1 = w1; break;
                case F3DEX_G_RDPHALF_2: rdphalf2 = w1; break;
                case F3DEX_G_CLEARGEOMETRYMODE: geometry_mode &= ~f3dex_geometry_bits(w1); break;
                case F3DEX_G_SETGEOMETRYMODE: geometry_mode |= f3dex_geometry_bits(w1); break;
                case F3DEX_G_ENDDL: op = G_ENDDL; goto op_enddl;
                case F3DEX_G_SETOTHERMODE_L: case F3DEX_G_SETOTHERMODE_H: {
                    // GBI1 gives the shift and length; F3DEX2 32 - shift - len and len - 1.
                    const uint32_t shift = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
                    const bool low = op == F3DEX_G_SETOTHERMODE_L;
                    op = low ? G_SETOTHERMODE_L : G_SETOTHERMODE_H;
                    w0 = ((uint32_t)op << 24) | (((32 - shift - len) & 0xFF) << 8) | ((len - 1) & 0xFF);
                    set_othermode(low, w0, w1);
                    break;
                }
                case F3DEX_G_TEXTURE: op = G_TEXTURE; goto op_texture;   // same fields
                case F3DEX_G_MOVEWORD: {
                    const uint8_t index = w0 & 0xFF;
                    uint32_t offset = (w0 >> 8) & 0xFFFF;
                    if (index == 0x02) {
                        // NUMLIGHT: 0x80000000 + 32 * (lights + 1) -> F3DEX2's lights * 24.
                        const int n = (int)((w1 - 0x80000000u) >> 5) - 1;
                        w1 = (uint32_t)(n < 0 ? 0 : n) * 24;
                    }
                    else if (index == 0x0A) {
                        // LIGHTCOL: GBI1 lights are 32 bytes apart in DMEM, F3DEX2's 24.
                        offset = (offset / 32) * 24 + (offset % 32);
                    }
                    else if (index == F3DEX_G_MW_POINTS) {
                        // gSPModifyVertex through G_MOVEWORD: offset = vertex * 40 + where.
                        const uint32_t v = offset / 40, where = offset % 40;
                        op = G_MODIFYVTX; w0 = ((uint32_t)G_MODIFYVTX << 24) | (where << 16) | ((v * 2) & 0xFFFF);
                        goto op_modifyvtx;
                    }
                    op = G_MOVEWORD; w0 = ((uint32_t)G_MOVEWORD << 24) | ((uint32_t)index << 16) | (offset & 0xFFFF);
                    goto op_moveword;
                }
                case F3DEX_G_POPMTX:
                    // Only the modelview stack pops (w1 = G_MTX_MODELVIEW), one level.
                    if (w1 != 0) break;
                    op = G_POPMTX; w1 = 64;
                    goto op_popmtx;
                case F3DEX_G_CULLDL: stats->cull_dl++; break;
                default:
                    stats->unknown++;
                    if (dbg_unknown_ < 8) {
                        dbg_unknown_++;
                        fprintf(stderr, "rt64-3ds: unknown opcode %02x at %06x: %08x %08x (depth %d)\n", w0 >> 24, pc, w0, w1, depth);
                    }
                    break;
            }
            pc = next;
        }
        return fullsync;
    }

    // Per-task RSP state that must not leak between lists, and the list's
    // own bookkeeping.
    // RenderDesc::merge_image_tiles: neighbouring rectangle records that
    // sample an image texture with the same state become one record (their
    // vertices follow each other). Run once the list's rectangle slices are
    // settled (merge_rect_slices moves vertices and scissors per record).
    void compact_rects() {
        std::vector<DrawRecord>& d = out->draws;
        if (d.size() < 2) return;
        const int32_t sample_last = out->frame_sample_last;
        int32_t new_sample_last = -1;
        const bool remap = ip != nullptr && ip->recording;
        if (remap) ip->draw_remap.assign(d.size(), 0);
        size_t w = 0;
        for (size_t i = 0; i < d.size(); i++) {
            if (w > 0) {
                DrawRecord& a = d[w - 1];
                const DrawRecord& b = d[i];
                if (a.image_rect && b.image_rect && a.kind == DrawRecord::TexRect && b.kind == DrawRecord::TexRect &&
                    a.first + a.count == b.first && a.mv_auto == b.mv_auto && same_rect_state(a, b)) {
                    a.count += b.count;
                    stats->rects_compacted++;
                    if ((int32_t)i == sample_last) new_sample_last = (int32_t)(w - 1);
                    if (remap) ip->draw_remap[i] = (uint32_t)(w - 1);
                    continue;
                }
            }
            if (remap) ip->draw_remap[i] = (uint32_t)w;
            if (w != i) d[w] = d[i];
            if ((int32_t)i == sample_last) new_sample_last = (int32_t)w;
            w++;
        }
        d.resize(w);
        out->frame_sample_last = new_sample_last;
    }
    // same_draw_state without uv1, which only places TEXEL1 (none here).
    static bool same_rect_state(const DrawRecord& a, const DrawRecord& b) {
        return !a.tex[1].valid && !b.tex[1].valid && a.cc_w0 == b.cc_w0 && a.cc_w1 == b.cc_w1 && a.othermode_h == b.othermode_h &&
               a.othermode_l == b.othermode_l && a.geometry_mode == b.geometry_mode && same_bits<4>(a.prim, b.prim) && same_bits<4>(a.env, b.env) &&
               same_bits<4>(a.blend, b.blend) && same_bits<4>(a.fog, b.fog) && a.prim_lod_frac == b.prim_lod_frac &&
               same_bits<sizeof(a.scissor)>(a.scissor, b.scissor) && a.proj_id == b.proj_id && a.perspective == b.perspective && a.rtt == b.rtt &&
               same_tex(a.tex[0], b.tex[0]);
    }

    void begin_list(FrameRecord& o) {
        out = &o;
        mv_depth = 0;
        mvp_dirty = true;
        proj_group_depth = mv_group_depth = 0;
        proj_groups[0] = 0;
        // Untagged modelview draws carry G_EX_ID_AUTO, as RT64's TransformGroup
        // does by default: DK64's crosshair quads are matched on that.
        mv_groups[0] = kIdAuto;
        proj_aspects[0] = 0;
        proj_id = 0;
        update_depth_terms();
        mv_seq = 0;
        vp_depth = 0;
        scissor_depth = 0;
        othermode_depth = combine_depth = geometry_depth = proj_depth = 0;
        for (int& d : color_depth) d = 0;
        load_count = 0;
        tlut_frame_seq = 0;
        for (int i = 0; i < kMaxRtt; i++) o.rtts[i] = rtt_images[i];
        for (auto& L : loads) L.valid = false;
        rect_slices.clear();
        list_seq++;
        screen_cimg = cur_rtt < 0 ? cimg : 0;
        screen_drawn = false;
        proj_inst = 0;
        inst_break = true;
        if (image_tiles) roll_images();
        plain = true;
        plain_cimg = 0;
        if (ip != nullptr) {
            // RT64 starts every workload with default groups (G_EX_ID_AUTO).
            mv_tg[0] = interp::Group();
            proj_tg[0] = interp::Group();
            mv_tg_idx = proj_tg_idx = -1;
            proj_seq++;                 // the first vertex load records the projection
            xf_i = pj_i = tl_i = -1;
            v_i = 0;
            tile_rec_seq = 0; tile_rec_tile = -1; tile_rec_xf = -2;
            tile_du[0] = tile_du[1] = tile_dv[0] = tile_dv[1] = 0.0f;
        }
    }
    // A whole frame drawn into the screen alone (Interpreter::last_list_plain).
    bool list_plain = false;
    void end_list(bool fullsync) {
        list_plain = fullsync && plain && cur_rtt < 0 && (plain_cimg == 0 || plain_cimg == cimg) &&
                     !cimg_is_depth_memory() && cimg < 0x800000 && is_presented_fb(cimg);
    }
};

Interpreter::Interpreter(uint8_t* rdram) : impl_(new Impl()) {
    impl_->rdram = rdram;
    impl_->stats = &stats_;
    Impl::identity(impl_->proj);
    Impl::identity(impl_->mv_stack[0]);
}

void Interpreter::note_presented_framebuffer(uint32_t addr) {
    for (uint32_t fb : impl_->vi_fbs) if (fb == addr) return;
    // A frame that ends on an off-screen image (Banjo's jiggy transition
    // leaves its image current) did not present it.
    if (impl_->rtt_enabled) {
        for (int i = 0; i < impl_->rtt_count; i++) if (impl_->rtt_images[i].addr == addr) return;
        if (addr >= 0x800000) return;
    }
    else if (impl_->frame_copy) {
        for (int i = 0; i < impl_->rtt_count; i++) if (impl_->rtt_images[i].addr == addr) return;
    }
    impl_->vi_fbs[impl_->vi_fb_next++ & 3] = addr;
}


void Interpreter::note_vi_origin(uint32_t origin) {
    const uint32_t addr = origin & kRdramMask;
    if (addr == 0) return;
    for (uint32_t o : impl_->vi_origins) if (o == addr) return;
    impl_->vi_origins[impl_->vi_origin_next++ & 3] = addr;
    if (impl_->vi_origin_next <= 6) fprintf(stderr, "rt64-3ds: VI origin %08x noted (a presented framebuffer ends a line or two below it)\n", addr);
}

bool Interpreter::run(uint32_t data_ptr, FrameRecord& out) {
    out.frame_w = (uint16_t)impl_->screen_w;
    out.frame_h = (uint16_t)impl_->screen_h;
    Interp* ip = impl_->ip;
    if (ip != nullptr) {
        // Frame interpolation: keep the state the list starts from (the
        // in-between run starts there too) and record its transforms.
        *before_ = *impl_;
        ip->cur ^= 1;
        ip->now().clear();
        ip->recording = true;
        ip->applying = false;
        ip->ctxs.clear();
        ip->lv.clear();
        ip->tris.clear();
        ip->tri_states.clear();
        ip->tri_uv.clear();
        impl_->tri_key_draw = ~0u;
        ip->draw_remap.clear();
        for (uint32_t& s : impl_->slot_src) s = ~0u;
    }
    // Deferred triangles (not with merge_image_tiles: compact_rects
    // renumbers the records). The vertex buffer as the list finds it.
    const bool defer = dfr_ != nullptr && !impl_->image_tiles;
    if (defer) {
        // (Single thread: a list whose triangles nobody took. Else the
        // renderer's drain is running already and the last finish put
        // drain's side back.)
        if (dfr_->pending) { FrameRecord scrap; dfr_->finish(scrap, stats_); dfr_->pending = false; }
        dfr_->begin(impl_->screen_w, impl_->screen_h);
        impl_->dfr = dfr_;
        for (int i = 0; i < kMaxVerts; i++) impl_->new_slot(i) = impl_->vtx[i];
        impl_->defer_open = -1;
        impl_->defer_key_draw = ~0u;
        impl_->defer_active = true;
    }
    impl_->begin_list(out);
    const bool fullsync = impl_->run(data_ptr);
    impl_->flush_rect_slices();
    if (impl_->image_tiles) impl_->compact_rects();
    impl_->end_list(fullsync);
    if (defer) {
        // The vertex buffer as the list leaves it, for the lists after.
        for (int i = 0; i < kMaxVerts; i++) impl_->vtx[i] = dfr_->resolve(impl_->dslot[i]);
        impl_->defer_active = false;
        impl_->defer_open = -1;
        dfr_->pending = true;
        dfr_->publish();     // all of it
    }
    else impl_->unify_world_depth(out);
    if (ip != nullptr) {
        ip->recording = false;
        ip->now().valid = fullsync;
        if (!ip->draw_remap.empty()) {
            for (Interp::TriState& s : ip->tri_states) if (s.draw < ip->draw_remap.size()) s.draw = ip->draw_remap[s.draw];
        }
        ip->rect_verts.clear();
        ip->rect_first.assign(out.draws.size(), ~0u);
        for (size_t i = 0; i < out.draws.size(); i++) {
            const DrawRecord& d = out.draws[i];
            if (d.kind == DrawRecord::Tris || d.first + d.count > out.verts.size()) continue;
            ip->rect_first[i] = (uint32_t)ip->rect_verts.size();
            ip->rect_verts.insert(ip->rect_verts.end(), &out.verts[d.first], &out.verts[d.first] + d.count);
        }
    }
    return fullsync;
}

// The in-between frame from the list run last, without running it again:
// its records with the vertices of every triangle transformed by the
// interpolated matrices (and moved vertices and scrolled tiles where the
// tags ask), then culled and clipped anew. What does not depend on the
// transforms is the game frame's: rectangles, lighting and texture
// generation, the draws' state and textures.
bool Interpreter::build_inbetween(const FrameRecord& real, FrameRecord& o, uint32_t* tris_out) {
    Interp* ip = impl_->ip;
    if (ip == nullptr || !ip->now().valid) return false;
    const interp::FrameLog& L = ip->now();
    const interp::FrameLog& P = ip->before();
    const float wgt = ip->weight, back = 1.0f - wgt;
    // The records, as the game frame has them.
    o.draws = real.draws;
    o.tlut = real.tlut;
    o.mips = real.mips;
    o.has_fullsync = real.has_fullsync;
    // The in-between stores nothing: the game's copy of the screen is the
    // real frame's, taken when that frame began.
    o.snapshot_request = false;
    o.snapshot_store = 0;
    o.color_image = real.color_image; o.color_width = real.color_width; o.depth_image = real.depth_image;
    o.frame_w = real.frame_w; o.frame_h = real.frame_h;
    o.has_cam = real.has_cam; o.cam_is_world = real.cam_is_world;
    memcpy(o.cam, real.cam, sizeof(o.cam));
    o.has_hud_cam = real.has_hud_cam;
    memcpy(o.hud_cam, real.hud_cam, sizeof(o.hud_cam));
    for (int i = 0; i < kMaxRtt; i++) o.rtts[i] = real.rtts[i];
    o.inst_cams = real.inst_cams;
    o.frame_sample_last = real.frame_sample_last;
    o.verts.clear();
    // Each load's matrix in the in-between frame: the game frame's where
    // neither its transform nor its projection is interpolated.
    const size_t nc = ip->ctxs.size();
    ip->ctx_m.resize(nc);
    for (size_t i = 0; i < nc; i++) {
        const Interp::LoadCtx& lc = ip->ctxs[i];
        const interp::XfEntry* xe = (lc.xf >= 0 && (size_t)lc.xf < L.xf.size()) ? &L.xf[lc.xf] : nullptr;
        const interp::PjEntry* pe = (lc.pj >= 0 && (size_t)lc.pj < L.pj.size()) ? &L.pj[lc.pj] : nullptr;
        interp::M4& m = ip->ctx_m[i];
        if ((xe == nullptr || !xe->interp) && (pe == nullptr || !pe->interp)) { memcpy(&m, lc.m, sizeof(m)); continue; }
        const interp::M4 mv = xe == nullptr ? interp::identity() : (xe->interp ? xe->lerped : xe->m);
        const interp::M4 pj = pe == nullptr ? interp::identity() : (pe->interp ? pe->lerped : pe->full);
        const interp::M4 mvp = interp::mul(mv, pj);
        for (int r = 0; r < 4; r++) {
            const float cx = mvp.m[r][0], cy = mvp.m[r][1], cz = mvp.m[r][2], cw = mvp.m[r][3];
            m.m[r][0] = cx * lc.sx + cw * lc.tx;
            m.m[r][1] = cy * lc.sy + cw * lc.ty;
            m.m[r][2] = 0.5f * (cz + cw);
            m.m[r][3] = cw;
        }
    }
    // The triangles, record by record: [d0, d1) of the records from
    // triangle ti0 into `out` with vertex cache `vc`, on whichever thread
    // runs it. Everything it reads is fixed until the next list's run; it
    // writes only its own records, cache and output.
    struct Range {
        Interp* ip;
        const interp::FrameLog* L;
        const interp::FrameLog* P;
        const FrameRecord* real;
        FrameRecord* o;
        VtxArray* out;
        Interp::VtxCache* vc;
        size_t d0, d1, ti0;
        float back;
        int screen_w, screen_h;
        uint32_t emitted, clipped, clip_tris;
        static void run(void* arg) {
            Range& r = *static_cast<Range*>(arg);
            Interp* ip = r.ip;
            const interp::FrameLog& L = *r.L;
            const interp::FrameLog& P = *r.P;
            const float back = r.back;
            Interp::VtxCache& vc = *r.vc;
            // Vertices, transformed when a triangle first uses them.
            vc.cur++;
            if (vc.stamp.size() < ip->lv.size()) vc.stamp.resize(ip->lv.size(), 0);
            if (vc.out.size() < ip->lv.size()) vc.out.resize(ip->lv.size());
            auto vertex = [&](uint32_t idx) -> const Vtx3ds& {
                Vtx3ds& v = vc.out[idx];
                if (vc.stamp[idx] == vc.cur) return v;
                vc.stamp[idx] = vc.cur;
                const Interp::LoadedVtx& q = ip->lv[idx];
                const Interp::LoadCtx& lc = ip->ctxs[q.ctx];
                const interp::M4& m = ip->ctx_m[q.ctx];
                float ob0 = q.ob[0], ob1 = q.ob[1], ob2 = q.ob[2];
                const interp::XfEntry* xe = (lc.xf >= 0 && (size_t)lc.xf < L.xf.size()) ? &L.xf[lc.xf] : nullptr;
                float dtu = 0.0f, dtv = 0.0f;
                if (xe != nullptr && (xe->vpos || xe->vtc) && q.vi < xe->vcount && ((size_t)xe->vprev + q.vi) * 5 + 4 < P.vdata.size()) {
                    const int16_t* p = &P.vdata[((size_t)xe->vprev + q.vi) * 5];
                    if (xe->vpos) { ob0 += ((float)p[0] - ob0) * back; ob1 += ((float)p[1] - ob1) * back; ob2 += ((float)p[2] - ob2) * back; }
                    if (xe->vtc && !lc.texgen) { dtu = ((float)p[3] - (float)q.tc[0]) * back * lc.uscale; dtv = ((float)p[4] - (float)q.tc[1]) * back * lc.vscale; }
                }
                v.x = ob0 * m.m[0][0] + ob1 * m.m[1][0] + ob2 * m.m[2][0] + m.m[3][0];
                v.y = ob0 * m.m[0][1] + ob1 * m.m[1][1] + ob2 * m.m[2][1] + m.m[3][1];
                v.z = ob0 * m.m[0][2] + ob1 * m.m[1][2] + ob2 * m.m[2][2] + m.m[3][2];
                v.w = ob0 * m.m[0][3] + ob1 * m.m[1][3] + ob2 * m.m[2][3] + m.m[3][3];
                v.u = dtu; v.v = dtv;        // texcoord moves (scaled by each triangle's tile)
                v.r = q.rgba[0]; v.g = q.rgba[1]; v.b = q.rgba[2]; v.a = q.rgba[3];
                if (q.fog) v.a = rsp_fog(v.z, v.w, lc.fm, lc.fo);
                return v;
            };
            VtxArray& out = *r.out;
            size_t ti = r.ti0;
            const size_t nt = ip->tris.size();
            for (size_t di = r.d0; di < r.d1; di++) {
                DrawRecord& d = r.o->draws[di];
                const uint32_t real_first = d.first, real_count = d.count;
                d.first = out.size();
                d.count = 0;
                if (d.kind != DrawRecord::Tris) {
                    // The list's own copy (Interp::rect_verts), not the game
                    // frame's, which the renderer may have changed already.
                    Vtx3ds* dst = out.append(real_count);
                    const uint32_t src = di < ip->rect_first.size() ? ip->rect_first[di] : ~0u;
                    if (dst != nullptr && src != ~0u && src + real_count <= ip->rect_verts.size()) {
                        memcpy(dst, &ip->rect_verts[src], real_count * sizeof(Vtx3ds));
                        d.count = real_count;
                    }
                    else if (dst != nullptr && real_first + real_count <= r.real->verts.size()) {
                        memcpy(dst, &r.real->verts[real_first], real_count * sizeof(Vtx3ds));
                        d.count = real_count;
                    }
                    continue;
                }
                while (ti < nt && ip->tri_states[ip->tris[ti].state].draw < di) ti++;
                for (; ti < nt && ip->tri_states[ip->tris[ti].state].draw == di; ti++) {
                    const Interp::TriRec& t = ip->tris[ti];
                    const Interp::TriState& s = ip->tri_states[t.state];
                    if (t.src[0] >= ip->lv.size() || t.src[1] >= ip->lv.size() || t.src[2] >= ip->lv.size()) continue;
                    Vtx3ds tri[3];
                    float du = 0.0f, dv = 0.0f;
                    if (s.plain && s.tile >= 0 && (size_t)s.tile < L.tiles.size() && L.tiles[s.tile].interp) { du = L.tiles[s.tile].du[0]; dv = L.tiles[s.tile].dv[0]; }
                    // Mapped texel coordinates: as stored; the vertices' own: as
                    // emit_tri worked them out, plus their move (v.u, v.v).
                    const float* uv = s.plain ? nullptr : &ip->tri_uv[s.uv_first + (size_t)(ti - s.first_tri) * 6];
                    for (int k = 0; k < 3; k++) {
                        const Vtx3ds& v = vertex(t.src[k]);
                        Vtx3ds& q = tri[k];
                        q = v;
                        if (s.zprim >= 0.0f) q.z = s.zprim * v.w;
                        if (uv != nullptr) { q.u = uv[k * 2] - du; q.v = uv[k * 2 + 1] - dv; }
                        else {
                            const Interp::LoadedVtx& lq = ip->lv[t.src[k]];
                            q.u = (lq.u * s.ss - s.uls) - du + v.u * s.ss;
                            q.v = (lq.v * s.st - s.ult) - dv + v.v * s.st;
                        }
                    }
                    if (s.cull != 0) {
                        const Vtx3ds& a = tri[0]; const Vtx3ds& b = tri[1]; const Vtx3ds& c = tri[2];
                        const float ab = a.w * b.w, bc = b.w * c.w, ac = a.w * c.w;
                        const float ax = a.x * bc, ay = a.y * bc;
                        const float bx = b.x * ac, by = b.y * ac;
                        const float cx = c.x * ab, cy = c.y * ab;
                        float cross = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                        if (ab * c.w < 0.0f) cross = -cross;
                        if ((s.cull & G_CULL_BACK) && cross > 0.0f) continue;
                        if ((s.cull & G_CULL_FRONT) && cross < 0.0f) continue;
                    }
                    r.emitted++;
                    if (!in_guard_band(tri[0], r.screen_w, r.screen_h) || !in_guard_band(tri[1], r.screen_w, r.screen_h) ||
                        !in_guard_band(tri[2], r.screen_w, r.screen_h)) {
                        clip_tri(TriSink{ &out, &d.count, &r.clip_tris, &r.clipped, r.screen_w, r.screen_h }, tri);
                        continue;
                    }
                    Vtx3ds* dst = out.append(3);
                    if (dst == nullptr) continue;
                    dst[0] = tri[0]; dst[1] = tri[1]; dst[2] = tri[2];
                }
                d.count = out.size() - d.first;
            }
        }
    };
    // Fewer triangles than this: not worth the hand-over (a few hundred
    // microseconds of work on the console).
    constexpr size_t kSplitMinTris = 512;
    const size_t nd = o.draws.size(), nt = ip->tris.size();
    Range a{ ip, &L, &P, &real, &o, &o.verts, &ip->cache[0], 0, nd, 0, back, impl_->screen_w, impl_->screen_h, 0, 0, 0 };
    // With a helper and enough triangles: the records from the one holding
    // the middle triangle (the triangles are in record order) go to it.
    size_t split = nd;
    if (helper_ != nullptr && nt >= kSplitMinTris) {
        split = ip->tri_states[ip->tris[nt / 2].state].draw;
        if (split == 0 || split >= nd) split = nd;
    }
    Range b = a;
    if (split < nd) {
        size_t ti = nt / 2;
        while (ti > 0 && ip->tri_states[ip->tris[ti - 1].state].draw >= split) ti--;
        a.d1 = split;
        b.d0 = split; b.ti0 = ti;
        b.out = &ip->split_out;
        b.vc = &ip->cache[1];
        ip->split_out.clear();
        helper_->start(&Range::run, &b);
    }
    Range::run(&a);
    if (split < nd) {
        helper_->finish();
        // The helper's vertices after this thread's, as far as they fit.
        // Only roughly what one thread does with a full buffer (65536
        // vertices): that drops each triangle that doesn't fit and may still
        // fit a smaller one after it; this cuts the helper's half off.
        const uint32_t base = o.verts.size();
        const uint32_t n = std::min(ip->split_out.size(), o.verts.room());
        Vtx3ds* dst = n != 0 ? o.verts.append(n) : nullptr;
        if (dst != nullptr) memcpy(dst, ip->split_out.data(), n * sizeof(Vtx3ds));
        for (size_t di = split; di < nd; di++) {
            DrawRecord& d = o.draws[di];
            const uint32_t first = std::min(d.first, n), end = std::min(d.first + d.count, n);
            d.first = base + first;
            d.count = end - first;
        }
    }
    impl_->unify_world_depth(o);
    if (tris_out != nullptr) *tris_out = a.emitted + (split < nd ? b.emitted : 0);
    return true;
}

void Interpreter::describe_inbetween_draw(uint32_t draw, char* out, size_t n) const {
    out[0] = 0;
    const Interp* ip = impl_->ip;
    if (ip == nullptr || n == 0) return;
    const interp::FrameLog& L = ip->logs[ip->cur];
    const interp::FrameLog& P = ip->logs[ip->cur ^ 1];
    size_t ti = 0;
    while (ti < ip->tris.size() && ip->tri_states[ip->tris[ti].state].draw != draw) ti++;
    if (ti == ip->tris.size() || ip->tris[ti].src[0] >= ip->lv.size()) { snprintf(out, n, "no triangles recorded"); return; }
    const Interp::LoadCtx& lc = ip->ctxs[ip->lv[ip->tris[ti].src[0]].ctx];
    // A matrix's scale (its first two rows' lengths) and offset.
    auto sc = [](const interp::M4& m, float* r) {
        r[0] = sqrtf(m.m[0][0] * m.m[0][0] + m.m[0][1] * m.m[0][1] + m.m[0][2] * m.m[0][2]);
        r[1] = sqrtf(m.m[1][0] * m.m[1][0] + m.m[1][1] * m.m[1][1] + m.m[1][2] * m.m[1][2]);
        r[2] = m.m[3][0]; r[3] = m.m[3][1];
    };
    size_t at = 0;
    if (lc.xf >= 0 && (size_t)lc.xf < L.xf.size()) {
        const interp::XfEntry& e = L.xf[lc.xf];
        const interp::Group& g = L.groups[e.group];
        float c[4], b[4] = {}, l[4] = {};
        sc(e.m, c);
        if (e.prev >= 0 && (size_t)e.prev < P.xf.size()) sc(P.xf[e.prev].m, b);
        if (e.interp) sc(e.lerped, l);
        at += snprintf(out + at, n - at, "transform %d id %lx order %u decompose %d paired with %d (key %08lx/%08lx) interp %d: scale,offset before %.3g,%.3g %.4g,%.4g now %.3g,%.3g %.4g,%.4g in-between %.3g,%.3g %.4g,%.4g",
                       lc.xf, (unsigned long)g.id, (unsigned)g.order, (int)g.decompose, e.prev, (unsigned long)e.key,
                       (unsigned long)(e.prev >= 0 && (size_t)e.prev < P.xf.size() ? P.xf[e.prev].key : 0), (int)e.interp,
                       b[0], b[1], b[2], b[3], c[0], c[1], c[2], c[3], l[0], l[1], l[2], l[3]);
    }
    if (lc.pj >= 0 && (size_t)lc.pj < L.pj.size() && at < n) {
        const interp::PjEntry& e = L.pj[lc.pj];
        const interp::Group& g = L.groups[e.group];
        float c[4], b[4] = {}, l[4] = {};
        sc(e.full, c);
        if (e.prev >= 0 && (size_t)e.prev < P.pj.size()) sc(P.pj[e.prev].full, b);
        if (e.interp) sc(e.lerped, l);
        snprintf(out + at, n - at, "; projection %d id %lx %s paired with %d interp %d: scale,offset before %.3g,%.3g %.4g,%.4g now %.3g,%.3g %.4g,%.4g in-between %.3g,%.3g %.4g,%.4g",
                 lc.pj, (unsigned long)g.id, e.perspective ? "perspective" : "ortho", e.prev, (int)e.interp,
                 b[0], b[1], b[2], b[3], c[0], c[1], c[2], c[3], l[0], l[1], l[2], l[3]);
    }
}

void Interpreter::set_interpolation(bool on) {
    if (on && ip_ == nullptr) {
        ip_ = new Interp();
        before_ = new Impl();
        after_ = new Impl();
    }
    if (!on && ip_ != nullptr) { ip_->logs[0].clear(); ip_->logs[1].clear(); }
    impl_->ip = on ? ip_ : nullptr;
}

interp::MatchResult Interpreter::match_previous(float weight) {
    Interp* ip = impl_->ip;
    if (ip == nullptr) return interp::MatchResult{};
    ip->weight = weight;
    auto world = [](uint32_t id, bool, void* ctx) { return static_cast<Impl*>(ctx)->is_world_proj(id); };
    return interp::match(ip->before(), ip->now(), weight, world, impl_);
}

interp::MatchResult Interpreter::reweight_previous(float weight) {
    interp::MatchResult res;
    Interp* ip = impl_->ip;
    if (ip == nullptr) return res;
    ip->weight = weight;
    interp::reweight(ip->before(), ip->now(), weight, &res);
    return res;
}

bool Interpreter::run_inbetween(uint32_t data_ptr, FrameRecord& out, uint32_t* tris) {
    Interp* ip = impl_->ip;
    if (ip == nullptr || !ip->now().valid) return false;
    // From the state the list started from, and back to where it left it.
    *after_ = *impl_;
    const InterpreterStats saved = stats_;
    *impl_ = *before_;
    ip->applying = true;
    ip->diverged = false;
    impl_->begin_list(out);
    const bool fullsync = impl_->run(data_ptr);
    impl_->flush_rect_slices();
    if (impl_->image_tiles) impl_->compact_rects();
    impl_->unify_world_depth(out);
    const interp::FrameLog& L = ip->now();
    const bool followed = !ip->diverged && fullsync && (size_t)(impl_->xf_i + 1) == L.xf.size() &&
                          (size_t)(impl_->pj_i + 1) == L.pj.size() && (size_t)(impl_->tl_i + 1) == L.tiles.size();
    ip->applying = false;
    if (tris != nullptr) *tris = stats_.tris - saved.tris;
    *impl_ = *after_;
    stats_ = saved;
    return followed;
}

bool Interpreter::last_list_plain() const { return impl_->list_plain; }

void Interpreter::set_profile(bool on) { impl_->profile = on; if (before_ != nullptr) before_->profile = on; if (after_ != nullptr) after_->profile = on; }

bool Interpreter::pass_over(uint32_t data_ptr, FrameRecord& scratch) {
    // A copy of the whole state (a few KB) to put back when the list turns
    // out to need drawing.
    if (saved_ == nullptr) saved_ = new Impl();
    *saved_ = *impl_;
    impl_->skim = true;
    impl_->begin_list(scratch);
    const bool fullsync = impl_->run(data_ptr);
    impl_->skim = false;
    impl_->end_list(fullsync);
    if (impl_->list_plain) return true;
    *impl_ = *saved_;
    return false;
}

void Interpreter::set_world_proj_ranges(const ProjIdRange* ranges, int count) {
    if (count > Impl::kWorldRanges) count = Impl::kWorldRanges;
    if (count < 0) count = 0;
    for (int i = 0; i < count; i++) impl_->world_ranges[i] = ranges[i];
    impl_->world_range_count = count;
}
void Interpreter::set_world_proj_ids(const uint32_t* ids, int count) {
    ProjIdRange ranges[Impl::kWorldRanges];
    if (count > Impl::kWorldRanges) count = Impl::kWorldRanges;
    for (int i = 0; i < count; i++) ranges[i] = { ids[i], ids[i] };
    set_world_proj_ranges(ranges, count);
}
void Interpreter::set_split_untagged_ortho(bool on) { impl_->split_untagged_ortho = on; }
void Interpreter::set_aim_hash(bool on) { impl_->aim_hash = on; }
void Interpreter::set_force_branch_z(bool on) { impl_->force_branch_z = on; }
void Interpreter::set_gbi(Gbi gbi) {
    impl_->gbi = gbi;
    impl_->op_table = gbi == Gbi::F3DEX ? kF3dexOp.v : kF3dex2Op.v;
}
void Interpreter::set_render_to_texture(bool on) { impl_->rtt_enabled = on; }
void Interpreter::set_sample_drawn_frame(bool on) { impl_->frame_copy = on; }
void Interpreter::set_primary_world_instance(bool on) { impl_->inst_track = on; }
void Interpreter::set_unify_world_depth(bool on) { impl_->unify_depth = on; impl_->update_depth_terms(); }
void Interpreter::set_defer_triangles(bool on, bool vertices, void (*notify)(void*), void* notify_arg) {
    if (on && dfr_ == nullptr) dfr_ = new Defer();
    else if (!on && dfr_ != nullptr) { delete dfr_; dfr_ = nullptr; impl_->dfr = nullptr; }
    if (dfr_ != nullptr) { dfr_->vertices = vertices; dfr_->notify = notify; dfr_->notify_arg = notify_arg; }
}
void Interpreter::drain_triangles() {
    if (dfr_ != nullptr) dfr_->drain();
}
bool Interpreter::finish_triangles(FrameRecord& out) {
    if (dfr_ == nullptr || !dfr_->pending) return false;
    dfr_->pending = false;
    dfr_->finish(out, stats_);
    impl_->unify_world_depth(out);
    return true;
}
void Interpreter::set_fix_rect_lr(bool on) { impl_->fix_rect_lr = on; }
void Interpreter::set_scissor_edge_slack(int px) { impl_->scissor_slack = px; }
void Interpreter::set_merge_rect_slices(bool on) { impl_->merge_rect_slices = on; }
void Interpreter::set_hud_camera_id(uint32_t id) { impl_->hud_camera_id = id; }
void Interpreter::set_merge_image_tiles(bool on) { impl_->image_tiles = on; }
void Interpreter::set_screen(const ScreenGeometry& geo) {
    impl_->geo = geo;
    impl_->screen_w = geo.w;
    impl_->screen_h = geo.h;
    const int16_t full[4] = { 0, 0, (int16_t)geo.w, (int16_t)geo.h };
    memcpy(impl_->scissor, full, sizeof(full));
    const float cx = (float)geo.w * 0.5f, cy = (float)geo.h * 0.5f;
    impl_->vp = Impl::Viewport{ { cx, -cy, 511 }, { cx, cy, 511 }, cx };
    impl_->inst_dirty = true;
}

}   // namespace rt64_3ds
