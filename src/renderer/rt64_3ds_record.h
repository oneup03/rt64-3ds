// The frame record: what the display-list interpreter produces and the
// citro3d backend replays (once per eye, later).
#ifndef RT64_3DS_RECORD_H
#define RT64_3DS_RECORD_H

#include <cstdint>
#include <cstdlib>
#include <utility>
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
    // How many more vertices fit (fixed storage; any number when it grows).
    uint32_t room() const { return (!owned_ && data_ != nullptr) ? cap_ - size_ : UINT32_MAX; }
    Vtx3ds* data() { return data_; }
    void swap(VtxArray& o) {
        Vtx3ds* d = data_; data_ = o.data_; o.data_ = d;
        uint32_t s = size_; size_ = o.size_; o.size_ = s;
        s = cap_; cap_ = o.cap_; o.cap_ = s;
        bool w = owned_; owned_ = o.owned_; o.owned_ = w;
    }
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
    uint32_t tlut_hash = 0; // of the palette entries it reads (CI formats)
    uint32_t tlut_index = 0;// offset of that palette's 256 entries in FrameRecord::tlut
    uint16_t tlut_mode = 0; // G_TT_* (0 none, 2 RGBA16, 3 IA16)
    bool bilerp = false;
    bool snapshot = false;  // samples the stored copy of a previous frame instead of RDRAM
    uint8_t snap_slot = 0;  // ... which of the kMaxSnapshots copies (the storage buffer it reads)
    int8_t rtt = -1;        // samples the off-screen image FrameRecord::rtts[rtt] rendered on the GPU
    // Samples the frame drawn so far (RenderDesc::sample_drawn_frame): the
    // texel coordinates are the snapshot's (rotated 256x512 layout).
    bool frame = false;
    // TMEM holds only this many of the tile's rows: the RDP's TMEM address
    // wraps (4 KB, or 2 KB for 32-bit texels and with the TLUT on), so row y
    // reads row y % wrap_rows (0 = the tile fits).
    uint16_t wrap_rows = 0;
    // LOADBLOCK row step: the RDP swaps the 32-bit halves of each 64-bit
    // TMEM word on rows its dxt counter calls odd, and the sampler swaps
    // them back on odd tile rows. Where the two disagree (dxt 0: data
    // pre-interleaved in RDRAM, as Rare's games store it) the decoder must
    // swap too. load_word = the tile's first 64-bit word within the load.
    bool block = false;
    uint16_t dxt = 0;
    uint16_t load_word = 0;
    // Mipmaps (lod_trilerp): the PICA texture's levels, the base included
    // (1 = none), and where the sources of the others sit in
    // FrameRecord::mips (levels - 1 of them, each half the one before).
    uint8_t levels = 1;
    uint32_t mip_index = 0;
};

// The RDP's mipmapping as games use it: LOD on (G_TL_LOD) in 2-cycle mode
// without detail or sharpen textures, the first cycle's colour lerping
// TEXEL0 to TEXEL1 (the next level's tile) by LOD fraction, its alpha doing
// the same or reading neither, the second cycle reading no TEXEL1. The
// PICA's mipmap filter blends the levels itself: the combiner reads TEXEL1
// as TEXEL0 (the lerp falls away) and no second texture is sampled.
inline bool lod_trilerp(uint32_t w0, uint32_t w1, uint32_t othermode_h) {
    if ((othermode_h & 0x10000) == 0 || ((othermode_h >> 20) & 3) != 1 || ((othermode_h >> 17) & 3) != 0) return false;
    const uint32_t a0 = (w0 >> 20) & 0xF, c0 = (w0 >> 15) & 0x1F, b0 = (w1 >> 28) & 0xF, d0 = (w1 >> 15) & 7;
    if (a0 != 2 || b0 != 1 || c0 != 13 || d0 != 1) return false;
    const uint32_t Aa0 = (w0 >> 12) & 7, Ac0 = (w0 >> 9) & 7, Ab0 = (w1 >> 12) & 7, Ad0 = (w1 >> 9) & 7;
    const bool alpha_lerp = Aa0 == 2 && Ab0 == 1 && Ac0 == 0 && Ad0 == 1;
    if (!alpha_lerp && (Aa0 == 2 || Ab0 == 2 || Ac0 == 2 || Ad0 == 2)) return false;
    const uint32_t a1 = (w0 >> 5) & 0xF, c1 = w0 & 0x1F, b1 = (w1 >> 24) & 0xF, d1 = (w1 >> 6) & 7;
    const uint32_t Aa1 = (w1 >> 21) & 7, Ac1 = (w1 >> 18) & 7, Ab1 = (w1 >> 3) & 7, Ad1 = w1 & 7;
    return a1 != 2 && b1 != 2 && c1 != 2 && c1 != 9 && d1 != 2 && Aa1 != 2 && Ab1 != 2 && Ac1 != 2 && Ad1 != 2;
}

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
    int8_t rtt = -1;                    // drawn into FrameRecord::rtts[rtt] instead of the screen
    uint32_t proj_id = 0;               // projection group id in force (gEXMatrixGroup), for stereo
    bool perspective = true;
    bool mv_auto = false;               // the modelview group is untagged (G_EX_ID_AUTO): reticle candidates
    uint32_t mv_seq = 0;                // modelview loads so far this list (reticle quads are split on it)
    // RenderDesc::primary_world_instance: the projection instance (one
    // camera's run of triangles, as RT64 counts its Projections: a new one
    // at every change of the projection or viewport, after a rectangle and
    // at a colour image change); 0 when not tracked.
    uint32_t proj_inst = 0;
    // RenderDesc::unify_world_depth: the depth terms of the projection the
    // draw was transformed by (z_clip = a * z_eye + b, its [2][2] and [3][2]
    // as depth_camera has them); 0, 0 when the option is off or the
    // projection is not a perspective.
    float depth_a = 0.0f, depth_b = 0.0f;
    // A rectangle sampling a texture of a whole image (RenderDesc::
    // merge_image_tiles): its neighbours alike merge into its record.
    bool image_rect = false;
    // RenderDesc::aim_texture_hashes: the hash of a candidate reticle
    // rectangle's texels (8..64 px, roughly square), computed in the read
    // while RDRAM still holds them; 0 when not a candidate.
    uint64_t tex_hash = 0;
    uint32_t dbg_vtx = 0;               // RDRAM address of the last vertex load (debug)
    float dbg_vp[4] = {};               // viewport scale x/y, translate x/y (debug logging)
    float dbg_proj[4] = {};             // projection m[1][1], m[3][1], m[2][3], m[3][3]
};

// An off-screen colour image the game draws into and later samples (Banjo's
// jiggy transition and picture puzzles). The backend renders it into a
// texture of pot_w x pot_h; texel coordinates that sample it arrive already
// in that texture's texels, rows counted from its bottom.
struct RttImage {
    uint32_t addr = 0;
    uint16_t width = 0;
    uint16_t pot_w = 0, pot_h = 0;
};
constexpr int kMaxRtt = 4;
// Storage buffers the game copies presented frames into, each with a GPU copy
// of its own (Dinosaur Planet's effects keep two and the pause a third).
constexpr int kMaxSnapshots = 4;

struct FrameRecord {
    VtxArray verts;
    std::vector<DrawRecord> draws;
    std::vector<uint16_t> tlut;         // palettes referenced by draws, 256 entries each
    std::vector<TexDesc> mips;          // mipmap levels past the base (TexDesc::mip_index)
    bool has_fullsync = false;
    bool snapshot_request = false;      // copy the last presented frame before drawing this one
    uint8_t snapshot_store = 0;         // ... into which snapshot slots (bits; TexDesc::snap_slot)
    uint32_t color_image = 0, color_width = 0, depth_image = 0;
    uint16_t frame_w = 0, frame_h = 0;  // the frame size the list was read at (RenderDesc::frame_sizes); 0 = the fixed one
    bool has_cam = false;               // a perspective projection was loaded this frame
    bool cam_is_world = false;          // ... under the world projection group (stereo depth terms)
    float cam[16] = {};
    // The HUD camera's projection (RenderDesc::hud_camera_id), for a frame
    // where that camera turns out to be the world's.
    bool has_hud_cam = false;
    float hud_cam[16] = {};
    RttImage rtts[kMaxRtt];             // indexed by DrawRecord::rtt and TexDesc::rtt
    // RenderDesc::primary_world_instance: the projection of each projection
    // instance under the HUD camera (RenderDesc::hud_camera_id), for the one
    // that turns out to be the frame's world.
    struct InstanceCam { uint32_t inst; float m[16]; };
    std::vector<InstanceCam> inst_cams;
    // RenderDesc::sample_drawn_frame: the last draw that samples the frame
    // drawn so far (TexDesc::frame), -1 = none. The backend keeps a copy of
    // each eye's frame up to there.
    int32_t frame_sample_last = -1;

    // Exchanges two records whole (frame interpolation keeps the in-between
    // frame beside the game's).
    void swap(FrameRecord& o) {
        verts.swap(o.verts);
        draws.swap(o.draws);
        tlut.swap(o.tlut);
        mips.swap(o.mips);
        std::swap(has_fullsync, o.has_fullsync);
        std::swap(snapshot_request, o.snapshot_request);
        std::swap(snapshot_store, o.snapshot_store);
        std::swap(color_image, o.color_image);
        std::swap(color_width, o.color_width);
        std::swap(frame_w, o.frame_w);
        std::swap(frame_h, o.frame_h);
        std::swap(depth_image, o.depth_image);
        std::swap(has_cam, o.has_cam);
        std::swap(cam_is_world, o.cam_is_world);
        std::swap(cam, o.cam);
        std::swap(has_hud_cam, o.has_hud_cam);
        std::swap(hud_cam, o.hud_cam);
        std::swap(rtts, o.rtts);
        inst_cams.swap(o.inst_cams);
        std::swap(frame_sample_last, o.frame_sample_last);
    }

    void clear() {
        verts.clear();
        draws.clear();
        tlut.clear();
        mips.clear();
        has_fullsync = false;
        snapshot_request = false;
        snapshot_store = 0;
        has_cam = false;
        cam_is_world = false;
        has_hud_cam = false;
        frame_sample_last = -1;
        inst_cams.clear();
    }
};

}   // namespace rt64_3ds

#endif
