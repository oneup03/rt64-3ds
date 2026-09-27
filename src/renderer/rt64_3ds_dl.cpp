// F3DEX2 + extended GBI interpreter. See rt64_3ds_dl.h.
//
// RDRAM is the recompiled runtime's byte-swapped image: byte a lives at
// [a ^ 3], halfword a at [a ^ 2], aligned words in host order.
#include "rt64_3ds_dl.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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
};

struct Light { float dir[3]; uint8_t col[3]; };

struct Mtx { float m[4][4]; };

constexpr int kMaxDlDepth = 18;
constexpr int kMaxMtxStack = 32;
constexpr int kMaxVerts = 64;

}   // namespace

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
    struct Viewport { float scale[3], trans[3]; };
    Viewport vp{ { 160, -120, 511 }, { 160, 120, 511 } };
    Viewport vp_stack[8]{};
    int vp_depth = 0;
    int16_t vp_align_x = 0, vp_align_y = 0;
    uint16_t vp_origin = 0;
    struct { uint16_t scaleS, scaleT; uint8_t tile, level; bool on; } texture{ 0xFFFF, 0xFFFF, 0, 0, false };
    Light lights[8]{};
    uint8_t ambient[3]{};
    int num_lights = 0;
    float lookat[2][3]{};
    int16_t fog_mul = 0, fog_off = 0;
    uint32_t proj_id = 0;
    struct { float x, y, z, w; float u, v; uint8_t r, g, b, a; } vtx[kMaxVerts]{};
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
    uint16_t tlut[256]{};
    uint32_t tlut_hash = 1;
    uint32_t tlut_frame_hash = 0, tlut_frame_index = 0;
    uint32_t state_seq = 1;    // bumped by every command that changes draw state
    uint32_t last_draw_seq = 0;
    int16_t rect_align[4]{};   // left/top/right/bottom offsets (extended)
    uint16_t rect_lorigin = 0x800, rect_rorigin = 0x800;
    uint8_t rect_aspect = 0;

    // ---- memory
    uint8_t r8(uint32_t a) const { return rdram[(a & 0xFFFFFF) ^ 3]; }
    int16_t r16(uint32_t a) const { return *(const int16_t*)(rdram + ((a & 0xFFFFFE) ^ 2)); }
    uint16_t ru16(uint32_t a) const { return *(const uint16_t*)(rdram + ((a & 0xFFFFFE) ^ 2)); }
    uint32_t r32(uint32_t a) const { return *(const uint32_t*)(rdram + (a & 0xFFFFFC)); }

    uint32_t seg(uint32_t a) const {
        if (extended_rdram && (a & 0x80000000)) return a & 0xFFFFFF;
        return (segments[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF;
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
    void update_mvp() {
        if (mvp_dirty) {
            mul(mvp, mv_stack[mv_depth], proj);
            mvp_dirty = false;
        }
    }

    // ---- vertices
    void load_vertices(uint32_t addr, int n, int v0) {
        update_mvp();
        const Mtx& mv = mv_stack[mv_depth];
        float ldir[8][3];
        bool lit = (geometry_mode & G_LIGHTING) != 0;
        if (lit) {
            for (int i = 0; i < num_lights; i++) {
                // Light directions are rotated by the modelview (transposed
                // product, as the RSP effectively does) so they can be dotted
                // with model-space normals.
                float d[3];
                for (int k = 0; k < 3; k++) d[k] = lights[i].dir[0] * mv.m[k][0] + lights[i].dir[1] * mv.m[k][1] + lights[i].dir[2] * mv.m[k][2];
                float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len > 0.0f) { d[0] /= len; d[1] /= len; d[2] /= len; }
                ldir[i][0] = d[0]; ldir[i][1] = d[1]; ldir[i][2] = d[2];
            }
        }
        float lk[2][3];
        bool texgen = (geometry_mode & G_TEXTURE_GEN) != 0;
        if (texgen) {
            for (int i = 0; i < 2; i++) {
                float d[3];
                for (int k = 0; k < 3; k++) d[k] = lookat[i][0] * mv.m[k][0] + lookat[i][1] * mv.m[k][1] + lookat[i][2] * mv.m[k][2];
                float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len > 0.0f) { d[0] /= len; d[1] /= len; d[2] /= len; }
                lk[i][0] = d[0]; lk[i][1] = d[1]; lk[i][2] = d[2];
            }
        }
        for (int i = 0; i < n; i++) {
            int dst = v0 + i;
            if (dst < 0 || dst >= kMaxVerts) continue;
            uint32_t a = addr + i * 16;
            float ob[3] = { (float)r16(a), (float)r16(a + 2), (float)r16(a + 4) };
            int16_t tc0 = r16(a + 8), tc1 = r16(a + 10);
            uint8_t cn[4] = { r8(a + 12), r8(a + 13), r8(a + 14), r8(a + 15) };
            auto& v = vtx[dst];
            v.x = ob[0] * mvp.m[0][0] + ob[1] * mvp.m[1][0] + ob[2] * mvp.m[2][0] + mvp.m[3][0];
            v.y = ob[0] * mvp.m[0][1] + ob[1] * mvp.m[1][1] + ob[2] * mvp.m[2][1] + mvp.m[3][1];
            v.z = ob[0] * mvp.m[0][2] + ob[1] * mvp.m[1][2] + ob[2] * mvp.m[2][2] + mvp.m[3][2];
            v.w = ob[0] * mvp.m[0][3] + ob[1] * mvp.m[1][3] + ob[2] * mvp.m[2][3] + mvp.m[3][3];
            // Texture coordinates in texels (10.5 * 0.16 scale / 32).
            v.u = (float)tc0 * (float)texture.scaleS / 65536.0f / 32.0f;
            v.v = (float)tc1 * (float)texture.scaleT / 65536.0f / 32.0f;
            if (lit) {
                float nx = (int8_t)cn[0], ny = (int8_t)cn[1], nz = (int8_t)cn[2];
                float len = sqrtf(nx * nx + ny * ny + nz * nz);
                if (len > 0.0f) { nx /= len; ny /= len; nz /= len; }
                float c[3] = { (float)ambient[0], (float)ambient[1], (float)ambient[2] };
                for (int l = 0; l < num_lights; l++) {
                    float d = nx * ldir[l][0] + ny * ldir[l][1] + nz * ldir[l][2];
                    if (d > 0.0f) { c[0] += d * lights[l].col[0]; c[1] += d * lights[l].col[1]; c[2] += d * lights[l].col[2]; }
                }
                v.r = (uint8_t)(c[0] > 255.0f ? 255 : c[0]);
                v.g = (uint8_t)(c[1] > 255.0f ? 255 : c[1]);
                v.b = (uint8_t)(c[2] > 255.0f ? 255 : c[2]);
                v.a = cn[3];
                if (texgen) {
                    float dx = nx * lk[0][0] + ny * lk[0][1] + nz * lk[0][2];
                    float dy = nx * lk[1][0] + ny * lk[1][1] + nz * lk[1][2];
                    if (dx < -1) dx = -1; if (dx > 1) dx = 1;
                    if (dy < -1) dy = -1; if (dy > 1) dy = 1;
                    if (geometry_mode & G_TEXTURE_GEN_LINEAR) {
                        dx = acosf(-dx) / 3.14159265f;
                        dy = acosf(-dy) / 3.14159265f;
                    }
                    else {
                        dx = (dx + 1.0f) * 0.25f;
                        dy = (dy + 1.0f) * 0.25f;
                    }
                    v.u = dx * (float)texture.scaleS / 32.0f;
                    v.v = dy * (float)texture.scaleT / 32.0f;
                }
            }
            else {
                v.r = cn[0]; v.g = cn[1]; v.b = cn[2]; v.a = cn[3];
            }
            if (geometry_mode & G_FOG) {
                // Fog factor into alpha: (z/w * mul + off) / 255, as the RSP does.
                float zw = v.w != 0.0f ? v.z / v.w : 0.0f;
                if (zw < -1) zw = -1; if (zw > 1) zw = 1;
                float f = zw * fog_mul + fog_off;
                if (f < 0) f = 0; if (f > 255) f = 255;
                v.a = (uint8_t)f;
            }
        }
    }

    // ---- texture description from a tile
    void describe_tile(int t, TexDesc& d) {
        const Tile& tl = tiles[t & 7];
        d.valid = false;
        // Find the load whose TMEM range holds this tile.
        // The most recent load covering the tile's TMEM address wins.
        const TmemLoad* best = nullptr;
        for (int i = 0; i < 16; i++) {
            const TmemLoad& L = loads[i];
            if (!L.valid) continue;
            if (tl.tmem >= L.tmem && tl.tmem < L.tmem + L.words) {
                if (best == nullptr || L.seq > best->seq) best = &L;
            }
        }
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
        int w = ((tl.lrs >> 2) - (tl.uls >> 2)) + 1;
        int h = ((tl.lrt >> 2) - (tl.ult >> 2)) + 1;
        if (tl.masks != 0 && w > (1 << tl.masks)) w = 1 << tl.masks;
        if (tl.maskt != 0 && h > (1 << tl.maskt)) h = 1 << tl.maskt;
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
        d.fmt = tl.fmt;
        d.siz = tl.siz;
        d.width = (uint16_t)w;
        d.height = (uint16_t)h;
        d.masks = tl.masks; d.maskt = tl.maskt; d.cms = tl.cms; d.cmt = tl.cmt;
        d.palette = tl.palette;
        d.nibble = (tl.siz == 0) ? best->nibble : 0;
        d.tlut_mode = (othermode_h >> 14) & 3;
        d.tlut_hash = (tl.fmt == 2) ? tlut_hash : 0;
        d.tlut_index = 0;
        if (tl.fmt == 2) {
            // Palettes change within a frame: keep the one this draw uses.
            if (tlut_frame_hash != tlut_hash) {
                tlut_frame_hash = tlut_hash;
                tlut_frame_index = (uint32_t)out->tlut.size();
                out->tlut.insert(out->tlut.end(), tlut, tlut + 256);
            }
            d.tlut_index = tlut_frame_index;
        }
        d.bilerp = ((othermode_h >> 12) & 3) != 0;
    }

    void snapshot(DrawRecord& r) {
        r.cc_w0 = cc_w0; r.cc_w1 = cc_w1;
        r.othermode_h = othermode_h; r.othermode_l = othermode_l;
        r.geometry_mode = geometry_mode;
        memcpy(r.prim, prim, 4); memcpy(r.env, env, 4); memcpy(r.blend, blend, 4); memcpy(r.fog, fog, 4);
        r.prim_lod_frac = prim_lod_frac;
        memcpy(r.scissor, scissor, sizeof(scissor));
        r.proj_id = proj_id;
        r.perspective = proj.m[2][3] != 0.0f || proj.m[3][3] == 0.0f;
    }

    bool uses_texel(int which) const {
        // Any combiner input referencing TEXEL0/1 in either cycle.
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
        // effects) are not part of the visible frame.
        return cimg >= 0x800000;
    }

    void emit_tri(int i0, int i1, int i2) {
        if (offscreen_target()) return;
        if (i0 >= kMaxVerts || i1 >= kMaxVerts || i2 >= kMaxVerts) return;
        DrawRecord* r = nullptr;
        if (!out->draws.empty()) {
            DrawRecord& last = out->draws.back();
            if (last.kind == DrawRecord::Tris && last.first + last.count == out->verts.size() && last_draw_seq == state_seq) r = &last;
        }
        if (r == nullptr) {
            last_draw_seq = state_seq;
            out->draws.emplace_back();
            r = &out->draws.back();
            r->kind = DrawRecord::Tris;
            r->first = (uint32_t)out->verts.size();
            r->count = 0;
            snapshot(*r);
            int t0 = texture.tile & 7;
            if (uses_texel(0)) describe_tile(t0, r->tex[0]);
            if (uses_texel(1)) describe_tile((t0 + 1) & 7, r->tex[1]);
        }
        int idx[3] = { i0, i1, i2 };
        if (geometry_mode & (G_CULL_FRONT | G_CULL_BACK)) {
            // Screen-space winding (y down); a vertex behind the eye flips it.
            const auto& a = vtx[i0]; const auto& b = vtx[i1]; const auto& c = vtx[i2];
            float ax = a.x * b.w * c.w, ay = a.y * b.w * c.w;
            float bx = b.x * a.w * c.w, by = b.y * a.w * c.w;
            float cx = c.x * a.w * b.w, cy = c.y * a.w * b.w;
            float cross = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            float sign = (a.w * b.w * c.w) < 0.0f ? -1.0f : 1.0f;
            cross *= sign * vp.scale[0] * vp.scale[1];   // the viewport can mirror an axis
            if ((geometry_mode & G_CULL_BACK) && cross > 0.0f) return;
            if ((geometry_mode & G_CULL_FRONT) && cross < 0.0f) return;
        }
        const Tile& tl = tiles[texture.tile & 7];
        float uls = (float)(tl.uls >> 2), ult = (float)(tl.ult >> 2);
        float ss = shift_scale(tl.shifts), st = shift_scale(tl.shiftt);
        for (int k = 0; k < 3; k++) {
            const auto& v = vtx[idx[k]];
            Vtx3ds o;
            o.x = vp.trans[0] * v.w + vp.scale[0] * v.x;
            o.y = vp.trans[1] * v.w + vp.scale[1] * v.y;
            o.z = 0.5f * v.z + 0.5f * v.w;
            o.w = v.w;
            o.u = v.u * ss - uls;
            o.v = v.v * st - ult;
            o.r = v.r; o.g = v.g; o.b = v.b; o.a = v.a;
            out->verts.push_back(o);
        }
        r->count += 3;
        stats->tris++;
    }

    static float shift_scale(int s) {
        if (s == 0) return 1.0f;
        if (s <= 10) return 1.0f / (float)(1 << s);
        return (float)(1 << (16 - s));
    }

    // Rectangles are emitted in N64 pixels with w = 1.
    void emit_rect(float ulx, float uly, float lrx, float lry, bool textured, int tile, float s, float t, float dsdx, float dtdy, bool flip) {
        if (offscreen_target()) return;
        uint32_t cyc = (othermode_h >> 20) & 3;
        if (!textured && zimg != 0 && cimg == zimg) return;   // depth clear: the target is cleared per frame
        state_seq++;
        out->draws.emplace_back();
        DrawRecord& r = out->draws.back();
        r.kind = textured ? DrawRecord::TexRect : DrawRecord::FillRect;
        r.first = (uint32_t)out->verts.size();
        r.count = 6;
        snapshot(r);
        if (textured) {
            describe_tile(tile, r.tex[0]);
            if (cyc == 2 && uses_texel(1)) describe_tile((tile + 1) & 7, r.tex[1]);
        }
        else {
            memcpy(r.prim, fill, 4);   // the fill colour rides in prim for the backend
        }
        // Fill and copy modes cover the lower-right pixel too.
        if (cyc == 2 || cyc == 3) { lrx += 1.0f; lry += 1.0f; }
        float w = lrx - ulx, h = lry - uly;
        const Tile& tl = tiles[tile & 7];
        float uls = (float)(tl.uls >> 2), ult = (float)(tl.ult >> 2);
        float u0 = s - uls, v0 = t - ult;
        float u1 = u0 + dsdx * w, v1 = v0 + dtdy * h;
        if (cyc == 2) u1 = u0 + dsdx * w / 4.0f;   // copy mode: dsdx is in 4-texel units
        Vtx3ds q[4];
        float xs[4] = { ulx, lrx, lrx, ulx }, ys[4] = { uly, uly, lry, lry };
        float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
        if (flip) { us[1] = u0; vs[1] = v1; us[3] = u1; vs[3] = v0; }
        for (int i = 0; i < 4; i++) {
            q[i].x = xs[i]; q[i].y = ys[i]; q[i].z = 0.0f; q[i].w = 1.0f;
            q[i].u = us[i]; q[i].v = vs[i];
            q[i].r = q[i].g = q[i].b = q[i].a = 255;
        }
        out->verts.push_back(q[0]); out->verts.push_back(q[1]); out->verts.push_back(q[2]);
        out->verts.push_back(q[0]); out->verts.push_back(q[2]); out->verts.push_back(q[3]);
        stats->rects++;
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
            case EX_SETREFRESHRATE: case EX_SETNEARCLIPPING: case EX_SETTEXCOORDWRAPPOINT:
            case EX_FORCEUPSCALE2D: case EX_FORCETRUEBILERP: case EX_FORCESCALELOD: case EX_FORCEBRANCH:
            case EX_SETRENDERTORAM: case EX_SETDITHERNOISESTRENGTH: case EX_VERTEXZTEST: case EX_ENDVERTEXZTEST:
                return 0;
            case EX_MATRIXGROUP: {
                uint32_t id = w1;
                uint32_t f = r32(pc + 8);
                bool is_proj = (f >> 1) & 1;
                if (is_proj && id != 0) proj_id = id;
                return 1;
            }
            case EX_POPMATRIXGROUP: return 0;
            case EX_EDITGROUPBYADDRESS: return 1;
            case EX_SETRECTALIGN: {
                rect_lorigin = w1 & 0xFFF; rect_rorigin = (w1 >> 12) & 0xFFF;
                uint32_t a = r32(pc + 8), b = r32(pc + 12);
                rect_align[0] = (int16_t)(a >> 16); rect_align[1] = (int16_t)(a & 0xFFFF);
                rect_align[2] = (int16_t)(b >> 16); rect_align[3] = (int16_t)(b & 0xFFFF);
                return 1;
            }
            case EX_SETVIEWPORTALIGN: {
                vp_origin = w1 & 0xFFF;
                uint32_t a = r32(pc + 8);
                vp_align_x = (int16_t)(a >> 16); vp_align_y = (int16_t)(a & 0xFFFF);
                return 1;
            }
            case EX_SETSCISSOR: {
                uint32_t a = r32(pc + 8), b = r32(pc + 12);
                scissor[0] = (int16_t)(a >> 16) / 4; scissor[1] = (int16_t)(a & 0xFFFF) / 4;
                scissor[2] = (int16_t)(b >> 16) / 4; scissor[3] = (int16_t)(b & 0xFFFF) / 4;
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
                if (vp_depth > 0) { vp = vp_stack[--vp_depth]; }
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
                uint32_t a = r32(pc + 8), b = r32(pc + 12), c = r32(pc + 16), d = r32(pc + 20);
                float ulx = (int16_t)(a >> 16) / 4.0f, uly = (int16_t)(a & 0xFFFF) / 4.0f;
                float lrx = (int16_t)(b >> 16) / 4.0f, lry = (int16_t)(b & 0xFFFF) / 4.0f;
                float s = (int16_t)(c >> 16) / 32.0f, t = (int16_t)(c & 0xFFFF) / 32.0f;
                float dsdx = (int16_t)(d >> 16) / 1024.0f, dtdy = (int16_t)(d & 0xFFFF) / 1024.0f;
                emit_rect(ulx, uly, lrx, lry, true, tile, s, t, dsdx, dtdy, flip);
                return 2;
            }
            case EX_FILLRECT: {
                uint32_t a = r32(pc + 8), b = r32(pc + 12);
                float ulx = (int16_t)(a >> 16) / 4.0f, uly = (int16_t)(a & 0xFFFF) / 4.0f;
                float lrx = (int16_t)(b >> 16) / 4.0f, lry = (int16_t)(b & 0xFFFF) / 4.0f;
                emit_rect(ulx, uly, lrx, lry, false, 0, 0, 0, 0, 0, false);
                return 1;
            }
            case EX_VERTEX: return 1;
            case EX_MATRIX_FLOAT: case EX_SETVERTEXSEGMENT: return 1;
            case EX_PUSHOTHERMODE: case EX_POPOTHERMODE: case EX_PUSHCOMBINE: case EX_POPCOMBINE:
            case EX_PUSHPROJMATRIX: case EX_POPPROJMATRIX: case EX_PUSHENVCOLOR: case EX_POPENVCOLOR:
            case EX_PUSHBLENDCOLOR: case EX_POPBLENDCOLOR: case EX_PUSHFOGCOLOR: case EX_POPFOGCOLOR:
            case EX_PUSHFILLCOLOR: case EX_POPFILLCOLOR: case EX_PUSHPRIMCOLOR: case EX_POPPRIMCOLOR:
            case EX_PUSHGEOMETRYMODE: case EX_POPGEOMETRYMODE: case EX_SETPROJMATRIXFLOAT: case EX_SETVIEWMATRIXFLOAT:
                return 0;
            default:
                stats->ex_unknown++;
                return 0;
        }
    }

    void load_viewport(uint32_t addr) {
        vp.scale[0] = r16(addr + 0) / 4.0f;
        vp.scale[1] = r16(addr + 2) / 4.0f;
        vp.scale[2] = r16(addr + 4);
        vp.trans[0] = r16(addr + 8) / 4.0f;
        vp.trans[1] = r16(addr + 10) / 4.0f;
        vp.trans[2] = r16(addr + 12);
    }

    // ---- main loop
    bool run(uint32_t data_ptr) {
        uint32_t stack[kMaxDlDepth];
        int depth = 0;
        uint32_t pc = seg(data_ptr);
        bool fullsync = false;
        for (uint32_t guard = 0; guard < 400000; guard++) {
            uint32_t w0 = r32(pc), w1 = r32(pc + 4);
            uint8_t op = w0 >> 24;
            stats->commands++;
            uint32_t next = pc + 8;
            // Anything but drawing, vertex loads and list flow may change the
            // state a draw snapshots (rects bump it themselves).
            if (!(op == G_TRI1 || op == G_TRI2 || op == G_QUAD || op == G_VTX || op == G_MODIFYVTX || op == G_DL || op == G_ENDDL ||
                  op == G_CULLDL || op == G_BRANCH_Z || op == G_NOOP || op == G_RDPPIPESYNC || op == G_RDPLOADSYNC || op == G_RDPTILESYNC ||
                  op == G_MTX || op == G_POPMTX || op == G_MOVEMEM || op == G_RDPHALF_1 || op == G_RDPHALF_2)) {
                state_seq++;
            }
            switch (op) {
                case G_NOOP: break;
                case G_SPNOOP:
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
                case G_MODIFYVTX: {
                    int where = (w0 >> 16) & 0xFF;
                    int v = (w0 & 0xFFFF) / 2;
                    if (v < kMaxVerts) {
                        if (where == 0x10) { vtx[v].r = w1 >> 24; vtx[v].g = w1 >> 16; vtx[v].b = w1 >> 8; vtx[v].a = w1; }
                        else if (where == 0x14) {
                            vtx[v].u = (float)(int16_t)(w1 >> 16) * (float)texture.scaleS / 65536.0f / 32.0f;
                            vtx[v].v = (float)(int16_t)(w1 & 0xFFFF) * (float)texture.scaleT / 65536.0f / 32.0f;
                        }
                    }
                    break;
                }
                case G_CULLDL: break;
                case G_BRANCH_Z: break;   // keep the detailed branch
                case G_TRI1:
                    emit_tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                    break;
                case G_TRI2: case G_QUAD:
                    emit_tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                    emit_tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                    break;
                case G_DMA_IO: case G_LOAD_UCODE: break;
                case G_TEXTURE:
                    texture.level = (w0 >> 11) & 7;
                    texture.tile = (w0 >> 8) & 7;
                    texture.on = (w0 & 0xFF) != 0;
                    texture.scaleS = w1 >> 16;
                    texture.scaleT = w1 & 0xFFFF;
                    break;
                case G_POPMTX: {
                    int n = (int)(w1 / 64);
                    mv_depth -= n;
                    if (mv_depth < 0) mv_depth = 0;
                    mvp_dirty = true;
                    break;
                }
                case G_GEOMETRYMODE:
                    geometry_mode = (geometry_mode & (w0 & 0xFFFFFF)) | w1;
                    break;
                case G_MTX: {
                    uint8_t p = (w0 & 0xFF) ^ 1;   // F3DEX2 inverts the push bit
                    Mtx m;
                    load_mtx(m, seg(w1));
                    if (p & 4) {
                        if (p & 2) proj = m; else mul(proj, m, proj);
                    }
                    else {
                        if (p & 1) {
                            if (mv_depth + 1 < kMaxMtxStack) { mv_stack[mv_depth + 1] = mv_stack[mv_depth]; mv_depth++; }
                        }
                        if (p & 2) mv_stack[mv_depth] = m; else mul(mv_stack[mv_depth], m, mv_stack[mv_depth]);
                    }
                    mvp_dirty = true;
                    break;
                }
                case G_MOVEWORD: {
                    uint8_t index = (w0 >> 16) & 0xFF;
                    uint16_t offset = w0 & 0xFFFF;
                    switch (index) {
                        case 0x06: segments[(offset / 4) & 0xF] = w1 & 0xFFFFFF; break;
                        case 0x08: fog_mul = (int16_t)(w1 >> 16); fog_off = (int16_t)(w1 & 0xFFFF); break;
                        case 0x02: num_lights = (int)(w1 / 24); if (num_lights > 7) num_lights = 7; break;
                        case 0x0A: {
                            int li = offset / 24;
                            if (li < 8) { lights[li].col[0] = w1 >> 24; lights[li].col[1] = w1 >> 16; lights[li].col[2] = w1 >> 8; }
                            break;
                        }
                        default: break;
                    }
                    break;
                }
                case G_MOVEMEM: {
                    uint8_t index = w0 & 0xFF;
                    uint32_t offset = ((w0 >> 8) & 0xFF) * 8;
                    uint32_t addr = seg(w1);
                    if (index == 8) {            // G_MV_VIEWPORT
                        load_viewport(addr);
                    }
                    else if (index == 10) {      // G_MV_LIGHT
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
                case G_DL:
                    if (((w0 >> 16) & 0xFF) == 0) {
                        if (depth < kMaxDlDepth) stack[depth++] = next; else stats->dl_depth_overflow++;
                    }
                    next = seg(w1);
                    break;
                case G_ENDDL:
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
                    break;
                case G_SETPRIMDEPTH: break;
                case G_LOADTLUT: {
                    int tile = (w1 >> 24) & 7;
                    int count = ((w1 >> 14) & 0x3FF) + 1;
                    const Tile& tl = tiles[tile];
                    int base = ((int)tl.tmem * 8 - 0x800) / 2;
                    uint32_t src = timg.addr + ((tl.uls >> 2) * 2);
                    uint32_t h = 2166136261u;
                    for (int i = 0; i < count; i++) {
                        int idx = base + i;
                        uint16_t v = ru16(src + i * 2);
                        if (idx >= 0 && idx < 256) tlut[idx] = v;
                        h = (h ^ v) * 16777619u;
                    }
                    tlut_hash = h;
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
                    L.seq = load_count;
                    L.nibble = (timg.siz == 0) ? ((uls >> 2) & 1) : 0;
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
                    cimg = seg(w1); cimg_width = (w0 & 0xFFF) + 1;
                    out->color_image = cimg; out->color_width = cimg_width; out->depth_image = zimg;
                    break;
                default:
                    stats->unknown++;
                    break;
            }
            pc = next;
        }
        return fullsync;
    }
};

Interpreter::Interpreter(uint8_t* rdram) : impl_(new Impl()) {
    impl_->rdram = rdram;
    impl_->stats = &stats_;
    Impl::identity(impl_->proj);
    Impl::identity(impl_->mv_stack[0]);
}

bool Interpreter::run(uint32_t data_ptr, FrameRecord& out) {
    impl_->out = &out;
    // Per-task RSP state that must not leak between lists.
    impl_->mv_depth = 0;
    impl_->mvp_dirty = true;
    impl_->vp_depth = 0;
    impl_->scissor_depth = 0;
    impl_->load_count = 0;
    impl_->tlut_frame_hash = 0;
    for (auto& L : impl_->loads) L.valid = false;
    return impl_->run(data_ptr);
}

}   // namespace rt64_3ds
