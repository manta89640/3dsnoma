// The amp on the Sound Player's stage (see amp.h).
//
// A small combo amplifier drawn in the channel's soft, clean style, made here (the channel has no
// such model): a charcoal cabinet with rounded edges standing on four little feet with a strap
// handle on top; in its front a woven cloth grille with the speaker behind it, and over the grille
// a darker control strip with three light grey knobs and a cyan pilot light; a soft round shadow on
// the floor under it.
//
// Everything is made once, at construction, in guest memory: the geometry as GX display lists
// (one quad list each for the cabinet, the speaker, the pilot light, the shadow, the grille and the
// light's glow; positions f32, colours RGBA8, texture coordinates f32), the grille's cloth (IA8
// 16x16) and a soft round blob for the shadow and the glow (I8 32x32). The lighting is baked into
// the vertex colours: a fixed key light from the upper left front, an ambient and a soft sheen on
// the rounded edges, like the studio's unlit materials and the Miis' baked shading. Each frame the
// lists are called with the amp's matrix (the camera's view matrix x its place) between g3d's
// opaque and translucent passes: the speaker's list gets its own matrix (it swells on the beat),
// and the speaker, the pilot light and its glow a colour (TEV register 1) that brightens on the
// beat. The renderers keep a display list's geometry, so the amp costs six draws a frame and no
// vertex data.
//
// Units are the stage's (a Mii is about 150 tall); the amp's origin is the middle of its footprint
// on the floor, its front faces +z.
#include "engine/soundplayer/amp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "engine/math/vecmath.h"
#include "engine/soundplayer/angles.h"
#include "engine/soundplayer/g3d.h"
#include "engine/soundplayer/lyt.h"
#include "noma_decomp.h"

namespace engine::soundplayer {

namespace {

using namespace engine::math;

// ---- GX's enums (the SDK's values)
enum : u32 {
    GX_VA_POS = 9, GX_VA_CLR0 = 11, GX_VA_TEX0 = 13, GX_DIRECT = 1,
    GX_POS_XYZ = 1, GX_F32 = 4, GX_CLR_RGBA = 1, GX_RGBA8 = 5, GX_TEX_ST = 1,
    GX_VTXFMT0 = 0, GX_DRAW_QUADS = 0x80,
    GX_COLOR0A0 = 4, GX_SRC_REG = 0, GX_SRC_VTX = 1, GX_DF_NONE = 0, GX_AF_NONE = 2,
    GX_TEXCOORD0 = 0, GX_TEXCOORD_NULL = 0xFF, GX_TEXMAP0 = 0, GX_TEXMAP_NULL = 0xFF,
    GX_TG_MTX2x4 = 1, GX_TG_TEX0 = 4, GX_IDENTITY = 60, GX_PTIDENTITY = 125,
    GX_CC_C1 = 4, GX_CC_TEXC = 8, GX_CC_RASC = 10, GX_CC_ZERO = 15,
    GX_CA_A1 = 2, GX_CA_TEXA = 4, GX_CA_RASA = 5, GX_CA_ZERO = 7,
    GX_TEV_ADD = 0, GX_TB_ZERO = 0, GX_CS_SCALE_1 = 0, GX_TEVPREV = 0, GX_TEVREG1 = 2,
    GX_TEV_SWAP0 = 0, GX_CH_RED = 0, GX_CH_GREEN = 1, GX_CH_BLUE = 2, GX_CH_ALPHA = 3,
    GX_ALWAYS = 7, GX_AOP_AND = 0, GX_LEQUAL = 3,
    GX_BM_NONE = 0, GX_BM_BLEND = 1, GX_BL_ONE = 1, GX_BL_SRCALPHA = 4, GX_BL_INVSRCALPHA = 5,
    GX_LO_CLEAR = 0, GX_CULL_NONE = 0, GX_PNMTX0 = 0,
    GX_TF_I8 = 1, GX_TF_IA8 = 3, GX_CLAMP = 0, GX_REPEAT = 1,
};

// ---- the amp's measures
// the cabinet: 58 wide, 46 high (on 3-high feet), 30 deep, edges rounded with a radius of 4
const f32 CAB_HX = 29, CAB_Y0 = 3, CAB_Y1 = 49, CAB_HZ = 15, CAB_R = 4;
// the grille's opening in the front, the speaker's cavity behind it (6 deep), the control strip
const f32 GRILLE_HX = 24, GRILLE_Y0 = 8, GRILLE_Y1 = 33, CAVITY_DEPTH = 6;
const f32 STRIP_Y0 = 35, STRIP_Y1 = 45;
// the speaker: its middle and radius on the cavity's back wall
const f32 CONE_Y = (GRILLE_Y0 + GRILLE_Y1) / 2, CONE_R = 11.5f;
// the knobs and the pilot light on the strip
const f32 KNOB_Y = (STRIP_Y0 + STRIP_Y1) / 2, KNOB_R = 3.3f, KNOB_DEPTH = 2.4f;
const f32 KNOB_X[] = {-15.5f, -6.5f, 2.5f};
const f32 PILOT_X = 15.5f, PILOT_R = 2.6f;

// colours (the light and the sheen change them per vertex)
struct Rgb {
    f32 r, g, b;
};
const Rgb CABINET = {92, 92, 96};
const Rgb STRIP = {62, 62, 65};
const Rgb CAVITY = {34, 34, 36};
const Rgb CLOTH = {124, 124, 128};
const Rgb FOOT = {48, 48, 50};
const Rgb HANDLE = {58, 58, 61};
const Rgb KNOB = {214, 214, 210};
const Rgb KNOB_MARK = {70, 70, 72};
const Rgb PILOT_BEZEL = {150, 150, 152};
const Rgb WHITE = {255, 255, 255};
// the room's focus cyan (lyt::CYAN), as floats: the pilot light lit, and dimmed between beats and
// when quiet
const Rgb CYAN = {(f32)lyt::CYAN.r, (f32)lyt::CYAN.g, (f32)lyt::CYAN.b};

// ---- vectors (engine::math's Vec3)
// a unit vector along a, or +z for a zero one (engine::math's normalize gives NaNs for it)
Vec3 normalize_or_z(Vec3 a) {
    f32 l = std::sqrt(dot(a, a));
    return l > 1e-6f ? a * (1.0f / l) : Vec3{0, 0, 1};
}
f32 axis(const Vec3& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }
void set_axis(Vec3& v, int a, f32 f) { (a == 0 ? v.x : a == 1 ? v.y : v.z) = f; }

// The baked light: a key light from the upper left front, an ambient, and a sheen (a broad
// highlight toward the camera, which looks at the amp from the front and a little above).
const Vec3 LIGHT = normalize_or_z({-0.30f, 0.75f, 0.60f});
const Vec3 HALF = normalize_or_z(LIGHT + normalize_or_z({0.0f, 0.15f, 1.0f}));
u8 clamp8(f32 v) { return (u8)std::max(0.0f, std::min(255.0f, v + 0.5f)); }
struct Rgba8 {
    u8 r, g, b, a;
};
Rgba8 shade(Rgb c, Vec3 n, f32 sheen = 0.18f) {
    f32 lit = 0.56f + 0.48f * std::max(0.0f, dot(n, LIGHT));
    f32 h = std::max(0.0f, dot(n, HALF));
    f32 h2 = h * h, h4 = h2 * h2;
    f32 spec = sheen * 255.0f * h4 * h4;
    return {clamp8(c.r * lit + spec), clamp8(c.g * lit + spec), clamp8(c.b * lit + spec), 255};
}
Rgba8 flat(Rgb c, u8 a = 255) { return {clamp8(c.r), clamp8(c.g), clamp8(c.b), a}; }

// ---- meshes: quads (a triangle is a quad with its last corner repeated)
struct Vertex {
    Vec3 p;
    Rgba8 c;
    f32 s, t;
};
struct Mesh {
    std::vector<Vertex> v;
    void quad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d) {
        v.push_back(a), v.push_back(b), v.push_back(c), v.push_back(d);
    }
};
Vertex vtx(Vec3 p, Rgba8 c, f32 s = 0, f32 t = 0) { return {p, c, s, t}; }

// A box with rounded edges, its faces cut along the lines of each axis (the edge bands at h - r and
// h on both sides, and the lines a face needs, e.g. an opening's edges: all faces share them, so
// the mesh has no cracks). A point of the plain box goes to the nearest point of the box shrunk by
// r, then r further the way it went: that rounds the edges, and the normal is that way (Gouraud
// shading across the edge bands makes them look round).
struct RoundedBox {
    Vec3 c, h;
    f32 r;
    std::vector<f32> lines[3];  // per axis, increasing, from c - h to c + h

    RoundedBox(Vec3 centre, Vec3 half, f32 radius) : c(centre), h(half), r(radius) {
        for (int a = 0; a < 3; a++) {
            f32 cc = axis(c, a), hh = axis(h, a);
            lines[a] = {cc - hh, cc - hh + r, cc + hh - r, cc + hh};
        }
    }
    void cut(int a, f32 at) {
        if (std::find(lines[a].begin(), lines[a].end(), at) != lines[a].end())
            return;
        lines[a].push_back(at);
        std::sort(lines[a].begin(), lines[a].end());
    }
    void round(Vec3 q, Vec3 face_normal, Vec3* p, Vec3* n) const {
        Vec3 inner;
        for (int a = 0; a < 3; a++) {
            f32 cc = axis(c, a), hh = axis(h, a) - r;
            set_axis(inner, a, std::max(cc - hh, std::min(cc + hh, axis(q, a))));
        }
        Vec3 d = q - inner;
        // a point of a face's flat part goes nowhere: its normal is the face's
        *n = dot(d, d) > 1e-6f ? normalize_or_z(d) : face_normal;
        *p = inner + *n * r;
    }
    // the faces (axis, side) asked for: colour(axis, side, cell middle) gives a cell's colour,
    // skip(...) leaves a cell out
    template <class Colour, class Skip>
    void emit(Mesh& m, const std::vector<std::pair<int, int>>& faces, Colour colour, Skip skip) const {
        for (const auto& face : faces) {
            int fa = face.first, side = face.second;
            int ua = (fa + 1) % 3, va = (fa + 2) % 3;
            const std::vector<f32>& U = lines[ua];
            const std::vector<f32>& V = lines[va];
            f32 plane = axis(c, fa) + side * axis(h, fa);
            Vec3 fn = {0, 0, 0};
            set_axis(fn, fa, (f32)side);
            for (size_t i = 0; i + 1 < U.size(); i++) {
                for (size_t j = 0; j + 1 < V.size(); j++) {
                    Vec3 mid;
                    set_axis(mid, fa, plane);
                    set_axis(mid, ua, (U[i] + U[i + 1]) / 2);
                    set_axis(mid, va, (V[j] + V[j + 1]) / 2);
                    if (skip(fa, side, mid))
                        continue;
                    Rgb col = colour(fa, side, mid);
                    Vertex q[4];
                    const f32 us[4] = {U[i], U[i + 1], U[i + 1], U[i]};
                    const f32 vs[4] = {V[j], V[j], V[j + 1], V[j + 1]};
                    for (int k = 0; k < 4; k++) {
                        Vec3 pt;
                        set_axis(pt, fa, plane);
                        set_axis(pt, ua, us[k]);
                        set_axis(pt, va, vs[k]);
                        Vec3 p, n;
                        round(pt, fn, &p, &n);
                        q[k] = vtx(p, shade(col, n));
                    }
                    m.quad(q[0], q[1], q[2], q[3]);
                }
            }
        }
    }
};

// a plain box's faces but its bottom and back (never seen: the camera looks from the front, above),
// and its top unless asked for
void box(Mesh& m, Vec3 lo, Vec3 hi, Rgb col, bool top = true) {
    Vec3 n = {0, 0, 1};
    m.quad(vtx({lo.x, lo.y, hi.z}, shade(col, n)), vtx({hi.x, lo.y, hi.z}, shade(col, n)),
           vtx({hi.x, hi.y, hi.z}, shade(col, n)), vtx({lo.x, hi.y, hi.z}, shade(col, n)));
    n = {0, 1, 0};
    if (top)
        m.quad(vtx({lo.x, hi.y, hi.z}, shade(col, n)), vtx({hi.x, hi.y, hi.z}, shade(col, n)),
               vtx({hi.x, hi.y, lo.z}, shade(col, n)), vtx({lo.x, hi.y, lo.z}, shade(col, n)));
    n = {-1, 0, 0};
    m.quad(vtx({lo.x, lo.y, lo.z}, shade(col, n)), vtx({lo.x, lo.y, hi.z}, shade(col, n)),
           vtx({lo.x, hi.y, hi.z}, shade(col, n)), vtx({lo.x, hi.y, lo.z}, shade(col, n)));
    n = {1, 0, 0};
    m.quad(vtx({hi.x, lo.y, hi.z}, shade(col, n)), vtx({hi.x, lo.y, lo.z}, shade(col, n)),
           vtx({hi.x, hi.y, lo.z}, shade(col, n)), vtx({hi.x, hi.y, hi.z}, shade(col, n)));
}

// a disc of rings facing +z around `centre`: per ring from the rim inward its radius, its z, its
// colour and how much its normal leans outward; the last ring's radius is 0 (the middle)
struct Ring {
    f32 r, z;
    Rgb col;
    f32 tilt;
};
void disc(Mesh& m, Vec3 centre, const std::vector<Ring>& rings, int segs, f32 sheen) {
    for (size_t k = 0; k + 1 < rings.size(); k++) {
        const Ring& o = rings[k];
        const Ring& i = rings[k + 1];
        for (int s = 0; s < segs; s++) {
            f32 a0 = 2 * PI * s / segs, a1 = 2 * PI * (s + 1) / segs;
            Vec3 d0 = {std::cos(a0), std::sin(a0), 0}, d1 = {std::cos(a1), std::sin(a1), 0};
            auto at = [&](const Ring& g, Vec3 d) {
                Vec3 n = normalize_or_z(d * g.tilt + Vec3{0, 0, 1});
                return vtx(centre + Vec3{d.x * g.r, d.y * g.r, g.z}, shade(g.col, n, sheen));
            };
            m.quad(at(o, d0), at(o, d1), at(i, d1), at(i, d0));
        }
    }
}

// A round knob facing +z at (x, y) on the plane z: a short cylinder, a slightly domed cap and a
// dark pointer line at `angle` (radians).
void knob(Mesh& m, f32 x, f32 y, f32 z, f32 angle) {
    const int SEGS = 7;
    for (int i = 0; i < SEGS; i++) {
        f32 a0 = 2 * PI * i / SEGS, a1 = 2 * PI * (i + 1) / SEGS;
        Vec3 n0 = {std::cos(a0), std::sin(a0), 0}, n1 = {std::cos(a1), std::sin(a1), 0};
        Vec3 p0 = {x + KNOB_R * n0.x, y + KNOB_R * n0.y, z}, p1 = {x + KNOB_R * n1.x, y + KNOB_R * n1.y, z};
        Vec3 f0 = p0 + Vec3{0, 0, KNOB_DEPTH}, f1 = p1 + Vec3{0, 0, KNOB_DEPTH};
        m.quad(vtx(p0, shade(KNOB, n0, 0.1f)), vtx(p1, shade(KNOB, n1, 0.1f)), vtx(f1, shade(KNOB, n1, 0.1f)),
               vtx(f0, shade(KNOB, n0, 0.1f)));
        // the cap: a fan of degenerate quads, its middle a little forward
        Vec3 e0 = normalize_or_z(n0 * 0.35f + Vec3{0, 0, 1}), e1 = normalize_or_z(n1 * 0.35f + Vec3{0, 0, 1});
        Vec3 top = {x, y, z + KNOB_DEPTH + 0.5f};
        Rgba8 tc = shade(KNOB, {0, 0, 1}, 0.25f);
        m.quad(vtx(f0, shade(KNOB, e0, 0.25f)), vtx(f1, shade(KNOB, e1, 0.25f)), vtx(top, tc), vtx(top, tc));
    }
    Vec3 d = {std::cos(angle), std::sin(angle), 0}, s = {-d.y * 0.45f, d.x * 0.45f, 0};
    f32 zf = z + KNOB_DEPTH + 0.55f;
    Vec3 a = {x + d.x * 0.6f, y + d.y * 0.6f, zf};
    Vec3 b = {x + d.x * (KNOB_R - 0.4f), y + d.y * (KNOB_R - 0.4f), zf};
    Rgba8 mark = shade(KNOB_MARK, {0, 0, 1}, 0.0f);
    m.quad(vtx(a - s, mark), vtx(b - s, mark), vtx(b + s, mark), vtx(a + s, mark));
}

// ---- textures, in GX's tiled layouts
// IA8: 4x4 texel tiles, 2 bytes a texel (alpha, then intensity); `ia` is (intensity, alpha) per
// texel, in rows
std::vector<u8> tile_ia8(int w, int h, const std::vector<u8>& ia) {
    std::vector<u8> out;
    for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4)
            for (int y = ty; y < ty + 4; y++)
                for (int x = tx; x < tx + 4; x++) {
                    out.push_back(ia[(y * w + x) * 2 + 1]);
                    out.push_back(ia[(y * w + x) * 2]);
                }
    return out;
}
// I8: 8x4 texel tiles
std::vector<u8> tile_i8(int w, int h, const std::vector<u8>& i8) {
    std::vector<u8> out;
    for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 8)
            for (int y = ty; y < ty + 4; y++)
                for (int x = tx; x < tx + 8; x++)
                    out.push_back(i8[y * w + x]);
    return out;
}

// The grille's cloth, 16x16 texels, repeated: a basket weave of threads 2 texels wide every 4, the
// thread on top alternating from cell to cell; light where a thread is, see-through between the
// threads (the speaker shows through).
const int CLOTH_SIZE = 16;
// the units of the grille a repeat covers (4 threads each way: a thread every 3.5 units, two
// pixels or more on the screen however far the camera is)
const f32 CLOTH_REPEAT = 14.0f;
std::vector<u8> make_cloth() {
    std::vector<u8> ia(CLOTH_SIZE * CLOTH_SIZE * 2);
    for (int y = 0; y < CLOTH_SIZE; y++)
        for (int x = 0; x < CLOTH_SIZE; x++) {
            bool across = (y % 4) < 2, down = (x % 4) < 2;
            bool across_on_top = ((x / 4) + (y / 4)) % 2 == 0;
            int i, a;
            if (across && down) {  // where two threads cross: the one on top, its edge darker
                bool edge = across_on_top ? (x % 4) == 1 : (y % 4) == 1;
                i = edge ? 225 : 255, a = 240;
            } else if (across || down) {  // one thread
                i = 200, a = 190;
            } else {  // between the threads
                i = 120, a = 40;
            }
            ia[(y * CLOTH_SIZE + x) * 2] = (u8)i;
            ia[(y * CLOTH_SIZE + x) * 2 + 1] = (u8)a;
        }
    return tile_ia8(CLOTH_SIZE, CLOTH_SIZE, ia);
}

// a soft round blob (the shadow, the glow): 255 in the middle, fading to 0 at the edge
const int BLOB_SIZE = 32;
std::vector<u8> make_blob() {
    std::vector<u8> i8(BLOB_SIZE * BLOB_SIZE);
    for (int y = 0; y < BLOB_SIZE; y++)
        for (int x = 0; x < BLOB_SIZE; x++) {
            f32 dx = (x + 0.5f) / BLOB_SIZE * 2 - 1, dy = (y + 0.5f) / BLOB_SIZE * 2 - 1;
            f32 d2 = dx * dx + dy * dy;
            f32 v = d2 >= 1 ? 0 : 1 - d2;
            i8[y * BLOB_SIZE + x] = clamp8(255 * v * v);
        }
    return tile_i8(BLOB_SIZE, BLOB_SIZE, i8);
}

// ---- guest data
void put32(std::vector<u8>& out, u32 v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back((u8)(v >> shift));
}
void putf(std::vector<u8>& out, f32 f) {
    u32 bits;
    std::memcpy(&bits, &f, 4);
    put32(out, bits);
}

// a block of guest memory the GPU reads (32-byte aligned and sized): written once, flushed
struct Block {
    u32 addr = 0, size = 0;
};
Block to_guest(std::vector<u8> bytes) {
    Block b;
    b.size = ((u32)bytes.size() + 31) & ~31u;
    bytes.resize(b.size, 0);  // GX_NOP after a display list
    b.addr = g3d::new_aligned(b.size);
    if (!b.addr)
        return Block{};
    guest::write_g(b.addr, bytes.data(), b.size);
    g::DCFlushRange(b.addr, b.size);
    return b;
}

// a quad list as a display list: one draw of all its vertices (position, colour[, texture
// coordinates])
Block make_list(const Mesh& m, bool textured) {
    std::vector<u8> dl;
    dl.push_back(GX_DRAW_QUADS | GX_VTXFMT0);
    dl.push_back((u8)(m.v.size() >> 8)), dl.push_back((u8)m.v.size());
    for (const Vertex& v : m.v) {
        putf(dl, v.p.x), putf(dl, v.p.y), putf(dl, v.p.z);
        dl.push_back(v.c.r), dl.push_back(v.c.g), dl.push_back(v.c.b), dl.push_back(v.c.a);
        if (textured)
            putf(dl, v.s), putf(dl, v.t);
    }
    return to_guest(dl);
}

// ---- matrices (engine::math's Mtx34: 3x4, rows)
// translate(t) x rotate about y (degrees; 0 faces +z, positive turns toward +x) x scale
Mtx34 place(Vec3 t, f32 yaw_deg, Vec3 scale = {1, 1, 1}) {
    f32 a = deg_to_rad(yaw_deg), c = std::cos(a), s = std::sin(a);
    return {{{c * scale.x, 0, s * scale.z, t.x}, {0, scale.y, 0, t.y}, {-s * scale.x, 0, c * scale.z, t.z}}};
}
// GXLoadPosMtxImm into matrix 0 from a copy on the guest stack, and make it the current one
void load_matrix(const Mtx34& mtx) {
    GFRAME(0x40);
    u32 a = (u32)&GS(u32, 0x8);
    store(a, mtx);
    g::GXLoadPosMtxImm(a, GX_PNMTX0);
    g::GXSetCurrentMtx(GX_PNMTX0);
}

// the vertex format of a list: position and colour, and the texture coordinates of a textured one
void vertex_format(bool textured) {
    g::GXClearVtxDesc();
    g::GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    g::GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    g::GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    g::GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    if (textured) {
        g::GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        g::GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    }
}

// the one TEV stage: colour = d + (1 - c) a + c b (alpha alike), to the TEV's output
void tev(u32 ca, u32 cb, u32 cc, u32 cd, u32 aa, u32 ab, u32 ac, u32 ad, bool textured) {
    g::GXSetNumTexGens(textured ? 1 : 0);
    if (textured)
        g::GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, 0, GX_PTIDENTITY);
    g::GXSetTevOrder(0, textured ? GX_TEXCOORD0 : GX_TEXCOORD_NULL, textured ? GX_TEXMAP0 : GX_TEXMAP_NULL,
                     GX_COLOR0A0);
    g::GXSetTevColorIn(0, ca, cb, cc, cd);
    g::GXSetTevAlphaIn(0, aa, ab, ac, ad);
    g::GXSetTevColorOp(0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    g::GXSetTevAlphaOp(0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
}

// TEV register 1 (GXColor, passed by address)
void tev_register(Rgba8 c) {
    GFRAME(0x10);
    u32 a = (u32)&GS(u32, 0x8);
    guest::store8(a, c.r), guest::store8(a + 1, c.g), guest::store8(a + 2, c.b), guest::store8(a + 3, c.a);
    g::GXSetTevColor(GX_TEVREG1, a);
}

Rgba8 mix(Rgb a, Rgb b, f32 t, f32 alpha) {
    return {clamp8(a.r + (b.r - a.r) * t), clamp8(a.g + (b.g - a.g) * t), clamp8(a.b + (b.b - a.b) * t),
            clamp8(255 * alpha)};
}

}  // namespace

struct Amp::Impl {
    // made in this order, freed in the reverse one (the heaps are stacks)
    Block cabinet, speaker, pilot, shadow, grille, glow;  // display lists
    Block cloth, blob;                                     // texture images
    u32 tex_objs = 0;  // two GXTexObj (0x20 bytes each): the cloth (repeated), the blob (clamped)
    bool ok = false;

    Impl();
    ~Impl();
};

Amp::Impl::Impl() {
    // the cabinet, its feet and handle, the speaker's cavity, the knobs: one opaque list
    Mesh body;
    RoundedBox cab({0, (CAB_Y0 + CAB_Y1) / 2, 0}, {CAB_HX, (CAB_Y1 - CAB_Y0) / 2, CAB_HZ}, CAB_R);
    cab.cut(0, -GRILLE_HX), cab.cut(0, GRILLE_HX);
    cab.cut(1, GRILLE_Y0), cab.cut(1, GRILLE_Y1), cab.cut(1, STRIP_Y0), cab.cut(1, STRIP_Y1);
    auto in_front = [](int a, int side, Vec3 p) {
        return a == 2 && side == 1 && p.x > -GRILLE_HX && p.x < GRILLE_HX;
    };
    // the front, the top and the sides (the bottom and the back are never seen)
    cab.emit(
        body, {{2, 1}, {1, 1}, {0, -1}, {0, 1}},
        [&](int a, int side, Vec3 p) {
            return in_front(a, side, p) && p.y > STRIP_Y0 && p.y < STRIP_Y1 ? STRIP : CABINET;
        },
        [&](int a, int side, Vec3 p) { return in_front(a, side, p) && p.y > GRILLE_Y0 && p.y < GRILLE_Y1; });
    // the cavity: its four walls from the opening back, and its back wall
    const f32 zf = CAB_HZ, zb = CAB_HZ - CAVITY_DEPTH;
    const f32 x0 = -GRILLE_HX, x1 = GRILLE_HX, y0 = GRILLE_Y0, y1 = GRILLE_Y1;
    auto wall = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n) {
        Rgba8 col = shade(CAVITY, n, 0);
        body.quad(vtx(a, col), vtx(b, col), vtx(c, col), vtx(d, col));
    };
    wall({x0, y1, zf}, {x1, y1, zf}, {x1, y1, zb}, {x0, y1, zb}, {0, -1, 0});
    wall({x0, y0, zb}, {x1, y0, zb}, {x1, y0, zf}, {x0, y0, zf}, {0, 1, 0});
    wall({x0, y0, zb}, {x0, y0, zf}, {x0, y1, zf}, {x0, y1, zb}, {1, 0, 0});
    wall({x1, y0, zf}, {x1, y0, zb}, {x1, y1, zb}, {x1, y1, zf}, {-1, 0, 0});
    Rgba8 back = flat({28, 28, 30});
    body.quad(vtx({x0, y0, zb}, back), vtx({x1, y0, zb}, back), vtx({x1, y1, zb}, back),
              vtx({x0, y1, zb}, back));
    // four little feet, a strap handle on top
    for (f32 fx : {-21.0f, 21.0f})
        for (f32 fz : {-9.0f, 9.0f})
            box(body, {fx - 3.5f, 0, fz - 3.5f}, {fx + 3.5f, CAB_Y0 + 0.5f, fz + 3.5f}, FOOT, false);
    for (f32 hx : {-13.0f, 13.0f})
        box(body, {hx - 1.5f, CAB_Y1 - 0.5f, -2}, {hx + 1.5f, CAB_Y1 + 3.5f, 2}, HANDLE, false);
    box(body, {-15.5f, CAB_Y1 + 3.5f, -2.4f}, {15.5f, CAB_Y1 + 6, 2.4f}, HANDLE);
    // the knobs, turned this way and that, and the pilot light's bezel
    const f32 KNOB_ANGLE[] = {2.2f, 1.2f, 0.4f};
    for (int k = 0; k < 3; k++)
        knob(body, KNOB_X[k], KNOB_Y, CAB_HZ, KNOB_ANGLE[k]);
    disc(body, {PILOT_X, KNOB_Y, CAB_HZ},
         {{PILOT_R + 1.1f, 0.0f, PILOT_BEZEL, 0.6f},
          {PILOT_R + 0.3f, 0.6f, PILOT_BEZEL, 0.2f},
          {PILOT_R, 0.5f, PILOT_BEZEL, -0.4f}},
         8, 0.4f);
    cabinet = make_list(body, false);

    // the speaker around its middle (its matrix puts it on the cavity's back wall): the surround's
    // roll, the cone (darker toward the middle, as it recedes), the dust cap's dome
    Mesh cone;
    disc(cone, {0, 0, 0},
         {{CONE_R, 0.0f, {44, 44, 46}, 0.0f},
          {CONE_R - 1.2f, 0.8f, {118, 118, 122}, 0.9f},
          {CONE_R - 2.4f, 0.3f, {132, 132, 136}, -0.3f},
          {4.2f, 0.6f, {78, 78, 82}, -0.5f},
          {0.0f, 1.8f, {196, 196, 200}, 0.0f}},
         12, 0.35f);
    speaker = make_list(cone, false);

    // the pilot light: a little white dome (TEV register 1 colours it)
    Mesh lamp;
    disc(lamp, {PILOT_X, KNOB_Y, CAB_HZ + 0.5f},
         {{PILOT_R, 0.0f, WHITE, 1.2f}, {PILOT_R * 0.6f, 0.9f, WHITE, 0.6f}, {0.0f, 1.3f, WHITE, 0.0f}}, 8,
         0.5f);
    pilot = make_list(lamp, false);

    // the shadow on the floor (wider than the cabinet), the cloth just inside the opening, the
    // glow around the light
    Rgba8 w = flat(WHITE);
    Mesh floor;
    const f32 SX = 44, SZ = 27, SY = 0.3f;
    floor.quad(vtx({-SX, SY, SZ}, w, 0, 1), vtx({SX, SY, SZ}, w, 1, 1), vtx({SX, SY, -SZ}, w, 1, 0),
               vtx({-SX, SY, -SZ}, w, 0, 0));
    shadow = make_list(floor, true);
    Mesh cloth_quad;
    const f32 zc = CAB_HZ - 0.6f;
    Rgba8 cc = shade(CLOTH, {0, 0, 1}, 0.0f);
    auto cv = [&](f32 x, f32 y) {
        return vtx({x, y, zc}, cc, (x + GRILLE_HX) / CLOTH_REPEAT, (GRILLE_Y1 - y) / CLOTH_REPEAT);
    };
    cloth_quad.quad(cv(x0, y0), cv(x1, y0), cv(x1, y1), cv(x0, y1));
    grille = make_list(cloth_quad, true);
    Mesh halo;
    const f32 G = 9.0f, GZ = CAB_HZ + 2.2f;
    halo.quad(vtx({PILOT_X - G, KNOB_Y - G, GZ}, w, 0, 1), vtx({PILOT_X + G, KNOB_Y - G, GZ}, w, 1, 1),
              vtx({PILOT_X + G, KNOB_Y + G, GZ}, w, 1, 0), vtx({PILOT_X - G, KNOB_Y + G, GZ}, w, 0, 0));
    glow = make_list(halo, true);

    // the textures and their GXTexObj
    cloth = to_guest(make_cloth());
    blob = to_guest(make_blob());
    tex_objs = g::operator_new(0x40);
    ok = cabinet.addr && speaker.addr && pilot.addr && shadow.addr && grille.addr && glow.addr &&
         cloth.addr && blob.addr && tex_objs;
    if (ok) {
        g::GXInitTexObj(tex_objs, cloth.addr, CLOTH_SIZE, CLOTH_SIZE, GX_TF_IA8, GX_REPEAT, GX_REPEAT, 0);
        g::GXInitTexObj(tex_objs + 0x20, blob.addr, BLOB_SIZE, BLOB_SIZE, GX_TF_I8, GX_CLAMP, GX_CLAMP, 0);
    }
    guest::log("soundplayer: amp: %u vertices in 6 display lists of %u bytes, textures %u bytes%s",
               (unsigned)(body.v.size() + cone.v.size() + lamp.v.size() + floor.v.size() +
                          cloth_quad.v.size() + halo.v.size()),
               (unsigned)(cabinet.size + speaker.size + pilot.size + shadow.size + grille.size + glow.size),
               (unsigned)(cloth.size + blob.size), ok ? "" : " (out of memory: not drawn)");
}

Amp::Impl::~Impl() {
    for (u32 p : {tex_objs, blob.addr, cloth.addr, glow.addr, grille.addr, shadow.addr, pilot.addr,
                  speaker.addr, cabinet.addr})
        if (p)
            g::operator_delete(p);
}

Amp::Amp() : impl_(new Impl) {}

Amp::~Amp() { delete impl_; }

void Amp::draw(u32 view, const f32 at[3], f32 yaw, f32 pulse) {
    const Impl& a = *impl_;
    if (!a.ok)
        return;
    pulse = std::max(0.0f, std::min(1.0f, pulse));
    Mtx34 model = concat(load<Mtx34>(view), place({at[0], at[1], at[2]}, yaw));

    // the state all the lists share: vertex colours unlit, one TEV stage, depth tested
    g::GXSetNumChans(1);
    g::GXSetChanCtrl(GX_COLOR0A0, 0, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    g::GXSetNumTevStages(1);
    g::GXSetNumIndStages(0);
    g::GXSetTevDirect(0);
    g::GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    g::GXSetTevSwapMode(0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    g::GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    g::GXSetZCompLoc(1);
    g::GXSetCullMode(GX_CULL_NONE);
    g::GXSetColorUpdate(1);

    // the cabinet: opaque, its colours as they are
    load_matrix(model);
    g::GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, 0, GX_LO_CLEAR);
    g::GXSetZMode(1, GX_LEQUAL, 1);
    vertex_format(false);
    tev(GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA,
        false);
    g::GXCallDisplayList(a.cabinet.addr, a.cabinet.size);

    // the speaker: swells and brightens on the beat (register 1 scales its colours)
    f32 swell = 1.0f + 0.10f * pulse;
    Vec3 cone_at = {0, CONE_Y, CAB_HZ - CAVITY_DEPTH + 0.05f};
    load_matrix(concat(model, place(cone_at, 0, {swell, swell, 1 + 2.0f * pulse})));
    tev_register(mix({205, 205, 205}, WHITE, pulse, 1));
    tev(GX_CC_ZERO, GX_CC_RASC, GX_CC_C1, GX_CC_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA, false);
    g::GXCallDisplayList(a.speaker.addr, a.speaker.size);

    // the pilot light: dim cyan, bright on the beat
    load_matrix(model);
    tev_register(mix({CYAN.r * 0.55f, CYAN.g * 0.55f, CYAN.b * 0.55f}, {150, 232, 255}, pulse, 1));
    g::GXCallDisplayList(a.pilot.addr, a.pilot.size);

    // the translucent parts: depth tested, not written
    g::GXSetZMode(1, GX_LEQUAL, 0);
    g::GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    vertex_format(true);

    // the shadow: black through the blob, at most a third dark
    g::GXLoadTexObj(a.tex_objs + 0x20, GX_TEXMAP0);
    tev_register({0, 0, 0, 92});
    tev(GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_A1, GX_CA_ZERO, true);
    g::GXCallDisplayList(a.shadow.addr, a.shadow.size);

    // the cloth over the speaker
    g::GXLoadTexObj(a.tex_objs, GX_TEXMAP0);
    tev(GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO, true);
    g::GXCallDisplayList(a.grille.addr, a.grille.size);

    // the light's glow, added: strong on the beat, faint between
    g::GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
    g::GXLoadTexObj(a.tex_objs + 0x20, GX_TEXMAP0);
    tev_register(mix(CYAN, CYAN, 0, 0.18f + 0.62f * pulse));
    tev(GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_A1, GX_CA_ZERO, true);
    g::GXCallDisplayList(a.glow.addr, a.glow.size);

    g::GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, 0, GX_LO_CLEAR);
    g::GXSetZMode(1, GX_LEQUAL, 1);
}

}  // namespace engine::soundplayer
