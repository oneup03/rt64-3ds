// Frame interpolation: RT64's transform matching and interpolation. See
// rt64_3ds_interp.h for where each piece comes from.
#include "rt64_3ds_interp.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

float g_cut_min_jump = 5.0f;   // RenderDesc::cut_min_jump (set_cut_min_jump)

namespace rt64_3ds {
namespace interp {

namespace {

bool eps0(float a) { return std::fabs(a) < FLT_EPSILON; }   // RT64's epsilonEqual(a, 0)

float det3(float a00, float a01, float a02, float a10, float a11, float a12, float a20, float a21, float a22) {
    return a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
}

float det4(const M4& a) {
    const float (*m)[4] = a.m;
    return m[0][0] * det3(m[1][1], m[1][2], m[1][3], m[2][1], m[2][2], m[2][3], m[3][1], m[3][2], m[3][3]) -
           m[0][1] * det3(m[1][0], m[1][2], m[1][3], m[2][0], m[2][2], m[2][3], m[3][0], m[3][2], m[3][3]) +
           m[0][2] * det3(m[1][0], m[1][1], m[1][3], m[2][0], m[2][1], m[2][3], m[3][0], m[3][1], m[3][3]) -
           m[0][3] * det3(m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2], m[3][0], m[3][1], m[3][2]);
}

M4 inverse4(const M4& a) {
    // Adjugate over the determinant.
    M4 r;
    const float d = det4(a);
    const float inv = d != 0.0f ? 1.0f / d : 0.0f;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            // Cofactor of a[j][i] (transposed).
            float s[3][3];
            for (int y = 0, yy = 0; y < 4; y++) {
                if (y == j) continue;
                for (int x = 0, xx = 0; x < 4; x++) {
                    if (x == i) continue;
                    s[yy][xx++] = a.m[y][x];
                }
                yy++;
            }
            const float c = det3(s[0][0], s[0][1], s[0][2], s[1][0], s[1][1], s[1][2], s[2][0], s[2][1], s[2][2]);
            r.m[i][j] = (((i + j) & 1) ? -c : c) * inv;
        }
    }
    return r;
}

float len3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float dot4(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }
bool any_nan(const M4& m) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) if (std::isnan(m.m[i][j])) return true;
    return false;
}

// hlslpp's quaternion product (Hamilton, q0 * q1).
void qmul(const float* a, const float* b, float* o) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] + a[1] * b[3] + a[2] * b[0] - a[0] * b[2];
    o[2] = a[3] * b[2] + a[2] * b[3] + a[0] * b[1] - a[1] * b[0];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
// hlslpp::quaternion::rotation_axis
void qaxis(float x, float y, float z, float angle, float* o) {
    const float h = angle * 0.5f, s = std::sin(h);
    o[0] = x * s; o[1] = y * s; o[2] = z * s; o[3] = std::cos(h);
}
// hlslpp's slerp(q0, q1, t): q0 + sin((1 - t) theta) / sin(theta) * (q1 - q0),
// theta the angle between them; (1 - t) when sin(theta) is 0. Note that it
// gives q1 at t = 0: RT64 passes 1 - weight.
void qslerp(const float* q0, const float* q1, float t, float* o) {
    float c = dot4(q0, q1);
    c = c < -1.0f ? -1.0f : c > 1.0f ? 1.0f : c;
    const float theta = std::acos(c);
    const float st = std::sin(theta);
    const float f = st == 0.0f ? (1.0f - t) : std::sin((1.0f - t) * theta) / st;
    for (int i = 0; i < 4; i++) o[i] = q0[i] + f * (q1[i] - q0[i]);
}
void qlerp(const float* q0, const float* q1, float t, float* o) {
    for (int i = 0; i < 4; i++) o[i] = q0[i] + (q1[i] - q0[i]) * t;
}
void qnormalize(float* q) {
    const float l = std::sqrt(dot4(q, q));
    for (int i = 0; i < 4; i++) q[i] /= l;
}
// hlslpp's float4x4(quaternion): row vectors.
M4 qmatrix(const float* q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    M4 m = identity();
    m.m[0][0] = 1.0f - 2.0f * (y * y + z * z); m.m[0][1] = 2.0f * (x * y + z * w); m.m[0][2] = 2.0f * (x * z - y * w);
    m.m[1][0] = 2.0f * (x * y - z * w); m.m[1][1] = 1.0f - 2.0f * (x * x + z * z); m.m[1][2] = 2.0f * (y * z + x * w);
    m.m[2][0] = 2.0f * (x * z + y * w); m.m[2][1] = 2.0f * (y * z - x * w); m.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
    return m;
}

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

}   // namespace

M4 identity() {
    M4 m;
    memset(&m, 0, sizeof(m));
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    return m;
}

M4 mul(const M4& a, const M4& b) {
    M4 t;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        t.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    }
    return t;
}

bool is_affine(const M4& m) { return m.m[0][3] == 0.0f && m.m[1][3] == 0.0f && m.m[2][3] == 0.0f && m.m[3][3] == 1.0f; }
bool is_identity(const M4& m) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) if (m.m[i][j] != (i == j ? 1.0f : 0.0f)) return false;
    return true;
}
bool is_view_proj(const M4& m) { return std::fabs(m.m[3][3]) >= 1e-6f && std::fabs(1.0f - m.m[3][3]) >= 1e-6f; }

void decompose_view_proj(const M4& vp, M4& v, M4& p) {
    v = identity();
    p = identity();
    p.m[2][3] = -1.0f;
    p.m[3][3] = 0.0f;
    v.m[0][2] = -vp.m[0][3];
    v.m[1][2] = -vp.m[1][3];
    v.m[2][2] = -vp.m[2][3];
    v.m[3][2] = -vp.m[3][3];
    p.m[2][2] = vp.m[0][2] / v.m[0][2];
    p.m[3][2] = vp.m[3][2] - p.m[2][2] * v.m[3][2];
    p.m[0][0] = std::sqrt(vp.m[0][0] * vp.m[0][0] + vp.m[1][0] * vp.m[1][0] + vp.m[2][0] * vp.m[2][0]);
    p.m[1][1] = std::sqrt(vp.m[0][1] * vp.m[0][1] + vp.m[1][1] * vp.m[1][1] + vp.m[2][1] * vp.m[2][1]);
    for (int i = 0; i < 4; i++) {
        v.m[i][0] = vp.m[i][0] / p.m[0][0];
        v.m[i][1] = vp.m[i][1] / p.m[1][1];
    }
    if (any_nan(v) || any_nan(p)) {
        v = identity();
        p = vp;
    }
}

M4 lerp_matrix(const M4& a, const M4& b, float t) {
    M4 c;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) c.m[i][j] = lerpf(a.m[i][j], b.m[i][j], t);
    return c;
}

M4 lerp_components(const M4& a, const M4& b, bool linear, bool angular, bool perspective, float t) {
    M4 r = b;
    if (angular) {
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i][j] = lerpf(a.m[i][j], b.m[i][j], t);
    }
    if (linear) {
        for (int j = 0; j < 3; j++) r.m[3][j] = lerpf(a.m[3][j], b.m[3][j], t);
    }
    if (perspective) {
        for (int i = 0; i < 4; i++) r.m[i][3] = lerpf(a.m[i][3], b.m[i][3], t);
    }
    return r;
}

Group Group::decode(uint32_t id, uint32_t w) {
    Group g;
    g.id = id;
    g.decompose = ((w >> 2) & 1) != 0;
    g.pos = (w >> 3) & 3;
    g.rot = (w >> 5) & 3;
    g.scale = (w >> 7) & 3;
    g.skew = (w >> 9) & 3;
    g.persp = (w >> 11) & 3;
    g.vert = (w >> 13) & 3;
    g.tile = (w >> 15) & 3;
    g.order = (w >> 17) & 3;
    g.edit = (w >> 19) & 1;
    g.aspect = (w >> 20) & 3;
    g.tc = (w >> 22) & 3;
    g.lookat = (w >> 24) & 3;
    return g;
}

void Group::set_props(const Group& g) {
    const uint32_t keep = id;
    *this = g;
    id = keep;
}

// common/rt64_math.cpp decomposeMatrix (from glm's matrix_decompose).
bool decompose(const M4& mtx, Decomposed& d) {
    d.valid = false;
    M4 L = mtx;
    if (eps0(L.m[3][3])) return false;
    const float w = L.m[3][3];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) L.m[i][j] /= w;
    M4 P = L;
    for (int i = 0; i < 3; i++) P.m[i][3] = 0.0f;
    P.m[3][3] = 1.0f;
    if (eps0(det4(P))) return false;
    if (!eps0(L.m[0][3]) || !eps0(L.m[1][3]) || !eps0(L.m[2][3])) {
        const float rhs[4] = { L.m[0][3], L.m[1][3], L.m[2][3], L.m[3][3] };
        const M4 inv = inverse4(P);
        // transpose(inverse) * rhs (HLSL mul(matrix, vector))
        for (int i = 0; i < 4; i++) d.p[i] = inv.m[0][i] * rhs[0] + inv.m[1][i] * rhs[1] + inv.m[2][i] * rhs[2] + inv.m[3][i] * rhs[3];
        L.m[0][3] = L.m[1][3] = L.m[2][3] = 0.0f;
        L.m[3][3] = 1.0f;
    }
    else {
        d.p[0] = d.p[1] = d.p[2] = 0.0f;
        d.p[3] = 1.0f;
    }
    for (int j = 0; j < 3; j++) d.t[j] = L.m[3][j];
    float row[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) row[i][j] = L.m[i][j];
    auto scale_to_1 = [](float* v) { const float l = len3(v); for (int k = 0; k < 3; k++) v[k] = v[k] * 1.0f / l; };
    d.s[0] = len3(row[0]);
    scale_to_1(row[0]);
    d.k[2] = dot3(row[0], row[1]);
    for (int k = 0; k < 3; k++) row[1][k] = row[1][k] + row[0][k] * -d.k[2];
    d.s[1] = len3(row[1]);
    scale_to_1(row[1]);
    d.k[2] /= d.s[1];
    d.k[1] = dot3(row[0], row[2]);
    for (int k = 0; k < 3; k++) row[2][k] = row[2][k] + row[0][k] * -d.k[1];
    d.k[0] = dot3(row[1], row[2]);
    for (int k = 0; k < 3; k++) row[2][k] = row[2][k] + row[1][k] * -d.k[0];
    d.s[2] = len3(row[2]);
    scale_to_1(row[2]);
    d.k[1] /= d.s[2];
    d.k[0] /= d.s[2];
    const float pd[3] = { row[1][1] * row[2][2] - row[1][2] * row[2][1], row[1][2] * row[2][0] - row[1][0] * row[2][2],
                          row[1][0] * row[2][1] - row[1][1] * row[2][0] };
    d.flip = dot3(row[0], pd) < 0.0f;
    if (d.flip) {
        for (int i = 0; i < 3; i++) {
            d.s[i] *= -1.0f;
            for (int k = 0; k < 3; k++) row[i][k] *= -1.0f;
        }
    }
    const float trace = row[0][0] + row[1][1] + row[2][2];
    if (trace > 0.0f) {
        float root = std::sqrt(trace + 1.0f);
        d.q[3] = 0.5f * root;
        root = 0.5f / root;
        d.q[0] = root * (row[1][2] - row[2][1]);
        d.q[1] = root * (row[2][0] - row[0][2]);
        d.q[2] = root * (row[0][1] - row[1][0]);
    }
    else {
        static const int next[3] = { 1, 2, 0 };
        int i = 0;
        if (row[1][1] > row[0][0]) i = 1;
        if (row[2][2] > row[i][i]) i = 2;
        const int j = next[i], k = next[j];
        float root = std::sqrt(row[i][i] - row[j][j] - row[k][k] + 1.0f);
        d.q[i] = 0.5f * root;
        root = 0.5f / root;
        d.q[j] = root * (row[i][j] + row[j][i]);
        d.q[k] = root * (row[i][k] + row[k][i]);
        d.q[3] = root * (row[j][k] - row[k][j]);
    }
    d.valid = true;
    return true;
}

// common/rt64_math.cpp recomposeMatrix: S * skews * R * T * P.
M4 recompose(const Decomposed& d) {
    M4 m = identity();
    m.m[0][3] = d.p[0]; m.m[1][3] = d.p[1]; m.m[2][3] = d.p[2]; m.m[3][3] = d.p[3];
    M4 t = identity();
    t.m[3][0] = d.t[0]; t.m[3][1] = d.t[1]; t.m[3][2] = d.t[2];
    m = mul(t, m);
    m = mul(qmatrix(d.q), m);
    if (std::fabs(d.k[0]) > 0.0f) { M4 s = identity(); s.m[2][1] = d.k[0]; m = mul(s, m); }
    if (std::fabs(d.k[1]) > 0.0f) { M4 s = identity(); s.m[2][0] = d.k[1]; m = mul(s, m); }
    if (std::fabs(d.k[2]) > 0.0f) { M4 s = identity(); s.m[1][0] = d.k[2]; m = mul(s, m); }
    M4 sc;
    memset(&sc, 0, sizeof(sc));
    sc.m[0][0] = d.s[0]; sc.m[1][1] = d.s[1]; sc.m[2][2] = d.s[2]; sc.m[3][3] = 1.0f;
    return mul(sc, m);
}

namespace {

// common/rt64_math.cpp lerpTransforms.
Decomposed lerp_transforms(const Decomposed& a, const Decomposed& b, float w, bool lt, bool lr, bool ls, bool lk, bool lp, bool slerp) {
    Decomposed r;
    for (int i = 0; i < 3; i++) r.t[i] = lt ? lerpf(a.t[i], b.t[i], w) : b.t[i];
    if (lr) {
        if (dot4(a.q, b.q) > 0.0f) {
            if (slerp) qslerp(a.q, b.q, 1.0f - w, r.q); else qlerp(a.q, b.q, w, r.q);
        }
        else {
            const float nb[4] = { -b.q[0], -b.q[1], -b.q[2], -b.q[3] };
            if (slerp) qslerp(a.q, nb, 1.0f - w, r.q); else qlerp(a.q, nb, w, r.q);
        }
        qnormalize(r.q);
    }
    else {
        memcpy(r.q, b.q, sizeof(r.q));
    }
    for (int i = 0; i < 3; i++) r.s[i] = ls ? lerpf(a.s[i], b.s[i], w) : b.s[i];
    for (int i = 0; i < 3; i++) r.k[i] = lk ? lerpf(a.k[i], b.k[i], w) : b.k[i];
    for (int i = 0; i < 4; i++) r.p[i] = lp ? lerpf(a.p[i], b.p[i], w) : b.p[i];
    r.valid = true;
    return r;
}

}   // namespace

// hle/rt64_rigid_body.cpp
void RigidBody::update_linear(const M4& prev, const M4& cur, uint8_t comp) {
    if (comp == kAuto) {
        const float eps = 1e-6f, velocity_tolerance = 5.0f, magnitude_threshold = 10.0f;
        float v[3];
        for (int i = 0; i < 3; i++) v[i] = cur.m[3][i] - prev.m[3][i];
        const float prev_mag = len3(lin_vel);
        float cur_mag = len3(v);
        float a[3], b[3];
        for (int i = 0; i < 3; i++) { a[i] = lin_vel[i] / std::max(prev_mag, eps); b[i] = v[i] / std::max(cur_mag, eps); }
        const float d = std::max(dot3(a, b), eps);
        cur_mag /= d;
        lerp_t = (cur_mag < velocity_tolerance) || (cur_mag / std::max(prev_mag, eps)) < magnitude_threshold;
        memcpy(lin_vel, v, sizeof(v));
    }
    else {
        lerp_t = comp == kInterpolate;
        lin_vel[0] = lin_vel[1] = lin_vel[2] = 0.0f;
    }
}

void RigidBody::update_angular(uint8_t rot, uint8_t scale, uint8_t skew) {
    // RT64 tracks an angular velocity here that nothing reads; AUTO always
    // interpolates, and scale and skew follow it when they are AUTO too.
    lerp_s = scale == kInterpolate;
    lerp_k = skew == kInterpolate;
    if (rot == kAuto) {
        lerp_r = true;
        if (scale == kAuto) lerp_s = lerp_r;
        if (skew == kAuto) lerp_k = lerp_r;
    }
    else {
        lerp_r = rot == kInterpolate;
    }
}

void RigidBody::update_perspective(uint8_t persp) { lerp_p = persp == kInterpolate; }

void RigidBody::update_decomposition(const M4& cur, bool dec) {
    const uint8_t n = idx ^ 1;
    if (dec) decompose(cur, xf[n]);
    else xf[n] = Decomposed();
    idx = n;
    lerp_dec = dec;
}

M4 RigidBody::lerp(float w, const M4& prev_m, const M4& cur_m, bool slerp) const {
    if (!lerp_dec || !xf[0].valid || !xf[1].valid) return lerp_components(prev_m, cur_m, lerp_t, lerp_r, lerp_p, w);
    const Decomposed& prev = xf[idx ^ 1];
    const Decomposed& cur = xf[idx];
    Decomposed pc = prev;
    if (pc.flip != cur.flip) {
        // A mirrored pair: bias the previous rotation and scale to the
        // equivalent closest to the current one.
        const float pi = 3.14159265f;
        float ax[4], ay[4], az[4], xr[4], yr[4], zr[4];
        qaxis(1.0f, 0.0f, 0.0f, pi, ax); qaxis(0.0f, 1.0f, 0.0f, pi, ay); qaxis(0.0f, 0.0f, 1.0f, pi, az);
        qmul(pc.q, ax, xr); qmul(pc.q, ay, yr); qmul(pc.q, az, zr);
        float rd = std::fabs(dot4(pc.q, cur.q));
        const float xd = std::fabs(dot4(xr, cur.q)), yd = std::fabs(dot4(yr, cur.q)), zd = std::fabs(dot4(zr, cur.q));
        if (xd > rd) { memcpy(pc.q, xr, sizeof(xr)); pc.s[0] = prev.s[0]; pc.s[1] = -prev.s[1]; pc.s[2] = -prev.s[2]; rd = xd; }
        if (yd > rd) { memcpy(pc.q, yr, sizeof(yr)); pc.s[0] = -prev.s[0]; pc.s[1] = prev.s[1]; pc.s[2] = -prev.s[2]; rd = yd; }
        if (zd > rd) { memcpy(pc.q, zr, sizeof(zr)); pc.s[0] = -prev.s[0]; pc.s[1] = -prev.s[1]; pc.s[2] = prev.s[2]; }
    }
    return recompose(lerp_transforms(pc, cur, w, lerp_t, lerp_r, lerp_s, lerp_k, lerp_p, slerp));
}

// ---- matching (hle/rt64_game_frame.cpp)

namespace {

bool auto_ordered(const Group& g) { return g.id == kIdAuto || (g.id != kIdIgnore && g.order == kOrderAuto); }
bool linear_ordered(const Group& g) { return g.id != kIdAuto && g.id != kIdIgnore && g.order == kOrderLinear; }

// computeTransformMatch: how far apart two transforms are, or < 0 when they
// can't be the same (mirrored from each other).
float transform_difference(const M4& cur, const M4& cur_vp, const M4& prev, const M4& prev_vp, const float* prev_vel) {
    const float d0 = det3(prev.m[0][0], prev.m[0][1], prev.m[0][2], prev.m[1][0], prev.m[1][1], prev.m[1][2], prev.m[2][0], prev.m[2][1], prev.m[2][2]);
    const float d1 = det3(cur.m[0][0], cur.m[0][1], cur.m[0][2], cur.m[1][0], cur.m[1][1], cur.m[1][2], cur.m[2][0], cur.m[2][1], cur.m[2][2]);
    if (d0 * d1 < 0.0f) return -1.0f;
    float dp[3];
    for (int i = 0; i < 3; i++) dp[i] = cur.m[3][i] - (prev.m[3][i] + (prev_vel != nullptr ? prev_vel[i] : 0.0f));
    float total = len3(dp);
    for (int r = 0; r < 3; r++) {
        const float lc = len3(cur.m[r]), lp = len3(prev.m[r]);
        float a[3], b[3];
        for (int k = 0; k < 3; k++) { a[k] = cur.m[r][k] / lc; b[k] = prev.m[r][k] / lp; }
        total += 1.0f - dot3(a, b);
    }
    float sp[4], sc[4];
    for (int j = 0; j < 4; j++) {
        sp[j] = prev.m[3][0] * prev_vp.m[0][j] + prev.m[3][1] * prev_vp.m[1][j] + prev.m[3][2] * prev_vp.m[2][j] + prev.m[3][3] * prev_vp.m[3][j];
        sc[j] = cur.m[3][0] * cur_vp.m[0][j] + cur.m[3][1] * cur_vp.m[1][j] + cur.m[3][2] * cur_vp.m[2][j] + cur.m[3][3] * cur_vp.m[3][j];
    }
    if (std::fabs(sp[3]) >= 1e-6f) for (int j = 0; j < 3; j++) sp[j] /= sp[3];
    if (std::fabs(sc[3]) >= 1e-6f) for (int j = 0; j < 3; j++) sc[j] /= sc[3];
    const float ds[3] = { sc[0] - sp[0], sc[1] - sp[1], sc[2] - sp[2] };
    total += len3(ds);
    return total;
}

int modulo(int a, int b) {
    if (b != 0) { const int r = a % b; return r < 0 ? r + b : r; }
    return a;
}

// One tile's scroll between two frames (GameFrame::matchScene's tile part).
void match_tile(const TileParams& cur, const TileParams& prev, TileMap& map) {
    const int ms = cur.masks > 0 ? (1 << cur.masks) : 0, mt = cur.maskt > 0 ? (1 << cur.maskt) : 0;
    const int cms = cur.cms | (cur.masks == 0 ? 2 : 0), cmt = cur.cmt | (cur.maskt == 0 ? 2 : 0);
    auto axis = [](float c, float p, int mask, int cm, float& prev_out, float& delta) {
        const float d = c - p;
        const long ci = std::lround(c), expected = std::lround(p + delta);
        const bool wrapped = cm == 0 && modulo((int)ci, mask * 4) == modulo((int)expected, mask * 4);
        prev_out = c - delta;
        delta = (wrapped || std::fabs(d) >= (float)(mask * 2)) ? delta : d;
    };
    axis((float)cur.uls, (float)prev.uls, ms, cms, map.prev_uls, map.d_uls);
    axis((float)cur.ult, (float)prev.ult, mt, cmt, map.prev_ult, map.d_ult);
    axis((float)cur.lrs, (float)prev.lrs, ms, cms, map.prev_lrs, map.d_lrs);
    axis((float)cur.lrt, (float)prev.lrt, mt, cmt, map.prev_lrt, map.d_lrt);
    map.mapped = true;
}

bool tiles_compatible(const TileParams& a, const TileParams& b) {
    const int acms = a.cms | (a.masks == 0 ? 2 : 0), acmt = a.cmt | (a.maskt == 0 ? 2 : 0);
    const int bcms = b.cms | (b.masks == 0 ? 2 : 0), bcmt = b.cmt | (b.maskt == 0 ? 2 : 0);
    return a.fmt == b.fmt && a.siz == b.siz && a.line == b.line && a.masks == b.masks && a.maskt == b.maskt &&
           a.shifts == b.shifts && a.shiftt == b.shiftt && acms == bcms && acmt == bcmt;
}

// The k-th occurrence of each key in cur with the k-th in prev: pairs of
// (cur index, prev index) in cur's order.
template <typename K, typename F>
void match_linear(std::vector<std::pair<K, uint32_t>>& c, std::vector<std::pair<K, uint32_t>>& p, F&& on_pair) {
    std::stable_sort(c.begin(), c.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::stable_sort(p.begin(), p.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t i = 0, j = 0;
    while (i < c.size() && j < p.size()) {
        if (c[i].first < p[j].first) i++;
        else if (c[i].first > p[j].first) j++;
        else { on_pair(c[i].second, p[j].second); i++; j++; }
    }
}

// The camera's position and forward direction from a view matrix.
void camera_of(const M4& v, float* eye, float* fwd) {
    for (int j = 0; j < 3; j++) {
        eye[j] = -(v.m[3][0] * v.m[j][0] + v.m[3][1] * v.m[j][1] + v.m[3][2] * v.m[j][2]);
        fwd[j] = -v.m[j][2];
    }
    const float l = len3(fwd);
    if (l > 0.0f) for (int j = 0; j < 3; j++) fwd[j] /= l;
}

}   // namespace

MatchResult match(const FrameLog& prev, FrameLog& cur, float w, bool (*world)(uint32_t, bool, void*), void* ctx) {
    MatchResult res;
    res.xf = (uint32_t)cur.xf.size();
    res.pj = (uint32_t)cur.pj.size();
    res.tiles = (uint32_t)cur.tiles.size();
    for (XfEntry& e : cur.xf) { e.prev = -1; e.rb = RigidBody(); e.interp = false; e.vpos = e.vtc = false; }
    for (PjEntry& e : cur.pj) { e.prev = -1; e.rb = RigidBody(); e.interp = false; e.cam_vel[0] = e.cam_vel[1] = e.cam_vel[2] = 0.0f; }
    for (TileEntry& e : cur.tiles) { e.prev = -1; e.map[0] = e.map[1] = TileMap(); e.interp = false; e.du[0] = e.du[1] = e.dv[0] = e.dv[1] = 0.0f; }
    if (!prev.valid || !cur.valid) return res;

    // ---- world transforms
    std::vector<uint8_t> prev_used(prev.xf.size(), 0);
    auto match_xf = [&](uint32_t i, uint32_t j) {
        XfEntry& e = cur.xf[i];
        const XfEntry& p = prev.xf[j];
        const Group& g = cur.groups[e.group];
        e.prev = (int32_t)j;
        e.rb = p.rb;
        e.rb.update_linear(p.m, e.m, g.pos);
        e.rb.update_angular(g.rot, g.scale, g.skew);
        e.rb.update_perspective(g.persp);
        e.rb.update_decomposition(e.m, g.decompose);
        prev_used[j] = 1;
        res.xf_matched++;
        // Vertex positions and texcoords, when the group asks and both
        // loaded as many vertices (GameFrame::matchTransform).
        if (e.vstored && p.vstored && e.vcount == p.vcount && e.vcount > 0) {
            const int16_t* a = &cur.vdata[(size_t)e.vfirst * 5];
            const int16_t* b = &prev.vdata[(size_t)p.vfirst * 5];
            bool pos_same = true, tc_same = true;
            for (uint32_t v = 0; v < e.vcount && (pos_same || tc_same); v++) {
                if (a[v * 5] != b[v * 5] || a[v * 5 + 1] != b[v * 5 + 1] || a[v * 5 + 2] != b[v * 5 + 2]) pos_same = false;
                if (a[v * 5 + 3] != b[v * 5 + 3] || a[v * 5 + 4] != b[v * 5 + 4]) tc_same = false;
            }
            e.vpos = g.vert != kSkip && !pos_same;
            e.vtc = g.tc != kSkip && (g.tc != kAuto || pos_same) && !tc_same;
            e.vprev = p.vfirst;
        }
    };
    {
        std::vector<std::pair<uint32_t, uint32_t>> c, p;
        for (uint32_t i = 0; i < cur.xf.size(); i++) if (linear_ordered(cur.groups[cur.xf[i].group])) c.emplace_back(cur.groups[cur.xf[i].group].id, i);
        for (uint32_t j = 0; j < prev.xf.size(); j++) if (linear_ordered(prev.groups[prev.xf[j].group])) p.emplace_back(prev.groups[prev.xf[j].group].id, j);
        match_linear(c, p, match_xf);
    }
    {
        // AUTO: RT64 pairs the transforms of draw calls that look alike and
        // takes the closest pairs first. Here transforms with the same group
        // id whose vertices come from the same address are candidates.
        struct Cand { uint32_t i, j; float d; };
        std::vector<Cand> cands;
        std::vector<std::pair<uint64_t, uint32_t>> pa;
        for (uint32_t j = 0; j < prev.xf.size(); j++) {
            const Group& g = prev.groups[prev.xf[j].group];
            if (auto_ordered(g) && !prev_used[j]) pa.emplace_back(((uint64_t)g.id << 32) | prev.xf[j].key, j);
        }
        std::sort(pa.begin(), pa.end());
        const M4 I = identity();
        for (uint32_t i = 0; i < cur.xf.size(); i++) {
            const XfEntry& e = cur.xf[i];
            const Group& g = cur.groups[e.group];
            if (!auto_ordered(g) || e.prev >= 0) continue;
            const uint64_t key = ((uint64_t)g.id << 32) | e.key;
            auto it = std::lower_bound(pa.begin(), pa.end(), std::make_pair(key, 0u));
            int n = 0;
            for (; it != pa.end() && it->first == key && n < 32; ++it, n++) {
                const XfEntry& p = prev.xf[it->second];
                const M4& cvp = e.pj >= 0 && (size_t)e.pj < cur.pj.size() ? cur.pj[e.pj].full : I;
                const M4& pvp = p.pj >= 0 && (size_t)p.pj < prev.pj.size() ? prev.pj[p.pj].full : I;
                const float d = transform_difference(e.m, cvp, p.m, pvp, p.rb.lin_vel);
                if (d >= 0.0f) cands.push_back({ i, it->second, d });
            }
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
        for (const Cand& c : cands) {
            if (cur.xf[c.i].prev >= 0 || prev_used[c.j]) continue;
            match_xf(c.i, c.j);
        }
    }
    // ---- projections: the k-th of each id with the k-th of the previous
    // frame (RT64 pairs them per scene, in order, when the ids agree), and
    // here of each kind too. A perspective paired with an ortho blends into
    // neither (w mixed in: everything drawn with it shrinks toward the
    // screen's corner), and a game may load more of one kind under an id in
    // some frames: DK64's world camera is reloaded under the tag its scope
    // and crosshair (ortho) are drawn with, which then took the camera's
    // projection for a partner and showed in a small box for an in-between.
    // An ortho's scale is part of the key too: orthos of other scales are
    // other spaces, and DK64's HUD has two under that tag (over 320x240,
    // and over four times that), so an element of the finer one appearing
    // paired the scope's ortho with it, blending the two scales to 62%
    // (the scope and crosshair pulled toward the top-left corner). A
    // perspective's field of view may change (a zoom): those still pair.
    {
        auto key = [](const Group& g, const PjEntry& e) {
            uint64_t k = ((uint64_t)g.id << 1) | (e.perspective ? 1u : 0u);
            if (!e.perspective) {
                // log2 of each axis's scale in eighths: a 9% step.
                auto q = [](float v) -> uint64_t {
                    v = std::fabs(v);
                    return v > 1e-12f ? (uint64_t)(std::lround(std::log2(v) * 8.0f) & 0xFFF) : 0u;
                };
                k |= (q(e.proj.m[0][0]) << 40) | (q(e.proj.m[1][1]) << 52);
            }
            return k;
        };
        std::vector<std::pair<uint64_t, uint32_t>> c, p;
        for (uint32_t i = 0; i < cur.pj.size(); i++) {
            const Group& g = cur.groups[cur.pj[i].group];
            if (g.id != kIdIgnore) c.emplace_back(key(g, cur.pj[i]), i);
        }
        for (uint32_t j = 0; j < prev.pj.size(); j++) {
            const Group& g = prev.groups[prev.pj[j].group];
            if (g.id != kIdIgnore) p.emplace_back(key(g, prev.pj[j]), j);
        }
        match_linear(c, p, [&](uint32_t i, uint32_t j) {
            PjEntry& e = cur.pj[i];
            const PjEntry& pp = prev.pj[j];
            const Group& g = cur.groups[e.group];
            // Untagged projections: simple interpolation of everything
            // (cameras look better without decomposition).
            bool dec = false;
            uint8_t lin = kInterpolate, rot = kInterpolate, sc = kInterpolate, sk = kInterpolate;
            if (g.id != kIdAuto) { dec = g.decompose; lin = g.pos; rot = g.rot; sc = g.scale; sk = g.skew; }
            e.prev = (int32_t)j;
            e.rb = pp.rb;
            e.rb.update_linear(pp.view, e.view, lin);
            e.rb.update_angular(rot, sc, sk);
            e.rb.update_decomposition(e.view, dec);
            res.pj_matched++;
            // Not RT64: a world camera that jumps is a scene cut, where an
            // in-between frame would show neither shot.
            if (world != nullptr && e.perspective && world(g.id, true, ctx)) {
                float e0[3], f0[3], e1[3], f1[3];
                camera_of(pp.view, e0, f0);
                camera_of(e.view, e1, f1);
                float v[3];
                for (int k = 0; k < 3; k++) { v[k] = e1[k] - e0[k]; e.cam_vel[k] = v[k]; }
                const float mag = len3(v), prev_mag = len3(pp.cam_vel);
                const bool jump = mag > g_cut_min_jump && mag > 10.0f * prev_mag;
                const bool turn = dot3(f0, f1) < 0.7071f;
                if (jump || turn) res.camera_cut = true;
            }
        });
    }

    // ---- tiles: the k-th draw of each kind with the k-th of the previous
    // frame, for transform groups whose tile component is not SKIP.
    {
        std::vector<std::pair<uint32_t, uint32_t>> c, p;
        for (uint32_t i = 0; i < cur.tiles.size(); i++) if (cur.tiles[i].flag != kSkip) c.emplace_back(cur.tiles[i].key, i);
        for (uint32_t j = 0; j < prev.tiles.size(); j++) if (prev.tiles[j].flag != kSkip) p.emplace_back(prev.tiles[j].key, j);
        match_linear(c, p, [&](uint32_t i, uint32_t j) {
            TileEntry& e = cur.tiles[i];
            const TileEntry& pp = prev.tiles[j];
            if (e.flag == kAuto && pp.flag == kAuto && e.tex != pp.tex) return;
            e.prev = (int32_t)j;
            for (int t = 0; t < e.ntiles && t < pp.ntiles; t++) {
                if (!tiles_compatible(e.t[t], pp.t[t])) continue;
                e.map[t] = pp.map[t];
                match_tile(e.t[t], pp.t[t], e.map[t]);
            }
        });
    }
    if (w >= 0.0f) reweight(prev, cur, w, &res);
    return res;
}

void reweight(const FrameLog& prev, FrameLog& cur, float w, MatchResult* res) {
    MatchResult counts;
    for (XfEntry& e : cur.xf) {
        e.interp = false;
        if (e.prev < 0) continue;
        const RigidBody& rb = e.rb;
        if (rb.lerp_t || rb.lerp_r || rb.lerp_s || rb.lerp_k || rb.lerp_p) {
            e.lerped = rb.lerp(w, prev.xf[e.prev].m, e.m, true);
            e.interp = true;
        }
        if (e.interp || e.vpos || e.vtc) counts.xf_interp++;
        if (e.vpos || e.vtc) counts.vert_entries++;
    }
    for (PjEntry& e : cur.pj) {
        e.interp = false;
        if (e.prev < 0 || !(e.rb.lerp_t || e.rb.lerp_r)) continue;
        const PjEntry& pp = prev.pj[e.prev];
        const M4 view = e.rb.lerp(w, pp.view, e.view, true);
        const M4 proj = lerp_matrix(pp.proj, e.proj, w);
        e.lerped = mul(view, proj);
        e.interp = true;
        counts.pj_interp++;
    }
    for (TileEntry& e : cur.tiles) {
        e.interp = false;
        if (e.prev < 0) continue;
        bool any = false;
        for (int t = 0; t < e.ntiles && t < 2; t++) {
            e.du[t] = e.dv[t] = 0.0f;
            const TileMap& m = e.map[t];
            if (!m.mapped) continue;
            const float us = m.prev_uls + ((float)e.t[t].uls - m.prev_uls) * w;
            const float ut = m.prev_ult + ((float)e.t[t].ult - m.prev_ult) * w;
            e.du[t] = (us - (float)e.t[t].uls) * 0.25f;
            e.dv[t] = (ut - (float)e.t[t].ult) * 0.25f;
            if (e.du[t] != 0.0f || e.dv[t] != 0.0f) any = true;
        }
        if (any) { e.interp = true; counts.tiles_interp++; }
    }
    if (res != nullptr) {
        res->xf_interp = counts.xf_interp;
        res->vert_entries = counts.vert_entries;
        res->pj_interp = counts.pj_interp;
        res->tiles_interp = counts.tiles_interp;
    }
}

}   // namespace interp
}   // namespace rt64_3ds
