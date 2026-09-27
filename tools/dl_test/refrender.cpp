// Host reference renderer: rasterises a FrameRecord with the RDP's own
// combiner and blender formulas (point sampling, no anti-aliasing), so a
// frame can be checked against what the N64 would show independently of
// the PICA mapping. Output is 400x240, N64 x = 0 at column 40.
#include "refrender.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

#include "rt64_3ds_texdecode.h"

namespace {

using rt64_3ds::DrawRecord;
using rt64_3ds::FrameRecord;
using rt64_3ds::TexDesc;
using rt64_3ds::Vtx3ds;

struct Tex { int w = 0, h = 0; std::vector<uint32_t> px; };

struct C4 { int r, g, b, a; };

inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

struct V { float x, y, z, w, u, v, r, g, b, a; };

V lerp(const V& p, const V& q, float t) {
    return { p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, p.w + (q.w - p.w) * t,
             p.u + (q.u - p.u) * t, p.v + (q.v - p.v) * t, p.r + (q.r - p.r) * t, p.g + (q.g - p.g) * t,
             p.b + (q.b - p.b) * t, p.a + (q.a - p.a) * t };
}

// Clip a polygon against z' >= 0 (the near plane; z' = (z + w) / 2).
std::vector<V> clip_near(const std::vector<V>& in) {
    std::vector<V> out;
    for (size_t i = 0; i < in.size(); i++) {
        const V& p = in[i];
        const V& q = in[(i + 1) % in.size()];
        float dp = p.z - 1e-5f * p.w, dq = q.z - 1e-5f * q.w;
        bool pin = dp >= 0 && p.w > 0, qin = dq >= 0 && q.w > 0;
        if (pin) out.push_back(p);
        if (pin != qin && dp != dq) out.push_back(lerp(p, q, dp / (dp - dq)));
    }
    return out;
}

class Renderer {
public:
    Renderer(const uint8_t* rdram, const FrameRecord& f, RefOptions opt) : rdram_(rdram), f_(f), opt_(opt) {
        color_.assign(400 * 240, 0x000000FFu);
        depth_.assign(400 * 240, 1.0f);
    }

    void run() {
        for (size_t i = 0; i < f_.draws.size(); i++) {
            if (opt_.only_draw >= 0 && (int)i != opt_.only_draw) continue;
            if (opt_.max_draw >= 0 && (int)i > opt_.max_draw) break;
            draw(f_.draws[i]);
        }
    }

    const std::vector<uint32_t>& color() const { return color_; }

private:
    const uint8_t* rdram_;
    const FrameRecord& f_;
    RefOptions opt_;
    std::vector<uint32_t> color_;
    std::vector<float> depth_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, Tex> texcache_;

    const Tex* texture(const TexDesc& d) {
        if (!d.valid || d.snapshot) return nullptr;
        auto key = std::make_tuple(d.addr, d.pitch | ((uint32_t)d.fmt << 16) | ((uint32_t)d.siz << 20), (uint32_t)d.width | ((uint32_t)d.height << 16),
                                   d.tlut_index, (uint32_t)d.palette | ((uint32_t)d.nibble << 8) | ((uint32_t)d.tlut_mode << 12) | ((uint32_t)d.block << 16),
                                   (uint32_t)d.dxt | ((uint32_t)d.load_word << 16));
        auto it = texcache_.find(key);
        if (it != texcache_.end()) return &it->second;
        Tex& t = texcache_[key];
        t.w = d.width; t.h = d.height;
        t.px.resize((size_t)t.w * t.h);
        const uint16_t* pal = f_.tlut.empty() ? nullptr : f_.tlut.data() + d.tlut_index;
        rt64_3ds::decode_rgba8(rdram_, d, pal, t.px.data());
        return &t;
    }

    static int wrap(int c, int size, int mask, int cm) {
        if (mask != 0) {
            int m = 1 << mask;
            if ((cm & 1) && (c & m)) c = ~c;   // mirror
            c &= m - 1;
        }
        else if (!(cm & 2)) {
            c %= size; if (c < 0) c += size;   // no mask, no clamp: wrap on the tile size
        }
        if (c < 0) c = 0;
        if (c >= size) c = size - 1;
        return c;
    }

    C4 sample(const TexDesc& d, const Tex* t, float u, float v) {
        if (d.snapshot) return { 128, 128, 128, 255 };
        if (t == nullptr) return { 255, 255, 255, 255 };
        int s = (int)floorf(u), tt = (int)floorf(v);
        if (d.cms & 2) { if (s < 0) s = 0; }
        if (d.cmt & 2) { if (tt < 0) tt = 0; }
        s = wrap(s, t->w, d.masks, d.cms);
        tt = wrap(tt, t->h, d.maskt, d.cmt);
        uint32_t p = t->px[(size_t)tt * t->w + s];
        return { (int)(p >> 24), (int)((p >> 16) & 0xFF), (int)((p >> 8) & 0xFF), (int)(p & 0xFF) };
    }

    // One combiner cycle, integer as the RDP: ((A - B) * C + 0x80 >> 8) + D.
    static int comb(int a, int b, int c, int d) { return clamp255((((a - b) * c) + 0x80) / 256 + d); }

    C4 combine(const DrawRecord& d, int cycle, const C4& t0, const C4& t1, const C4& sh, const C4& prev) {
        uint32_t w0 = d.cc_w0, w1 = d.cc_w1;
        uint32_t a, b, c, dd, aa, ab, ac, ad;
        if (cycle == 0) {
            a = (w0 >> 20) & 0xF; c = (w0 >> 15) & 0x1F; aa = (w0 >> 12) & 7; ac = (w0 >> 9) & 7;
            b = (w1 >> 28) & 0xF; dd = (w1 >> 15) & 7; ab = (w1 >> 12) & 7; ad = (w1 >> 9) & 7;
        }
        else {
            a = (w0 >> 5) & 0xF; c = w0 & 0x1F; b = (w1 >> 24) & 0xF; aa = (w1 >> 21) & 7; ac = (w1 >> 18) & 7;
            dd = (w1 >> 6) & 7; ab = (w1 >> 3) & 7; ad = w1 & 7;
        }
        const C4 prim = { d.prim[0], d.prim[1], d.prim[2], d.prim[3] };
        const C4 env = { d.env[0], d.env[1], d.env[2], d.env[3] };
        const int lod = opt_.lod_fraction, plf = d.prim_lod_frac;
        auto rgb = [&](int ch, uint32_t sel, int which) -> int {
            auto chn = [&](const C4& v) { return ch == 0 ? v.r : (ch == 1 ? v.g : v.b); };
            switch (which) {
                case 0:   // A
                    switch (sel) { case 0: return chn(prev); case 1: return chn(t0); case 2: return chn(t1); case 3: return chn(prim);
                                   case 4: return chn(sh); case 5: return chn(env); case 6: return 255; case 7: return 128; default: return 0; }
                case 1:   // B
                    switch (sel) { case 0: return chn(prev); case 1: return chn(t0); case 2: return chn(t1); case 3: return chn(prim);
                                   case 4: return chn(sh); case 5: return chn(env); default: return 0; }
                case 2:   // C (0..256)
                    switch (sel) { case 0: return chn(prev); case 1: return chn(t0); case 2: return chn(t1); case 3: return chn(prim);
                                   case 4: return chn(sh); case 5: return chn(env); case 7: return prev.a; case 8: return t0.a; case 9: return t1.a;
                                   case 10: return prim.a; case 11: return sh.a; case 12: return env.a; case 13: return lod; case 14: return plf; default: return 0; }
                default:  // D
                    switch (sel) { case 0: return chn(prev); case 1: return chn(t0); case 2: return chn(t1); case 3: return chn(prim);
                                   case 4: return chn(sh); case 5: return chn(env); case 6: return 255; default: return 0; }
            }
        };
        auto alpha = [&](uint32_t sel, bool is_c) -> int {
            if (is_c) {
                switch (sel) { case 0: return lod; case 1: return t0.a; case 2: return t1.a; case 3: return prim.a; case 4: return sh.a; case 5: return env.a; case 6: return plf; default: return 0; }
            }
            switch (sel) { case 0: return prev.a; case 1: return t0.a; case 2: return t1.a; case 3: return prim.a; case 4: return sh.a; case 5: return env.a; case 6: return 255; default: return 0; }
        };
        auto cval = [](int v) { return v == 255 ? 256 : v; };
        C4 o;
        o.r = comb(rgb(0, a, 0), rgb(0, b, 1), cval(rgb(0, c, 2)), rgb(0, dd, 3));
        o.g = comb(rgb(1, a, 0), rgb(1, b, 1), cval(rgb(1, c, 2)), rgb(1, dd, 3));
        o.b = comb(rgb(2, a, 0), rgb(2, b, 1), cval(rgb(2, c, 2)), rgb(2, dd, 3));
        o.a = comb(alpha(aa, false), alpha(ab, false), cval(alpha(ac, true)), alpha(ad, false));
        return o;
    }

    void blend_pixel(const DrawRecord& d, int idx, C4 in, int shade_a) {
        uint32_t l = d.othermode_l;
        uint32_t cyc = (d.othermode_h >> 20) & 3;
        uint32_t memc = color_[idx];
        C4 mem = { (int)(memc >> 24), (int)((memc >> 16) & 0xFF), (int)((memc >> 8) & 0xFF), 255 };
        C4 blendc = { d.blend[0], d.blend[1], d.blend[2], d.blend[3] }, fogc = { d.fog[0], d.fog[1], d.fog[2], d.fog[3] };
        bool force_bl = (l & 0x4000) != 0;
        auto cycle_blend = [&](int cy, const C4& inp, bool last) -> C4 {
            uint32_t p = cy == 0 ? (l >> 30) & 3 : (l >> 28) & 3;
            uint32_t a = cy == 0 ? (l >> 26) & 3 : (l >> 24) & 3;
            uint32_t m = cy == 0 ? (l >> 22) & 3 : (l >> 20) & 3;
            uint32_t b = cy == 0 ? (l >> 18) & 3 : (l >> 16) & 3;
            auto pm = [&](uint32_t s) { return s == 0 ? inp : (s == 1 ? mem : (s == 2 ? blendc : fogc)); };
            int av = a == 0 ? in.a : (a == 1 ? fogc.a : (a == 2 ? shade_a : 0));
            int bv = b == 0 ? 255 - av : (b == 1 ? 255 : (b == 2 ? 255 : 0));
            C4 P = pm(p), M = pm(m);
            if (last && !force_bl) return P;   // full coverage: no blend
            int den = (b == 0) ? 255 : av + bv;
            if (den == 0) return P;
            return { clamp255((P.r * av + M.r * bv) / den), clamp255((P.g * av + M.g * bv) / den), clamp255((P.b * av + M.b * bv) / den), inp.a };
        };
        C4 out;
        if (cyc == 1) out = cycle_blend(1, cycle_blend(0, in, false), true);
        else out = cycle_blend(0, in, true);
        color_[idx] = ((uint32_t)out.r << 24) | ((uint32_t)out.g << 16) | ((uint32_t)out.b << 8) | 0xFF;
    }

    void draw(const DrawRecord& d) {
        uint32_t cyc = (d.othermode_h >> 20) & 3;
        const Tex* t0 = texture(d.tex[0]);
        const Tex* t1 = texture(d.tex[1]);
        int sx0 = d.scissor[0] + 40, sy0 = d.scissor[1], sx1 = d.scissor[2] + 40, sy1 = d.scissor[3];
        if (d.scissor[0] <= 0 && d.scissor[2] >= 320) { sx0 = 0; sx1 = 400; }
        if (sx0 < 0) sx0 = 0; if (sy0 < 0) sy0 = 0; if (sx1 > 400) sx1 = 400; if (sy1 > 240) sy1 = 240;
        uint32_t l = d.othermode_l;
        bool is_rect = d.kind != DrawRecord::Tris;
        bool zcmp = (l & 0x10) && !is_rect && cyc < 2, zupd = (l & 0x20) && !is_rect && cyc < 2;
        uint32_t zmode = (l >> 10) & 3;
        for (uint32_t k = d.first; k + 2 < d.first + d.count; k += 3) {
            std::vector<V> poly;
            for (int j = 0; j < 3; j++) {
                const Vtx3ds& s = f_.verts[k + j];
                poly.push_back({ s.x, s.y, s.z, s.w, s.u, s.v, (float)s.r, (float)s.g, (float)s.b, (float)s.a });
            }
            if (!is_rect) poly = clip_near(poly);   // rectangles sit at z = 0 with w = 1
            if (poly.size() < 3) continue;
            for (size_t j = 1; j + 1 < poly.size(); j++) raster(d, cyc, poly[0], poly[j], poly[j + 1], t0, t1, sx0, sy0, sx1, sy1, zcmp, zupd, zmode);
        }
    }

    void raster(const DrawRecord& d, uint32_t cyc, const V& a, const V& b, const V& c, const Tex* t0, const Tex* t1,
                int sx0, int sy0, int sx1, int sy1, bool zcmp, bool zupd, uint32_t zmode) {
        const V* vs[3] = { &a, &b, &c };
        float X[3], Y[3], IW[3];
        for (int i = 0; i < 3; i++) { IW[i] = 1.0f / vs[i]->w; X[i] = vs[i]->x * IW[i] + 40.0f; Y[i] = vs[i]->y * IW[i]; }
        float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
        if (fabsf(area) < 1e-6f) return;
        int minx = (int)floorf(fminf(X[0], fminf(X[1], X[2]))), maxx = (int)ceilf(fmaxf(X[0], fmaxf(X[1], X[2])));
        int miny = (int)floorf(fminf(Y[0], fminf(Y[1], Y[2]))), maxy = (int)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
        if (minx < sx0) minx = sx0; if (maxx > sx1) maxx = sx1; if (miny < sy0) miny = sy0; if (maxy > sy1) maxy = sy1;
        uint32_t l = d.othermode_l;
        for (int py = miny; py < maxy; py++) {
            for (int px = minx; px < maxx; px++) {
                float fx = px + 0.5f, fy = py + 0.5f;
                float w0 = ((X[1] - fx) * (Y[2] - fy) - (X[2] - fx) * (Y[1] - fy)) / area;
                float w1 = ((X[2] - fx) * (Y[0] - fy) - (X[0] - fx) * (Y[2] - fy)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float iw = w0 * IW[0] + w1 * IW[1] + w2 * IW[2];
                float p0 = w0 * IW[0] / iw, p1 = w1 * IW[1] / iw, p2 = w2 * IW[2] / iw;   // perspective-correct weights
                float z = (w0 * a.z * IW[0] + w1 * b.z * IW[1] + w2 * c.z * IW[2]);       // z'/w is affine in screen space
                int idx = py * 400 + px;
                if (zcmp) {
                    if (zmode == 3) { if (z > depth_[idx] + 0.002f) continue; }
                    else if (z > depth_[idx]) continue;
                }
                float u = p0 * a.u + p1 * b.u + p2 * c.u, v = p0 * a.v + p1 * b.v + p2 * c.v;
                C4 sh = { clamp255((int)(p0 * a.r + p1 * b.r + p2 * c.r)), clamp255((int)(p0 * a.g + p1 * b.g + p2 * c.g)),
                          clamp255((int)(p0 * a.b + p1 * b.b + p2 * c.b)), clamp255((int)(p0 * a.a + p1 * b.a + p2 * c.a)) };
                if (d.kind != DrawRecord::Tris && !opt_.rect_shade_white) sh = { 0, 0, 0, 0 };
                C4 out;
                if (cyc == 3) out = { d.prim[0], d.prim[1], d.prim[2], d.prim[3] };        // fill colour rides in prim
                else {
                    C4 tx0 = d.tex[0].valid ? sample(d.tex[0], t0, u, v) : C4{ 0, 0, 0, 0 };
                    C4 tx1 = d.tex[1].valid ? sample(d.tex[1], t1, u * d.uv1[0] + d.uv1[2], v * d.uv1[1] + d.uv1[3]) : tx0;
                    if (cyc == 2) out = tx0;
                    else {
                        out = combine(d, 0, tx0, tx1, sh, C4{ 0, 0, 0, 0 });
                        if (cyc == 1) out = combine(d, 1, tx0, tx1, sh, out);
                    }
                }
                // Alpha compare and coverage-from-alpha.
                uint32_t ac = l & 3;
                if (cyc != 3) {
                    if (ac == 1 && out.a < (d.blend[3] ? d.blend[3] : 1)) continue;
                    if (ac == 3 && out.a == 0) continue;
                    if ((l & 0x2000) && out.a < 32) continue;
                    if (cyc == 2 && ac && out.a == 0) continue;
                }
                if (cyc >= 2) {
                    color_[idx] = ((uint32_t)out.r << 24) | ((uint32_t)out.g << 16) | ((uint32_t)out.b << 8) | 0xFF;
                }
                else blend_pixel(d, idx, out, sh.a);
                if (zupd) depth_[idx] = z;
            }
        }
    }
};

}   // namespace

bool ref_render(const uint8_t* rdram, const rt64_3ds::FrameRecord& frame, const RefOptions& opt, const char* ppm_path) {
    Renderer r(rdram, frame, opt);
    r.run();
    FILE* f = fopen(ppm_path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n400 240\n255\n");
    for (uint32_t p : r.color()) { uint8_t px[3] = { (uint8_t)(p >> 24), (uint8_t)(p >> 16), (uint8_t)(p >> 8) }; fwrite(px, 1, 3, f); }
    fclose(f);
    return true;
}

bool dump_texture(const uint8_t* rdram, const rt64_3ds::FrameRecord& frame, const rt64_3ds::TexDesc& d, const char* ppm_path) {
    if (!d.valid) return false;
    std::vector<uint32_t> px((size_t)d.width * d.height);
    const uint16_t* pal = frame.tlut.empty() ? nullptr : frame.tlut.data() + d.tlut_index;
    rt64_3ds::decode_rgba8(rdram, d, pal, px.data());
    FILE* f = fopen(ppm_path, "wb");
    if (!f) return false;
    // RGB over a checkerboard where alpha is low, scaled x4.
    const int s = 4;
    fprintf(f, "P6\n%u %u\n255\n", d.width * s * 2, d.height * s);
    for (uint32_t y = 0; y < (uint32_t)d.height * s; y++) {
        for (uint32_t half = 0; half < 2; half++) {
            for (uint32_t x = 0; x < (uint32_t)d.width * s; x++) {
                uint32_t p = px[(y / s) * d.width + x / s];
                uint8_t o[3];
                if (half == 0) { o[0] = p >> 24; o[1] = p >> 16; o[2] = p >> 8; }
                else { o[0] = o[1] = o[2] = p & 0xFF; }   // alpha on the right
                fwrite(o, 1, 3, f);
            }
        }
    }
    fclose(f);
    return true;
}
