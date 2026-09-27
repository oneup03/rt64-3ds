// F3DEX2 + extended GBI interpreter. See rt64_3ds_dl.h.
//
// RDRAM is the recompiled runtime's byte-swapped image: byte a lives at
// [a ^ 3], halfword a at [a ^ 2], aligned words in host order.
#include "rt64_3ds_dl.h"

#include <cmath>
#include <cstdlib>
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
    uint16_t dxt = 0;        // LOADBLOCK row step (1.11 per 64-bit word)
};

struct Light { float dir[3]; uint8_t col[3]; };

struct Mtx { float m[4][4]; };

constexpr int kMaxDlDepth = 18;

// 1 for opcodes that may change the state a draw snapshots.
struct StateOpTable {
    uint8_t v[256];
    constexpr StateOpTable() : v{} {
        for (int i = 0; i < 256; i++) v[i] = 1;
        for (int op : { G_TRI1, G_TRI2, G_QUAD, G_VTX, G_MODIFYVTX, G_DL, G_ENDDL, G_CULLDL, G_BRANCH_Z, G_NOOP,
                        G_RDPPIPESYNC, G_RDPLOADSYNC, G_RDPTILESYNC, G_MTX, G_POPMTX, G_MOVEMEM, G_RDPHALF_1, G_RDPHALF_2 })
            v[op] = 0;
    }
};
constexpr StateOpTable kStateOpTable{};
#define kStateOp kStateOpTable.v
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
    // Loaded vertices, already in N64 screen homogeneous space (the RSP
    // applies the viewport at load time): x/w, y/w are pixels (y down),
    // z/w in [0, 1]; u/v are texel coordinates after the G_TEXTURE scale.
    struct { float x, y, z, w; float u, v; uint8_t r, g, b, a; } vtx[kMaxVerts]{};
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
    uint16_t tlut[256]{};
    uint32_t tlut_hash = 1;
    uint32_t tlut_frame_hash = 0, tlut_frame_index = 0;
    uint32_t state_seq = 1;    // bumped by every command that changes draw state
    uint32_t last_draw_seq = 0;
    int16_t rect_align[4]{};   // left/top/right/bottom offsets (extended)
    uint32_t rect_lorigin = 0x800, rect_rorigin = 0x800;
    uint8_t rect_aspect = 0;

    // Framebuffer effects: the game copies the front buffer into a storage
    // buffer in RDRAM with texture rectangles, then draws with it. Draws
    // into the storage are dropped; the first one requests a GPU snapshot of
    // the last presented frame, and loads from the storage sample it.
    uint32_t vi_fbs[4] = {};
    int vi_fb_next = 0;
    uint32_t snapshot_base = 0;         // storage buffer that holds the snapshot
    bool snapshot_valid = false;
    int snapshot_x_offset = 40;         // where N64 x = 0 sits on the 400 px screen

    bool is_vi_framebuffer(uint32_t a) const {
        for (uint32_t fb : vi_fbs) if (fb != 0 && a >= fb && a < fb + 320 * 240 * 2) return true;
        return false;
    }

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
    void update_light_dirs() {
        // Light and look-at directions rotated by the modelview (transposed
        // product, as the RSP effectively does) so they can be dotted with
        // model-space normals; the vertex normals themselves are used as the
        // RSP uses them, unnormalised, scaled by 1/127.
        const Mtx& mv = mv_stack[mv_depth];
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
        last_vtx_addr = addr;
        update_mvp();
        const bool lit = (geometry_mode & G_LIGHTING) != 0;
        const bool texgen = (geometry_mode & G_TEXTURE_GEN) != 0;
        const bool fogged = (geometry_mode & G_FOG) != 0;
        if ((lit || texgen) && ldir_dirty) update_light_dirs();
        const float uscale = (float)texture.scaleS / (65536.0f * 32.0f), vscale = (float)texture.scaleT / (65536.0f * 32.0f);
        // Model -> clip -> screen in one matrix: the viewport is a scale and
        // translate of x/w and y/w (y negated: screen y grows downward) and
        // z' = (z + w) / 2, all linear in homogeneous coordinates.
        const float sx = vp.scale[0], sy = -vp.scale[1], tx = vp.trans[0], ty = vp.trans[1];
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
        for (int i = lo; i < hi; i++) {
            const uint8_t* src = rdram + ((addr + i * 16) & 0xFFFFFC);
            // RDRAM is stored in 32-bit little-endian words: halves are at ^2, bytes at ^3.
            const float ob0 = (float)*(const int16_t*)(src + 2), ob1 = (float)*(const int16_t*)(src + 0), ob2 = (float)*(const int16_t*)(src + 6);
            const int16_t tc0 = *(const int16_t*)(src + 10), tc1 = *(const int16_t*)(src + 8);
            const uint8_t c0 = src[15], c1 = src[14], c2 = src[13], c3 = src[12];
            auto& v = vtx[v0 + i];
            v.x = ob0 * m[0][0] + ob1 * m[1][0] + ob2 * m[2][0] + m[3][0];
            v.y = ob0 * m[0][1] + ob1 * m[1][1] + ob2 * m[2][1] + m[3][1];
            v.z = ob0 * m[0][2] + ob1 * m[1][2] + ob2 * m[2][2] + m[3][2];
            v.w = ob0 * m[0][3] + ob1 * m[1][3] + ob2 * m[2][3] + m[3][3];
            v.u = (float)tc0 * uscale;
            v.v = (float)tc1 * vscale;
            if (lit) {
                // Integer lighting, as the RSP does it: the unnormalised
                // signed normal dotted with 16.16 unit directions.
                const int32_t nx = (int8_t)c0, ny = (int8_t)c1, nz = (int8_t)c2;
                int32_t r = amb0, g = amb1, b = amb2;
                for (int l = 0; l < num_lights; l++) {
                    int32_t d = nx * ldir_i[l][0] + ny * ldir_i[l][1] + nz * ldir_i[l][2];
                    if (d > 0) { r += d * lights[l].col[0]; g += d * lights[l].col[1]; b += d * lights[l].col[2]; }
                }
                r >>= 16; g >>= 16; b >>= 16;
                v.r = (uint8_t)(r > 255 ? 255 : r);
                v.g = (uint8_t)(g > 255 ? 255 : g);
                v.b = (uint8_t)(b > 255 ? 255 : b);
                v.a = c3;
                if (texgen) {
                    float fx = (float)nx, fy = (float)ny, fz = (float)nz;
                    float dx = fx * lk[0][0] + fy * lk[0][1] + fz * lk[0][2];
                    float dy = fx * lk[1][0] + fy * lk[1][1] + fz * lk[1][2];
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
                v.r = c0; v.g = c1; v.b = c2; v.a = c3;
            }
            if (fogged) {
                // Fog factor into alpha: (z/w * mul + off) / 255, as the RSP
                // does, with z/w the clip-space depth = 2 z' / w - 1.
                float zw = v.w != 0.0f ? 2.0f * v.z / v.w - 1.0f : 0.0f;
                if (zw < -1) zw = -1; if (zw > 1) zw = 1;
                float f = zw * fm + fo;
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
        d.snapshot = snapshot_valid && d.addr >= snapshot_base && d.addr < snapshot_base + 320 * 240 * 2;
        d.fmt = tl.fmt;
        d.siz = tl.siz;
        d.width = (uint16_t)w;
        d.height = (uint16_t)h;
        d.masks = tl.masks; d.maskt = tl.maskt; d.cms = tl.cms; d.cmt = tl.cmt;
        d.palette = tl.palette;
        d.nibble = (tl.siz == 0) ? best->nibble : 0;
        d.block = best->block;
        d.dxt = best->dxt;
        d.load_word = (uint16_t)(tl.tmem - best->tmem);
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

    // TEXEL1's coordinates relative to TEXEL0's (tile t0 and the next one).
    void set_uv1(DrawRecord& r, int t0) const {
        const Tile& a = tiles[t0 & 7];
        const Tile& b = tiles[(t0 + 1) & 7];
        float k_u = shift_scale(b.shifts) / shift_scale(a.shifts), k_v = shift_scale(b.shiftt) / shift_scale(a.shiftt);
        r.uv1[0] = k_u; r.uv1[1] = k_v;
        r.uv1[2] = (float)(a.uls >> 2) * k_u - (float)(b.uls >> 2);
        r.uv1[3] = (float)(a.ult >> 2) * k_v - (float)(b.ult >> 2);
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
        r.dbg_vtx = last_vtx_addr;
        set_uv1(r, texture.tile);
        r.dbg_vp[0] = vp.scale[0]; r.dbg_vp[1] = vp.scale[1]; r.dbg_vp[2] = vp.trans[0]; r.dbg_vp[3] = vp.trans[1];
        r.dbg_proj[0] = proj.m[1][1]; r.dbg_proj[1] = proj.m[3][1]; r.dbg_proj[2] = proj.m[2][3]; r.dbg_proj[3] = proj.m[3][3];
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
            if (last.kind == DrawRecord::Tris && last.first + last.count == out->verts.size()) {
                if (last_draw_seq == state_seq) {
                    r = &last;
                }
                else {
                    // State commands ran since the last triangle; if they left
                    // the same state (re-sent textures and modes are common)
                    // the record continues, sparing a draw call.
                    DrawRecord probe;
                    probe.kind = DrawRecord::Tris;
                    snapshot(probe);
                    int t0 = texture.tile & 7;
                    if (uses_texel(0)) describe_tile(t0, probe.tex[0]);
                    if (uses_texel(1)) describe_tile((t0 + 1) & 7, probe.tex[1]);
                    if (same_draw_state(probe, last)) {
                        last_draw_seq = state_seq;
                        r = &last;
                        stats->draws_merged++;
                    }
                }
            }
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
        const auto& a = vtx[i0]; const auto& b = vtx[i1]; const auto& c = vtx[i2];
        if (geometry_mode & (G_CULL_FRONT | G_CULL_BACK)) {
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
            if ((geometry_mode & G_CULL_BACK) && cross > 0.0f) return;
            if ((geometry_mode & G_CULL_FRONT) && cross < 0.0f) return;
        }
        Vtx3ds* o = out->verts.append(3);
        if (o == nullptr) return;
        const Tile& tl = tiles[texture.tile & 7];
        const float uls = (float)(tl.uls >> 2), ult = (float)(tl.ult >> 2);
        const float ss = shift_scale(tl.shifts), st = shift_scale(tl.shiftt);
        const bool snap = r->tex[0].snapshot;
        for (int k = 0; k < 3; k++) {
            const auto& v = k == 0 ? a : (k == 1 ? b : c);
            o[k].x = v.x; o[k].y = v.y; o[k].z = v.z; o[k].w = v.w;
            o[k].u = v.u * ss - uls;
            o[k].v = v.v * st - ult;
            if (snap) snapshot_uv(r->tex[0], o[k].u, o[k].v, o[k].u, o[k].v);
            o[k].r = v.r; o[k].g = v.g; o[k].b = v.b; o[k].a = v.a;
        }
        r->count += 3;
        stats->tris++;
    }

    // Texel coordinates relative to a tile that reads the stored frame ->
    // snapshot texture texels. The snapshot is the rotated 240x400 colour
    // buffer copied into a 256x512 texture: its u axis runs along screen y
    // (bottom row first), its v axis along screen x.
    void snapshot_uv(const TexDesc& d, float u, float v, float& su, float& sv) const {
        uint32_t texel = (d.addr - snapshot_base) / 2;
        float x0 = (float)(texel % 320), y0 = (float)(texel / 320);
        float sx = x0 + u + (float)snapshot_x_offset;     // screen pixel
        float sy = y0 + v;
        su = 240.0f - sy;
        sv = sx;
    }

    static bool same_tex(const TexDesc& a, const TexDesc& b) {
        if (a.valid != b.valid) return false;
        if (!a.valid) return true;
        return a.addr == b.addr && a.pitch == b.pitch && a.fmt == b.fmt && a.siz == b.siz && a.width == b.width && a.height == b.height &&
               a.masks == b.masks && a.maskt == b.maskt && a.cms == b.cms && a.cmt == b.cmt && a.palette == b.palette &&
               a.nibble == b.nibble && a.tlut_hash == b.tlut_hash && a.tlut_mode == b.tlut_mode && a.bilerp == b.bilerp && a.snapshot == b.snapshot &&
               a.block == b.block && a.dxt == b.dxt && a.load_word == b.load_word;
    }
    static bool same_draw_state(const DrawRecord& a, const DrawRecord& b) {
        return a.cc_w0 == b.cc_w0 && a.cc_w1 == b.cc_w1 && a.othermode_h == b.othermode_h && a.othermode_l == b.othermode_l &&
               a.geometry_mode == b.geometry_mode && memcmp(a.prim, b.prim, 4) == 0 && memcmp(a.env, b.env, 4) == 0 &&
               memcmp(a.blend, b.blend, 4) == 0 && memcmp(a.fog, b.fog, 4) == 0 && a.prim_lod_frac == b.prim_lod_frac &&
               memcmp(a.scissor, b.scissor, sizeof(a.scissor)) == 0 && a.proj_id == b.proj_id && a.perspective == b.perspective &&
               memcmp(a.uv1, b.uv1, sizeof(a.uv1)) == 0 &&
               same_tex(a.tex[0], b.tex[0]) && same_tex(a.tex[1], b.tex[1]);
    }

    static float shift_scale(int s) {
        if (s == 0) return 1.0f;
        if (s <= 10) return 1.0f / (float)(1 << s);
        return (float)(1 << (16 - s));
    }

    // Rectangles are emitted in N64 pixels with w = 1.
    void emit_rect(float ulx, float uly, float lrx, float lry, bool textured, int tile, float s, float t, float dsdx, float dtdy, bool flip) {
        emit_rect_aligned(ulx, uly, lrx, lry, textured, tile, s, t, dsdx, dtdy, flip, rect_lorigin, rect_rorigin, rect_align);
    }
    void emit_rect_aligned(float ulx, float uly, float lrx, float lry, bool textured, int tile, float s, float t, float dsdx, float dtdy, bool flip,
                           uint32_t lorigin, uint32_t rorigin, const int16_t* align) {
        ulx += align[0] + origin_shift(lorigin); uly += align[1];
        lrx += align[2] + origin_shift(rorigin); lry += align[3];
        if (offscreen_target()) {
            // A copy of the front buffer into the storage: snapshot instead.
            if (textured) {
                TexDesc src;
                describe_tile(tile, src);
                if (src.valid && is_vi_framebuffer(src.addr)) {
                    if (!out->snapshot_request) stats->snapshots++;
                    out->snapshot_request = true;
                    snapshot_base = cimg;
                    snapshot_valid = true;
                }
            }
            return;
        }
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
            set_uv1(r, tile);
        }
        else if (cyc == 3) {
            memcpy(r.prim, fill, 4);   // fill mode: the fill colour rides in prim for the backend
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
            if (r.tex[0].snapshot) snapshot_uv(r.tex[0], q[i].u, q[i].v, q[i].u, q[i].v);
            q[i].r = q[i].g = q[i].b = q[i].a = 0;   // rectangles have no shade: SHADE reads as 0 (as RT64)
        }
        Vtx3ds* o = out->verts.append(6);
        if (o == nullptr) { out->draws.pop_back(); return; }
        o[0] = q[0]; o[1] = q[1]; o[2] = q[2];
        o[3] = q[0]; o[4] = q[2]; o[5] = q[3];
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
                apply_viewport_align();
                return 1;
            }
            case EX_SETSCISSOR: {
                // x edges are relative to an origin each (left/centre/right
                // of the N64 screen, or absolute).
                uint32_t lorigin = (w1 >> 2) & 0xFFF, rorigin = (w1 >> 14) & 0xFFF;
                uint32_t a = r32(pc + 8), b = r32(pc + 12);
                scissor[0] = (int16_t)(a >> 16) / 4 + origin_x(lorigin); scissor[1] = (int16_t)(a & 0xFFFF) / 4;
                scissor[2] = (int16_t)(b >> 16) / 4 + origin_x(rorigin); scissor[3] = (int16_t)(b & 0xFFFF) / 4;
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
                uint32_t lorigin = (w1 >> 3) & 0xFFF, rorigin = (w1 >> 15) & 0xFFF;
                uint32_t a = r32(pc + 8), b = r32(pc + 12), c = r32(pc + 16), d = r32(pc + 20);
                float ulx = (int16_t)(a >> 16) / 4.0f + origin_x(lorigin), uly = (int16_t)(a & 0xFFFF) / 4.0f;
                float lrx = (int16_t)(b >> 16) / 4.0f + origin_x(rorigin), lry = (int16_t)(b & 0xFFFF) / 4.0f;
                float s = (int16_t)(c >> 16) / 32.0f, t = (int16_t)(c & 0xFFFF) / 32.0f;
                float dsdx = (int16_t)(d >> 16) / 1024.0f, dtdy = (int16_t)(d & 0xFFFF) / 1024.0f;
                static const int16_t none[4] = { 0, 0, 0, 0 };
                emit_rect_aligned(ulx, uly, lrx, lry, true, tile, s, t, dsdx, dtdy, flip, 0x800, 0x800, none);
                return 2;
            }
            case EX_FILLRECT: {
                uint32_t lorigin = w1 & 0xFFF, rorigin = (w1 >> 12) & 0xFFF;
                uint32_t a = r32(pc + 8), b = r32(pc + 12);
                float ulx = (int16_t)(a >> 16) / 4.0f + origin_x(lorigin), uly = (int16_t)(a & 0xFFFF) / 4.0f;
                float lrx = (int16_t)(b >> 16) / 4.0f + origin_x(rorigin), lry = (int16_t)(b & 0xFFFF) / 4.0f;
                static const int16_t none[4] = { 0, 0, 0, 0 };
                emit_rect_aligned(ulx, uly, lrx, lry, false, 0, 0, 0, 0, 0, false, 0x800, 0x800, none);
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

    // gEX origins: 0 left, 0x200 centre, 0x400 right, 0x800 none. A
    // coordinate is relative to the origin's N64 position (0/160/320), and
    // the origin itself sits on the matching edge of the wide screen, whose
    // N64 x range is -40..360 here; "none" keeps the 4:3 placement.
    static int origin_x(uint32_t origin) {
        switch (origin) { case 0x000: return -40; case 0x200: return 160; case 0x400: return 360; default: return 0; }
    }
    static int origin_shift(uint32_t origin) {
        switch (origin) { case 0x000: return -40; case 0x400: return 40; default: return 0; }
    }

    float raw_vtrans_x = 640.0f;   // quarter pixels, as loaded
    void load_viewport(uint32_t addr) {
        vp.scale[0] = r16(addr + 0) / 4.0f;
        vp.scale[1] = r16(addr + 2) / 4.0f;
        vp.scale[2] = r16(addr + 4);
        raw_vtrans_x = (float)r16(addr + 8);
        vp.trans[1] = r16(addr + 10) / 4.0f;
        vp.trans[2] = r16(addr + 12);
        apply_viewport_align();
    }
    void apply_viewport_align() {
        // gEXSetViewportAlign: the translate is re-based on an origin and
        // shifted by an offset (quarter pixels); the origin then sits on the
        // wide screen's edge.
        float x = raw_vtrans_x;
        if (vp_origin < 0x800) x += (float)vp_origin * 320.0f * 4.0f / 1024.0f;
        vp.trans[0] = (x + (float)vp_align_x) / 4.0f + (float)origin_shift(vp_origin);
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
            stats->op_hist[op]++;
#ifndef __3DS__
            // Host tools: DL_TRACE=<first draw>,<last draw> prints the commands
            // that run while the frame has that many draws.
            static int trace_lo = -2, trace_hi = -1;
            if (trace_lo == -2) { trace_lo = -1; if (const char* e = getenv("DL_TRACE")) sscanf(e, "%d,%d", &trace_lo, &trace_hi); }
            if (trace_lo >= 0 && (int)out->draws.size() >= trace_lo && (int)out->draws.size() <= trace_hi)
                fprintf(stderr, "  [%zu] %08x: %02x %08x %08x\n", out->draws.size(), pc, op, w0, w1);
#endif
            uint32_t next = pc + 8;
            // Anything but drawing, vertex loads and list flow may change the
            // state a draw snapshots (rects bump it themselves).
            state_seq += kStateOp[op];
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
                    ldir_dirty = true;
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
                        ldir_dirty = true;
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
                case G_MOVEMEM: {
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
                    L.dxt = (uint16_t)(w1 & 0xFFF);
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

void Interpreter::note_presented_framebuffer(uint32_t addr) {
    for (uint32_t fb : impl_->vi_fbs) if (fb == addr) return;
    impl_->vi_fbs[impl_->vi_fb_next++ & 3] = addr;
}

void Interpreter::set_snapshot_layout(int screen_x_offset) {
    impl_->snapshot_x_offset = screen_x_offset;
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
