// The frame record: what the display-list interpreter produces and the
// citro3d backend replays (once per eye, later).
#ifndef RT64_3DS_RECORD_H
#define RT64_3DS_RECORD_H

#include <cstdint>
#include <vector>

namespace rt64_3ds {

// One vertex, in "N64 screen homogeneous" space: x/w and y/w are N64 screen
// pixels (viewport applied), z/w in [0,1] near to far, so every draw shares
// one projection matrix per render target. u/v are texels relative to the
// draw's tile origin; the backend scales them by the PICA texture size.
struct Vtx3ds {
    float x, y, z, w;
    float u, v;
    uint8_t r, g, b, a;
};

// A texture as the RDP would sample it: where the texels come from and the
// tile's addressing. Enough for the texture cache to decode and key it.
struct TexDesc {
    bool valid = false;
    uint32_t addr = 0;      // RDRAM address of the first texel row
    uint32_t pitch = 0;     // bytes between rows in RDRAM
    uint8_t fmt = 0, siz = 0;
    uint16_t width = 0, height = 0;   // texels
    uint8_t masks = 0, maskt = 0, cms = 0, cmt = 0;
    uint8_t palette = 0;
    uint8_t nibble = 0;     // 4-bit formats: the first texel is the low nibble of its byte
    uint32_t tlut_hash = 0; // of the palette in use (CI formats)
    uint32_t tlut_index = 0;// offset of that palette's 256 entries in FrameRecord::tlut
    uint16_t tlut_mode = 0; // G_TT_* (0 none, 2 RGBA16, 3 IA16)
    bool bilerp = false;
    bool snapshot = false;  // samples the stored copy of a previous frame instead of RDRAM
};

// One draw of triangles or a rectangle with a snapshot of the RDP state that
// matters for the PICA.
struct DrawRecord {
    uint32_t first = 0, count = 0;      // triangle-list vertices in FrameRecord::verts
    enum Kind : uint8_t { Tris, TexRect, FillRect } kind = Tris;
    uint32_t cc_w0 = 0, cc_w1 = 0;      // G_SETCOMBINE words
    uint32_t othermode_h = 0, othermode_l = 0;
    uint32_t geometry_mode = 0;
    uint8_t prim[4] = {}, env[4] = {}, blend[4] = {}, fog[4] = {};
    uint8_t prim_lod_frac = 0;
    int16_t scissor[4] = { 0, 0, 320, 240 };  // ulx uly lrx lry in N64 pixels
    TexDesc tex[2];
    uint32_t proj_id = 0;               // gEXMatrixGroup id in force (stereo later)
    bool perspective = true;
    float dbg_vp[4] = {};               // viewport scale x/y, translate x/y (debug logging)
    float dbg_proj[4] = {};             // projection m[1][1], m[3][1], m[2][3], m[3][3]
};

struct FrameRecord {
    std::vector<Vtx3ds> verts;
    std::vector<DrawRecord> draws;
    std::vector<uint16_t> tlut;         // palettes referenced by draws, 256 entries each
    bool has_fullsync = false;
    bool snapshot_request = false;      // copy the last presented frame before drawing this one
    uint32_t color_image = 0, color_width = 0, depth_image = 0;

    void clear() {
        verts.clear();
        draws.clear();
        tlut.clear();
        has_fullsync = false;
        snapshot_request = false;
    }
};

}   // namespace rt64_3ds

#endif
