// NOMA_SELFTEST=waterfx: the World's water effects (engine/g3d/water_fx.h) through the host's software
// backend (engine/g3d/soft_backend), without running the game.
//
// 1. A package of the effects alone, made here (a uniform IA8 atlas, the stager's two materials -- the
//    texture's alpha times the vertices', and less them --, its four dynamic batches and their WAFX
//    record): a World fed splashes of every kind and a swimmer for 150 frames (some frames several
//    sixtieths long) builds both passes each frame. Checked: the effects' items are the translucent
//    pass's only, each batch at most once, each a range of the packet's vertices (16-byte aligned,
//    inside them, none overlapping another, its indices whole quads or patches of its batch), the stats
//    their count, all four batches drawn over the run, none over its capacity; the same inputs give the
//    same vertices (a World of its own); the frame drawn is the same with every byte of the packet's
//    vertices outside the ranges garbage (the backend reads only the ranges); a range's first indices
//    are all it draws (a quad past them isn't); the erode material's alpha is the picture's less the
//    vertex's (none at a vertex alpha of 255), the multiply material's their product.
// 2. The staged island (NOMA_G3D_PACKAGE, default wuhu/island.ng3d: run from a data folder), if it has
//    the effects: a splash on the open sea nearest view_beach's eye (its waves whole: away from the
//    shore; NOMA_WATER_AT=x,z another place), seen from the shore's side, at 0.3, 1.0 and 1.5 in turn
//    with a swimmer passing, both passes built through the World each frame and drawn: the effects
//    change the frame, the World's ranges hold what the effects make on their own (a WaterFx fed the
//    same, whose time a frame is logged), and pictures go to NOMA_G3D_OUT (default waterfx): the whole
//    frame and a crop about the splash at a few of its ages, s<strength x 10>_age<sixtieths>[_crop].png.
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "runtime.h"
#include "engine/g3d/material.h"
#include "engine/g3d/native_draw.h"
#include "engine/g3d/package.h"
#include "engine/g3d/package_writer.h"
#include "engine/g3d/soft_backend.h"
#include "engine/g3d/water_fx.h"
#include "engine/g3d/world.h"
#include "hw/gx/raster.h"
#include "hw/gx/regs.h"

bool write_png(const std::string& path, const u32* rgba, u32 w, u32 h);

namespace guest {

namespace {

namespace g3 = engine::g3d;
namespace fm = engine::g3d::fmt;
using engine::math::Vec3;

constexpr u32 W = 640, H = 456;
constexpr f32 NEAR = 1.5f, FAR = 80000.0f;
constexpr f32 MII_HEIGHT = 21.5f;  // the island's Mii (package units)
constexpr f32 MII_SCALE = MII_HEIGHT / 15.24f;

int s_failures, s_checks;
void expect(bool ok, const char* what, ...) __attribute__((format(printf, 2, 3)));
void expect(bool ok, const char* what, ...) {
    s_checks++;
    if (ok)
        return;
    s_failures++;
    char buf[400];
    va_list ap;
    va_start(ap, what);
    vsnprintf(buf, sizeof(buf), what, ap);
    va_end(ap);
    log("selftest waterfx: FAILED: %s", buf);
}

// ---------------------------------------------------------------- the frame (selftest_g3drender's)
void set_xf_f32(u32 addr, f32 v) {
    u32 b;
    memcpy(&b, &v, 4);
    g_gx.xf[addr] = b;
}

// GXSetProjection for the camera in guest units, a W x H viewport and scissor (GX's 342 bias)
void set_gx_frame(const g3::Camera& guest_cam) {
    const f32 cot = 1.0f / std::tan(guest_cam.fovy_deg * 0.5f * 3.14159265f / 180.0f);
    const f32 n = guest_cam.near_, f = guest_cam.far_;
    const f32 p[6] = {cot / guest_cam.aspect, 0, cot, 0, -n / (f - n), -(f * n) / (f - n)};
    for (int i = 0; i < 6; i++)
        set_xf_f32(XF_PROJECTION + i, p[i]);
    g_gx.xf[XF_PROJECTION + 6] = 0;
    const f32 v[6] = {W * 0.5f, -(H * 0.5f), 16777215.0f, W * 0.5f + 342, H * 0.5f + 342, 16777215.0f};
    for (int i = 0; i < 6; i++)
        set_xf_f32(XF_VIEWPORT + i, v[i]);
    g_gx.bp[BP_SCISSOR_TL] = 342u << 12 | 342u;
    g_gx.bp[BP_SCISSOR_BR] = (342u + W - 1) << 12 | (342u + H - 1);
    g_gx.bp[BP_SCISSOR_OFFSET] = 171u | 171u << 10;
    g_gx.bp[BP_ZCONTROL] = 0;
}

void clear_efb(u32 rgba) {
    const HostEfb e = gx_host_efb();
    std::fill(e.color, e.color + e.width * e.height, rgba | 0xFF000000u);
    std::fill(e.depth, e.depth + e.width * e.height, 0xFFFFFFu);
}

std::vector<u32> efb_frame() {
    const HostEfb e = gx_host_efb();
    std::vector<u32> out((size_t)W * H);
    for (u32 y = 0; y < H; y++)
        memcpy(&out[(size_t)y * W], e.color + (size_t)y * e.width, W * 4);
    return out;
}

g3::Camera camera(Vec3 eye, Vec3 target) {
    g3::Camera c;
    c.eye = eye, c.target = target;
    c.fovy_deg = 45, c.aspect = (f32)W / H, c.near_ = NEAR, c.far_ = FAR;
    return c;
}

// a point's pixel under the camera (false: behind it)
bool project(const g3::Camera& c, Vec3 p, f32* px, f32* py) {
    const engine::math::Mtx34 v = g3::look_at(c);
    f32 q[3];
    for (int r = 0; r < 3; r++)
        q[r] = v.m[r][0] * p.x + v.m[r][1] * p.y + v.m[r][2] * p.z + v.m[r][3];
    if (q[2] >= -1e-3f)
        return false;
    const f32 cot = 1.0f / std::tan(c.fovy_deg * 0.5f * 3.14159265f / 180.0f);
    *px = W * 0.5f * (1 + cot / c.aspect * q[0] / -q[2]);
    *py = H * 0.5f * (1 - cot * q[1] / -q[2]);
    return true;
}

// the effects' items left out (what the frame is without them)
g3::Packet without_fx(const g3::Packet& p) {
    g3::Packet q = p;
    q.items.erase(std::remove_if(q.items.begin(), q.items.end(), [](const g3::DrawItem& it) { return it.range != 0xFFFF; }),
                  q.items.end());
    return q;
}

u32 pixels_differ(const std::vector<u32>& a, const std::vector<u32>& b) {
    u32 n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); i++)
        n += a[i] != b[i];
    return n;
}

// The checks on a translucent packet's effect items (part 1's list); the quads and patches they hold,
// the batches drawn added to `seen` (bit k: WaterFxBatch k)
void check_items(const g3::Packet& p, const g3::Package& pkg, const char* when, u32* quads, u32* patches, u32* seen) {
    const fm::WaterFx& rec = *pkg.water_fx();
    *quads = *patches = 0;
    u32 used = 0;
    std::vector<std::pair<u32, u32>> spans;
    for (const g3::DrawItem& it : p.items) {
        if (it.range == 0xFFFF)
            continue;
        int k = -1;
        for (int i = 0; i < g3::WFX_BATCHES; i++)
            if (rec.batch[i] == it.batch)
                k = i;
        expect(k >= 0 && (it.flags & g3::DRAW_DYNAMIC), "%s: a ranged item is an effect batch's, dynamic (batch %u)",
               when, it.batch);
        if (k < 0)
            continue;
        expect(!(used >> k & 1), "%s: batch %d drawn once", when, k);
        used |= 1u << k, *seen |= 1u << k;
        expect(it.range < p.dynamic_ranges.size(), "%s: range %u of %zu", when, it.range, p.dynamic_ranges.size());
        if (it.range >= p.dynamic_ranges.size())
            continue;
        const g3::DynamicRange& r = p.dynamic_ranges[it.range];
        const fm::Batch& b = pkg.batches()[it.batch];
        const bool air = k < 2;
        const u32 per_i = air ? 6 : 24, per_v = air ? 4 : 9, cap = air ? rec.air_quads : rec.surf_patches;
        const u32 n = r.n_indices / per_i;
        expect(r.at % 16 == 0 && (size_t)r.at + r.bytes <= p.dynamic.size(),
               "%s: batch %d's range %u+%u inside the packet's %zu bytes, aligned", when, k, r.at, r.bytes, p.dynamic.size());
        expect(r.n_indices && r.n_indices % per_i == 0 && r.n_indices <= b.n_indices && n <= cap &&
                   r.bytes == n * per_v * 24,
               "%s: batch %d's range: %u indices, %u bytes (capacity %u)", when, k, r.n_indices, r.bytes, cap);
        expect(12 * ((size_t)it.matrix + 1) <= p.matrices.size(), "%s: batch %d's matrix", when, k);
        (air ? *quads : *patches) += n;
        spans.push_back({r.at, r.at + r.bytes});
    }
    std::sort(spans.begin(), spans.end());
    for (size_t i = 1; i < spans.size(); i++)
        expect(spans[i].first >= spans[i - 1].second, "%s: ranges overlap", when);
}

g3::Package* load_bytes(const std::vector<u8>& bytes, const char* what) {
    void* block = malloc(bytes.size());
    memcpy(block, bytes.data(), bytes.size());
    std::string error;
    g3::Package* p = g3::Package::load(block, (u32)bytes.size(), &error);
    if (!p)
        log("selftest waterfx: %s: %s", what, error.c_str());
    return p;
}

// ---------------------------------------------------------------- 1. the effects alone
constexpr u16 QUADS = 64, PATCHES = 16;
constexpr u8 TEX_A = 200, TEX_I = 200;  // the atlas: one value everywhere (alpha, intensity)
const u8 K0[3] = {0xCE, 0xD0, 0xD3};    // the materials' secondary colour (the stager's)

// The effects' package: one IA8 atlas of 8 x 8 texels all (TEX_A, TEX_I), the multiply and erode
// materials as tools/wuhu/stage_wuhu.py water_fx_material makes them, the four batches (quads: top left,
// top right, bottom left, bottom right, triangles a, a+1, a+2 and a+2, a+1, a+3; patches: 3 x 3 row by
// row, eight triangles about the middle), the WAFX record.
std::vector<u8> fx_package() {
    g3::StringTable strs;
    fm::Texture t = {};
    t.name = strs.add("waterfx_atlas");
    t.width = t.height = t.src_width = t.src_height = 8;
    t.format = fm::TF_LA8, t.levels = 1, t.flags = fm::TEX_GX_RAW | fm::TEX_ALPHA, t.gx_format = 3;
    t.data = 0, t.size = 8 * 8 * 2;
    std::vector<u8> tdat(t.size);
    for (u32 i = 0; i < t.size; i += 2)
        tdat[i] = TEX_A, tdat[i + 1] = TEX_I;  // (GX's IA8: alpha, then intensity)
    std::vector<fm::Material> mats;
    for (int erode = 0; erode < 2; erode++) {
        fm::Material m = {};
        m.name = strs.add(erode ? "waterfx_erode" : "waterfx_mul");
        m.flags = fm::MAT_XLU;
        m.n_stages = 1, m.n_texgens = 1, m.n_chans = 1, m.cull = 0;
        // colour: K0 + (RASC - K0) x TEXC; alpha: TEXA x RASA, or TEXA - RASA (konst alpha 1)
        m.color_env[0] = 15 | 8 << 4 | 10 << 8 | 14 << 12 | 1 << 19;
        m.alpha_env[0] = erode ? 4 << 4 | 6 << 7 | 5 << 10 | 7 << 13 | 1 << 18 | 1 << 19
                               : 7 << 4 | 4 << 7 | 5 << 10 | 7 << 13 | 1 << 19;
        m.tref[0] = 1 << 6;                    // map 0, coordinate 0, colour channel 0
        m.ksel[0] = 0 | 1 << 2 | 0x0C << 4;    // swap table 0: R, G; stage 0's konst colour K0, alpha 1
        m.ksel[1] = 2 | 3 << 2;                // ... B, A
        for (int c = 0; c < 3; c++)
            m.tev_konst[0][c] = K0[c];
        m.tev_konst[0][3] = 255;
        m.alpha_compare = 7 << 16 | 7 << 19;
        m.zmode = 1 | 3 << 1;                  // less or equal, not written
        m.blend = 1 | 1 << 3 | 5 << 5 | 4 << 8;  // source alpha, one less it
        m.chan_color = m.chan_alpha = 1;       // the vertex's, unlit
        memset(m.mat_color, 255, 4);
        for (auto& mp : m.maps)
            mp.texture = fm::NONE16;
        m.maps[0].texture = 0, m.maps[0].min_filter = m.maps[0].mag_filter = 1;
        for (auto& g : m.texgens)
            g.matrix = g.post_mtx = fm::NONE16;
        m.texgens[0].type = 1, m.texgens[0].source = 4;  // MTX2x4 from TEX0
        mats.push_back(m);
    }
    std::vector<fm::VertexBuffer> vbs;
    std::vector<fm::Batch> batches;
    std::vector<u16> idx;
    u32 vdat_bytes = 0;
    static const u16 k_quad[6] = {0, 1, 2, 2, 1, 3};
    for (int k = 0; k < g3::WFX_BATCHES; k++) {
        const bool air = k < 2;
        const u32 n = air ? QUADS : PATCHES, per = air ? 4 : 9;
        fm::VertexBuffer vb = {};
        vb.data = vdat_bytes, vb.count = n * per, vb.attrs = fm::VA_POS | fm::VA_CLR | fm::VA_UV0, vb.stride = 24;
        vb.flags = fm::VB_UV_FLOAT;
        vdat_bytes += (vb.count * 24 + 15) & ~15u;
        fm::Batch b = {};
        b.material = (u16)(k & 1), b.vbuf = (u16)k, b.first_index = (u32)idx.size();
        for (u32 q = 0; q < n; q++) {
            if (air) {
                for (u16 i : k_quad)
                    idx.push_back((u16)(4 * q + i));
                continue;
            }
            for (int row = 0; row < 2; row++)
                for (int col = 0; col < 2; col++) {
                    const u16 tl = (u16)(9 * q + 3 * row + col), tr = (u16)(tl + 1), bl = (u16)(tl + 3), br = (u16)(tl + 4);
                    const u16 tri[6] = {tl, tr, br, tl, br, bl}, alt[6] = {tl, tr, bl, bl, tr, br};
                    const u16* s = (row + col) % 2 == 0 ? tri : alt;
                    idx.insert(idx.end(), s, s + 6);
                }
        }
        b.n_indices = (u32)idx.size() - b.first_index;
        for (int i = 0; i < 3; i++)
            b.bbox[i] = -64, b.bbox[3 + i] = 64;
        b.node = fm::NONE16, b.flags = fm::BATCH_DYNAMIC;
        vbs.push_back(vb);
        batches.push_back(b);
    }
    std::vector<u8> vdat(vdat_bytes, 0);
    fm::WaterFx rec = {};
    rec.version = fm::WATER_FX_VERSION, rec.texture = 0;
    for (int k = 0; k < 4; k++)
        rec.batch[k] = (u16)k;
    rec.air_quads = QUADS, rec.surf_patches = PATCHES, rec.n_cells = fm::WATER_FX_CELLS;
    for (int c = 0; c < fm::WATER_FX_CELLS; c++) {
        const f32 u = (f32)(c % 4) / 4, v = (f32)(c / 4) / 4;
        rec.cell[c][0] = u, rec.cell[c][1] = v, rec.cell[c][2] = u + 0.25f, rec.cell[c][3] = v + 0.25f;
    }
    g3::PackageWriter w;
    w.section(fm::SEC_STRS, strs.bytes().data(), (u32)strs.bytes().size(), 0);
    w.array(fm::SEC_TEXS, std::vector<fm::Texture>{t});
    w.relocate(fm::SEC_TEXS, offsetof(fm::Texture, data), fm::SEC_TDAT);
    w.array(fm::SEC_MATS, mats);
    w.array(fm::SEC_TMTX, std::vector<fm::TexMtx>());
    w.array(fm::SEC_VTXB, vbs);
    for (size_t i = 0; i < vbs.size(); i++)
        w.relocate(fm::SEC_VTXB, (u32)(i * sizeof(fm::VertexBuffer) + offsetof(fm::VertexBuffer, data)), fm::SEC_VDAT);
    w.array(fm::SEC_IDAT, idx);
    w.array(fm::SEC_BTCH, batches);
    w.section(fm::SEC_VDAT, vdat.data(), (u32)vdat.size(), 0);
    w.section(fm::SEC_TDAT, tdat.data(), (u32)tdat.size(), 0, 128);
    w.section(fm::SEC_WAFX, &rec, sizeof(rec), 1);
    return w.finish();
}

// part 1's inputs at a frame: splashes of every kind, a swimmer crossing
void feed(g3::World& w, u32 f) {
    if (f == 0)
        w.water_splash({{0, 0, 0}, 1.5f, 0, 1, 40, MII_SCALE, g3::WaterSplash::ENTRY, false});
    if (f == 40)
        w.water_splash({{-6, 0, 4}, 0, 0, 0, 0, MII_SCALE, g3::WaterSplash::HAND, false});
    if (f == 55)
        w.water_splash({{18, 0, -6}, 0, 0, 0, 0, MII_SCALE, g3::WaterSplash::RING, false});
    if (f == 70)
        w.water_splash({{-12, 0, -10}, 0.35f, 1, 0, 10, MII_SCALE, g3::WaterSplash::DROP_IN, false});
    if (f == 90)
        w.water_splash({{8, 0, 8}, 0.3f, 0, 0, 0, MII_SCALE, g3::WaterSplash::ENTRY, false});
    // swimming across (+x) until 110, then treading, then out of the water from 130
    const f32 x = -30 + 0.5f * (f32)std::min<u32>(f, 110);
    w.set_swimmer({f < 130, {x, -0.62f * MII_HEIGHT, 14}, 90, f < 110 ? 30.0f : 0.0f, MII_SCALE, false});
}

// A packet of quads of an effect batch (`written` of them, the first `drawn` in the item's range: the
// frame's middle, the next each to the last's right; `rgba` their vertices), from a World's
// translucent packet (its package, materials and view)
g3::Packet quads(const g3::Packet& from, u16 batch, const u8 rgba[4], u32 written = 1, u32 drawn = 1) {
    g3::Packet p = from;
    p.items.clear();
    p.dynamic.assign(64, 0xEE);  // (bytes before the range: never read)
    p.dynamic_ranges.clear();
    const u32 at = 64;
    p.dynamic.resize(at + written * 4 * 24);
    for (u32 q = 0; q < written; q++) {
        const f32 x0 = -4 + 10.0f * (f32)q, corners[4][3] = {{x0, 8, 0}, {x0 + 8, 8, 0}, {x0, 0, 0}, {x0 + 8, 0, 0}};
        for (u32 v = 0; v < 4; v++) {
            u8* o = p.dynamic.data() + at + (q * 4 + v) * 24;
            memcpy(o, corners[v], 12);
            memcpy(o + 12, rgba, 4);
            const f32 uv[2] = {(f32)(v & 1), (f32)(v >> 1)};
            memcpy(o + 16, uv, 8);
        }
    }
    p.dynamic_ranges.push_back({at, written * 4 * 24, drawn * 6});
    g3::DrawItem it = {batch, 0, 0xFFFF, g3::DRAW_DYNAMIC};
    it.range = 0;
    p.items.push_back(it);
    return p;
}

void test_alone() {
    std::unique_ptr<g3::Package> pkg(load_bytes(fx_package(), "the effects' package"));
    expect(pkg != nullptr && pkg->water_fx(), "the effects' package loads with its WAFX");
    if (!pkg || !pkg->water_fx())
        return;
    const fm::WaterFx& rec = *pkg->water_fx();
    g3::World w(*pkg), twin(*pkg);
    expect(w.water_fx(), "a World of a package with WAFX has the effects");
    const g3::Camera cam = camera({0, 14, 70}, {0, 6, 0});
    const f32 k = w.world_scale();
    g3::SoftBackend sb;
    sb.prepare(*pkg);
    set_gx_frame(g3::scaled(cam, k));
    const g3::GxView view = g3::native_draw_view();
    u32 frame = 0, seen = 0, most_q = 0, most_p = 0, drawn = 0, changed_most = 0;
    bool same_twin = true, garbage_same = true, opaque_clean = true;
    for (u32 f = 0; f < 150; f++) {
        // (some frames two or three sixtieths: the steps catch up)
        frame += f % 7 == 3 ? 3 : f % 5 == 1 ? 2 : 1;
        for (g3::World* x : {&w, &twin}) {
            feed(*x, f);
            x->update((f32)frame);
            x->set_camera(cam);
        }
        g3::Packet opa, xlu, opa2, xlu2;
        w.build_packet(g3::Pass::OPAQUE, &opa);
        w.build_packet(g3::Pass::TRANSLUCENT, &xlu);
        twin.build_packet(g3::Pass::OPAQUE, &opa2);
        twin.build_packet(g3::Pass::TRANSLUCENT, &xlu2);
        same_twin = same_twin && xlu.dynamic == xlu2.dynamic && xlu.items.size() == xlu2.items.size();
        for (const g3::DrawItem& it : opa.items)
            opaque_clean = opaque_clean && it.range == 0xFFFF;
        char when[32];
        snprintf(when, sizeof(when), "frame %u", f);
        u32 q = 0, pt = 0;
        check_items(xlu, *pkg, when, &q, &pt, &seen);
        const g3::WorldStats& st = w.stats();
        expect(st.water_quads == q && st.water_patches == pt, "%s: the stats say %u quads and %u patches (%u, %u)",
               when, st.water_quads, st.water_patches, q, pt);
        most_q = std::max(most_q, q), most_p = std::max(most_p, pt);
        // every fifth frame drawn: as built, then with every byte of the vertices outside the ranges
        // garbage (and a capacity's worth more after them)
        if (f % 5 || xlu.items.empty())
            continue;
        drawn++;
        clear_efb(0x202020);
        sb.draw(xlu, view);
        const std::vector<u32> a = efb_frame();
        changed_most = std::max(changed_most, pixels_differ(a, std::vector<u32>(a.size(), 0xFF202020u)));
        g3::Packet junk = xlu;
        std::vector<u8> keep(junk.dynamic.size(), 0);
        for (const g3::DynamicRange& r : junk.dynamic_ranges)
            memset(keep.data() + r.at, 1, r.bytes);
        for (size_t i = 0; i < junk.dynamic.size(); i++)
            if (!keep[i])
                junk.dynamic[i] = (u8)(0x4E + i);
        junk.dynamic.insert(junk.dynamic.end(), 4 * 24 * QUADS, 0x4E);
        clear_efb(0x202020);
        sb.draw(junk, view);
        garbage_same = garbage_same && efb_frame() == a;
    }
    log("selftest waterfx: the effects alone: 150 frames (%u sixtieths), batches drawn %X, at most %u quads and %u "
        "patches (capacities %u, %u), %u frames drawn, at most %u pixels changed",
        frame, seen, most_q, most_p, rec.air_quads, rec.surf_patches, drawn, changed_most);
    expect(seen == 15, "all four batches drawn over the run (%X)", seen);
    expect(most_q > 32 && most_p > 2, "the crowns and the rings drawn (%u quads, %u patches)", most_q, most_p);
    expect(opaque_clean, "the opaque pass has no effect items");
    expect(same_twin, "the same inputs make the same vertices");
    expect(garbage_same, "the frame is the same with the vertices outside the ranges garbage");
    expect(changed_most > 2000, "the effects are seen (%u pixels at most)", changed_most);

    // quads straight on: the pixel in the middle of the first, over black
    const g3::Camera flat = camera({0, 4, 30}, {0, 4, 0});
    w.set_camera(flat);
    g3::Packet base;
    w.build_packet(g3::Pass::TRANSLUCENT, &base);
    set_gx_frame(g3::scaled(flat, k));
    const g3::GxView fv = g3::native_draw_view();
    f32 mx = 0, my = 0;
    project(flat, {0, 4, 0}, &mx, &my);
    auto middle = [&](const g3::Packet& p) {
        clear_efb(0);
        sb.draw(p, fv);
        const HostEfb e = gx_host_efb();
        return (int)(e.color[(size_t)my * e.width + (size_t)mx] & 255);
    };
    // what the materials give over black: colour K0 + (white - K0) x TEXC (red), alpha by the material
    const f32 tc = TEX_I / 255.0f;
    auto want = [&](f32 alpha) { return (int)std::lround((K0[0] + (255 - K0[0]) * tc) * alpha); };
    struct Case {
        bool erode;
        u8 alpha;
        f32 expect_alpha;
    };
    const Case cases[] = {
        {true, 255, 0},                                // all eaten away
        {true, 0, TEX_A / 255.0f},                     // the picture's alpha
        {true, 100, (TEX_A - 100) / 255.0f},           // less the vertex's
        {true, 230, 0},                                // (more than it has: none)
        {false, 255, TEX_A / 255.0f},                  // the picture's
        {false, 128, TEX_A / 255.0f * 128 / 255.0f},   // times the vertex's
    };
    for (const Case& c : cases) {
        const u8 rgba[4] = {255, 255, 255, c.alpha};
        const int got = middle(quads(base, rec.batch[c.erode ? g3::WFX_AIR_ERODE : g3::WFX_AIR_MUL], rgba));
        const int exp = want(c.expect_alpha);
        log("selftest waterfx: %s material, vertex alpha %u: red %d (expected %d)", c.erode ? "erode" : "multiply",
            c.alpha, got, exp);
        expect(std::abs(got - exp) <= 3, "%s material at vertex alpha %u: %d, not %d", c.erode ? "erode" : "multiply",
               c.alpha, got, exp);
    }
    // a range's indices: two quads written, one drawn -- the second (to its right) isn't
    const u8 white[4] = {255, 255, 255, 255};
    clear_efb(0);
    sb.reset_stats();
    sb.draw(quads(base, rec.batch[g3::WFX_AIR_MUL], white, 2, 1), fv);
    const std::vector<u32> one = efb_frame();
    const u32 tris_one = sb.stats().triangles;
    clear_efb(0);
    sb.reset_stats();
    sb.draw(quads(base, rec.batch[g3::WFX_AIR_MUL], white, 2, 2), fv);
    const u32 tris_two = sb.stats().triangles, more = pixels_differ(one, efb_frame());
    log("selftest waterfx: a range of two quads with one quad's indices: %u triangles drawn (with both's: %u, %u "
        "pixels more)",
        tris_one, tris_two, more);
    expect(tris_one == 2 && tris_two == 4 && more > 100, "a range draws its first indices only (%u, %u triangles)",
           tris_one, tris_two);
    sb.release(*pkg);
}

// ---------------------------------------------------------------- 2. the staged island
void test_island() {
    const char* path_env = getenv("NOMA_G3D_PACKAGE");
    const std::string path = path_env ? path_env : "wuhu/island.ng3d";
    const std::string out = getenv("NOMA_G3D_OUT") ? getenv("NOMA_G3D_OUT") : "waterfx";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        log("selftest waterfx: no package at %s (NOMA_G3D_PACKAGE): the island's case is skipped", path.c_str());
        return;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<u8> bytes((size_t)n);
    const bool read_ok = fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    std::unique_ptr<g3::Package> pkg(read_ok ? load_bytes(bytes, path.c_str()) : nullptr);
    expect(pkg != nullptr, "%s loads", path.c_str());
    if (!pkg)
        return;
    if (!pkg->water_fx()) {
        log("selftest waterfx: %s was staged before the water effects (no WAFX): the island's case is skipped",
            path.c_str());
        return;
    }
    mkdir(out.c_str(), 0755);
    g3::World w(*pkg);
    const f32 k = w.world_scale();
    const g3::Sea& sea = w.sea();
    // the splash: the open sea nearest view_beach's eye (its waves whole: away from the shore, a
    // lattice of 25 units searched out to 4,000), at the sea's level, seen from the shore's side
    const fm::Marker* m = pkg->find_marker("view_beach");
    const Vec3 beach = m ? Vec3{m->position[0], m->position[1], m->position[2]} : Vec3{6300, 115, -200};
    const f32 level = sea.valid() ? sea.params().level : 0.0f;
    Vec3 at = {beach.x, level, beach.z};
    f32 best = 1e30f;
    for (f32 dz = -4000; dz <= 4000 && sea.valid(); dz += 25)
        for (f32 dx = -4000; dx <= 4000; dx += 25) {
            const f32 d = dx * dx + dz * dz;
            if (d < best && sea.amplitude(beach.x + dx, beach.z + dz) > 0.99f)
                best = d, at = {beach.x + dx, level, beach.z + dz};
        }
    if (const char* e = getenv("NOMA_WATER_AT"))
        sscanf(e, "%f,%f", &at.x, &at.z);
    Vec3 dir = at - beach;
    dir.y = 0;
    dir = dir.x == 0 && dir.z == 0 ? Vec3{0, 0, 1} : engine::math::normalize(dir);
    // a follow camera's view of it: 70 units back toward the shore, 22 up, looking out to sea
    const g3::Camera cam = camera(at - dir * 70 + Vec3{0, 22, 0}, at + Vec3{0, 5, 0});
    const Vec3 side = {dir.z, 0, -dir.x};
    log("selftest waterfx: the island: the splash at (%.1f, %.1f, %.1f) (sea level %.1f, waves there %.2f), seen "
        "from (%.1f, %.1f, %.1f)",
        at.x, at.y, at.z, level, sea.valid() ? sea.amplitude(at.x, at.z) : 0.0f, cam.eye.x, cam.eye.y, cam.eye.z);
    w.set_shadow_map(512);
    w.set_shadow_focus(at);
    g3::SoftBackend sb;
    sb.prepare(*pkg);
    set_gx_frame(g3::scaled(cam, k));
    const g3::GxView view = g3::native_draw_view();
    u32 clear = 0x00E6B380u;
    for (const fm::Fog& fg : pkg->fogs())
        if (fg.type != fm::FOG_NONE) {
            clear = fg.rgba[0] | fg.rgba[1] << 8 | fg.rgba[2] << 16;
            break;
        }
    f32 px = W / 2.0f, py = H / 2.0f;
    project(cam, at, &px, &py);
    // (the effects on their own beside the World's, fed the same: their time, and their vertices the
    // World's ranges' -- the view as World::Impl::fx_view makes it)
    g3::WaterFx solo(*pkg);
    g3::WaterFxView sv = {};
    {
        const Vec3 f = engine::math::normalize(cam.target - cam.eye);
        sv.right = engine::math::normalize(engine::math::cross(f, cam.up));
        sv.eye = cam.eye, sv.up = engine::math::cross(sv.right, f);
        sv.tan_half_fovy = std::tan(cam.fovy_deg * 0.5f * 3.14159265f / 180.0f);
        sv.sea = sea.valid() ? &sea : nullptr;
        const f32 snap = sea.valid() ? sea.params().grid_snap : 1.0f;
        sv.grid_c[0] = std::round(cam.eye.x / snap) * snap, sv.grid_c[1] = std::round(cam.eye.z / snap) * snap;
    }
    double solo_us = 0, solo_most_us = 0;
    u32 solo_frames = 0;
    bool solo_same = true;
    static const f32 k_strength[3] = {0.3f, 1.0f, 1.5f};
    static const u32 k_ages[] = {3, 8, 14, 22, 34, 50, 80};
    u32 frame = 600, seen = 0, most_changed = 0;
    for (f32 strength : k_strength) {
        for (u32 age = 0; age <= 80; age++) {
            frame++;
            const g3::WaterSplash splash = {at, strength, dir.x, dir.z, 20, MII_SCALE, g3::WaterSplash::ENTRY, true};
            // a swimmer crossing in front, left to right, its feet SWIM_SINK of its height under the waves
            const f32 across = -40 + 0.5f * (f32)age;
            Vec3 feet = at + side * across - dir * 18;
            feet.y = w.sea_height(feet.x, feet.z, frame / 60.0) - 0.62f * MII_HEIGHT;
            const g3::Swimmer swimmer = {true, feet, std::atan2(side.x, side.z) * 180 / 3.14159265f, 30, MII_SCALE, true};
            if (age == 0)
                w.water_splash(splash), solo.splash(splash);
            w.set_swimmer(swimmer), solo.set_swimmer(swimmer);
            w.update((f32)frame);
            w.set_camera(cam);
            g3::Packet opa, xlu;
            w.build_packet(g3::Pass::OPAQUE, &opa);
            w.build_packet(g3::Pass::TRANSLUCENT, &xlu);
            sv.grid_on = w.stats().sea_grid != 0;
            sv.seconds = frame / 60.0;
            g3::Packet own;
            g3::WaterFxDraw d[4];
            const auto t0 = std::chrono::steady_clock::now();
            solo.step((f32)frame);
            u32 nd = solo.build(&own, true, sv, d);
            nd += solo.build(&own, false, sv, d + nd);
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            solo_us += us, solo_most_us = std::max(solo_most_us, us), solo_frames++;
            u32 k = 0;
            for (const g3::DrawItem& it : xlu.items) {
                if (it.range == 0xFFFF)
                    continue;
                const g3::DynamicRange& r = xlu.dynamic_ranges[it.range];
                solo_same = solo_same && k < nd && d[k].batch == it.batch && d[k].range.bytes == r.bytes &&
                            !memcmp(xlu.dynamic.data() + r.at, own.dynamic.data() + d[k].range.at, r.bytes);
                k++;
            }
            solo_same = solo_same && k == nd;
            char when[48];
            snprintf(when, sizeof(when), "the island, strength %.1f, age %u", (double)strength, age);
            u32 q = 0, pt = 0;
            check_items(xlu, *pkg, when, &q, &pt, &seen);
            if (std::find(std::begin(k_ages), std::end(k_ages), age) == std::end(k_ages))
                continue;
            clear_efb(clear);
            sb.draw(opa, view);
            sb.draw(without_fx(xlu), view);
            const std::vector<u32> bare = efb_frame();
            clear_efb(clear);
            sb.draw(opa, view);
            sb.draw(xlu, view);
            const std::vector<u32> img = efb_frame();
            const u32 changed = pixels_differ(img, bare);
            most_changed = std::max(most_changed, changed);
            log("selftest waterfx: %s: %u quads, %u patches, %u pixels changed", when, q, pt, changed);
            char file[32];
            snprintf(file, sizeof(file), "/s%02d_age%02u", (int)std::lround(strength * 10), age);
            const std::string name = out + file;
            write_png(name + ".png", img.data(), W, H);
            // (a crop about the splash, 240 x 180)
            const u32 cw = 240, ch = 180;
            const u32 x0 = (u32)std::max(0.0f, std::min(px - cw / 2.0f, (f32)(W - cw)));
            const u32 y0 = (u32)std::max(0.0f, std::min(py - ch * 0.6f, (f32)(H - ch)));
            std::vector<u32> crop((size_t)cw * ch);
            for (u32 y = 0; y < ch; y++)
                memcpy(&crop[(size_t)y * cw], &img[(size_t)(y0 + y) * W + x0], cw * 4);
            write_png(name + "_crop.png", crop.data(), cw, ch);
        }
    }
    log("selftest waterfx: the island: batches drawn %X, at most %u pixels changed; pictures in %s", seen, most_changed,
        out.c_str());
    log("selftest waterfx: the island: the effects on their own (a step and both builds) %.1f us a frame, at most %.1f "
        "(the host's); their vertices %s the World's ranges'",
        solo_us / std::max<u32>(solo_frames, 1), solo_most_us, solo_same ? "are" : "are NOT");
    expect(solo_same, "the island: the World's ranges hold the effects' vertices");
    expect(most_changed > 1000, "the island: the effects change the frame (%u pixels at most)", most_changed);
    expect((seen & 7) == 7, "the island: the air's batches and the water's multiply batch drawn (%X)", seen);
    sb.release(*pkg);
}

}  // namespace

void selftest_water_fx() {
    s_failures = s_checks = 0;
    test_alone();
    test_island();
    log("selftest waterfx: %d checks, %d failed: %s", s_checks, s_failures, s_failures ? "MISMATCH" : "ok");
}

}  // namespace guest
