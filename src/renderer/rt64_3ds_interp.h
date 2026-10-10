// Frame interpolation (RenderDesc::frame_interpolation, Settings::frame_cap
// 2): RT64's matching of transform groups between two game frames and its
// interpolation of the matched transforms, ported to plain floats for the
// display-list interpreter. The interpreter records every transform a list
// draws with (the "world transforms" and projections RT64 records, with the
// gEXMatrixGroup tags in force), match() pairs them with the previous
// list's, and a second run of the list draws with the interpolated ones.
//
// The math follows lib/rt64 (oneup03/rt64-3D, Goemon3D):
//   common/rt64_math.cpp            decomposeMatrix, recomposeMatrix, lerpTransforms,
//                                   lerpMatrix(Components), matrixDecomposeViewProj
//   hle/rt64_rigid_body.cpp         RigidBody (per-component INTERPOLATE / SKIP / AUTO)
//   hle/rt64_game_frame.cpp         matching: ids in LINEAR order, AUTO by closest
//                                   transform, IGNORE never; tile scroll deltas
//   render/rt64_transform_processor.cpp, rt64_projection_processor.cpp,
//   rt64_tile_processor.cpp         what is interpolated with which weight
// hlslpp's conventions are kept: row-major 4x4 matrices applied to row
// vectors (v * M), its quaternion layout (x, y, z, w) and its slerp (which
// RT64 calls with 1 - weight).
#ifndef RT64_3DS_INTERP_H
#define RT64_3DS_INTERP_H

#include <cstdint>
#include <vector>

namespace rt64_3ds {
namespace interp {

struct M4 { float m[4][4]; };

M4 identity();
M4 mul(const M4& a, const M4& b);            // a * b
bool is_affine(const M4& m);
bool is_identity(const M4& m);
bool is_view_proj(const M4& m);
void decompose_view_proj(const M4& vp, M4& v, M4& p);
M4 lerp_matrix(const M4& a, const M4& b, float t);
M4 lerp_components(const M4& a, const M4& b, bool linear, bool angular, bool perspective, float t);

// rt64_extended_gbi.h
enum : uint8_t { kSkip = 0, kInterpolate = 1, kAuto = 2 };
enum : uint8_t { kOrderLinear = 0, kOrderAuto = 1 };
constexpr uint32_t kIdIgnore = 0x0, kIdAuto = 0xFFFFFFFFu;

// RT64's TransformGroup (hle/rt64_transform_group.h): its defaults are what
// an untagged transform has.
struct Group {
    uint32_t id = kIdAuto;
    bool decompose = true;
    uint8_t pos = kAuto, rot = kAuto, scale = kAuto, skew = kAuto, persp = kAuto;
    uint8_t vert = kSkip, tc = kSkip, tile = kAuto, lookat = kAuto;
    uint8_t order = kOrderAuto, aspect = 0, edit = 0;
    // gEXMatrixGroup / gEXEditGroupByAddress: the id word and the flags word.
    static Group decode(uint32_t id, uint32_t w);
    // RT64's setGroupProperties: everything but the id.
    void set_props(const Group& g);
};

struct Decomposed {
    float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };    // rotation quaternion x, y, z, w
    float s[3] = {}, k[3] = {}, t[3] = {};       // scale, skew, translation
    float p[4] = { 0.0f, 0.0f, 0.0f, 1.0f };     // perspective
    bool flip = false, valid = false;
};
bool decompose(const M4& m, Decomposed& d);
M4 recompose(const Decomposed& d);

struct RigidBody {
    Decomposed xf[2];
    float lin_vel[3] = { 0.0f, 0.0f, 0.0f };
    uint8_t idx = 0;
    bool lerp_t = false, lerp_r = false, lerp_s = false, lerp_k = false, lerp_p = false, lerp_dec = true;
    void update_linear(const M4& prev, const M4& cur, uint8_t comp);
    void update_angular(uint8_t rot, uint8_t scale, uint8_t skew);
    void update_perspective(uint8_t persp);
    void update_decomposition(const M4& cur, bool decompose);
    M4 lerp(float w, const M4& prev, const M4& cur, bool slerp) const;
};

// What the interpreter records of one list.
struct XfEntry {                  // a world transform: the modelview a vertex load used
    M4 m;
    uint32_t group = 0;           // FrameLog::groups
    uint32_t key = 0;             // the RDRAM address of its first vertex load (AUTO matching)
    uint32_t addr = 0;            // the matrix's RDRAM address (gEXEditGroupByAddress)
    int32_t pj = -1;              // the projection in force (FrameLog::pj)
    uint32_t vfirst = 0, vcount = 0;   // its vertices in FrameLog::vdata (vstored), and how many were loaded
    bool vstored = false;
    // match():
    int32_t prev = -1;
    RigidBody rb;
    bool interp = false;          // lerped differs from m
    M4 lerped;
    bool vpos = false, vtc = false;    // vertex positions / texcoords interpolated
    uint32_t vprev = 0;           // the matched transform's vertices in the previous log
};
struct PjEntry {                  // a projection: RT64's view and projection split of it
    M4 view, proj, full;          // full = what the interpreter applies (view * proj)
    uint32_t group = 0;
    uint32_t addr = 0;
    bool perspective = false;
    // match():
    int32_t prev = -1;
    RigidBody rb;
    bool interp = false;
    M4 lerped;
    float cam_vel[3] = { 0.0f, 0.0f, 0.0f };   // a world camera's last move (the cut test)
};
struct TileParams {
    uint16_t uls = 0, ult = 0, lrs = 0, lrt = 0;   // 10.2
    uint16_t line = 0;
    uint8_t fmt = 0, siz = 0, masks = 0, maskt = 0, shifts = 0, shiftt = 0, cms = 0, cmt = 0;
};
struct TileMap {                  // RT64's GameFrameMap::TileMap, 10.2 units
    float prev_uls = 0.0f, prev_ult = 0.0f, prev_lrs = 0.0f, prev_lrt = 0.0f;
    float d_uls = 0.0f, d_ult = 0.0f, d_lrs = 0.0f, d_lrt = 0.0f;
    bool mapped = false;
};
struct TileEntry {                // a draw's texture tiles (RT64 matches them per draw call)
    uint32_t key = 0;             // combiner, modes and the transform's id
    uint32_t tex = 0;             // the texture's identity (AUTO tile matching)
    uint8_t flag = kSkip;         // the transform group's tile component
    uint8_t ntiles = 1;
    TileParams t[2];
    // match():
    int32_t prev = -1;
    TileMap map[2];
    bool interp = false;
    float du[2] = {}, dv[2] = {};  // texels to add to each tile's integer origin
};

struct FrameLog {
    std::vector<Group> groups;
    std::vector<XfEntry> xf;
    std::vector<PjEntry> pj;
    std::vector<TileEntry> tiles;
    std::vector<int16_t> vdata;   // 5 per vertex: x, y, z, s, t as loaded
    bool valid = false;           // recorded from a whole list
    void clear() { groups.clear(); xf.clear(); pj.clear(); tiles.clear(); vdata.clear(); valid = false; }
};

struct MatchResult {
    uint32_t xf = 0, xf_matched = 0, xf_interp = 0;
    uint32_t pj = 0, pj_matched = 0, pj_interp = 0;
    uint32_t tiles = 0, tiles_interp = 0;
    uint32_t vert_entries = 0;    // transforms whose vertices are interpolated
    bool camera_cut = false;      // a world camera jumped (a scene cut)
};

// Pairs cur's transforms with prev's as RT64 does, carries the rigid bodies
// over and, with weight >= 0, computes what the in-between frame draws with.
// world(id, perspective) says which projections are world cameras for the
// cut test.
MatchResult match(const FrameLog& prev, FrameLog& cur, float weight, bool (*world)(uint32_t id, bool perspective, void* ctx), void* ctx);

// What another in-between frame of the same pair draws with: the matches
// match() made, interpolated at a new weight (a game frame of three
// refreshes has two in-betweens, at 1/3 and 2/3). Fills res's
// interpolation counts.
void reweight(const FrameLog& prev, FrameLog& cur, float weight, MatchResult* res = nullptr);

}   // namespace interp
}   // namespace rt64_3ds

// RenderDesc::cut_min_jump: the camera move a scene cut needs at least.
extern float g_cut_min_jump;
inline void set_cut_min_jump(float units) { g_cut_min_jump = units; }

#endif
