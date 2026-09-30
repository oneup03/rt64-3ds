// F3DEX2 / F3DEX + extended GBI interpreter. See rt64_3ds_dl.h.
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

// ---- F3DEX 1.x (GBI1) RSP opcodes, the ones whose number or encoding
// differs from F3DEX2's. translate_f3dex() turns each into its F3DEX2 form
// (or runs it) so one decoder serves both; the RDP opcodes are shared.
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

// 1 for opcodes that may change what describe_tile returns (tiles, loads,
// palettes, the texture tile index, texture filter/TLUT modes, and the
// combiner inputs that decide which texels are used).
struct TexOpTable {
    uint8_t v[256];
    constexpr TexOpTable() : v{} {
        for (int op : { G_SETTILE, G_SETTILESIZE, G_LOADBLOCK, G_LOADTILE, G_LOADTLUT, G_SETOTHERMODE_H, G_RDPSETOTHERMODE,
                        G_TEXTURE, G_SETCOMBINE })
            v[op] = 1;
    }
};
constexpr TexOpTable kTexOpTable{};
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
    // group sets the top. RT64 reads gEXPopMatrixGroup's projection flag from
    // a bit that is always clear, so pops only ever reach the modelview stack;
    // mirrored, since the desktop stereo was tuned against that.
    static constexpr int kGroupStack = 16;
    static constexpr uint32_t kIdAuto = 0xFFFFFFFFu;
    uint32_t proj_groups[kGroupStack] = {}, mv_groups[kGroupStack] = {};
    uint8_t proj_aspects[kGroupStack] = {};   // G_EX_ASPECT_* of each projection group
    int proj_group_depth = 0, mv_group_depth = 0;
    uint32_t mv_seq = 0;
    uint32_t world_proj_ids[4] = { 5, 5, 5, 5 };
    bool is_world_proj(uint32_t id) const {
        return id == world_proj_ids[0] || id == world_proj_ids[1] || id == world_proj_ids[2] || id == world_proj_ids[3];
    }
    bool split_untagged_ortho = false;
    bool force_branch_z = true;        // RenderDesc::force_lod_branch, or gEXForceBranch
    Gbi gbi = Gbi::F3DEX2;
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
    uint32_t tlut_hash = 1;
    uint32_t tlut_frame_hash = 0, tlut_frame_index = 0;
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
            if (uses_texel(0)) describe_tile(t0, cached_tex[0]);
            if (uses_texel(1)) describe_tile((t0 + 1) & 7, cached_tex[1]);
            cached_tex_seq = tex_seq;
            cached_tile = t0;
        }
        return cached_tex;
    }
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

    // Render to texture (RenderDesc::render_to_texture): a colour image that
    // is neither a presented framebuffer nor the depth buffer is drawn into
    // a GPU texture of its own, which later loads from that image sample.
    bool rtt_enabled = false;
    RttImage rtt_images[kMaxRtt];
    int rtt_count = 0, rtt_next = 0;
    int cur_rtt = -1;                   // the image the current colour image draws into, or -1
    bool is_presented_fb(uint32_t a) const {
        for (uint32_t fb : vi_fbs) if (fb != 0 && a == fb) return true;
        return false;
    }
    int rtt_for_cimg(uint32_t a, uint32_t width) {
        if (!rtt_enabled || a == zimg) return -1;
        // Known once two framebuffers have been presented (a double-buffered
        // game's second one is drawn before it is ever shown).
        const bool known = vi_fbs[0] != 0 && vi_fbs[1] != 0;
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
#ifdef DL_BENCH_SKIP_VTX
        return;
#endif
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
        // Reads of the stored frame are 16-bit tile loads out of a
        // screen-wide image; anything else at that address is ordinary
        // texture data (the storage may be heap memory the game reuses later).
        d.snapshot = snapshot_valid && d.addr >= snapshot_base && d.addr < snapshot_base + (uint32_t)(screen_w * screen_h * 2) &&
                     !best->block && best->pitch == (uint32_t)screen_w * 2 && tl.siz == 2;
        d.rtt = -1;
        if (rtt_enabled && !d.snapshot && !best->block && tl.siz == 2) {
            for (int i = 0; i < rtt_count; i++) {
                const RttImage& img = rtt_images[i];
                if (d.addr >= img.addr && d.addr < img.addr + (uint32_t)img.width * 2 * img.pot_h && best->pitch == (uint32_t)img.width * 2) {
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
        d.tlut_hash = (d.fmt == 2) ? tlut_hash : 0;
        d.tlut_index = 0;
        if (d.fmt == 2) {
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
    void compute_uv1(float* uv1, int t0) const {
        const Tile& a = tiles[t0 & 7];
        const Tile& b = tiles[(t0 + 1) & 7];
        float k_u = shift_scale(b.shifts) / shift_scale(a.shifts), k_v = shift_scale(b.shiftt) / shift_scale(a.shiftt);
        uv1[0] = k_u; uv1[1] = k_v;
        uv1[2] = (float)(a.uls >> 2) * k_u - (float)(b.uls >> 2);
        uv1[3] = (float)(a.ult >> 2) * k_v - (float)(b.ult >> 2);
    }
    void set_uv1(DrawRecord& r, int t0) const { compute_uv1(r.uv1, t0); }

    void snapshot(DrawRecord& r) {
        r.cc_w0 = cc_w0; r.cc_w1 = cc_w1;
        r.othermode_h = othermode_h; r.othermode_l = othermode_l;
        r.geometry_mode = geometry_mode;
        memcpy(r.prim, prim, 4); memcpy(r.env, env, 4); memcpy(r.blend, blend, 4); memcpy(r.fog, fog, 4);
        r.prim_lod_frac = prim_lod_frac;
        effective_scissor(r.scissor, r.kind == DrawRecord::Tris && proj_stretch());
        r.proj_id = proj_id;
        r.rtt = (int8_t)cur_rtt;
        r.perspective = proj.m[2][3] != 0.0f || proj.m[3][3] == 0.0f;
        r.mv_auto = mv_groups[mv_group_depth] == kIdAuto;
        r.mv_seq = mv_seq;
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
        // effects) are not part of the visible frame; with render to texture,
        // every off-screen image is, and so is colour drawn into the depth
        // buffer's memory (Banjo stores the paused frame there): its copy
        // of the front buffer becomes a snapshot.
        return rtt_enabled ? (cur_rtt >= 0 || (zimg != 0 && cimg == zimg)) : cimg >= 0x800000;
    }

    void emit_tri(int i0, int i1, int i2) {
#ifdef DL_BENCH_SKIP_TRI
        return;
#endif
        if (offscreen_target() && cur_rtt < 0) return;
        if (i0 >= kMaxVerts || i1 >= kMaxVerts || i2 >= kMaxVerts) return;
        DrawRecord* r = nullptr;
        if (!out->draws.empty()) {
            DrawRecord& last = out->draws.back();
            // Reticle candidates (see set_split_untagged_ortho) end at a
            // modelview load.
            bool split = split_untagged_ortho && !last.perspective && last.mv_auto && last.mv_seq != mv_seq;
            if (last.kind == DrawRecord::Tris && last.first + last.count == out->verts.size() && !split) {
                if (last_draw_seq == state_seq) {
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
                    if (same_as_last(last, current_textures())) {
                        last_draw_seq = state_seq;
                        r = &last;
                        stats->draws_merged++;
                    }
                }
            }
        }
        if (r == nullptr) {
            last_draw_seq = state_seq;
            const TexDesc* tex = current_textures();
            out->draws.emplace_back();
            r = &out->draws.back();
            r->kind = DrawRecord::Tris;
            r->first = (uint32_t)out->verts.size();
            r->count = 0;
            snapshot(*r);
            r->tex[0] = tex[0];
            r->tex[1] = tex[1];
        }
#ifdef DL_BENCH_NO_EMIT
        return;
#endif
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
        const Tile& tl = tiles[texture.tile & 7];
        const float uls = (float)(tl.uls >> 2), ult = (float)(tl.ult >> 2);
        const float ss = shift_scale(tl.shifts), st = shift_scale(tl.shiftt);
        const bool snap = r->tex[0].snapshot;
        const bool zprim = (othermode_l & G_ZS_PRIM) != 0;
        Vtx3ds tri[3];
        for (int k = 0; k < 3; k++) {
            const auto& v = k == 0 ? a : (k == 1 ? b : c);
            Vtx3ds& t = tri[k];
            t.x = v.x; t.y = v.y; t.z = zprim ? prim_z() * v.w : v.z; t.w = v.w;
            t.u = v.u * ss - uls;
            t.v = v.v * st - ult;
            if (snap) snapshot_uv(r->tex[0], t.u, t.v, t.u, t.v, proj_stretch());
            else if (r->tex[0].rtt >= 0) rtt_uv(r->tex[0], t.u, t.v);
            t.r = v.r; t.g = v.g; t.b = v.b; t.a = v.a;
        }
        stats->tris++;
        if (!inside_guard_band(tri[0]) || !inside_guard_band(tri[1]) || !inside_guard_band(tri[2])) {
            emit_clipped(*r, tri);
            return;
        }
        Vtx3ds* o = out->verts.append(3);
        if (o == nullptr) return;
        o[0] = tri[0]; o[1] = tri[1]; o[2] = tri[2];
        r->count += 3;
    }

    // The PICA rasterises in fixed point over a limited range: a triangle
    // with a corner far off screen (or behind the eye) is dropped on the
    // console, where emulators draw it. Such triangles are clipped here to a
    // guard band around the screen and to the near plane (z' >= 0), in
    // homogeneous screen space, then drawn as a fan.
    static constexpr float kGuard = 1024.0f;   // pixels beyond the screen centre, each axis
    bool inside_guard_band(const Vtx3ds& v) const {
        if (!(v.w > 0.0f) || v.z < 0.0f) return false;
        const float gx = kGuard * v.w;
        const float dx = v.x - (float)screen_w * 0.5f * v.w, dy = v.y - (float)screen_h * 0.5f * v.w;
        return dx <= gx && dx >= -gx && dy <= gx && dy >= -gx;
    }
    static Vtx3ds lerp_vtx(const Vtx3ds& p, const Vtx3ds& q, float t) {
        Vtx3ds o;
        o.x = p.x + (q.x - p.x) * t; o.y = p.y + (q.y - p.y) * t; o.z = p.z + (q.z - p.z) * t; o.w = p.w + (q.w - p.w) * t;
        o.u = p.u + (q.u - p.u) * t; o.v = p.v + (q.v - p.v) * t;
        auto mix = [t](uint8_t a, uint8_t b) { float f = (float)a + ((float)b - (float)a) * t; return (uint8_t)(f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f + 0.5f)); };
        o.r = mix(p.r, q.r); o.g = mix(p.g, q.g); o.b = mix(p.b, q.b); o.a = mix(p.a, q.a);
        return o;
    }
    void emit_clipped(DrawRecord& r, const Vtx3ds* tri) {
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
        stats->clipped++;
        Vtx3ds* o = out->verts.append((uint32_t)(n - 2) * 3);
        if (o == nullptr) return;
        for (int i = 1; i + 1 < n; i++) { *o++ = in[0]; *o++ = in[i]; *o++ = in[i + 1]; }
        r.count += (uint32_t)(n - 2) * 3;
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
        uint32_t texel = (d.addr - snapshot_base) / 2;
        float x0 = (float)(texel % (uint32_t)screen_w), y0 = (float)(texel / (uint32_t)screen_w);
        float nx = stretch ? (x0 + u) * stretch_k() - geo.x0 : x0 + u;   // N64 x on the wide screen
        float sx = geo.screen_x(nx);                                       // screen pixel, 0..400
        float sy = geo.screen_y(y0 + v);
        su = 240.0f - sy;
        sv = 512.0f - sx;
    }

    static bool same_tex(const TexDesc& a, const TexDesc& b) {
        if (a.valid != b.valid) return false;
        if (!a.valid) return true;
        return a.addr == b.addr && a.pitch == b.pitch && a.fmt == b.fmt && a.siz == b.siz && a.width == b.width && a.height == b.height &&
               a.masks == b.masks && a.maskt == b.maskt && a.cms == b.cms && a.cmt == b.cmt && a.palette == b.palette &&
               a.nibble == b.nibble && a.tlut_hash == b.tlut_hash && a.tlut_mode == b.tlut_mode && a.bilerp == b.bilerp && a.snapshot == b.snapshot && a.rtt == b.rtt &&
               a.block == b.block && a.dxt == b.dxt && a.load_word == b.load_word;
    }
    static bool same_draw_state(const DrawRecord& a, const DrawRecord& b) {
        return a.cc_w0 == b.cc_w0 && a.cc_w1 == b.cc_w1 && a.othermode_h == b.othermode_h && a.othermode_l == b.othermode_l &&
               a.geometry_mode == b.geometry_mode && memcmp(a.prim, b.prim, 4) == 0 && memcmp(a.env, b.env, 4) == 0 &&
               memcmp(a.blend, b.blend, 4) == 0 && memcmp(a.fog, b.fog, 4) == 0 && a.prim_lod_frac == b.prim_lod_frac &&
               memcmp(a.scissor, b.scissor, sizeof(a.scissor)) == 0 && a.proj_id == b.proj_id && a.perspective == b.perspective && a.rtt == b.rtt &&
               memcmp(a.uv1, b.uv1, sizeof(a.uv1)) == 0 &&
               same_tex(a.tex[0], b.tex[0]) && same_tex(a.tex[1], b.tex[1]);
    }

    // The current state against a recorded draw (what same_draw_state
    // compares, without building a probe record).
    bool same_as_last(const DrawRecord& a, const TexDesc* tex) const {
        if (a.cc_w0 != cc_w0 || a.cc_w1 != cc_w1 || a.othermode_h != othermode_h || a.othermode_l != othermode_l ||
            a.geometry_mode != geometry_mode || a.prim_lod_frac != prim_lod_frac || a.proj_id != proj_id || a.rtt != cur_rtt) return false;
        if (a.mv_auto != (mv_groups[mv_group_depth] == kIdAuto)) return false;
        if (memcmp(a.prim, prim, 4) != 0 || memcmp(a.env, env, 4) != 0 || memcmp(a.blend, blend, 4) != 0 || memcmp(a.fog, fog, 4) != 0) return false;
        int16_t sc[4];
        effective_scissor(sc, proj_stretch());
        if (memcmp(a.scissor, sc, sizeof(a.scissor)) != 0) return false;
        if (a.perspective != (proj.m[2][3] != 0.0f || proj.m[3][3] == 0.0f)) return false;
        float uv1[4];
        compute_uv1(uv1, texture.tile);
        if (memcmp(a.uv1, uv1, sizeof(uv1)) != 0) return false;
        return same_tex(a.tex[0], tex[0]) && same_tex(a.tex[1], tex[1]);
    }

    static float shift_scale(int s) {
        if (s == 0) return 1.0f;
        if (s <= 10) return 1.0f / (float)(1 << s);
        return (float)(1 << (16 - s));
    }

    // Rectangles are emitted in N64 pixels with w = 1.
    void emit_rect(float ulx, float uly, float lrx, float lry, bool textured, int tile, float s, float t, float dsdx, float dtdy, bool flip) {
        emit_rect_aligned(ulx, uly, lrx, lry, textured, tile, s, t, dsdx, dtdy, flip, rect_lorigin, rect_rorigin, rect_align,
                          rect_lorigin == 0x800 && rect_rorigin == 0x800);
    }
    // plain_origins: neither edge is anchored to a screen edge (the only
    // rectangles RT64 stretches for covering the width).
    void emit_rect_aligned(float ulx, float uly, float lrx, float lry, bool textured, int tile, float s, float t, float dsdx, float dtdy, bool flip,
                           uint32_t lorigin, uint32_t rorigin, const int16_t* align, bool plain_origins) {
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
                    tex_seq++;
                    return;
                }
            }
            if (cur_rtt < 0) return;
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
        // RT64 spreads a rectangle over the whole wide screen when it reads a
        // frame copy, spans the full width with plain origins (full-screen
        // fills and tints), or is tagged STRETCH, unless it is tagged ADJUST.
        const bool stretch = ((textured && r.tex[0].snapshot) || (plain_origins && ulx <= 0.0f && lrx >= (float)screen_w) ||
                              rect_aspect == kAspectStretch) && rect_aspect != kAspectAdjust && cur_rtt < 0;
        if (stretch) {
            ulx = stretch_x(ulx);
            lrx = stretch_x(lrx);
            effective_scissor(r.scissor, true);
        }
        float xs[4] = { ulx, lrx, lrx, ulx }, ys[4] = { uly, uly, lry, lry };
        float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
        if (flip) { us[1] = u0; vs[1] = v1; us[3] = u1; vs[3] = v0; }
        for (int i = 0; i < 4; i++) {
            q[i].x = xs[i]; q[i].y = ys[i]; q[i].z = (othermode_l & G_ZS_PRIM) ? prim_z() : 0.0f; q[i].w = 1.0f;
            q[i].u = us[i]; q[i].v = vs[i];
            if (r.tex[0].snapshot) snapshot_uv(r.tex[0], q[i].u, q[i].v, q[i].u, q[i].v, stretch);
            else if (r.tex[0].rtt >= 0) rtt_uv(r.tex[0], q[i].u, q[i].v);
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
                return 1;
            }
            case EX_POPMATRIXGROUP: {
                int count = w1 & 0xFF;
                bool is_proj = (sub >> 8) & 1;     // always clear in practice, as in RT64
                int& depth = is_proj ? proj_group_depth : mv_group_depth;
                depth = depth > count ? depth - count : 0;
                if (is_proj) proj_id = proj_groups[proj_group_depth];
                return 0;
            }
            case EX_EDITGROUPBYADDRESS: return 1;
            case EX_SETRECTALIGN: {
                rect_lorigin = w1 & 0xFFF; rect_rorigin = (w1 >> 12) & 0xFFF;
                uint32_t a = r32(pc + 8), b = r32(pc + 12);
                rect_align[0] = (int16_t)(a >> 16); rect_align[1] = (int16_t)(a & 0xFFFF);
                rect_align[2] = (int16_t)(b >> 16); rect_align[3] = (int16_t)(b & 0xFFFF);
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
                emit_rect_aligned(ulx, uly, lrx, lry, true, tile, s, t, dsdx, dtdy, flip, 0x800, 0x800, none, lorigin == 0x800 && rorigin == 0x800);
                return 2;
            }
            case EX_FILLRECT: {
                uint32_t lorigin = w1 & 0xFFF, rorigin = (w1 >> 12) & 0xFFF;
                uint32_t a = r32(pc + 8), b = r32(pc + 12);
                float ulx = (int16_t)(a >> 16) / 4.0f + origin_x(lorigin), uly = (int16_t)(a & 0xFFFF) / 4.0f;
                float lrx = (int16_t)(b >> 16) / 4.0f + origin_x(rorigin), lry = (int16_t)(b & 0xFFFF) / 4.0f;
                static const int16_t none[4] = { 0, 0, 0, 0 };
                emit_rect_aligned(ulx, uly, lrx, lry, false, 0, 0, 0, 0, 0, false, 0x800, 0x800, none, lorigin == 0x800 && rorigin == 0x800);
                return 1;
            }
            case EX_VERTEX: return 1;
            case EX_MATRIX_FLOAT:
                matrix_float(seg(r32(pc + 12)), w1);
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
                if (proj_depth < kStateStack) proj_stack[proj_depth++] = proj;
                return 0;
            case EX_POPPROJMATRIX:
                if (proj_depth > 0) { proj = proj_stack[--proj_depth]; mvp_dirty = true; }
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
            if (proj.m[2][3] != 0.0f) {
                bool world = is_world_proj(proj_id);
                if (!out->has_cam || (world && !out->cam_is_world)) {
                    out->has_cam = true;
                    out->cam_is_world = world;
                    memcpy(out->cam, &proj, sizeof(out->cam));
                }
            }
        }
        else {
            mv_seq++;
            if (p & 1) {
                if (mv_depth + 1 < kMaxMtxStack) { mv_stack[mv_depth + 1] = mv_stack[mv_depth]; mv_depth++; }
            }
            if (p & 2) mv_stack[mv_depth] = m; else mul(mv_stack[mv_depth], m, mv_stack[mv_depth]);
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

    // Rewrites an F3DEX command in place as the F3DEX2 command that does the
    // same (the RDP commands are shared). Returns false when the command was
    // carried out here and the F3DEX2 decoder must skip it.
    bool translate_f3dex(uint8_t& op, uint32_t& w0, uint32_t& w1) {
        switch (op) {
            case F3DEX_G_SPNOOP:
                // The RT64 hook rides on GBI1's no-op (RT64_HOOK_OPCODE 0x00).
                op = G_SPNOOP; w0 = ((uint32_t)G_SPNOOP << 24) | (w0 & 0xFFFFFF);
                return true;
            case F3DEX_G_MTX: {
                // GBI1 flags: projection 1, load 2, push 4. F3DEX2 stores
                // projection 4, load 2, push 1, the push bit inverted.
                uint32_t p = (w0 >> 16) & 0xFF;
                uint32_t p2 = ((p & 1) ? 4u : 0u) | (p & 2) | ((p & 4) ? 1u : 0u);
                op = G_MTX; w0 = ((uint32_t)G_MTX << 24) | (p2 ^ 1);
                return true;
            }
            case F3DEX_G_MOVEMEM: {
                uint8_t index = (w0 >> 16) & 0xFF;
                uint32_t offset;   // F3DEX2 G_MV_LIGHT offset: look-at X, Y, then the lights
                if (index == F3DEX_G_MV_VIEWPORT) { op = G_MOVEMEM; w0 = ((uint32_t)G_MOVEMEM << 24) | 8; return true; }
                if (index == F3DEX_G_MV_LOOKATX) offset = 0;
                else if (index == F3DEX_G_MV_LOOKATY) offset = 0x18;
                else if (index >= F3DEX_G_MV_L0 && index <= F3DEX_G_MV_L7) offset = 0x30 + (uint32_t)((index - F3DEX_G_MV_L0) / 2) * 0x18;
                else { stats->unknown++; return false; }   // G_MV_MATRIX_* (forced matrices), G_MV_TXTATT
                op = G_MOVEMEM; w0 = ((uint32_t)G_MOVEMEM << 24) | ((offset / 8) << 8) | 10;
                return true;
            }
            case F3DEX_G_VTX: {
                uint32_t n = (w0 >> 10) & 0x3F, v0 = (w0 >> 17) & 0x7F;
                op = G_VTX; w0 = ((uint32_t)G_VTX << 24) | (n << 12) | (((v0 + n) & 0x7F) << 1);
                return true;
            }
            case F3DEX_G_DL: op = G_DL; w0 = ((uint32_t)G_DL << 24) | (w0 & 0xFFFFFF); return true;
            case F3DEX_G_SPRITE2D_BASE: stats->unknown++; return false;
            case F3DEX_G_LOAD_UCODE: op = G_LOAD_UCODE; return true;
            case F3DEX_G_BRANCH_Z: op = G_BRANCH_Z; w0 = ((uint32_t)G_BRANCH_Z << 24) | (w0 & 0xFFFFFF); return true;
            case F3DEX_G_TRI2: op = G_TRI2; return true;             // same layout: two index triples (x2)
            case F3DEX_G_MODIFYVTX: op = G_MODIFYVTX; return true;   // same layout
            case F3DEX_G_RDPHALF_1: op = G_RDPHALF_1; return true;
            case F3DEX_G_RDPHALF_2: op = G_RDPHALF_2; return true;
            case F3DEX_G_QUAD:
                // gSP1Quadrangle: four indices (x2) in w1, drawn as a, b, c and a, c, d.
                // (L3DEX's G_LINE3D shares this number; Banjo draws no lines.)
                emit_tri(((w1 >> 24) & 0xFF) / 2, ((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2);
                emit_tri(((w1 >> 24) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                return false;
            case F3DEX_G_CLEARGEOMETRYMODE:
                geometry_mode &= ~f3dex_geometry_bits(w1);
                state_seq++;
                return false;
            case F3DEX_G_SETGEOMETRYMODE:
                geometry_mode |= f3dex_geometry_bits(w1);
                state_seq++;
                return false;
            case F3DEX_G_ENDDL: op = G_ENDDL; return true;
            case F3DEX_G_SETOTHERMODE_L: case F3DEX_G_SETOTHERMODE_H: {
                // GBI1 gives the shift and length; F3DEX2 32 - shift - len and len - 1.
                uint32_t shift = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
                op = op == F3DEX_G_SETOTHERMODE_L ? G_SETOTHERMODE_L : G_SETOTHERMODE_H;
                w0 = ((uint32_t)op << 24) | (((32 - shift - len) & 0xFF) << 8) | ((len - 1) & 0xFF);
                return true;
            }
            case F3DEX_G_TEXTURE: op = G_TEXTURE; return true;       // same fields
            case F3DEX_G_MOVEWORD: {
                uint8_t index = w0 & 0xFF;
                uint32_t offset = (w0 >> 8) & 0xFFFF;
                if (index == 0x02) {
                    // NUMLIGHT: 0x80000000 + 32 * (lights + 1) -> F3DEX2's lights * 24.
                    int n = (int)((w1 - 0x80000000u) >> 5) - 1;
                    w1 = (uint32_t)(n < 0 ? 0 : n) * 24;
                }
                else if (index == 0x0A) {
                    // LIGHTCOL: GBI1 lights are 32 bytes apart in DMEM, F3DEX2's 24.
                    offset = (offset / 32) * 24 + (offset % 32);
                }
                else if (index == F3DEX_G_MW_POINTS) {
                    // gSPModifyVertex through G_MOVEWORD: offset = vertex * 40 + where.
                    uint32_t v = offset / 40, where = offset % 40;
                    op = G_MODIFYVTX; w0 = ((uint32_t)G_MODIFYVTX << 24) | (where << 16) | ((v * 2) & 0xFFFF);
                    return true;
                }
                op = G_MOVEWORD; w0 = ((uint32_t)G_MOVEWORD << 24) | ((uint32_t)index << 16) | (offset & 0xFFFF);
                return true;
            }
            case F3DEX_G_POPMTX:
                // Only the modelview stack pops (w1 = G_MTX_MODELVIEW), one level.
                if (w1 != 0) return false;
                op = G_POPMTX; w1 = 64;
                return true;
            case F3DEX_G_CULLDL: op = G_CULLDL; return true;
            case F3DEX_G_TRI1:
                // GBI1 carries the indices (x2) in w1; F3DEX2 in w0.
                op = G_TRI1; w0 = ((uint32_t)G_TRI1 << 24) | (w1 & 0xFFFFFF);
                return true;
            case G_RDPNOOP: return false;
            case G_EXTENDED: return true;   // RT64_EXTENDED_OPCODE is the same in both
            default:
                // 0x07, 0x02, 0x05 and 0xD6..0xE3 mean F3DEX2 commands only, which a
                // GBI1 list never holds; everything else is an RDP command.
                if (op < 0xC0) { stats->unknown++; return false; }
                if (op >= G_DMA_IO && op <= G_SETOTHERMODE_H) { stats->unknown++; return false; }
                return true;
        }
    }

    // ---- main loop
    bool run(uint32_t data_ptr) {
        tex_seq++;   // FrameRecord::tlut restarts: cached palette indices are stale
        uint32_t stack[kMaxDlDepth];
        int depth = 0;
        uint32_t pc = seg(data_ptr);
        bool fullsync = false;
        for (uint32_t guard = 0; guard < 400000; guard++) {
            uint32_t w0 = r32(pc), w1 = r32(pc + 4);
            uint8_t op = w0 >> 24;
            stats->commands++;
            stats->op_hist[op]++;
            if (gbi == Gbi::F3DEX && !translate_f3dex(op, w0, w1)) {
                pc += 8;
                continue;
            }
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
                            // The vertex buffer holds coordinates already
                            // scaled by G_TEXTURE: the new ones go in as given.
                            vtx[v].u = (float)(int16_t)(w1 >> 16) / 32.0f;
                            vtx[v].v = (float)(int16_t)(w1 & 0xFFFF) / 32.0f;
                        }
                    }
                    break;
                }
                case G_CULLDL: stats->cull_dl++; break;
                case G_BRANCH_Z: {
                    // gSPBranchLessZ, the games' level of detail: jump to the
                    // list in RDPHALF_1 when the vertex's screen depth (the
                    // RSP's z/w * vscale + vtrans, 0..1023) is below w1/65536.
                    // The near model sits behind the branch; what follows is
                    // the far one, or nothing for objects that vanish. The
                    // recomp frontends force the branch (RT64's forceBranch,
                    // or gEXForceBranch): always the near model.
                    int vi = (w0 >> 1) & 0x7FF;
                    if (vi < kMaxVerts) {
                        const auto& v = vtx[vi];
                        float zw = 2.0f * v.z / (v.w != 0.0f ? v.w : 1e-6f) - 1.0f;
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
                    Mtx m;
                    load_mtx(m, seg(w1));
                    apply_matrix(m, (w0 & 0xFF) ^ 1);   // F3DEX2 inverts the push bit
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
                case G_SETPRIMDEPTH: prim_depth_z = (uint16_t)(w1 >> 16); break;
                case G_LOADTLUT: {
                    int tile = (w1 >> 24) & 7;
                    int count = ((w1 >> 14) & 0x3FF) + 1;
                    const Tile& tl = tiles[tile];
                    // Palettes sit in TMEM's upper half, one entry per 64-bit
                    // word (quadrupled): entry = word - 256. A 16-colour
                    // palette p is loaded at word 256 + 16p.
                    int base = (int)tl.tmem - 256;
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
                    cur_rtt = rtt_for_cimg(cimg, cimg_width);
                    if (cur_rtt >= 0) out->rtts[cur_rtt] = rtt_images[cur_rtt];
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
    // A frame that ends on an off-screen image (Banjo's jiggy transition
    // leaves its image current) did not present it.
    if (impl_->rtt_enabled) {
        for (int i = 0; i < impl_->rtt_count; i++) if (impl_->rtt_images[i].addr == addr) return;
        if (addr >= 0x800000) return;
    }
    impl_->vi_fbs[impl_->vi_fb_next++ & 3] = addr;
}


bool Interpreter::run(uint32_t data_ptr, FrameRecord& out) {
    impl_->out = &out;
    // Per-task RSP state that must not leak between lists.
    impl_->mv_depth = 0;
    impl_->mvp_dirty = true;
    impl_->proj_group_depth = impl_->mv_group_depth = 0;
    impl_->proj_groups[0] = 0;
    // Untagged modelview draws carry G_EX_ID_AUTO, as RT64's TransformGroup
    // does by default: DK64's crosshair quads are matched on that.
    impl_->mv_groups[0] = Impl::kIdAuto;
    impl_->proj_aspects[0] = 0;
    impl_->proj_id = 0;
    impl_->mv_seq = 0;
    impl_->vp_depth = 0;
    impl_->scissor_depth = 0;
    impl_->othermode_depth = impl_->combine_depth = impl_->geometry_depth = impl_->proj_depth = 0;
    for (int& d : impl_->color_depth) d = 0;
    impl_->load_count = 0;
    impl_->tlut_frame_hash = 0;
    for (int i = 0; i < kMaxRtt; i++) out.rtts[i] = impl_->rtt_images[i];
    for (auto& L : impl_->loads) L.valid = false;
    return impl_->run(data_ptr);
}

void Interpreter::set_world_proj_ids(const uint32_t* ids, int count) {
    for (int i = 0; i < 4; i++) impl_->world_proj_ids[i] = count > 0 ? ids[i < count ? i : count - 1] : 5;
}
void Interpreter::set_split_untagged_ortho(bool on) { impl_->split_untagged_ortho = on; }
void Interpreter::set_force_branch_z(bool on) { impl_->force_branch_z = on; }
void Interpreter::set_gbi(Gbi gbi) { impl_->gbi = gbi; }
void Interpreter::set_render_to_texture(bool on) { impl_->rtt_enabled = on; }
void Interpreter::set_screen(const ScreenGeometry& geo) {
    impl_->geo = geo;
    impl_->screen_w = geo.w;
    impl_->screen_h = geo.h;
    const int16_t full[4] = { 0, 0, (int16_t)geo.w, (int16_t)geo.h };
    memcpy(impl_->scissor, full, sizeof(full));
    const float cx = (float)geo.w * 0.5f, cy = (float)geo.h * 0.5f;
    impl_->vp = Impl::Viewport{ { cx, -cy, 511 }, { cx, cy, 511 }, cx };
}

}   // namespace rt64_3ds
