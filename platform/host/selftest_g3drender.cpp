// NOMA_SELFTEST=g3drender: the host's software backend for native draws (engine/g3d/soft_backend),
// without running the game.
//
// 1. Depth parity with GX: random triangles at view distances from 2 to 30,000 package units, drawn
//    once as a GX draw through raster.cpp (position matrix = the packet's view x S(k)) and once as a
//    native draw, under the same GX projection and viewport: the two depth buffers must agree (what
//    lets the island and the Mii share the frame's depth).
// 2. A real package (NOMA_G3D_PACKAGE, default wuhu/island.ng3d) drawn from each of its view_*
//    camera markers: the World's packets (both passes) through the backend into the host EFB, under
//    the GX state a frame of the game would have (GXSetProjection from the camera scaled by k, a
//    640x456 viewport and scissor, the 342 bias) -- PNGs to NOMA_G3D_OUT (default g3drender), the
//    time each view takes (with the backend's threads, and with one), and, with NOMA_G3D_REF set to
//    a directory of tools/g3d/preview_views.py's renders of the same package (<view>.ppm), the
//    difference against them: PSNR, the share of pixels off by more than 16 in a channel, a
//    difference picture. The first view is also drawn the way the game will draw it: World::emit's
//    commands through the FIFO, decoded, executed by a renderer like the host's (and retired
//    undrawn in a frame it doesn't draw).
// 3. Overlays (material.h is_overlay) against the terrain: each view's translucent pass drawn again
//    with pulls toward the eye (native_draw.h overlay_pull) from none to 1/256 of the distance, with
//    and without a limit in units; the pixels the overlays change, by the view distance under them,
//    show where strips would fight the terrain and where a pull reaches through what is in front of
//    them (an unlimited one's gain is written as a picture).
//
// The camera: NOMA_G3D_FOV (degrees, default 45), near 1.5 and far 80,000 package units, the
// world's animations at NOMA_G3D_FRAME (default 600); NOMA_G3D_VIEWS=a,b picks views.
#include <sys/stat.h>

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "runtime.h"
#include "engine/g3d/material.h"
#include "engine/g3d/native_draw.h"
#include "engine/g3d/package.h"
#include "engine/g3d/package_writer.h"
#include "engine/g3d/soft_backend.h"
#include "engine/g3d/world.h"
#include "hw/gx/raster.h"
#include "hw/gx/regs.h"

bool write_png(const std::string& path, const u32* rgba, u32 w, u32 h);

namespace guest {

extern void (*g_gather_capture)(const u8* burst);

namespace {

namespace g3 = engine::g3d;
namespace fm = engine::g3d::fmt;
using engine::math::Vec3;

constexpr u32 W = 640, H = 456;
constexpr f32 NEAR = 1.5f, FAR = 80000.0f;

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
    log("selftest g3drender: FAILED: %s", buf);
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void set_xf_f32(u32 addr, f32 v) {
    u32 b;
    memcpy(&b, &v, 4);
    g_gx.xf[addr] = b;
}

// The GX state of a frame: GXSetProjection(C_MTXPerspective(fovy, aspect, near, far)) for the camera
// in guest units, GXSetViewport(0, 0, W, H, 0, 1), GXSetScissor(0, 0, W, H) with the offset GX's
// 342-pixel bias makes, an RGB8 EFB
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

bool read_ppm(const std::string& path, std::vector<u32>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    int w = 0, h = 0, max = 0;
    const bool ok = fscanf(f, "P6 %d %d %d", &w, &h, &max) == 3 && w == (int)W && h == (int)H && max == 255 &&
                    fgetc(f) != EOF;
    std::vector<u8> rgb((size_t)W * H * 3);
    const bool data = ok && fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
    fclose(f);
    if (!data)
        return false;
    out->resize((size_t)W * H);
    for (size_t i = 0; i < out->size(); i++)
        (*out)[i] = rgb[3 * i] | rgb[3 * i + 1] << 8 | rgb[3 * i + 2] << 16 | 0xFF000000u;
    return true;
}

g3::Package* load_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return nullptr;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* block = malloc((size_t)n);
    const bool ok = fread(block, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        free(block);
        return nullptr;
    }
    std::string error;
    g3::Package* p = g3::Package::load(block, (u32)n, &error);
    if (!p)
        log("selftest g3drender: %s: %s", path.c_str(), error.c_str());
    return p;
}

// the view distance (package units) a window depth stands for, under the frame's projection
f64 view_distance(u32 z, f32 k) {
    const f64 n = NEAR * k, f = FAR * k, zn = z / 16777215.0;
    return (f * n / (f - zn * (f - n)) / k);
}

// ---------------------------------------------------------------- 1. depth parity with GX
struct ParityVertex {
    f32 pos[3];
    u8 clr[4];
};
static_assert(sizeof(ParityVertex) == 16, "ParityVertex");

// a package of one batch of triangles in one vertex buffer, one material: the vertex colour, depth
// test LEQUAL with update, no culling
std::vector<u8> parity_package(const std::vector<ParityVertex>& v) {
    g3::StringTable strs;
    fm::Material m = {};
    m.name = strs.add("mt_parity");
    m.n_stages = 1, m.n_chans = 1, m.cull = 0;
    m.color_env[0] = 15 | 15 << 4 | 15 << 8 | 10 << 12 | 1 << 19;  // a = RASC: lerp(ras, 0, 0) = ras
    m.alpha_env[0] = 7 << 4 | 7 << 7 | 7 << 10 | 5 << 13 | 1 << 19;
    m.tref[0] = 0 << 7;  // colour channel 0, no texture
    m.alpha_compare = 7 << 16 | 7 << 19;
    m.zmode = 1 | 3 << 1 | 0x10;
    m.blend = 0x08;
    m.chan_color = 1, m.chan_alpha = 1;
    memset(m.mat_color, 255, 4);
    for (auto& mp : m.maps)
        mp.texture = fm::NONE16;
    for (auto& g : m.texgens)
        g.matrix = g.post_mtx = fm::NONE16;
    std::vector<fm::Material> mats = {m};
    fm::VertexBuffer vb = {};
    vb.count = (u32)v.size(), vb.attrs = fm::VA_POS | fm::VA_CLR, vb.stride = sizeof(ParityVertex);
    std::vector<fm::VertexBuffer> vbs = {vb};
    std::vector<u16> idx(v.size());
    for (size_t i = 0; i < v.size(); i++)
        idx[i] = (u16)i;
    fm::Batch b = {};
    b.material = 0, b.vbuf = 0, b.first_index = 0, b.n_indices = (u32)v.size();
    b.node = fm::NONE16;
    std::vector<fm::Batch> batches = {b};
    g3::PackageWriter w;
    w.section(fm::SEC_STRS, strs.bytes().data(), (u32)strs.bytes().size(), 0);
    w.array(fm::SEC_TEXS, std::vector<fm::Texture>());
    w.array(fm::SEC_MATS, mats);
    w.array(fm::SEC_TMTX, std::vector<fm::TexMtx>());
    w.array(fm::SEC_VTXB, vbs);
    w.relocate(fm::SEC_VTXB, offsetof(fm::VertexBuffer, data), fm::SEC_VDAT);
    w.array(fm::SEC_IDAT, idx);
    w.array(fm::SEC_BTCH, batches);
    w.section(fm::SEC_VDAT, v.data(), (u32)(v.size() * sizeof(ParityVertex)), 0);
    w.section(fm::SEC_TDAT, nullptr, 0, 0, 128);
    return w.finish();
}

// a GX draw of triangles (positions f32, direct) with position matrix 0 = mv and the projection and
// viewport as set: depth LEQUAL with update, no culling, one TEV stage (black)
void gx_draw_triangles(const f32 mv[3][4], const std::vector<ParityVertex>& v) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            set_xf_f32((u32)(r * 4 + c), mv[r][c]);
    g_gx.xf[XF_MATINDEX_A] = 0, g_gx.xf[XF_MATINDEX_B] = 0;
    g_gx.xf[XF_NUMCOLORS] = 0, g_gx.xf[XF_NUMTEXGENS] = 0;
    gx_xf_written(g_gx, 0, 12);
    g_gx.cp[CP_VCD_LO] = 1u << 9, g_gx.cp[CP_VCD_HI] = 0;
    g_gx.cp[CP_VAT_A] = 1u | 4u << 1;  // position: xyz, f32
    g_gx.cp[CP_VAT_B] = g_gx.cp[CP_VAT_C] = 0;
    g_gx.bp[BP_GENMODE] = 0;
    g_gx.bp[BP_TREF] = 7u << 7;
    g_gx.bp[BP_TEV_COLOR_ENV] = 0xFFFFu | 1u << 19;
    g_gx.bp[BP_TEV_COLOR_ENV + 1] = 7u << 4 | 7u << 7 | 7u << 10 | 7u << 13 | 1u << 19;
    g_gx.bp[BP_ALPHACOMPARE] = 7u << 16 | 7u << 19;
    g_gx.bp[BP_ZMODE] = 1u | 3u << 1 | 0x10u;
    g_gx.bp[BP_CMODE0] = 0x08;
    g_gx.bp[BP_ZCONTROL] = 0;
    std::vector<u8> data(v.size() * 12);
    for (size_t i = 0; i < v.size(); i++)
        for (int k = 0; k < 3; k++) {
            u32 b;
            memcpy(&b, &v[i].pos[k], 4);
            for (int j = 0; j < 4; j++)
                data[i * 12 + k * 4 + j] = (u8)(b >> (24 - 8 * j));
        }
    SoftRenderer r;
    r.draw(0x90, 0, (u32)v.size(), data.data());
}

void test_parity() {
    const f32 k = 9.2f;  // (the island's scale: a package unit is 9.2 of the guest's)
    g3::Camera cam;
    cam.eye = {100, 50, 200}, cam.target = {130, 40, -300}, cam.fovy_deg = 45, cam.aspect = (f32)W / H;
    cam.near_ = NEAR, cam.far_ = FAR;
    const g3::Camera gc = g3::scaled(cam, k);
    const engine::math::Mtx34 view = g3::look_at(gc);
    // triangles in front of the camera, in package units, from 2 to 30,000 away (log-uniform), each
    // 0.3 to 30% of its distance across
    u32 rng = 99;
    auto rnd = [&] { return (rng = rng * 1103515245u + 12345u) >> 8 & 0xFFFF; };
    auto unit = [&] { return (f32)rnd() / 65535.0f; };
    const Vec3 fwd = engine::math::normalize(cam.target - cam.eye);
    const Vec3 right = engine::math::normalize(engine::math::cross(fwd, Vec3{0, 1, 0}));
    const Vec3 up = engine::math::cross(right, fwd);
    std::vector<ParityVertex> v;
    for (int t = 0; t < 600; t++) {
        const f32 d = 2.0f * std::pow(15000.0f, unit());
        const f32 half = std::tan(22.5f * 3.14159265f / 180.0f) * d;
        const Vec3 c = cam.eye + fwd * d + right * ((unit() * 2 - 1) * half * 1.4f) + up * ((unit() * 2 - 1) * half);
        const f32 s = d * (0.003f + 0.3f * unit());
        for (int i = 0; i < 3; i++) {
            const Vec3 p = c + right * ((unit() * 2 - 1) * s) + up * ((unit() * 2 - 1) * s) + fwd * ((unit() * 2 - 1) * s * 0.5f);
            ParityVertex pv = {{p.x, p.y, p.z}, {200, 200, 200, 255}};
            v.push_back(pv);
        }
    }
    set_gx_frame(gc);
    // GX: matrix = view x S(k)
    f32 mv[3][4];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            mv[r][c] = c < 3 ? view.m[r][c] * k : view.m[r][3];
    clear_efb(0);
    gx_draw_triangles(mv, v);
    const HostEfb e = gx_host_efb();
    std::vector<u32> gx_depth(e.depth, e.depth + e.width * e.height);
    // native: the same triangles through a package and a packet
    std::string error;
    const std::vector<u8> bytes = parity_package(v);
    void* block = malloc(bytes.size());
    memcpy(block, bytes.data(), bytes.size());
    g3::Package* pkg = g3::Package::load(block, (u32)bytes.size(), &error);
    expect(pkg != nullptr, "the parity package loads: %s", error.c_str());
    if (!pkg)
        return;
    g3::Material mat;
    g3::build_material(*pkg, 0, &mat);
    g3::Packet p;
    p.reset();
    p.package = pkg, p.materials = &mat;
    memcpy(p.view, view.m, sizeof(p.view));
    p.world_scale = k;
    p.items.push_back({0, 0, 0xFFFF, 0});
    g3::SoftBackend sb;
    sb.prepare(*pkg);
    clear_efb(0);
    sb.draw(p, g3::native_draw_view());
    // per pixel both drew: the depths, and the view distances they stand for. Far away a step of the
    // buffer is a long way (35 units at 30,000), and the two differ by a few steps from rounding;
    // near the camera a step is a tiny distance (at 2 units a pixel across a slanted triangle is
    // 67,000 steps), and the float rounding of either transform shows as tens of steps: there they
    // are compared relative to the distance. Where triangles overlap, a pixel on the edge of the
    // nearer one may be covered in one draw and not the other (raster.cpp's float edges, the
    // backend's 1/256-pixel ones): those few differ by much more.
    u64 both = 0, only_gx = 0, only_native = 0, steps = 0;
    u64 agree[4] = {};  // equal, within 4 steps, within 1e-4 of the distance, the rest
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) {
            const u32 a = gx_depth[y * e.width + x], b = e.depth[y * e.width + x];
            const bool ga = a != 0xFFFFFF, nb = b != 0xFFFFFF;
            if (ga && nb) {
                both++;
                const u32 dz = a > b ? a - b : b - a;
                steps += dz;
                const f64 da = view_distance(a, k), db = view_distance(b, k);
                const int bin = dz == 0 ? 0 : dz <= 4 ? 1 : std::fabs(da - db) < 1e-4 * da ? 2 : 3;
                agree[bin]++;
                static int shown;
                if (bin == 3 && getenv("NOMA_G3D_VERBOSE") && shown++ < 30)
                    log("  (%u, %u): %u / %u steps, %.4f / %.4f units", x, y, a, b, da, db);
            } else if (ga) {
                only_gx++;
            } else if (nb) {
                only_native++;
            }
        }
    log("selftest g3drender: depth parity, %zu triangles at 2-30000 units (k %.1f): %llu pixels drawn by both, %llu "
        "only by GX, %llu only natively; their depths equal at %llu, within 4 steps at %llu, within 1e-4 of the "
        "view distance at %llu, further apart at %llu (mean difference %.1f steps)",
        v.size() / 3, (double)k, (unsigned long long)both, (unsigned long long)only_gx, (unsigned long long)only_native,
        (unsigned long long)agree[0], (unsigned long long)agree[1], (unsigned long long)agree[2],
        (unsigned long long)agree[3], both ? (double)steps / (double)both : 0.0);
    expect(both > 50000, "the parity triangles cover the frame (%llu pixels)", (unsigned long long)both);
    expect((only_gx + only_native) * 1000 < both, "GX and native coverage agree (%llu + %llu of %llu)",
           (unsigned long long)only_gx, (unsigned long long)only_native, (unsigned long long)both);
    expect(agree[3] * 1000 < both, "GX and native depth agree but at overlapping edges (%llu pixels further apart)",
           (unsigned long long)agree[3]);
    sb.release(*pkg);
    delete pkg;
}

// ---------------------------------------------------------------- 2./3. a real package
struct Compare {
    double psnr;
    double off;  // share of pixels with a channel off by more than 16
};

Compare compare(const std::vector<u32>& a, const std::vector<u32>& b, std::vector<u32>* diff) {
    double se = 0;
    u64 off = 0;
    diff->resize(a.size());
    for (size_t i = 0; i < a.size(); i++) {
        int worst = 0;
        u32 d = 0xFF000000u;
        for (int c = 0; c < 3; c++) {
            const int e = (int)(a[i] >> (8 * c) & 255) - (int)(b[i] >> (8 * c) & 255);
            se += (double)e * e;
            worst = std::max(worst, std::abs(e));
            d |= (u32)std::min(255, std::abs(e) * 4) << (8 * c);
        }
        off += worst > 16;
        (*diff)[i] = d;
    }
    const double mse = se / (3.0 * (double)a.size());
    return {mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99.0, 100.0 * (double)off / (double)a.size()};
}

// ---------------------------------------------------------------- through the GX stream
// The host's renderer as platform/host/main.cpp has it: native draws executed in frames it draws,
// retired in the others
struct StreamRenderer : SoftRenderer {
    bool drawing = true;
    void native(u32 id) override { engine::g3d::native_draw_execute(id, drawing); }
};
std::vector<u8> s_stream;
// the gather pipe's bursts decoded as they come (a renderer keeping up)
void capture_burst(const u8* burst) {
    s_stream.insert(s_stream.end(), burst, burst + 32);
    const u32 used = gx_decode(s_stream.data(), (u32)s_stream.size(), false);
    s_stream.erase(s_stream.begin(), s_stream.begin() + used);
}
void flush_pipe() {
    while (g_gather_n)
        gx_fifo_write(0, 0, 1);
}

// The world's two passes emitted (World::emit: packets and their commands in the FIFO) and decoded,
// with the backend registered: the frame drawn directly (`want`), and nothing in a frame the
// renderer doesn't draw
void test_stream(g3::World& w, g3::Package& pkg, g3::SoftBackend& sb, u32 clear, const std::vector<u32>& want,
                 const char* name) {
    StreamRenderer rr;
    g3::native_draw_set_backend(&sb);
    gx_set_renderer(&rr);
    g_gather_capture = capture_burst;
    const g3::NativeDrawStats s0 = g3::native_draw_stats();
    clear_efb(clear);
    w.emit(g3::Pass::OPAQUE);
    w.emit(g3::Pass::TRANSLUCENT);
    flush_pipe();
    const bool same = efb_frame() == want;
    rr.drawing = false;
    clear_efb(clear);
    w.emit(g3::Pass::OPAQUE);
    w.emit(g3::Pass::TRANSLUCENT);
    flush_pipe();
    const std::vector<u32> undrawn = efb_frame();
    const bool untouched = std::all_of(undrawn.begin(), undrawn.end(), [&](u32 c) { return c == (clear | 0xFF000000u); });
    const g3::NativeDrawStats s1 = g3::native_draw_stats();
    log("selftest g3drender: %s through the GX stream: %u packets submitted, %u executed, %u drawn; the frame %s the "
        "one drawn directly; a frame not drawn %s",
        name, s1.submitted - s0.submitted, s1.executed - s0.executed, s1.drawn - s0.drawn, same ? "is" : "is NOT",
        untouched ? "left the EFB alone" : "DREW");
    expect(same, "%s: the same frame through the GX stream", name);
    expect(untouched && s1.drawn - s0.drawn == 2 && s1.executed - s0.executed == 4,
           "%s: packets of a frame not drawn are retired (%u drawn, %u executed)", name, s1.drawn - s0.drawn,
           s1.executed - s0.executed);
    // (the backend's resources go with the package, on the renderer's thread)
    g3::native_draw_release(pkg);
    flush_pipe();
    g_gather_capture = nullptr;
    gx_set_renderer(nullptr);
    g3::native_draw_set_backend(nullptr);
    s_stream.clear();
}

void test_views() {
    const char* path_env = getenv("NOMA_G3D_PACKAGE");
    const std::string path = path_env ? path_env : "wuhu/island.ng3d";
    const std::string out = getenv("NOMA_G3D_OUT") ? getenv("NOMA_G3D_OUT") : "g3drender";
    const char* ref = getenv("NOMA_G3D_REF");
    const char* views = getenv("NOMA_G3D_VIEWS");
    const f32 fov = getenv("NOMA_G3D_FOV") ? (f32)atof(getenv("NOMA_G3D_FOV")) : 45.0f;
    const f32 frame = getenv("NOMA_G3D_FRAME") ? (f32)atof(getenv("NOMA_G3D_FRAME")) : 600.0f;
    auto t0 = std::chrono::steady_clock::now();
    g3::Package* pkg = load_file(path);
    if (!pkg) {
        log("selftest g3drender: no package at %s (NOMA_G3D_PACKAGE): the views are skipped", path.c_str());
        return;
    }
    const double load_ms = ms_since(t0);
    mkdir(out.c_str(), 0755);
    g3::World w(*pkg);
    const f32 k = w.world_scale();
    w.update(frame);
    g3::SoftBackend sb;
    t0 = std::chrono::steady_clock::now();
    sb.prepare(*pkg);
    const double prepare_ms = ms_since(t0);
    u32 overlays = 0;
    std::string overlay_names;
    for (const g3::Material& m : w.materials())
        if (g3::is_overlay(m)) {
            overlay_names += std::string(overlays++ ? ", " : "") + pkg->str(pkg->materials()[m.index].name);
        }
    log("selftest g3drender: %s loaded in %.1f ms, textures decoded for the backend in %.1f ms; k = %g; %u overlay "
        "materials (%s), pulled toward the eye by 1/%g of their distance, at most %g units",
        path.c_str(), load_ms, prepare_ms, (double)k, overlays, overlay_names.c_str(),
        1.0 / (double)g3::OVERLAY_DEPTH_PULL, (double)g3::OVERLAY_PULL_LIMIT);
    // the clear colour: the first fog in use's, as preview.py's background (a package that turns
    // fog 0 off, as the restaged island does, clears to the sea's fog 1)
    u32 clear = 0x00E6B380u;  // (0.5, 0.7, 0.9)
    for (const fm::Fog& f : pkg->fogs())
        if (f.type != fm::FOG_NONE) {
            clear = f.rgba[0] | f.rgba[1] << 8 | f.rgba[2] << 16;
            break;
        }
    const std::vector<g3::Material>& mats = w.materials();
    auto overlay_item = [&](const g3::DrawItem& it) { return g3::is_overlay(mats[pkg->batches()[it.batch].material]); };
    const HostEfb e = gx_host_efb();
    double worst_ms = 0;
    u32 n_views = 0;
    for (const fm::Marker& m : pkg->markers()) {
        const char* name = pkg->str(m.name);
        if (strncmp(name, "view_", 5) != 0)
            continue;
        if (views && !strstr(views, name))
            continue;
        n_views++;
        g3::Camera cam;
        cam.eye = {m.position[0], m.position[1], m.position[2]};
        cam.target = m.kind == 2 ? Vec3{m.dir[0], m.dir[1], m.dir[2]} : cam.eye + Vec3{m.dir[0], m.dir[1], m.dir[2]};
        cam.fovy_deg = fov, cam.aspect = (f32)W / H, cam.near_ = NEAR, cam.far_ = FAR;
        g3::Packet opa, xlu;
        opa.reset(), xlu.reset();
        w.set_camera(cam);
        w.build_packet(g3::Pass::OPAQUE, &opa);
        w.build_packet(g3::Pass::TRANSLUCENT, &xlu);
        set_gx_frame(g3::scaled(cam, k));
        const g3::GxView v = g3::native_draw_view();
        // the frame, with the backend's threads (timed: the best of three) and with one
        double ms_opa = 1e9, ms_xlu = 1e9;
        for (int rep = 0; rep < 3; rep++) {
            clear_efb(clear);
            sb.reset_stats();
            sb.set_threads(0);
            t0 = std::chrono::steady_clock::now();
            sb.draw(opa, v);
            ms_opa = std::min(ms_opa, ms_since(t0));
            t0 = std::chrono::steady_clock::now();
            sb.draw(xlu, v);
            ms_xlu = std::min(ms_xlu, ms_since(t0));
        }
        const g3::SoftBackend::Stats st = sb.stats();
        const std::vector<u32> img = efb_frame();
        clear_efb(clear);
        sb.set_threads(1);
        t0 = std::chrono::steady_clock::now();
        sb.draw(opa, v);
        sb.draw(xlu, v);
        const double ms_one = ms_since(t0);
        sb.set_threads(0);
        expect(efb_frame() == img, "%s: drawn the same with one thread", name);
        const double ms = ms_opa + ms_xlu;
        worst_ms = std::max(worst_ms, ms);
        if (n_views == 1)
            test_stream(w, *pkg, sb, clear, img, name);
        write_png(out + "/" + name + ".png", img.data(), W, H);
        std::string vs;
        if (ref) {
            std::vector<u32> want, diff;
            if (read_ppm(std::string(ref) + "/" + name + ".ppm", &want)) {
                const Compare c = compare(img, want, &diff);
                write_png(out + "/" + name + "_diff.png", diff.data(), W, H);
                char buf[160];
                snprintf(buf, sizeof(buf), "; against preview.py: PSNR %.1f dB, %.1f%% of pixels off by more than 16",
                         c.psnr, c.off);
                vs = buf;
                expect(c.psnr > 15, "%s: close to preview.py's render (%.1f dB)", name, c.psnr);
            } else {
                vs = "; no reference";
            }
        }
        log("selftest g3drender: %s: %u items (%u opaque), %u triangles (%u culled, %u clipped), %llu fragments, "
            "%llu drawn; %.1f ms (opaque %.1f + translucent %.1f), one thread %.1f ms%s",
            name, st.items, (u32)opa.items.size(), st.triangles, st.culled, st.clipped,
            (unsigned long long)st.fragments, (unsigned long long)st.written, ms, ms_opa, ms_xlu, ms_one, vs.c_str());
        expect(st.written > W * H / 2, "%s: the frame is drawn (%llu pixels)", name, (unsigned long long)st.written);

        // 3. the overlays: the opaque pass's frame and depth kept; the translucent pass drawn again
        // without them, then with them at each pull; the pixels they change, by the view distance of
        // what lies under them
        clear_efb(clear);
        sb.draw(opa, v);
        const std::vector<u32> opa_color(e.color, e.color + e.width * e.height);
        const std::vector<u32> opa_depth(e.depth, e.depth + e.width * e.height);
        auto restore = [&] {
            memcpy(e.color, opa_color.data(), opa_color.size() * 4);
            memcpy(e.depth, opa_depth.data(), opa_depth.size() * 4);
        };
        g3::Packet bare = xlu;
        bare.items.erase(std::remove_if(bare.items.begin(), bare.items.end(), overlay_item), bare.items.end());
        const u32 n_overlay_items = (u32)(xlu.items.size() - bare.items.size());
        if (!n_overlay_items)
            continue;
        restore();
        sb.draw(bare, v);
        const std::vector<u32> base = efb_frame();
        static const f32 k_bucket[4] = {300, 1000, 3000, 1e30f};
        struct Setting {
            f64 pull, limit;  // a fraction of the distance, at most so many units (1e30: no limit)
        };
        // (the shared setting first, then no pull, pulls alone and limited)
        static const Setting k_settings[] = {{g3::OVERLAY_DEPTH_PULL, g3::OVERLAY_PULL_LIMIT}, {0, 1e30},
                                             {1.0 / 4096, 1e30}, {1.0 / 1024, 1e30}, {1.0 / 256, 1e30},
                                             {1.0 / 256, 1}, {1.0 / 256, 4}, {1.0 / 256, 16}};
        std::string table;
        std::vector<u32> at0, shared;
        for (const Setting& set : k_settings) {
            const bool first = &set == &k_settings[0];
            restore();
            sb.set_overlay_pull(set.pull, set.limit);
            sb.reset_stats();
            sb.draw(xlu, v);
            const std::vector<u32> img_b = efb_frame();
            u32 count[4] = {};
            for (u32 i = 0; i < W * H; i++) {
                if (img_b[i] == base[i])
                    continue;
                const f32 d = (f32)view_distance(opa_depth[(i / W) * e.width + i % W], k);
                int bk = 0;
                while (d >= k_bucket[bk])
                    bk++;
                count[bk]++;
            }
            char buf[200], lim[24] = "";
            if (set.limit < 1e29)
                snprintf(lim, sizeof(lim), ", at most %g units", set.limit);
            snprintf(buf, sizeof(buf), "\n    pull %s%g%s: %u/%u/%u/%u", set.pull > 0 ? "1/" : "",
                     set.pull > 0 ? 1.0 / set.pull : 0.0, lim, count[0], count[1], count[2], count[3]);
            table += buf;
            if (first)
                shared = img_b;
            if (set.pull == 0)
                at0 = img_b;
            if (set.pull == 1.0 / 256 && set.limit > 1e29) {
                // what an unlimited pull adds against none (red), and what the shared one does (written
                // first: the frame's own overlays)
                std::vector<u32> gain(img_b);
                for (u32 i = 0; i < W * H; i++)
                    if (img_b[i] != at0[i])
                        gain[i] = 0xFF0000FFu;
                write_png(out + "/" + name + "_overlay_pull0.png", at0.data(), W, H);
                write_png(out + "/" + name + "_overlay_unlimited_gain.png", gain.data(), W, H);
                u32 differ = 0;
                for (u32 i = 0; i < W * H; i++)
                    differ += shared[i] != at0[i];
                char b2[96];
                snprintf(b2, sizeof(b2), "\n    (the shared setting changes %u pixels against no pull)", differ);
                table += b2;
            }
        }
        sb.set_overlay_pull(g3::OVERLAY_DEPTH_PULL, g3::OVERLAY_PULL_LIMIT);
        log("selftest g3drender: %s: %u overlay items; by pull: the pixels they change, by the view distance under "
            "them (under 300 / 300-1000 / 1000-3000 / beyond 3000 units):%s",
            name, n_overlay_items, table.c_str());
    }
    if (n_views)
        log("selftest g3drender: %u views, the slowest %.1f ms (target: under 500)", n_views, worst_ms);
    else
        log("selftest g3drender: the package has no view_* markers");
    expect(worst_ms < 500, "a 640x456 frame in under 0.5 s (the slowest %.1f ms)", worst_ms);
    sb.release(*pkg);
    delete pkg;
}

}  // namespace

void selftest_g3drender() {
    s_failures = s_checks = 0;
    test_parity();
    test_views();
    log("selftest g3drender: %d checks, %d failed: %s", s_checks, s_failures, s_failures ? "MISMATCH" : "ok");
}

}  // namespace guest
