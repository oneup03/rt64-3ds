// The frame record: what the display-list interpreter produces and the
// citro3d backend replays (once per eye, later).
#ifndef RT64_3DS_RECORD_H
#define RT64_3DS_RECORD_H

#include <cstdint>
#include <cstdlib>
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

// The frame's vertices. The backend hands it the linear-memory vertex buffer
// the GPU reads, so the interpreter writes the triangles it emits straight
// into place; without storage it allocates its own (host tools).
class VtxArray {
public:
    ~VtxArray() { if (owned_) free(data_); }
    void set_storage(Vtx3ds* p, uint32_t cap) { if (owned_) free(data_); owned_ = false; data_ = p; cap_ = cap; size_ = 0; }
    void clear() { size_ = 0; }
    uint32_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    Vtx3ds* data() { return data_; }
    const Vtx3ds* data() const { return data_; }
    Vtx3ds& operator[](uint32_t i) { return data_[i]; }
    const Vtx3ds& operator[](uint32_t i) const { return data_[i]; }
    // Room for n more vertices; returns where to write them or nullptr when
    // the buffer is full (fixed storage) and the triangle must be dropped.
    Vtx3ds* append(uint32_t n) {
        if (size_ + n > cap_) {
            if (!owned_ && data_ != nullptr) return nullptr;
            uint32_t nc = cap_ ? cap_ * 2 : 4096;
            while (nc < size_ + n) nc *= 2;
            data_ = (Vtx3ds*)realloc(data_, nc * sizeof(Vtx3ds));
            cap_ = nc; owned_ = true;
        }
        Vtx3ds* p = data_ + size_;
        size_ += n;
        return p;
    }
private:
    Vtx3ds* data_ = nullptr;
    uint32_t size_ = 0, cap_ = 0;
    bool owned_ = false;
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
    // LOADBLOCK row step: the RDP swaps the 32-bit halves of each 64-bit
    // TMEM word on rows its dxt counter calls odd, and the sampler swaps
    // them back on odd tile rows. Where the two disagree (dxt 0: data
    // pre-interleaved in RDRAM, as Rare's games store it) the decoder must
    // swap too. load_word = the tile's first 64-bit word within the load.
    bool block = false;
    uint16_t dxt = 0;
    uint16_t load_word = 0;
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
    // TEXEL1's texel coordinates from TEXEL0's: u1 = u0 * uv1[0] + uv1[2],
    // v1 = v0 * uv1[1] + uv1[3] (the two tiles' shifts and origins differ).
    float uv1[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
    uint32_t proj_id = 0;               // gEXMatrixGroup id in force (stereo later)
    bool perspective = true;
    uint32_t dbg_vtx = 0;               // RDRAM address of the last vertex load (debug)
    float dbg_vp[4] = {};               // viewport scale x/y, translate x/y (debug logging)
    float dbg_proj[4] = {};             // projection m[1][1], m[3][1], m[2][3], m[3][3]
};

struct FrameRecord {
    VtxArray verts;
    std::vector<DrawRecord> draws;
    std::vector<uint16_t> tlut;         // palettes referenced by draws, 256 entries each
    bool has_fullsync = false;
    bool snapshot_request = false;      // copy the last presented frame before drawing this one
    uint32_t color_image = 0, color_width = 0, depth_image = 0;
    bool has_cam = false;               // the first camera projection (group 5) loaded this frame (debug)
    float cam[16] = {};

    void clear() {
        verts.clear();
        draws.clear();
        tlut.clear();
        has_fullsync = false;
        snapshot_request = false;
        has_cam = false;
    }
};

}   // namespace rt64_3ds

#endif
