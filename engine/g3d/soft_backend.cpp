// The host's software backend for native draws (soft_backend.h).
//
// A draw is two passes over the packet. The first, on the calling thread, makes each item's state
// (Item: its combiners, texture units, lighting, pixel state), transforms the vertices its triangles
// use (each once: a per-buffer stamp), lights them, culls, clips and sets each triangle up (Tri:
// fixed-point edge functions, attribute planes). The second rasterizes the triangle list, the scissor
// box's rows shared out among threads.
#include "engine/g3d/soft_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "engine/g3d/texture.h"

namespace engine::g3d {

namespace {

using engine::gx::CombinerStage;
using engine::gx::Combiners;
using s32 = int32_t;
using s64 = int64_t;
using f64 = double;

inline s32 clampi(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }
inline f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : v > hi ? hi : v; }

// float -> int with NaN, infinities and huge values held in range (raster.cpp's to_int)
inline s32 to_int(f32 f) {
    if (!(f > -1e9f))
        return -1000000000;
    if (!(f < 1e9f))
        return 1000000000;
    return (s32)f;
}

// log2 of a positive float to within 0.008 (the level of detail needs no more): the exponent, and a
// quadratic through log2's ends for the mantissa in [1, 2)
inline f32 fast_log2(f32 x) {
    u32 b;
    memcpy(&b, &x, 4);
    const f32 e = (f32)((s32)((b >> 23) & 255) - 127);
    b = (b & 0x007FFFFF) | 0x3F800000;
    f32 m;
    memcpy(&m, &b, 4);
    return e + (-0.3443f * m + 2.0329f) * m - 1.6886f;
}

// ---------------------------------------------------------------- textures
constexpr u32 MAX_LEVELS = 16;

struct SoftLevel {
    const u32* texels = nullptr;  // RGBA8, R in bits 0-7, rows from t = 0 down
    u32 pitch = 0;                // texels a row (the level's decoded width)
    s32 w = 1, h = 1;             // texels the coordinates address (the source image at this level)
    bool pow2 = true;
};

struct SoftTexture {
    u32 levels = 0;
    f32 width = 1, height = 1;  // the source image's level 0: what the level of detail is measured in
    SoftLevel level[MAX_LEVELS];
    std::vector<u32> texels;
};

struct Prepared {
    std::vector<SoftTexture> textures;
};

inline s32 wrap(s32 i, s32 n, bool pow2, u8 mode) {
    switch (mode) {
    case 2:  // repeat
        return pow2 ? (i & (n - 1)) : ((i % n) + n) % n;
    case 3: {  // mirrored repeat
        const s32 p = pow2 ? (i & (2 * n - 1)) : ((i % (2 * n)) + 2 * n) % (2 * n);
        return p < n ? p : 2 * n - 1 - p;
    }
    default:  // clamp to edge (and border, which the materials never ask for)
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    }
}

// ---------------------------------------------------------------- per item
constexpr int NA = 11;  // attributes: colour RGBA (0-255), (s, t) for texture units 0-2, the shadow map's depth

// what a unit samples: its material's texture, the shadow map (unit 0 of a receiver: (s, t) and the
// depth from the position), or a texture at coordinates from the position (the decals' fade window)
enum UnitKind : u8 { UK_TEXTURE = 0, UK_SHADOW = 1, UK_POSITION = 2 };

struct Unit {
    bool on = false;            // sampled by the combiners
    u8 kind = UK_TEXTURE;
    TexCoordSource source = TexCoordSource::UV0;
    const f32 (*mtx)[4] = nullptr;  // the texture matrix (2x4) or none
    const SoftTexture* tex = nullptr;  // none: reads white
    u8 wrap_s = 0, wrap_t = 0;
    bool mag_linear = true, min_linear = true;
    u8 mip = 0;
    f32 lod_bias = 0, min_lod = 0, max_lod = 0;
    // PROJECTION: the coordinates as affine functions of the window position (sample point),
    // s = ps[0] X + ps[1] Y + ps[2], and their constant level of detail
    f32 ps[3] = {}, pt[3] = {};
    f32 proj_lod = 0;
};

struct LightIn {
    f32 to_light[3];  // unit, world space (against the direction it travels)
    f32 rgb[3];       // 0-1
};

struct Item {
    const fmt::Batch* batch = nullptr;
    const CombinerStage* stage = nullptr;
    Combiners own;               // the combiners when the shadows change the material's (stage points here)
    bool shadow = false, fade = false;  // DRAW_SHADOW_RECEIVER, DRAW_SHADOW_FADE (as the backend takes them)
    f64 prow[3][4] = {};         // the buffer's positions -> the shadow map's (s, t, depth) or the fade's (s, t)
    u8 n_stages = 1, buf_rgb = 0, buf_a = 0;
    Unit unit[3];
    bool need_color = false;     // the combiners read the colour channel
    bool chan_zero = false;      // the material has no colour channel: it reads 0
    // colour channel 0
    bool lit = false, alpha_lit = false, mat_from_vertex = false, alpha_mat_from_vertex = false;
    bool amb_from_vertex = false;
    u8 diffuse = 2;
    f32 mat[4] = {}, amb[4] = {};  // 0-1
    LightIn lights[8];
    u32 n_lights = 0;
    // transform: clip = pmv (pos, 1) (in double: the depth of a strip 0.03 units above the terrain
    // 5,000 units away differs from the terrain's by 0.03 of a depth step, less than a float's
    // rounding there); normals to world (nw) and to view (nv, from world)
    f64 pmv[4][4] = {};
    bool pos16 = false;          // its buffer's positions are s16 (fmt::VA_POS16: origin and scale in pmv)
    f32 nw[3][3] = {}, nv[3][3] = {};
    bool need_normal_world = false, need_normal_view = false;
    // pixel pipeline
    bool depth_test = true, depth_write = true;
    u8 depth_func = 5;
    bool alpha_test = false;
    u8 alpha_func = 1, alpha_ref = 0;
    bool blend = false, logic = false, color_write = true, alpha_write = false, const_alpha = false;
    u8 blend_eq = 0, blend_src = 1, blend_dst = 0, logic_op = 3, const_alpha_value = 0;
    u8 cull_gx = 0;              // GX's cull mode
    bool fog = false;
    f32 fog_a = 0, fog_b = 0;  // fog amount = clamp(w fog_a + fog_b, 0, 1) (w: view depth, guest units)
    s32 fog_rgb[3] = {};
    // the sea (native_draw.h DRAW_SEA_*): the grid marks the pixels it draws, the flat sea skips them
    bool stencil_write = false, stencil_masked = false;
};

// a vertex transformed and lit, in clip space
struct Vtx {
    f64 c[4];
    f32 a[NA];
};

// a triangle set up for rasterizing
struct Tri {
    u32 item;
    s32 x0, x1, y0, y1;             // its pixels' box (inclusive), within the scissor
    s64 ea[3], eb[3], ec[3];        // edge functions of the sample point in 1/256 pixels: inside where
                                    // all three ea X + eb Y + ec are >= 0 (the top-left rule folded in)
    f64 ox, oy;                     // where the planes are taken from (window pixels)
    f64 z, dzdx, dzdy;              // window depth (0 near ... 0xFFFFFF far)
    f32 iw, diwdx, diwdy;           // 1/w
    f32 a[NA], dadx[NA], dady[NA];  // attributes / w
};

constexpr s32 SUB = 256;            // subpixel steps
constexpr s32 SAMPLE_FP = 144;      // the sample point within a pixel: 0.5625 (raster.cpp's)
constexpr f32 SAMPLE = 0.5625f;
constexpr f32 GUARD = 2048.0f;      // pixels beyond the frame buffer before a triangle is clipped

}  // namespace

// ---------------------------------------------------------------- the backend's state
struct SoftBackend::Impl {
    // the draw: its items and their triangles, set up
    std::vector<Item> items;
    std::vector<Tri> tris;
    // the sea: the wave grid's vertices as the vertex program makes them (displaced, shaded, their
    // texture coordinates the sea's at their place: sea_grid), and the stencil marks it leaves (a
    // value a pixel of the frame buffer; this packet's value, a frame's own)
    std::vector<u8> grid_vertices;
    mutable std::vector<u8> stencil;
    u8 stencil_ref = 0;
    // the vertex cache of the item being set up: stamp[i] == gen when vcache[i] holds vertex i
    std::vector<u32> stamp;
    std::vector<Vtx> vcache;
    u32 gen = 0;

    // where it lands
    guest::HostEfb efb{};
    f32 proj[4][4] = {};  // the GX projection
    f32 vp[6] = {};       // the viewport: scale x y z, offset x y z
    f32 ox = 0, oy = 0;   // window x = clip x / w * vp[0] + ox (ox: the viewport's offset less the scissor's)
    f32 k = 1;            // package units -> guest units
    // the overlays' pull toward the eye (native_draw.h overlay_pull; the test hook's when set)
    f64 pull = OVERLAY_DEPTH_PULL, pull_limit = OVERLAY_PULL_LIMIT;
    // The shadow map (native_draw.h ShadowMap): drawn by an opaque packet's casters, sampled by both
    // passes' receivers. Texels as the 3DS GPU keeps them: the depth in 24 bits (0xFFFFFF: nothing),
    // row t = 0 first; the fade window as the texture both backends sample.
    std::vector<u32> smap;
    u32 smap_size = 0;
    f64 smap_bias = 0;
    SoftTexture fade_tex;

    void draw_shadow_map(const Packet& p, const Prepared& prep, Stats& st);
    // the shadow map's filtered compare at (s, t) for a receiver at depth d (0-255: 255 lit), as the
    // 3DS GPU makes it (Azahar's shadowTexture): the four texels about the point, each lit when what it
    // holds lies beyond the receiver's depth less the bias (a texel off the map: lit), mixed bilinearly
    s32 shadow_sample(f32 s, f32 t, f32 d) const {
        const s64 z = std::max<s64>(0, (s64)(std::min(std::fabs((f64)d), 1.0) * 16777215.0) - (s64)(smap_bias * 16777216.0));
        const f32 x = s * (f32)smap_size - 0.5f, y = t * (f32)smap_size - 0.5f;
        if (!(x > -2 && x < (f32)smap_size + 1 && y > -2 && y < (f32)smap_size + 1))
            return 255;
        const f32 fx0 = std::floor(x), fy0 = std::floor(y);
        const s32 ix = (s32)fx0, iy = (s32)fy0;
        const f32 fx = x - fx0, fy = y - fy0;
        auto lit = [&](s32 u, s32 v) -> f32 {
            if (u < 0 || v < 0 || u >= (s32)smap_size || v >= (s32)smap_size)
                return 1.0f;
            return (s64)smap[(u32)v * smap_size + (u32)u] <= z ? 0.0f : 1.0f;
        };
        const f32 top = lit(ix, iy) + (lit(ix + 1, iy) - lit(ix, iy)) * fx;
        const f32 bottom = lit(ix, iy + 1) + (lit(ix + 1, iy + 1) - lit(ix, iy + 1)) * fx;
        return (s32)((top + (bottom - top) * fy) * 255.0f + 0.5f);
    }

    void setup(const Packet& p, const GxView& view, Stats& st);
    void sea_grid(const Packet& p, const fmt::Batch& b, fmt::VertexBuffer* vb);
    void make_item(const Packet& p, const DrawItem& di, const Prepared& prep, Item& it);
    const Vtx& vertex(const Item& it, const fmt::VertexBuffer& vb, const u8* data, u32 i);
    void triangle(u32 item_index, const Item& it, const Vtx& a, const Vtx& b, const Vtx& c, Stats& st);
    void raster(u32 item_index, const Vtx* poly, u32 n);
    void draw_rows(u32 band, u32 bands, Stats& st) const;

    // the view distance (package units) of a window depth, under the draw's projection: window z =
    // z/w vp[2] + vp[5], and z/w = -p22 + p23 / w for GX's perspective
    f64 view_distance(u32 z) const {
        const f64 zn = ((f64)z - vp[5]) / vp[2];
        return proj[2][3] / (zn + proj[2][2]) / k;
    }
};

namespace {

// ---------------------------------------------------------------- combiners (PICA arithmetic)
struct C4 {
    s32 v[4];
};

inline void rgb_operand(const C4& c, u8 op, s32* o) {
    switch (op) {
    case engine::gx::OP_RGB_SRC_COLOR: o[0] = c.v[0], o[1] = c.v[1], o[2] = c.v[2]; return;
    case engine::gx::OP_RGB_ONE_MINUS_SRC_COLOR: o[0] = 255 - c.v[0], o[1] = 255 - c.v[1], o[2] = 255 - c.v[2]; return;
    case engine::gx::OP_RGB_SRC_ALPHA: o[0] = o[1] = o[2] = c.v[3]; return;
    case engine::gx::OP_RGB_ONE_MINUS_SRC_ALPHA: o[0] = o[1] = o[2] = 255 - c.v[3]; return;
    case engine::gx::OP_RGB_SRC_R: o[0] = o[1] = o[2] = c.v[0]; return;
    case engine::gx::OP_RGB_ONE_MINUS_SRC_R: o[0] = o[1] = o[2] = 255 - c.v[0]; return;
    case engine::gx::OP_RGB_SRC_G: o[0] = o[1] = o[2] = c.v[1]; return;
    case engine::gx::OP_RGB_ONE_MINUS_SRC_G: o[0] = o[1] = o[2] = 255 - c.v[1]; return;
    case engine::gx::OP_RGB_SRC_B: o[0] = o[1] = o[2] = c.v[2]; return;
    case engine::gx::OP_RGB_ONE_MINUS_SRC_B: o[0] = o[1] = o[2] = 255 - c.v[2]; return;
    default: o[0] = c.v[0], o[1] = c.v[1], o[2] = c.v[2]; return;  // (not used by the translation)
    }
}

inline s32 alpha_operand(const C4& c, u8 op) {
    switch (op) {
    case engine::gx::OP_A_SRC_ALPHA: return c.v[3];
    case engine::gx::OP_A_ONE_MINUS_SRC_ALPHA: return 255 - c.v[3];
    case engine::gx::OP_A_SRC_R: return c.v[0];
    case engine::gx::OP_A_ONE_MINUS_SRC_R: return 255 - c.v[0];
    case engine::gx::OP_A_SRC_G: return c.v[1];
    case engine::gx::OP_A_ONE_MINUS_SRC_G: return 255 - c.v[1];
    case engine::gx::OP_A_SRC_B: return c.v[2];
    default: return 255 - c.v[2];
    }
}

// one channel of a combiner function, inputs 0-255 (the PICA's: products and interpolations
// truncated after the divide by 255, sums clamped)
inline s32 combine(u8 f, s32 a, s32 b, s32 c) {
    switch (f) {
    case engine::gx::FUNC_REPLACE: return a;
    case engine::gx::FUNC_MODULATE: return a * b / 255;
    case engine::gx::FUNC_ADD: return std::min<s32>(a + b, 255);
    case engine::gx::FUNC_ADD_SIGNED: return clampi(a + b - 128, 0, 255);
    case engine::gx::FUNC_INTERPOLATE: return (a * c + b * (255 - c)) / 255;
    case engine::gx::FUNC_SUBTRACT: return std::max<s32>(a - b, 0);
    case engine::gx::FUNC_MULTIPLY_ADD: return std::min<s32>((a * b + 255 * c) / 255, 255);
    case engine::gx::FUNC_ADD_MULTIPLY: return std::min<s32>(a + b, 255) * c / 255;
    default: return a;
    }
}

// the dot product of DOT3_RGB(A): inputs as signed (2x - 1), each product rounded to 1/256
inline s32 dot3(const s32* a, const s32* b) {
    s32 r = 0;
    for (int i = 0; i < 3; i++)
        r += ((a[i] * 2 - 255) * (b[i] * 2 - 255) + 128) / 256;
    return clampi(r, 0, 255);
}

// the combiners for one pixel: src holds PRIMARY_COLOR and TEXTURE0-2 (by source number). The buffer
// stage 0 reads is 0; what goes in it first is GPUREG_TEXENV_BUFFER_COLOR, which both PICA paths
// (gx_pica.cpp, g3d_pica.cpp) set to white: what stage 1 reads, and later stages until a stage updates it
inline C4 run_combiners(const Item& it, C4* src) {
    C4 prev = {{0, 0, 0, 0}}, buffer = {{0, 0, 0, 0}}, next = {{255, 255, 255, 255}};
    for (u32 s = 0; s < it.n_stages; s++) {
        const CombinerStage& st = it.stage[s];
        src[engine::gx::SRC_PREVIOUS_BUFFER] = buffer;
        src[engine::gx::SRC_PREVIOUS] = prev;
        const u32 k = st.konst;
        src[engine::gx::SRC_CONSTANT] = {{(s32)(k & 255), (s32)(k >> 8 & 255), (s32)(k >> 16 & 255), (s32)(k >> 24)}};
        s32 in[3][3];
        for (int i = 0; i < 3; i++)
            rgb_operand(src[st.src_rgb[i] & 15], st.op_rgb[i], in[i]);
        C4 out;
        if (st.func_rgb == engine::gx::FUNC_DOT3_RGB || st.func_rgb == engine::gx::FUNC_DOT3_RGBA) {
            out.v[0] = out.v[1] = out.v[2] = dot3(in[0], in[1]);
        } else {
            for (int c = 0; c < 3; c++)
                out.v[c] = combine(st.func_rgb, in[0][c], in[1][c], in[2][c]);
        }
        if (st.func_rgb == engine::gx::FUNC_DOT3_RGBA) {
            out.v[3] = out.v[0];  // (the alpha combiner is ignored)
        } else {
            const s32 a0 = alpha_operand(src[st.src_a[0] & 15], st.op_a[0]);
            const s32 a1 = alpha_operand(src[st.src_a[1] & 15], st.op_a[1]);
            const s32 a2 = alpha_operand(src[st.src_a[2] & 15], st.op_a[2]);
            out.v[3] = st.func_a == engine::gx::FUNC_DOT3_RGB || st.func_a == engine::gx::FUNC_DOT3_RGBA
                           ? out.v[0]
                           : combine(st.func_a, a0, a1, a2);
        }
        if (st.scale_rgb)
            for (int c = 0; c < 3; c++)
                out.v[c] = std::min<s32>(out.v[c] << st.scale_rgb, 255);
        if (st.scale_a)
            out.v[3] = std::min<s32>(out.v[3] << st.scale_a, 255);
        // the buffer a stage reads is what the stage before last put in it (the PICA's one-stage delay)
        buffer = next;
        if (s < 4 && (it.buf_rgb >> s & 1))
            next.v[0] = out.v[0], next.v[1] = out.v[1], next.v[2] = out.v[2];
        if (s < 4 && (it.buf_a >> s & 1))
            next.v[3] = out.v[3];
        prev = out;
    }
    return prev;
}

// GPU_TESTFUNC
inline bool test(u8 f, u32 a, u32 b) {
    switch (f) {
    case 0: return false;
    case 1: return true;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    default: return a >= b;
    }
}

// ---------------------------------------------------------------- sampling
inline u32 texel_at(const SoftLevel& l, s32 x, s32 y, u8 ws, u8 wt) {
    return l.texels[(u32)wrap(y, l.h, l.pow2, wt) * l.pitch + (u32)wrap(x, l.w, l.pow2, ws)];
}

// a coordinate in texels, held where a float -> int conversion is defined
inline f32 hold(f32 v) {
    if (!(v > -16777216.0f))
        return -16777216.0f;
    if (!(v < 16777216.0f))
        return 16777216.0f;
    return v;
}

u32 sample_level(const SoftLevel& l, f32 s, f32 t, bool linear, u8 ws, u8 wt) {
    if (!linear) {
        const s32 x = (s32)std::floor(hold(s * (f32)l.w)), y = (s32)std::floor(hold(t * (f32)l.h));
        return texel_at(l, x, y, ws, wt);
    }
    const f32 u = hold(s * (f32)l.w - 0.5f), v = hold(t * (f32)l.h - 0.5f);
    const f32 fu = std::floor(u), fv = std::floor(v);
    const s32 x = (s32)fu, y = (s32)fv;
    const u32 ax = (u32)((u - fu) * 256.0f), ay = (u32)((v - fv) * 256.0f);
    const u32 c00 = texel_at(l, x, y, ws, wt), c10 = texel_at(l, x + 1, y, ws, wt);
    const u32 c01 = texel_at(l, x, y + 1, ws, wt), c11 = texel_at(l, x + 1, y + 1, ws, wt);
    // weights summing to 256; red/blue and green/alpha two lanes a word
    const u32 w11 = (ax * ay) >> 8, w10 = ax - w11, w01 = ay - w11, w00 = 256 - ax - ay + w11;
    const u32 rb = ((c00 & 0x00FF00FF) * w00 + (c10 & 0x00FF00FF) * w10 + (c01 & 0x00FF00FF) * w01 +
                    (c11 & 0x00FF00FF) * w11) >> 8;
    const u32 ga = (((c00 >> 8) & 0x00FF00FF) * w00 + ((c10 >> 8) & 0x00FF00FF) * w10 +
                    ((c01 >> 8) & 0x00FF00FF) * w01 + ((c11 >> 8) & 0x00FF00FF) * w11) >> 8;
    return (rb & 0x00FF00FF) | (ga & 0x00FF00FF) << 8;
}

// a texture unit's sample at (s, t) with level of detail `lod` (before the bias): magnified or
// minified by the biased level, the level(s) read from it clamped to the unit's range
inline u32 sample(const Unit& u, f32 s, f32 t, f32 lod) {
    const SoftTexture& tx = *u.tex;
    lod += u.lod_bias;
    const bool minified = lod > 0;
    lod = clampf(lod, u.min_lod, u.max_lod);
    if (!minified || lod <= 0 || u.mip == 0 || tx.levels < 2)
        return sample_level(tx.level[0], s, t, minified ? u.min_linear : u.mag_linear, u.wrap_s, u.wrap_t);
    const f32 top = (f32)(tx.levels - 1);
    if (u.mip == 1) {  // the nearest level
        const u32 l = (u32)std::min(lod + 0.5f, top);
        return sample_level(tx.level[l], s, t, u.min_linear, u.wrap_s, u.wrap_t);
    }
    if (lod >= top)
        return sample_level(tx.level[tx.levels - 1], s, t, u.min_linear, u.wrap_s, u.wrap_t);
    const u32 l = (u32)lod;
    const u32 f = (u32)((lod - (f32)l) * 256.0f);
    const u32 a = sample_level(tx.level[l], s, t, u.min_linear, u.wrap_s, u.wrap_t);
    const u32 b = sample_level(tx.level[l + 1], s, t, u.min_linear, u.wrap_s, u.wrap_t);
    const u32 rb = ((a & 0x00FF00FF) * (256 - f) + (b & 0x00FF00FF) * f) >> 8;
    const u32 ga = (((a >> 8) & 0x00FF00FF) * (256 - f) + ((b >> 8) & 0x00FF00FF) * f) >> 8;
    return (rb & 0x00FF00FF) | (ga & 0x00FF00FF) << 8;
}

inline C4 unpack(u32 c) { return {{(s32)(c & 255), (s32)(c >> 8 & 255), (s32)(c >> 16 & 255), (s32)(c >> 24)}}; }

// ---------------------------------------------------------------- the frame buffer
// raster.cpp's quantize: the EFB's pixel format (0 RGB8, 1 RGBA6, 2 RGB565)
inline u32 quantize(const s32* c, u32 pixfmt) {
    u32 r = (u32)c[0], g = (u32)c[1], b = (u32)c[2], a = (u32)c[3];
    switch (pixfmt) {
    case 1:
        r = (r >> 2) * 255 / 63, g = (g >> 2) * 255 / 63, b = (b >> 2) * 255 / 63, a = (a >> 2) * 255 / 63;
        break;
    case 2: r = (r >> 3) * 255 / 31, g = (g >> 2) * 255 / 63, b = (b >> 3) * 255 / 31, a = 255; break;
    default: a = 255; break;
    }
    return r | g << 8 | b << 16 | a << 24;
}

// GPU_BLENDFACTOR for channel ch (the constant colours, which the translation never asks for, as one)
inline s32 blend_factor(u8 f, const s32* src, const s32* dst, int ch) {
    switch (f) {
    case 0: return 0;
    case 1: return 255;
    case 2: return src[ch];
    case 3: return 255 - src[ch];
    case 4: return dst[ch];
    case 5: return 255 - dst[ch];
    case 6: return src[3];
    case 7: return 255 - src[3];
    case 8: return dst[3];
    case 9: return 255 - dst[3];
    default: return 255;
    }
}

// GPU_LOGICOP
inline u32 logic(u8 op, u32 s, u32 d) {
    switch (op) {
    case 0: return 0;
    case 1: return s & d;
    case 2: return s & ~d;
    case 3: return s;
    case 4: return 255;
    case 5: return ~s;
    case 6: return d;
    case 7: return ~d;
    case 8: return ~(s & d);
    case 9: return s | d;
    case 10: return ~(s | d);
    case 11: return s ^ d;
    case 12: return ~(s ^ d);
    case 13: return ~s & d;
    case 14: return s | ~d;
    default: return ~s | d;
    }
}

void write_pixel(const Item& it, u32* color, const s32* src, u32 pixfmt) {
    const u32 dstc = *color;
    s32 dst[4] = {(s32)(dstc & 255), (s32)(dstc >> 8 & 255), (s32)(dstc >> 16 & 255), (s32)(dstc >> 24)};
    s32 out[4] = {src[0], src[1], src[2], src[3]};
    if (it.blend) {
        for (int i = 0; i < 4; i++) {
            const s32 s = src[i] * blend_factor(it.blend_src, src, dst, i);
            const s32 d = dst[i] * blend_factor(it.blend_dst, src, dst, i);
            switch (it.blend_eq) {
            case 1: out[i] = clampi((s - d) / 255, 0, 255); break;
            case 2: out[i] = clampi((d - s) / 255, 0, 255); break;
            case 3: out[i] = std::min(src[i], dst[i]); break;
            case 4: out[i] = std::max(src[i], dst[i]); break;
            default: out[i] = clampi((s + d + 127) / 255, 0, 255); break;
            }
        }
    } else if (it.logic) {
        for (int i = 0; i < 4; i++)
            out[i] = (s32)(logic(it.logic_op, (u32)src[i], (u32)dst[i]) & 255);
    }
    if (it.const_alpha)
        out[3] = it.const_alpha_value;
    u32 q = quantize(out, pixfmt);
    // (raster.cpp's write masks: the alpha is kept only by RGBA6)
    u32 keep = 0;
    if (!it.color_write)
        keep |= 0x00FFFFFF;
    if (!it.alpha_write || pixfmt != 1)
        keep |= 0xFF000000;
    if (pixfmt != 1)
        q |= 0xFF000000;
    *color = (dstc & keep) | (q & ~keep);
}

// 3x3 inverse transpose (for normals); the matrix itself if it is singular
void inverse_transpose(const f32 m[3][4], f32 out[3][3]) {
    const f32 a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0],
              h = m[2][1], i = m[2][2];
    const f32 A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const f32 det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-12f) {
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++)
                out[r][k] = m[r][k];
        return;
    }
    const f32 s = 1.0f / det;
    // (the cofactor matrix over the determinant is the inverse's transpose)
    out[0][0] = A * s, out[0][1] = B * s, out[0][2] = C * s;
    out[1][0] = -(b * i - c * h) * s, out[1][1] = (a * i - c * g) * s, out[1][2] = -(a * h - b * g) * s;
    out[2][0] = (b * f - c * e) * s, out[2][1] = -(a * f - c * d) * s, out[2][2] = (a * e - b * d) * s;
}

}  // namespace

// ---------------------------------------------------------------- item state
void SoftBackend::Impl::make_item(const Packet& p, const DrawItem& di, const Prepared& prep, Item& it) {
    const Package& pkg = *p.package;
    const fmt::Batch& b = pkg.batches()[di.batch];
    const Material& m = p.materials[b.material];
    const fmt::Material& fm = pkg.materials()[b.material];
    const MatDynamic* dyn = di.mat_state != 0xFFFF && di.mat_state < p.mat_states.size() ? &p.mat_states[di.mat_state] : nullptr;
    it = Item();
    it.batch = &b;
    const Combiners& comb = dyn ? dyn->comb : m.comb;
    it.stage = comb.stage;
    it.n_stages = (u8)std::max<u32>(1, std::min<u32>(comb.n_stages, 6));
    it.buf_rgb = comb.buf_rgb, it.buf_a = comb.buf_a;
    // what the combiners read
    u32 reads = 0;
    for (u32 s = 0; s < it.n_stages; s++)
        for (int i = 0; i < 3; i++)
            reads |= 1u << (comb.stage[s].src_rgb[i] & 15) | 1u << (comb.stage[s].src_a[i] & 15);
    it.need_color = reads & 1u << engine::gx::SRC_PRIMARY_COLOR;
    it.chan_zero = fm.n_chans == 0;
    const f32(*tex_mtx)[2][4] = dyn ? dyn->tex_mtx : m.tex_mtx;
    // the transform: model -> view (guest units) = view S(k) matrix; then the GX projection
    static const f32 k_identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const f32* mm = (size_t)di.matrix * 12 + 12 <= p.matrices.size() ? &p.matrices[(size_t)di.matrix * 12] : k_identity;
    const f32 k = p.world_scale;
    f64 mv[3][4];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            f64 v = 0;
            for (int l = 0; l < 3; l++)
                v += (f64)p.view[r][l] * mm[l * 4 + c];
            mv[r][c] = k * v + (c == 3 ? p.view[r][3] : 0.0);
        }
    }
    // an overlay pulled toward the eye, as the PICA backend draws it (native_draw.h overlay_pull)
    if (!(di.flags & DRAW_SKY) && is_overlay(m)) {
        static const f32 k_origin[3] = {0, 0, 0};
        const f64 s = 1.0 - overlay_pull(mv, b.bbox, k_origin, k, pull, pull_limit);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++)
                mv[r][c] *= s;
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            f64 v = 0;
            for (int l = 0; l < 3; l++)
                v += (f64)proj[r][l] * mv[l][c];
            it.pmv[r][c] = v + (c == 3 ? proj[r][3] : 0.0);
        }
    // compact positions (fmt::VA_POS16): s16 x the buffer's scale + its origin, folded in (the
    // vertices are then exact)
    const fmt::VertexBuffer& vb = pkg.vertex_buffers()[b.vbuf];
    it.pos16 = (vb.attrs & fmt::VA_POS16) && !(di.flags & (DRAW_SEA_GRID | DRAW_DYNAMIC));
    if (it.pos16)
        for (int r = 0; r < 4; r++) {
            it.pmv[r][3] += it.pmv[r][0] * vb.pos_origin[0] + it.pmv[r][1] * vb.pos_origin[1] +
                            it.pmv[r][2] * vb.pos_origin[2];
            for (int c = 0; c < 3; c++)
                it.pmv[r][c] *= vb.pos_scale;
        }
    f32 m34[3][4];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            m34[r][c] = mm[r * 4 + c];
    inverse_transpose(m34, it.nw);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            it.nv[r][c] = p.view[r][c];
    // texture units
    for (int u = 0; u < 3; u++) {
        Unit& un = it.unit[u];
        const TexUnit& tu = m.unit[u];
        un.on = reads & 1u << (engine::gx::SRC_TEXTURE0 + u);
        if (!un.on)
            continue;
        un.source = tu.source;
        un.mtx = tu.matrix >= 0 && tu.matrix < 3 ? tex_mtx[tu.matrix] : nullptr;
        un.tex = tu.texture != fmt::NONE16 && tu.texture < prep.textures.size() && prep.textures[tu.texture].levels
                     ? &prep.textures[tu.texture]
                     : nullptr;
        un.wrap_s = tu.wrap_s, un.wrap_t = tu.wrap_t;
        un.mag_linear = tu.mag_linear, un.min_linear = tu.min_linear;
        un.mip = tu.mip;
        un.lod_bias = tu.lod_bias, un.min_lod = tu.min_lod;
        un.max_lod = un.tex ? std::min(tu.max_lod, (f32)(un.tex->levels - 1)) : 0;
        if (un.max_lod < un.min_lod)
            un.max_lod = un.min_lod;
        if (tu.source == TexCoordSource::ENV_CAMERA)
            it.need_normal_view = true;
        if (tu.source == TexCoordSource::PROJECTION) {
            // (0.5 + 0.5 x/w, 0.5 - 0.5 y/w) from the window position: x/w = (X - ox) / vp[0]
            const f32 sx = 0.5f / vp[0], sy = -0.5f / vp[1];
            const f32 s_[3] = {sx, 0, 0.5f - sx * ox}, t_[3] = {0, sy, 0.5f - sy * oy};
            const f32* r0 = un.mtx ? un.mtx[0] : nullptr;
            const f32* r1 = un.mtx ? un.mtx[1] : nullptr;
            for (int i = 0; i < 3; i++) {
                un.ps[i] = r0 ? r0[0] * s_[i] + r0[1] * t_[i] : s_[i];
                un.pt[i] = r1 ? r1[0] * s_[i] + r1[1] * t_[i] : t_[i];
            }
            if (r0)
                un.ps[2] += r0[2] + r0[3], un.pt[2] += r1[2] + r1[3];
            if (un.tex) {
                const f32 rho = std::max(std::max(std::fabs(un.ps[0]) * un.tex->width, std::fabs(un.pt[0]) * un.tex->height),
                                         std::max(std::fabs(un.ps[1]) * un.tex->width, std::fabs(un.pt[1]) * un.tex->height));
                un.proj_lod = fast_log2(std::max(rho, 1e-20f));
            }
        }
    }
    // the shadows: a receiver's unit 0 the shadow map (its own unit 0 as unit 2), a decal's unit 1 the
    // fade window, their combiners with the stages that take them (material.h); coordinates from the
    // position: the map's rows times the item's matrix and its buffer's origin and scale
    const ShadowMap& sm = p.shadow;
    it.shadow = (di.flags & DRAW_SHADOW_RECEIVER) && m.shadow_receiver && sm.size && sm.size == smap_size;
    it.fade = (di.flags & DRAW_SHADOW_FADE) && m.shadow_decal && sm.size && !it.shadow;
    if (it.shadow || it.fade) {
        if (it.shadow) {
            shadow_receiver_combiners(comb, sm.darkness, &it.own);
            it.unit[2] = it.unit[0];
        } else {
            shadow_decal_combiners(comb, &it.own);
        }
        it.stage = it.own.stage;
        it.n_stages = (u8)std::max<u32>(1, std::min<u32>(it.own.n_stages, 6));
        it.buf_rgb = it.own.buf_rgb, it.buf_a = it.own.buf_a;
        Unit& un = it.unit[it.shadow ? 0 : 1];
        un = Unit();
        un.on = true;
        un.kind = it.shadow ? UK_SHADOW : UK_POSITION;
        un.tex = it.shadow ? nullptr : &fade_tex;
        un.mag_linear = un.min_linear = true;
        const f32(*rows)[4] = it.shadow ? sm.light : sm.fade;
        for (int r = 0; r < (it.shadow ? 3 : 2); r++) {
            f64 w[4];
            for (int c = 0; c < 4; c++)
                w[c] = (f64)rows[r][0] * mm[c] + (f64)rows[r][1] * mm[4 + c] + (f64)rows[r][2] * mm[8 + c] +
                       (c == 3 ? (f64)rows[r][3] : 0.0);
            if (it.pos16) {
                w[3] += w[0] * vb.pos_origin[0] + w[1] * vb.pos_origin[1] + w[2] * vb.pos_origin[2];
                for (int c = 0; c < 3; c++)
                    w[c] *= vb.pos_scale;
            }
            memcpy(it.prow[r], w, sizeof(w));
        }
        reads = 0;
        for (u32 st = 0; st < it.n_stages; st++)
            for (int i = 0; i < 3; i++)
                reads |= 1u << (it.stage[st].src_rgb[i] & 15) | 1u << (it.stage[st].src_a[i] & 15);
        it.need_color = reads & 1u << engine::gx::SRC_PRIMARY_COLOR;
    }
    // colour channel 0
    const Lighting& L = m.light;
    const u8* mat_color = dyn ? dyn->mat_color : L.mat_color;
    const u8* amb_color = dyn ? dyn->amb_color : L.amb_color;
    it.lit = L.lit, it.alpha_lit = L.alpha_lit;
    it.mat_from_vertex = L.mat_from_vertex, it.alpha_mat_from_vertex = L.alpha_mat_from_vertex;
    it.amb_from_vertex = L.amb_from_vertex;
    it.diffuse = L.diffuse;
    for (int c = 0; c < 4; c++)
        it.mat[c] = mat_color[c] / 255.0f, it.amb[c] = amb_color[c] / 255.0f;
    if ((it.lit || it.alpha_lit) && L.light_set >= 0 && (u32)L.light_set < p.light_sets.size()) {
        // the scene's light set: its ambient and lights (as nw4r's ScnRoot gives them)
        const fmt::LightSet& ls = p.light_sets[(u32)L.light_set];
        for (int c = 0; c < 4; c++)
            it.amb[c] = ls.ambient[c] / 255.0f;
        it.amb_from_vertex = false;
        for (u16 li : ls.lights) {
            if (li == fmt::NONE16 || li >= p.lights.size() || it.n_lights >= 8)
                continue;
            const Packet::Light& pl = p.lights[li];
            LightIn& o = it.lights[it.n_lights++];
            for (int c = 0; c < 3; c++)
                o.to_light[c] = -pl.dir[c], o.rgb[c] = pl.rgba[c] / 255.0f;
        }
    }
    it.need_normal_world = it.lit && it.n_lights > 0;
    // pixel pipeline
    const PixelState& px = m.pixel;
    const bool sky = di.flags & DRAW_SKY;
    it.depth_test = px.depth_test && !sky;
    it.depth_write = px.depth_write && !sky;
    it.depth_func = px.depth_func;
    it.cull_gx = px.cull_gx;
    it.alpha_test = px.alpha_test && px.alpha_func != 1;
    it.alpha_func = px.alpha_func, it.alpha_ref = px.alpha_ref;
    it.blend = px.blend, it.blend_eq = px.blend_eq, it.blend_src = px.blend_src, it.blend_dst = px.blend_dst;
    it.logic = px.logic, it.logic_op = px.logic_op;
    it.color_write = px.color_write, it.alpha_write = px.alpha_write;
    it.const_alpha = px.const_alpha, it.const_alpha_value = px.const_alpha_value;
    it.stencil_write = di.flags & DRAW_SEA_GRID;
    it.stencil_masked = di.flags & DRAW_SEA_MASKED;
    if (m.fog && m.fog_index < p.fogs.size() && p.fogs[m.fog_index].type != fmt::FOG_NONE) {
        const fmt::Fog& f = p.fogs[m.fog_index];
        const f32 span = (f.end - f.start) * k;
        if (span > 0) {
            it.fog = true;
            it.fog_a = 1.0f / span, it.fog_b = -f.start * k / span;
            for (int c = 0; c < 3; c++)
                it.fog_rgb[c] = f.rgba[c];
        }
    }
}

// a vertex of the item's batch, transformed, lit and given its texture coordinates (once an item)
const Vtx& SoftBackend::Impl::vertex(const Item& it, const fmt::VertexBuffer& vb, const u8* data, u32 i) {
    if (stamp[i] == gen)
        return vcache[i];
    stamp[i] = gen;
    Vtx& v = vcache[i];
    memset(v.a, 0, sizeof(v.a));
    const u8* q = data + (size_t)i * vb.stride;
    f64 pos[3];
    u32 at;
    if (it.pos16) {  // (the origin and scale are in pmv)
        s16 sp[3];
        memcpy(sp, q, 6);
        pos[0] = sp[0], pos[1] = sp[1], pos[2] = sp[2];
        at = 8;
    } else {
        f32 fp[3];
        memcpy(fp, q, 12);
        pos[0] = fp[0], pos[1] = fp[1], pos[2] = fp[2];
        at = 12;
    }
    for (int r = 0; r < 4; r++)
        v.c[r] = it.pmv[r][0] * pos[0] + it.pmv[r][1] * pos[1] + it.pmv[r][2] * pos[2] + it.pmv[r][3];
    if (it.shadow || it.fade) {  // (the map's s, t on unit 0's attributes and its depth last; the fade's on unit 1's)
        const int at0 = it.shadow ? 4 : 6;
        for (int r = 0; r < (it.shadow ? 3 : 2); r++)
            v.a[r < 2 ? at0 + r : 10] = (f32)(it.prow[r][0] * pos[0] + it.prow[r][1] * pos[1] + it.prow[r][2] * pos[2] +
                                              it.prow[r][3]);
    }
    f32 n[3] = {0, 0, 0};
    bool has_n = false;
    if (vb.attrs & fmt::VA_NRM) {
        const s8* sn = (const s8*)(q + at);
        n[0] = sn[0], n[1] = sn[1], n[2] = sn[2];
        has_n = n[0] != 0 || n[1] != 0 || n[2] != 0;
        at += 4;
    }
    f32 vc[4] = {1, 1, 1, 1};
    bool has_c = false;
    if (vb.attrs & fmt::VA_CLR) {
        for (int c = 0; c < 4; c++)
            vc[c] = q[at + c] / 255.0f;
        has_c = true;
        at += 4;
    }
    f32 uv[2][2] = {{0, 0}, {0, 0}};
    for (int k = 0; k < 2; k++) {
        if (!(vb.attrs & (k ? fmt::VA_UV1 : fmt::VA_UV0)))
            continue;
        if (vb.flags & fmt::VB_UV_FLOAT) {
            memcpy(uv[k], q + at, 8);
            at += 8;
        } else {
            s16 s[2];
            memcpy(s, q + at, 4);
            uv[k][0] = s[0] * vb.uv_scale[k], uv[k][1] = s[1] * vb.uv_scale[k];
            at += 4;
        }
    }
    // normals: to world space (lighting), and on to view space (the camera's environment map)
    f32 nw[3] = {0, 0, 0}, nv[3] = {0, 0, 1};
    if (has_n && (it.need_normal_world || it.need_normal_view)) {
        for (int r = 0; r < 3; r++)
            nw[r] = it.nw[r][0] * n[0] + it.nw[r][1] * n[1] + it.nw[r][2] * n[2];
        const f32 l = std::sqrt(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);
        if (l > 0)
            nw[0] /= l, nw[1] /= l, nw[2] /= l;
        for (int r = 0; r < 3; r++)
            nv[r] = it.nv[r][0] * nw[0] + it.nv[r][1] * nw[1] + it.nv[r][2] * nw[2];
        const f32 lv = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
        if (lv > 0)
            nv[0] /= lv, nv[1] /= lv, nv[2] /= lv;
    }
    // colour channel 0 (GX's per-vertex lighting): material colour x clamp(ambient + sum of
    // diffuse x light colour); a channel the vertex has no colour for takes the register's
    if (it.need_color) {
        if (it.chan_zero) {
            v.a[0] = v.a[1] = v.a[2] = v.a[3] = 0;
        } else {
            f32 col[4];
            for (int c = 0; c < 3; c++)
                col[c] = it.mat_from_vertex && has_c ? vc[c] : it.mat[c];
            col[3] = it.alpha_mat_from_vertex && has_c ? vc[3] : it.mat[3];
            if (it.lit) {
                f32 light[3];
                for (int c = 0; c < 3; c++)
                    light[c] = it.amb_from_vertex && has_c ? vc[c] : it.amb[c];
                if (has_n)
                    for (u32 l = 0; l < it.n_lights; l++) {
                        const LightIn& li = it.lights[l];
                        f32 d = nw[0] * li.to_light[0] + nw[1] * li.to_light[1] + nw[2] * li.to_light[2];
                        if (it.diffuse == 0)
                            d = 1;
                        else if (it.diffuse != 1 && d < 0)
                            d = 0;
                        for (int c = 0; c < 3; c++)
                            light[c] += d * li.rgb[c];
                    }
                for (int c = 0; c < 3; c++)
                    col[c] *= clampf(light[c], 0, 1);
            }
            if (it.alpha_lit)
                col[3] *= clampf(it.amb_from_vertex && has_c ? vc[3] : it.amb[3], 0, 1);
            for (int c = 0; c < 4; c++)
                v.a[c] = col[c] * 255.0f;
        }
    }
    // texture coordinates (the projection's come from the pixel)
    for (int u = 0; u < 3; u++) {
        const Unit& un = it.unit[u];
        if (!un.on || un.kind != UK_TEXTURE || un.source == TexCoordSource::PROJECTION)
            continue;
        f32 s, t;
        switch (un.source) {
        case TexCoordSource::UV1: s = uv[1][0], t = uv[1][1]; break;
        case TexCoordSource::ENV_CAMERA: s = 0.5f + 0.5f * nv[0], t = 0.5f - 0.5f * nv[1]; break;
        default: s = uv[0][0], t = uv[0][1]; break;
        }
        if (un.mtx) {
            const f32 s2 = un.mtx[0][0] * s + un.mtx[0][1] * t + un.mtx[0][2] + un.mtx[0][3];
            t = un.mtx[1][0] * s + un.mtx[1][1] * t + un.mtx[1][2] + un.mtx[1][3];
            s = s2;
        }
        v.a[4 + 2 * u] = s, v.a[5 + 2 * u] = t;
    }
    return v;
}

// ---------------------------------------------------------------- triangles
void SoftBackend::Impl::triangle(u32 item_index, const Item& it, const Vtx& a, const Vtx& b, const Vtx& c,
                                 Stats& st) {
    st.triangles++;
    // culling (raster.cpp's, from the clip coordinates: GX's front faces and the viewport's mirroring).
    // The material has GXSetCullMode's value; raster.cpp reads GENMODE's, where GXSetCullMode puts
    // front and back the other way round (bits swapped: 1 <-> 2)
    const u8 cull = (u8)((it.cull_gx >> 1 & 1) | (it.cull_gx << 1 & 2));
    if (cull) {
        const f64 *p0 = a.c, *p1 = b.c, *p2 = c.c;
        const f64 nz = (p0[0] * p2[3] - p2[0] * p0[3]) * p1[1] + (p2[0] * p0[1] - p0[0] * p2[1]) * p1[3] +
                       (p2[1] * p0[3] - p0[1] * p2[3]) * p1[0];
        bool back = nz <= 0;
        if (vp[1] > 0)
            back = !back;
        if (vp[0] < 0)
            back = !back;
        if ((!back && (cull == 1 || cull == 3)) || (back && (cull == 2 || cull == 3))) {
            st.culled++;
            return;
        }
    }
    // planes as linear forms of (x, y, z, w): near z >= -w, far z <= 0, then the scissor box's sides
    // (trivial rejection) and the guard band's (clipping)
    const f32 L = (f32)efb.scissor_left, R = (f32)efb.scissor_right + 1, T = (f32)efb.scissor_top,
              B = (f32)efb.scissor_bottom + 1;
    const f32 W = (f32)efb.width, H = (f32)efb.height;
    auto side = [&](const Vtx& v, f32 edge, bool x, bool upper) {
        // window x = c0 / w * vp0 + ox  >=  edge  <=>  c0 vp0 + (ox - edge) w >= 0 (w > 0)
        const f64 s = x ? vp[0] : vp[1], o = x ? ox : oy, cc = x ? v.c[0] : v.c[1];
        const f64 d = cc * s + (o - edge) * v.c[3];
        return upper ? -d : d;
    };
    const Vtx* tri[3] = {&a, &b, &c};
    auto all_out = [&](auto dist) { return dist(*tri[0]) < 0 && dist(*tri[1]) < 0 && dist(*tri[2]) < 0; };
    if (all_out([](const Vtx& v) { return v.c[2] + v.c[3]; }) || all_out([](const Vtx& v) { return -v.c[2]; }) ||
        all_out([&](const Vtx& v) { return side(v, L, true, false); }) ||
        all_out([&](const Vtx& v) { return side(v, R, true, true); }) ||
        all_out([&](const Vtx& v) { return side(v, T, false, false); }) ||
        all_out([&](const Vtx& v) { return side(v, B, false, true); })) {
        st.culled++;
        return;
    }
    // clipping (Sutherland-Hodgman), only against the planes a vertex is outside
    Vtx buf[2][12];
    u32 n = 3;
    buf[0][0] = a, buf[0][1] = b, buf[0][2] = c;
    int cur = 0;
    bool clipped = false;
    for (int plane = 0; plane < 6; plane++) {
        auto dist = [&](const Vtx& v) -> f64 {
            switch (plane) {
            case 0: return v.c[2] + v.c[3];
            case 1: return -v.c[2];
            case 2: return side(v, -GUARD, true, false);
            case 3: return side(v, W + GUARD, true, true);
            case 4: return side(v, -GUARD, false, false);
            default: return side(v, H + GUARD, false, true);
            }
        };
        bool all_in = true;
        for (u32 i = 0; i < n; i++)
            if (dist(buf[cur][i]) < 0)
                all_in = false;
        if (all_in)
            continue;
        clipped = true;
        u32 m = 0;
        for (u32 i = 0; i < n; i++) {
            const Vtx& p = buf[cur][i];
            const Vtx& q = buf[cur][(i + 1) % n];
            const f64 dp = dist(p), dq = dist(q);
            if (dp >= 0 && m < 12)
                buf[cur ^ 1][m++] = p;
            if ((dp >= 0) != (dq >= 0) && m < 12) {
                const f64 t = dp / (dp - dq);
                Vtx& o = buf[cur ^ 1][m++];
                for (int k = 0; k < 4; k++)
                    o.c[k] = p.c[k] + (q.c[k] - p.c[k]) * t;
                for (int k = 0; k < NA; k++)
                    o.a[k] = p.a[k] + (q.a[k] - p.a[k]) * (f32)t;
            }
        }
        cur ^= 1;
        n = m;
        if (n < 3) {
            st.culled++;
            return;
        }
    }
    if (clipped)
        st.clipped++;
    raster(item_index, buf[cur], n);
}

// a convex polygon in clip space (inside the near plane and the guard band) as a fan of set-up
// triangles
void SoftBackend::Impl::raster(u32 item_index, const Vtx* poly, u32 n) {
    struct SV {
        s64 fx, fy;  // window position in 1/256 pixels (for the edges: which pixels it covers)
        f64 x, y;    // window position (for the planes: what the pixels get, as raster.cpp has it)
        f64 z;
        f32 iw;
        f32 a[NA];
    } sv[12];
    for (u32 i = 0; i < n; i++) {
        const Vtx& v = poly[i];
        if (!(v.c[3] > 0))
            return;  // (on the eye: degenerate)
        const f64 iw = 1.0 / v.c[3];
        const f64 x = v.c[0] * iw * vp[0] + ox, y = v.c[1] * iw * vp[1] + oy;
        SV& s = sv[i];
        s.fx = (s64)std::lround(x * SUB), s.fy = (s64)std::lround(y * SUB);
        s.x = x, s.y = y;
        s.z = v.c[2] * iw * vp[2] + vp[5];
        s.iw = (f32)iw;
        for (int k = 0; k < NA; k++)
            s.a[k] = v.a[k] * (f32)iw;
    }
    for (u32 f = 1; f + 1 < n; f++) {
        const SV* p0 = &sv[0];
        const SV* p1 = &sv[f];
        const SV* p2 = &sv[f + 1];
        s64 area = (p1->fx - p0->fx) * (p2->fy - p0->fy) - (p2->fx - p0->fx) * (p1->fy - p0->fy);
        if (area == 0)
            continue;
        if (area < 0) {
            std::swap(p1, p2);
            area = -area;
        }
        // the pixel box: sample points (x + 0.5625) inside the vertices' box, within the scissor
        const s64 minx = std::min({p0->fx, p1->fx, p2->fx}), maxx = std::max({p0->fx, p1->fx, p2->fx});
        const s64 miny = std::min({p0->fy, p1->fy, p2->fy}), maxy = std::max({p0->fy, p1->fy, p2->fy});
        auto first_px = [](s64 lo) { return (s32)((lo - SAMPLE_FP + SUB - 1 + (s64)SUB * 4096) / SUB - 4096); };
        auto last_px = [](s64 hi) { return (s32)((hi - SAMPLE_FP + (s64)SUB * 4096) / SUB - 4096); };
        Tri t;
        t.x0 = std::max(first_px(minx), efb.scissor_left), t.x1 = std::min(last_px(maxx), efb.scissor_right);
        t.y0 = std::max(first_px(miny), efb.scissor_top), t.y1 = std::min(last_px(maxy), efb.scissor_bottom);
        t.x1 = std::min(t.x1, (s32)efb.width - 1), t.y1 = std::min(t.y1, (s32)efb.height - 1);
        if (t.x0 > t.x1 || t.y0 > t.y1)
            continue;
        t.item = item_index;
        // edges b->c, c->a, a->b: e(P) = (q - p) x (P - p) >= 0 inside; pixels exactly on an edge
        // belong to top and left edges only (raster.cpp's rule)
        const SV* e0[3] = {p1, p2, p0};
        const SV* e1[3] = {p2, p0, p1};
        for (int k = 0; k < 3; k++) {
            const s64 dx = e1[k]->fx - e0[k]->fx, dy = e1[k]->fy - e0[k]->fy;
            const bool top_left = dy < 0 || (dy == 0 && dx > 0);
            t.ea[k] = -dy;
            t.eb[k] = dx;
            t.ec[k] = dy * e0[k]->fx - dx * e0[k]->fy - (top_left ? 0 : 1);
        }
        // attribute planes, from p0 (the exact positions; a triangle only the snapping gave an area
        // has no planes and draws nothing)
        const f64 d1x = p1->x - p0->x, d1y = p1->y - p0->y, d2x = p2->x - p0->x, d2y = p2->y - p0->y;
        const f64 den = d1x * d2y - d2x * d1y;
        if (!(std::fabs(den) > 1e-12))
            continue;
        const f64 inv = 1.0 / den;
        auto grad = [&](f64 v0, f64 v1, f64 v2, f64* gx, f64* gy) {
            *gx = ((v1 - v0) * d2y - (v2 - v0) * d1y) * inv;
            *gy = ((v2 - v0) * d1x - (v1 - v0) * d2x) * inv;
        };
        t.ox = p0->x, t.oy = p0->y;
        t.z = p0->z;
        grad(p0->z, p1->z, p2->z, &t.dzdx, &t.dzdy);
        f64 gx, gy;
        t.iw = p0->iw;
        grad(p0->iw, p1->iw, p2->iw, &gx, &gy);
        t.diwdx = (f32)gx, t.diwdy = (f32)gy;
        for (int k = 0; k < NA; k++) {
            t.a[k] = p0->a[k];
            grad(p0->a[k], p1->a[k], p2->a[k], &gx, &gy);
            t.dadx[k] = (f32)gx, t.dady[k] = (f32)gy;
        }
        tris.push_back(t);
    }
}

// ---------------------------------------------------------------- the packet
void SoftBackend::Impl::setup(const Packet& p, const GxView& view, Stats& st) {
    items.clear();
    tris.clear();
    const Package& pkg = *p.package;
    const Prepared& prep = *(const Prepared*)pkg.backend_data;
    memcpy(proj, view.projection, sizeof(proj));
    memcpy(vp, view.viewport, sizeof(vp));
    k = p.world_scale > 0 ? p.world_scale : 1.0f;
    ox = vp[3] - efb.x_offset, oy = vp[4] - efb.y_offset;
    items.resize(p.items.size());
    for (u32 i = 0; i < p.items.size(); i++) {
        const DrawItem& di = p.items[i];
        if (di.batch >= pkg.batches().count)
            continue;
        Item& it = items[i];
        make_item(p, di, prep, it);
        const fmt::Batch& b = *it.batch;
        const Material& m = p.materials[b.material];
        if (m.pixel.cull_gx == 3)
            continue;
        // (its vertices: the package's; the packet's for a dynamic item, all from the start or its range
        // of them with its first indices; the wave grid's made here)
        fmt::VertexBuffer vb = pkg.vertex_buffers()[b.vbuf];
        const u8* data = pkg.at(vb.data);
        u32 n_indices = b.n_indices;
        const bool ranged = (di.flags & DRAW_DYNAMIC) && di.range != 0xFFFF;
        if (ranged) {
            if (di.range >= p.dynamic_ranges.size() || !vb.stride)
                continue;
            const DynamicRange& r = p.dynamic_ranges[di.range];
            if ((size_t)r.at + r.bytes > p.dynamic.size() || r.n_indices > b.n_indices)
                continue;
            data = p.dynamic.data() + r.at;
            vb.count = r.bytes / vb.stride;
            n_indices = r.n_indices;
        } else if (di.flags & DRAW_DYNAMIC) {
            if (p.dynamic.size() < (size_t)vb.count * vb.stride)
                continue;
            data = p.dynamic.data();
        } else if (di.flags & DRAW_SEA_GRID) {
            sea_grid(p, b, &vb);
            if (!vb.count)
                continue;
            data = grid_vertices.data();
        }
        if (stamp.size() < vb.count)
            stamp.resize(vb.count, 0), vcache.resize(vb.count);
        if (++gen == 0) {
            std::fill(stamp.begin(), stamp.end(), 0);
            gen = 1;
        }
        const u16* idx = pkg.indices().data + b.first_index;
        st.items++;
        for (u32 k = 0; k + 2 < n_indices; k += 3) {
            // (a range's indices refer to its own vertices: nothing past it is read for one that doesn't)
            if (ranged && (idx[k] >= vb.count || idx[k + 1] >= vb.count || idx[k + 2] >= vb.count))
                continue;
            const Vtx& v0 = vertex(it, vb, data, idx[k]);
            const Vtx& v1 = vertex(it, vb, data, idx[k + 1]);
            const Vtx& v2 = vertex(it, vb, data, idx[k + 2]);
            triangle(i, it, v0, v1, v2, st);
        }
    }
}

// The wave grid's vertices, as the PICA backend's vertex program (platform/3ds/shaders/g3d_sea.v.pica)
// makes them, into grid_vertices with `vb` describing them (position f32, colour, uv0 f32): each the
// grid's own (about its centre: the item's matrix) raised by the waves (sea.h sea_wave, its amplitude
// the packet's), its colour channel the shade (half of it) and the brightening (sea_shade), its uv0
// the sea's texture coordinates at its place (fmt::Sea::uv_map: the material's matrices do the rest,
// as on the flat sea). vb's count 0 if the packet has no grid.
void SoftBackend::Impl::sea_grid(const Packet& p, const fmt::Batch& b, fmt::VertexBuffer* vb) {
    const Package& pkg = *p.package;
    const fmt::Sea* sp = pkg.sea();
    const fmt::VertexBuffer& src = pkg.vertex_buffers()[b.vbuf];
    if (!sp || !(src.attrs & fmt::VA_POS) || p.sea.amp.size() < 4 * (size_t)src.count) {
        vb->count = 0;
        return;
    }
    constexpr u32 STRIDE = 24;
    grid_vertices.resize((size_t)src.count * STRIDE);
    const u8* in = pkg.at(src.data);
    const Packet::SeaDraw& sd = p.sea;
    for (u32 i = 0; i < src.count; i++) {
        f32 q[3];
        memcpy(q, in + (size_t)i * src.stride, 12);
        f32 h, dx, dz, shade, bright;
        sea_wave(sd.waves, q[0], q[2], sd.amp[4 * (size_t)i] * (1.0f / 255.0f), &h, &dx, &dz);
        sea_shade(*sp, q[0], h, q[2], dx, dz, sd.eye, &shade, &bright);
        u8* o = grid_vertices.data() + (size_t)i * STRIDE;
        const f32 pos[3] = {q[0], h, q[2]};
        memcpy(o, pos, 12);
        const u8 c = (u8)clampf(0.5f * shade * 255.0f + 0.5f, 0, 255);
        o[12] = o[13] = o[14] = c;
        o[15] = (u8)clampf(bright * 255.0f + 0.5f, 0, 255);
        const f64 wx = (f64)sd.origin[0] + q[0], wz = (f64)sd.origin[2] + q[2];
        const f32 uv[2] = {(f32)(sp->uv_map[0][0] * wx + sp->uv_map[0][1] * wz + sp->uv_map[0][2]),
                           (f32)(sp->uv_map[1][0] * wx + sp->uv_map[1][1] * wz + sp->uv_map[1][2])};
        memcpy(o + 16, uv, 8);
    }
    vb->count = src.count;
    vb->attrs = fmt::VA_POS | fmt::VA_CLR | fmt::VA_UV0;
    vb->flags = fmt::VB_UV_FLOAT;
    vb->stride = STRIDE;
}

// the triangles' pixels on this band's rows (rows in blocks of 4, block j to band j % bands)
void SoftBackend::Impl::draw_rows(u32 band, u32 bands, Stats& st) const {
    const u32 W = efb.width;
    const u32 pixfmt = efb.pixel_format;
    C4 src[16];
    memset(src, 0, sizeof(src));
    for (const Tri& t : tris) {
        const Item& it = items[t.item];
        for (s32 y = t.y0; y <= t.y1; y++) {
            if (((u32)y >> 2) % bands != band) {
                y |= 3;  // (the rest of this block of four)
                continue;
            }
            // the row's span: where each edge function is >= 0 (exact, in integers; pixels from x0 on)
            const s64 Y = (s64)y * SUB + SAMPLE_FP, X0 = (s64)t.x0 * SUB + SAMPLE_FP;
            s64 lo = 0, hi = t.x1 - t.x0;
            for (int k = 0; k < 3 && lo <= hi; k++) {
                const s64 e = t.ea[k] * X0 + t.eb[k] * Y + t.ec[k], step = t.ea[k] * SUB;
                if (step > 0) {
                    if (e < 0)
                        lo = std::max(lo, (-e + step - 1) / step);
                } else if (step < 0) {
                    hi = e < 0 ? -1 : std::min(hi, e / -step);
                } else if (e < 0) {
                    hi = -1;
                }
            }
            if (lo > hi)
                continue;
            const s32 xl = t.x0 + (s32)lo, xr = t.x0 + (s32)hi;
            const f64 dy = (f64)y + SAMPLE - t.oy, dx0 = (f64)xl + SAMPLE - t.ox;
            const f32 sy = (f32)dy, sx0 = (f32)dx0;
            f64 z = t.z + t.dzdx * dx0 + t.dzdy * dy;
            f32 iw = t.iw + t.diwdx * sx0 + t.diwdy * sy;
            f32 a[NA];
            for (int k = 0; k < NA; k++)
                a[k] = t.a[k] + t.dadx[k] * sx0 + t.dady[k] * sy;
            u32* color = efb.color + (size_t)y * W;
            u32* depth = efb.depth + (size_t)y * W;
            u8* mark = stencil.data() + (size_t)y * W;
            for (s32 x = xl; x <= xr; x++, z += t.dzdx, iw += t.diwdx) {
                if (x != xl)
                    for (int k = 0; k < NA; k++)
                        a[k] += t.dadx[k];
                st.fragments++;
                if (it.stencil_masked && mark[x] == stencil_ref)
                    continue;
                const u32 zi = z <= 0 ? 0u : z >= 16777215.0 ? 0xFFFFFFu : (u32)z;
                if (it.depth_test && !test(it.depth_func, zi, depth[x]))
                    continue;
                const f32 w = iw != 0 ? 1.0f / iw : 0.0f;
                if (it.need_color)
                    for (int c = 0; c < 4; c++)
                        src[engine::gx::SRC_PRIMARY_COLOR].v[c] = clampi(to_int(a[c] * w + 0.5f), 0, 255);
                for (int u = 0; u < 3; u++) {
                    const Unit& un = it.unit[u];
                    if (!un.on)
                        continue;
                    C4& out = src[engine::gx::SRC_TEXTURE0 + u];
                    if (un.kind == UK_SHADOW) {
                        const s32 v = shadow_sample(a[4] * w, a[5] * w, a[10] * w);
                        out = {{v, v, v, v}};
                        continue;
                    }
                    if (un.kind == UK_POSITION) {
                        out = unpack(sample(un, a[4 + 2 * u] * w, a[5 + 2 * u] * w, -64.0f));
                        continue;
                    }
                    if (!un.tex) {
                        out = {{255, 255, 255, 255}};
                        continue;
                    }
                    f32 s, tt, lod;
                    const f32 X = (f32)x + SAMPLE, Y2 = (f32)y + SAMPLE;
                    if (un.source == TexCoordSource::PROJECTION) {
                        s = un.ps[0] * X + un.ps[1] * Y2 + un.ps[2];
                        tt = un.pt[0] * X + un.pt[1] * Y2 + un.pt[2];
                        lod = un.proj_lod;
                    } else {
                        s = a[4 + 2 * u] * w, tt = a[5 + 2 * u] * w;
                        // d(s)/dx = (d(s/w)/dx - s d(1/w)/dx) w, in texels of level 0; the level of
                        // detail from the largest of the four (GX's rule, as Dolphin has it)
                        const f32 dsx = std::fabs((t.dadx[4 + 2 * u] - s * t.diwdx) * w * un.tex->width);
                        const f32 dtx = std::fabs((t.dadx[5 + 2 * u] - tt * t.diwdx) * w * un.tex->height);
                        const f32 dsy = std::fabs((t.dady[4 + 2 * u] - s * t.diwdy) * w * un.tex->width);
                        const f32 dty = std::fabs((t.dady[5 + 2 * u] - tt * t.diwdy) * w * un.tex->height);
                        const f32 rho = std::max(std::max(dsx, dtx), std::max(dsy, dty));
                        lod = rho > 1e-15f ? fast_log2(rho) : -64.0f;
                    }
                    out = unpack(sample(un, s, tt, lod));
                }
                C4 c = run_combiners(it, src);
                if (it.alpha_test && !test(it.alpha_func, (u32)c.v[3], it.alpha_ref))
                    continue;
                if (it.depth_write)
                    depth[x] = zi;
                if (it.stencil_write)
                    mark[x] = stencil_ref;
                if (it.fog) {
                    const f32 f = clampf(w * it.fog_a + it.fog_b, 0, 1);
                    const s32 fi = (s32)(f * 256.0f);
                    for (int k = 0; k < 3; k++)
                        c.v[k] = (c.v[k] * (256 - fi) + it.fog_rgb[k] * fi + 128) >> 8;
                }
                st.written++;
                write_pixel(it, &color[x], c.v, pixfmt);
            }
        }
    }
}

// ---------------------------------------------------------------- SoftBackend
SoftBackend::SoftBackend() : impl_(new Impl) {
    // the decals' fade window (native_draw.h shadow_fade_alpha), sampled as its texture
    SoftTexture& f = impl_->fade_tex;
    const u32 n = SHADOW_FADE_TEXELS;
    f.texels.resize(n * n);
    for (u32 y = 0; y < n; y++)
        for (u32 x = 0; x < n; x++) {
            const u32 a = (u32)std::lround(255.0f * shadow_fade_alpha(((f32)x + 0.5f) / n, ((f32)y + 0.5f) / n));
            f.texels[y * n + x] = 0x00FFFFFFu | a << 24;
        }
    f.levels = 1;
    f.width = f.height = (f32)n;
    f.level[0] = {f.texels.data(), n, (s32)n, (s32)n, true};
}

// The casters into the shadow map (native_draw.h ShadowMap), as the 3DS GPU draws them in its shadow
// mode: each pixel whose centre a triangle covers keeps the nearer of its depth (0-1, 24 bits) and what
// is there; no culling (both faces cast); an alpha-tested material's texture alpha tested as the
// material tests it (a tree's leaves), its texture's level from the coordinates' rate across the map.
void SoftBackend::Impl::draw_shadow_map(const Packet& p, const Prepared& prep, Stats& st) {
    const ShadowMap& sm = p.shadow;
    const Package& pkg = *p.package;
    smap_size = sm.size;
    smap_bias = sm.bias;
    smap.assign((size_t)sm.size * sm.size, 0xFFFFFFu);
    const f64 S = sm.size;
    for (const ShadowCaster& c : p.casters) {
        if (c.batch >= pkg.batches().count || 12 * ((size_t)c.matrix + 1) > p.matrices.size())
            continue;
        const fmt::Batch& b = pkg.batches()[c.batch];
        const Material& m = p.materials[b.material];
        const fmt::VertexBuffer& vb = pkg.vertex_buffers()[b.vbuf];
        const f32* mm = &p.matrices[12 * (size_t)c.matrix];
        // the buffer's positions -> the map's (X, Y in texels, depth)
        f64 rows[3][4];
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 4; k++)
                rows[r][k] = (f64)sm.light[r][0] * mm[k] + (f64)sm.light[r][1] * mm[4 + k] +
                             (f64)sm.light[r][2] * mm[8 + k] + (k == 3 ? (f64)sm.light[r][3] : 0.0);
            if (vb.attrs & fmt::VA_POS16) {
                rows[r][3] += rows[r][0] * vb.pos_origin[0] + rows[r][1] * vb.pos_origin[1] + rows[r][2] * vb.pos_origin[2];
                for (int k = 0; k < 3; k++)
                    rows[r][k] *= vb.pos_scale;
            }
            if (r < 2)
                for (int k = 0; k < 4; k++)
                    rows[r][k] *= S;
        }
        // the alpha test's texture: unit 0's, at its coordinates (uv0 or uv1 through its matrix)
        const TexUnit& tu = m.unit[0];
        const SoftTexture* tex = m.pixel.alpha_test && m.pixel.alpha_func != 1 && tu.texture != fmt::NONE16 &&
                                         tu.texture < prep.textures.size() && prep.textures[tu.texture].levels &&
                                         (tu.source == TexCoordSource::UV0 || tu.source == TexCoordSource::UV1)
                                     ? &prep.textures[tu.texture]
                                     : nullptr;
        const int uvk = tu.source == TexCoordSource::UV1;
        const bool lum = m.lum_units & 1;
        Unit un;
        if (tex) {
            un.tex = tex, un.wrap_s = tu.wrap_s, un.wrap_t = tu.wrap_t, un.mag_linear = tu.mag_linear;
            un.min_linear = tu.min_linear, un.mip = tu.mip, un.lod_bias = tu.lod_bias, un.min_lod = tu.min_lod;
            un.max_lod = std::max(tu.min_lod, std::min(tu.max_lod, (f32)(tex->levels - 1)));
        }
        const f32(*T)[4] = tu.matrix >= 0 && tu.matrix < 3 ? m.tex_mtx[tu.matrix] : nullptr;
        u32 uv_at = ((vb.attrs & fmt::VA_POS16) ? 8 : 12) + ((vb.attrs & fmt::VA_NRM) ? 4 : 0) +
                    ((vb.attrs & fmt::VA_CLR) ? 4 : 0);
        const u32 uv_size = (vb.flags & fmt::VB_UV_FLOAT) ? 8 : 4;
        if (uvk)
            uv_at += (vb.attrs & fmt::VA_UV0) ? uv_size : 0;
        const bool has_uv = vb.attrs & (uvk ? fmt::VA_UV1 : fmt::VA_UV0);
        const u8* data = pkg.at(vb.data);
        const u16* idx = pkg.indices().data;
        struct V {
            f64 x, y, d;
            f32 s, t;
        };
        auto vertex = [&](u16 i) {
            const u8* q = data + (size_t)i * vb.stride;
            f64 pos[3];
            if (vb.attrs & fmt::VA_POS16) {
                s16 sp[3];
                memcpy(sp, q, 6);
                pos[0] = sp[0], pos[1] = sp[1], pos[2] = sp[2];
            } else {
                f32 fp[3];
                memcpy(fp, q, 12);
                pos[0] = fp[0], pos[1] = fp[1], pos[2] = fp[2];
            }
            V v;
            v.x = rows[0][0] * pos[0] + rows[0][1] * pos[1] + rows[0][2] * pos[2] + rows[0][3];
            v.y = rows[1][0] * pos[0] + rows[1][1] * pos[1] + rows[1][2] * pos[2] + rows[1][3];
            v.d = rows[2][0] * pos[0] + rows[2][1] * pos[1] + rows[2][2] * pos[2] + rows[2][3];
            v.s = v.t = 0;
            if (tex && has_uv) {
                f32 uv[2];
                if (vb.flags & fmt::VB_UV_FLOAT) {
                    memcpy(uv, q + uv_at, 8);
                } else {
                    s16 su[2];
                    memcpy(su, q + uv_at, 4);
                    uv[0] = su[0] * vb.uv_scale[uvk], uv[1] = su[1] * vb.uv_scale[uvk];
                }
                v.s = T ? T[0][0] * uv[0] + T[0][1] * uv[1] + T[0][2] + T[0][3] : uv[0];
                v.t = T ? T[1][0] * uv[0] + T[1][1] * uv[1] + T[1][2] + T[1][3] : uv[1];
            }
            return v;
        };
        for (u32 k = c.first_index; k + 2 < c.first_index + c.n_indices; k += 3) {
            const V a = vertex(idx[k]), bb = vertex(idx[k + 1]), cc = vertex(idx[k + 2]);
            st.shadow_triangles++;
            const f64 area = (bb.x - a.x) * (cc.y - a.y) - (cc.x - a.x) * (bb.y - a.y);
            if (std::fabs(area) < 1e-12)
                continue;
            // (depth outside 0..1 is clipped, as the GPU clips z)
            const s32 x0 = std::max(0, (s32)std::floor(std::min({a.x, bb.x, cc.x}) - 0.5) + 1);
            const s32 x1 = std::min((s32)sm.size - 1, (s32)std::floor(std::max({a.x, bb.x, cc.x}) - 0.5));
            const s32 y0 = std::max(0, (s32)std::floor(std::min({a.y, bb.y, cc.y}) - 0.5) + 1);
            const s32 y1 = std::min((s32)sm.size - 1, (s32)std::floor(std::max({a.y, bb.y, cc.y}) - 0.5));
            if (x0 > x1 || y0 > y1)
                continue;
            const f64 inv = 1.0 / area;
            // the planes: depth and the texture coordinates over the map (affine: an orthographic view)
            auto plane = [&](f64 va, f64 vb2, f64 vc, f64* gx, f64* gy) {
                *gx = ((vb2 - va) * (cc.y - a.y) - (vc - va) * (bb.y - a.y)) * inv;
                *gy = ((vc - va) * (bb.x - a.x) - (vb2 - va) * (cc.x - a.x)) * inv;
            };
            f64 dzx, dzy, dsx = 0, dsy = 0, dtx = 0, dty = 0;
            plane(a.d, bb.d, cc.d, &dzx, &dzy);
            f32 lod = 0;
            if (tex) {
                plane(a.s, bb.s, cc.s, &dsx, &dsy);
                plane(a.t, bb.t, cc.t, &dtx, &dty);
                const f64 rho = std::max(std::max(std::fabs(dsx) * tex->width, std::fabs(dtx) * tex->height),
                                         std::max(std::fabs(dsy) * tex->width, std::fabs(dty) * tex->height));
                lod = rho > 1e-15 ? fast_log2((f32)rho) : -64.0f;
            }
            for (s32 y = y0; y <= y1; y++)
                for (s32 x = x0; x <= x1; x++) {
                    const f64 px = x + 0.5, py = y + 0.5;
                    const f64 e0 = (bb.x - a.x) * (py - a.y) - (bb.y - a.y) * (px - a.x);
                    const f64 e1 = (cc.x - bb.x) * (py - bb.y) - (cc.y - bb.y) * (px - bb.x);
                    const f64 e2 = (a.x - cc.x) * (py - cc.y) - (a.y - cc.y) * (px - cc.x);
                    if (!((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)))
                        continue;
                    const f64 d = a.d + dzx * (px - a.x) + dzy * (py - a.y);
                    if (d < 0 || d > 1)
                        continue;
                    if (tex) {
                        const f32 s = (f32)(a.s + dsx * (px - a.x) + dsy * (py - a.y));
                        const f32 t = (f32)(a.t + dtx * (px - a.x) + dty * (py - a.y));
                        const u32 texel = sample(un, s, t, lod);
                        const u32 alpha = lum ? texel & 255 : texel >> 24;
                        if (!test(m.pixel.alpha_func, alpha, m.pixel.alpha_ref))
                            continue;
                    }
                    u32& at = smap[(u32)y * sm.size + (u32)x];
                    const u32 di = (u32)(d * 16777215.0);
                    if (di < at)
                        at = di;
                    st.shadow_texels++;
                }
        }
    }    // NOMA_SOFT3D_SHADOW_DUMP=file.pgm: the map as a picture (white: nothing; darker: nearer the sun)
    if (const char* dump = getenv("NOMA_SOFT3D_SHADOW_DUMP"))
        if (FILE* f = fopen(dump, "wb")) {
            fprintf(f, "P5\n%u %u\n255\n", (unsigned)sm.size, (unsigned)sm.size);
            for (u32 v : smap)
                fputc((int)(v >> 16), f);
            fclose(f);
        }
}
SoftBackend::~SoftBackend() { delete impl_; }

void SoftBackend::set_overlay_pull(f64 pull, f64 limit) { impl_->pull = pull, impl_->pull_limit = limit; }

void SoftBackend::prepare(Package& pkg) {
    if (pkg.backend_data)
        return;
    Prepared* p = new Prepared;
    p->textures.resize(pkg.textures().count);
    for (u32 i = 0; i < pkg.textures().count; i++) {
        const Package::TextureImage img = pkg.texture_image(i);
        SoftTexture& t = p->textures[i];
        const u32 levels = std::min<u32>(img.levels, MAX_LEVELS);
        u32 total = 0;
        for (u32 l = 0; l < levels; l++)
            total += (u32)(img.width >> l) * (u32)(img.height >> l);
        t.texels.resize(total);
        u32 at = 0;
        for (u32 l = 0; l < levels; l++) {
            const TexLevel lv = pica_level(img.format, img.width, img.height, l);
            if (lv.offset + lv.size > img.size || !lv.width || !lv.height)
                break;
            pica_decode(img.format, img.texels + lv.offset, lv.width, lv.height, &t.texels[at]);
            SoftLevel& sl = t.level[l];
            sl.texels = t.texels.data() + at;  // (the vector is sized once: the pointer stays good)
            sl.pitch = lv.width;
            // the source image at this level (a padded texture's coordinates address only it)
            sl.w = (s32)std::min<u32>(std::max<u32>(1, (u32)img.src_width >> l), lv.width);
            sl.h = (s32)std::min<u32>(std::max<u32>(1, (u32)img.src_height >> l), lv.height);
            sl.pow2 = is_pow2((u32)sl.w) && is_pow2((u32)sl.h);
            at += lv.width * lv.height;
            t.levels = l + 1;
        }
        t.width = (f32)std::max<u32>(1, img.src_width), t.height = (f32)std::max<u32>(1, img.src_height);
    }
    pkg.backend_data = p;
}

void SoftBackend::release(Package& pkg) {
    delete (Prepared*)pkg.backend_data;
    pkg.backend_data = nullptr;
}

void SoftBackend::draw(const Packet& packet, const GxView& view) { draw(packet, view, guest::gx_host_efb()); }

void SoftBackend::draw(const Packet& packet, const GxView& view, const guest::HostEfb& target) {
    if (packet.kind != PacketKind::DRAW || !packet.package || !packet.materials || packet.items.empty() ||
        view.orthographic)
        return;
    if (!packet.package->backend_data)
        prepare(*const_cast<Package*>(packet.package));
    Impl& m = *impl_;
    m.efb = target;
    if (m.efb.scissor_left > m.efb.scissor_right || m.efb.scissor_top > m.efb.scissor_bottom)
        return;
    // the shadow map, before the opaque packet's items (a packet without one: none for its receivers)
    if (!packet.shadow.size)
        m.smap_size = 0;
    else if (packet.pass == Pass::OPAQUE)
        m.draw_shadow_map(packet, *(const Prepared*)packet.package->backend_data, stats_);
    // (the stencil marks: a value a pixel, this packet's the grid's; cleared when the values come
    // round again, so that an old mark never counts)
    if (m.stencil.size() != (size_t)m.efb.width * m.efb.height)
        m.stencil.assign((size_t)m.efb.width * m.efb.height, 0);
    if (packet.pass == Pass::OPAQUE) {
        if (packet.sea.stencil != m.stencil_ref && packet.sea.stencil == 1)
            std::fill(m.stencil.begin(), m.stencil.end(), 0);
        m.stencil_ref = packet.sea.stencil;
    }
    m.setup(packet, view, stats_);
    if (m.tris.empty())
        return;
    // the rows among threads
    u32 n = threads_;
    if (!n) {
        static const u32 s_env = getenv("NOMA_SOFT3D_THREADS") ? (u32)atoi(getenv("NOMA_SOFT3D_THREADS")) : 0;
        n = s_env ? s_env : std::min<u32>(8, std::max<u32>(1, std::thread::hardware_concurrency()));
    }
    n = std::max<u32>(1, std::min<u32>(n, 64));
    if (n == 1) {
        m.draw_rows(0, 1, stats_);
        return;
    }
    std::vector<Stats> part(n);
    std::vector<std::thread> pool;
    pool.reserve(n - 1);
    for (u32 b = 1; b < n; b++)
        pool.emplace_back([&m, &part, b, n] { m.draw_rows(b, n, part[b]); });
    m.draw_rows(0, n, part[0]);
    for (std::thread& t : pool)
        t.join();
    for (const Stats& s : part)
        stats_.fragments += s.fragments, stats_.written += s.written;
}

}  // namespace engine::g3d
