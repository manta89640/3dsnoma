// The native g3d on the 3DS GPU: engine/g3d's Backend (native_draw.h) for the PICA200. A World's
// packets are drawn into the GX renderer's frame (gx_pica.cpp) at the point of the GX stream their
// command is at, into the depth buffer the GX draws (the Mii) use and through the GX projection set
// there, so that the two meet as if one renderer drew both.
//
// prepare() puts a package where the GPU reads it, in linear memory: each texture with all its
// levels (packages are baked: PICA formats, tiled, the levels one after the other, as the GPU
// samples them), the vertex data in one block and the u16 indices in another, flushed from the
// CPU's cache. The attribute buffers' offsets count from the lower of the two blocks (they have 28
// bits; the GX path's base is its own buffers'). Each vertex buffer's positions are moved to an
// origin of its own -- a cell's buffers to the cell's centre, a model's to its box's -- and the
// draw's matrix moves them back: the GPU's floats keep 16 bits of mantissa, a step of 0.06-0.25
// package units at the island's coordinates (0.016-0.03 from a cell's centre), and the buffers of a
// cell share their origin so that an overlay and the ground under it (_blend layers) round alike.
//
// draw() writes the GPU's registers directly, its own shader program (shaders/g3d.v.pica)
// included, between gx_pica.cpp's pica_native_begin and pica_native_end, which bring the GX path's
// frame up to date before and take nothing it kept about the GPU as still true after. Per item:
// the vertex buffer's attribute layout and offset; the uniforms (the model-view-projection -- the
// 3DS's clip transform, the GX projection, the view, the package's scale, the item's matrix and
// its buffer's origin, multiplied in double precision --, the normal matrix, the material's colours
// and its light set's lights in view space, the texture matrices with the coordinates' scale and
// nw4r's env and projection mappings folded in); the material's combiners (engine/gx/tev_combiner:
// the GX path's own translation), textures (each unit with the material's filters, wrap and LOD),
// the pixel pipeline (alpha test, blending, depth, culling); the draw. Nothing the GPU already has
// from the item before is sent again. The depth goes out complemented, for precision (see
// begin_packet); fog is a second pass of the item (fog_pass). The command list is gx_pica's, and an
// item is drawn only with room left in it for the GX draws to come (ITEM_WORDS).
// NOMA_G3D_STATS=N (env.txt): every N frames the draws, triangles, time, state changes and memory.
#include <3ds.h>
#include <citro3d.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "engine/g3d/native_draw.h"
#include "g3d_pica.h"
#include "gx_pica.h"
#include "runtime/runtime.h"

extern "C" const uint8_t g3d_shbin[];
extern "C" const size_t g3d_shbin_size;

namespace guest {

namespace {

namespace g3 = engine::g3d;
namespace fmt = engine::g3d::fmt;
namespace tev = engine::gx;

// ---------------------------------------------------------------- vertex layouts
// The layouts the format allows (fmt::VertexBuffer: attrs, flags, stride) as the attribute loaders
// take them: the attributes the buffer has, in its order (position f32 x3, normal s8 x4 -- its pad
// byte as w, unread --, colour u8 x4, uv0 and uv1 s16 or f32 x2), then fixed values for the shader
// inputs it lacks (the normal up, the colour white, the coordinates 0).
struct Layout {
    u8 attrs, flags, stride;
    u32 format_low, format_high;   // GPUREG_ATTRIBBUFFERS_FORMAT_LOW/HIGH
    u32 buf_config1, buf_config2;  // buffer 0's attribute order; its stride and attribute count
    u32 permutation;               // attribute -> shader input (v0 position .. v4 uv1)
    u32 n_fixed;
    u8 fixed_id[5];                // the attribute of each fixed value ...
    f32 fixed[5][4];               // ... and the value
};
std::vector<Layout> s_layouts;

u8 layout_of(u8 attrs, u8 flags, u8 stride) {
    for (u32 i = 0; i < s_layouts.size(); i++)
        if (s_layouts[i].attrs == attrs && s_layouts[i].flags == flags && s_layouts[i].stride == stride)
            return (u8)i;
    Layout L = {};
    L.attrs = attrs, L.flags = flags, L.stride = stride;
    struct In {
        u8 bit, format, count;
        f32 fixed[4];
    };
    const u8 uv_format = (flags & fmt::VB_UV_FLOAT) ? GPU_FLOAT : GPU_SHORT;
    const In inputs[5] = {
        {fmt::VA_POS, GPU_FLOAT, 3, {0, 0, 0, 1}},
        {fmt::VA_NRM, GPU_BYTE, 4, {0, 127, 0, 0}},
        {fmt::VA_CLR, GPU_UNSIGNED_BYTE, 4, {255, 255, 255, 255}},
        {fmt::VA_UV0, uv_format, 2, {0, 0, 0, 0}},
        {fmt::VA_UV1, uv_format, 2, {0, 0, 0, 0}},
    };
    u32 id = 0, n_loaders = 0;
    u32 to_input[5] = {};
    L.format_high = 0xFFFu << 16;  // (every attribute fixed but the loaders, as citro3d's AttrInfo)
    for (u32 r = 0; r < 5; r++)  // the loaders, in the buffer's order
        if (attrs & inputs[r].bit) {
            L.format_low |= (u32)((inputs[r].count - 1) << 2 | (inputs[r].format & 3)) << (4 * id);
            L.format_high &= ~(1u << (16 + id));
            L.buf_config1 |= id << (4 * n_loaders++);
            to_input[id++] = r;
        }
    for (u32 r = 0; r < 5; r++)  // then the fixed ones
        if (!(attrs & inputs[r].bit)) {
            L.fixed_id[L.n_fixed] = (u8)id;
            memcpy(L.fixed[L.n_fixed++], inputs[r].fixed, sizeof(inputs[r].fixed));
            to_input[id++] = r;
        }
    L.format_high |= (id - 1) << 28;
    for (u32 a = 0; a < id; a++)
        L.permutation |= to_input[a] << (4 * a);
    L.buf_config2 = (u32)stride << 16 | n_loaders << 28;
    s_layouts.push_back(L);
    return (u8)(s_layouts.size() - 1);
}

// ---------------------------------------------------------------- the package on the GPU
struct VbRes {
    u32 offset = 0;          // from the attribute buffers' base
    u8 layout = 0;
    bool ok = false;         // drawable (it has positions)
    f32 centre[3] = {};      // its positions are from here (package units)
    f32 uv_scale[2] = {1, 1};
};
struct TexRes {
    void* data = nullptr;
    u32 bytes = 0, addr = 0;  // addr: physical >> 3, as the texture units take it
    u32 dim = 0;              // height | width << 16
    u8 format = 0, levels = 0;
    f32 sx = 1, sy = 1;       // the image's part of the texture (padded to powers of two)
};
// a material's texture unit as the unit's registers take it
struct UnitRes {
    bool on = false;
    u32 regs[6] = {};         // TEXUNITn_BORDER_COLOR, _DIM, _PARAM, _LOD, _ADDR; _TYPE (the format)
    f32 sx = 1, sy = 1;
};
// what a material needs that depends on it alone, made at its first draw
struct MatRes {
    bool made = false;
    bool drawable = true;     // (a texture it samples couldn't be made)
    bool projects = false;    // a unit maps the position (its rows depend on the matrix)
    UnitRes unit[3];
    u32 bools = 0;            // the shader's flags for it, but haslights
    u32 pixel[6] = {};        // alpha test, depth and colour mask, blend, colour operation, logic op, cull
};
struct PkgRes {
    bool ok = false;
    void* vtx = nullptr;
    void* idx = nullptr;
    u32 vtx_bytes = 0, idx_bytes = 0;
    u32 base = 0;             // the attribute buffers' base (physical)
    u32 idx_offset = 0;       // the indices' offset from it
    std::vector<VbRes> vbs;
    std::vector<TexRes> texs;
    u32 tex_bytes = 0, tex_missing = 0;
    const g3::Material* materials = nullptr;  // the runtime materials `mats` were made from
    std::vector<MatRes> mats;
};

// ---------------------------------------------------------------- the shader
// its uniforms' registers (found by name)
struct Regs {
    int mvp, nmtx, matc, selmat, ambc, selamb, sellit, lcfg, tsel, tmtx, lights;
    int b_lit, b_haslights, b_usenrm, b_proj0, b_tc1on, b_tc2on;  // (proj1, proj2 follow proj0)
} R;
const int MAX_LIGHTS = 8;

// The LOD bias the PICA gets is GX's. The GPU picks a level from the texels a pixel of the 3DS's
// 400x240 covers, GX from those of a pixel of its 640x456 frame, so for the same view the PICA's
// level is log2(1.6..1.9) = 0.7-0.9 higher: the level that shows the texture on the smaller screen
// without aliasing, as sharp relative to its pixels as GX's bias made it on the Wii. Matching GX's
// levels instead (-0.8) would shimmer on a screen without the Wii's deflicker filter.
// NOMA_G3D_LOD_BIAS (env.txt) adds to it, to try others on a console.
f32 s_lod_bias_add = 0;

// The fog ramp (fog_pass): an 8x8 A8 texture whose alpha rises from 0 in the first column to 255
// in the last, sampled at s = 1/16 + 14/16 u: the texels' centres, so that the bilinear filter
// gives u itself, and the texture's edge clamps it to 0..1.
void* s_fog_tex;
u32 s_fog_regs[6];

const u32 k_unit_border[3] = {GPUREG_TEXUNIT0_BORDER_COLOR, GPUREG_TEXUNIT1_BORDER_COLOR, GPUREG_TEXUNIT2_BORDER_COLOR};
const u32 k_unit_type[3] = {GPUREG_TEXUNIT0_TYPE, GPUREG_TEXUNIT1_TYPE, GPUREG_TEXUNIT2_TYPE};

// The command list is gx_pica's, one for the frame (C3D_Init's 2 MB), and a full one stops the
// program (GPUCMD_Add breaks): an item isn't drawn unless the list keeps room for the most words an
// item takes (its state, uniforms, draw and fog pass, ~600) and for the GX draws still to come in
// the frame (the room's whole frame fits in far less). The island's packets take at most 62,000
// words, 24,000 a frame on average (host check, 21 views). NOMA_G3D_STATS counts the items not
// drawn; the first is logged.
const u32 ITEM_WORDS = 2048, GX_RESERVE_WORDS = 64u << 10;

const f32 k_identity24[2][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}};

// ---------------------------------------------------------------- the backend
class PicaBackend : public g3::Backend {
public:
    bool init();
    void prepare(g3::Package& package) override;
    void draw(const g3::Packet& packet, const g3::GxView& view) override;
    void release(g3::Package& package) override;

private:
    DVLB_s* dvlb_ = nullptr;
    shaderProgram_s prog_;

    // ---- what the GPU has in this packet (sent again only when it changes; ~0: unknown)
    u32 uv_[96][4];        // float uniforms as the GPU takes them (w, z, y, x)
    u64 dirty_lo_ = 0;     // c0-c63
    u32 dirty_hi_ = 0;     // c64-c95
    u32 bools_ = ~0u, int0_ = ~0u;
    u32 texenv_[6][5];
    u32 texenv_buf_ = ~0u;
    u32 units_[3][6];
    u32 unit_config_ = ~0u;
    u32 pixel_[6];
    int layout_ = -1, vbuf_ = -1;
    int light_set_ = -2;   // whose lights the uniforms hold
    u32 n_lights_ = 0;
    // ---- what the uniforms were made for (the matrices: the item's matrix and its buffer's origin,
    // which a cell's buffers share, so that a cell's items after the first send none)
    struct MatrixKey {
        int matrix = -1;
        f32 centre[3] = {};
        u16 flags = 0;
        bool pulled = false;
        bool operator==(const MatrixKey& o) const {
            return matrix == o.matrix && !memcmp(centre, o.centre, sizeof(centre)) && flags == o.flags &&
                   pulled == o.pulled;
        }
    } mkey_;
    u32 mat_key_ = ~0u;    // material << 16 | its state
    int rows_vbuf_ = -1;   // the vertex buffer the texture rows were made for

    // ---- the packet's: the clip matrix times the GX projection, the GX projection, the lights
    double cp_[4][4], proj_[4][4];
    std::vector<f32> lights_;     // per packet light: direction toward it (view space), colour
    // ---- the item's: model (from its buffer's origin) -> view, in the guest's units
    double mv_[3][4];

    // ---- NOMA_G3D_STATS
    int stats_every_ = 0;
    struct Stats {
        u32 frames, draws, fog_draws, triangles, indices, items_skipped;
        u32 mat_changes, matrices, uniform_vectors, cmd_words;
        u64 ticks, ticks_max_frame, ticks_frame;
    } st_ = {};
    bool skipped_logged_ = false;

    void uni(int reg, const f32* v);
    void uni(int reg, f32 x, f32 y, f32 z, f32 w) {
        const f32 v[4] = {x, y, z, w};
        uni(reg, v);
    }
    void upload_uniforms();
    void set_bools(u32 bools);
    void set_lights(const g3::Packet& p, int set);
    void begin_packet(const PkgRes& r);
    void set_layout(const PkgRes& r, int vbuf);
    void set_texenv(const tev::Combiners& comb);
    void set_units(const UnitRes units[3]);
    void set_pixel(const u32 pixel[6]);
    void draw_batch(const PkgRes& r, const fmt::Batch& b);
    void make_material(PkgRes& r, u32 index, const g3::Material& m);
    void item_matrix(const g3::Packet& p, const PkgRes& r, const g3::DrawItem& it, int vbuf, bool sky, bool pulled);
    void material_uniforms(const g3::Packet& p, const g3::Material& m, const g3::MatDynamic* dyn);
    void texture_rows(const g3::Material& m, const MatRes& mr, const g3::MatDynamic* dyn, const VbRes& vb);
    bool fog_rows(const g3::Packet& p, const g3::GxView& view, const g3::Material& m, const fmt::Batch& b,
                  const VbRes& vb, bool pulled, f32 rows[3][4], u32* color);
    void fog_pass(const g3::Material& m, const fmt::Batch& b, const PkgRes& r, const f32 rows[3][4], u32 color);
    void stats_packet(const g3::Packet& p, const PkgRes& r, u64 ticks, u32 words);
};

// ---------------------------------------------------------------- small helpers
void mul44(const double a[4][4], const double b[4][4], double out[4][4]) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
}
// a 4x4 times an affine 3x4 (its fourth row 0 0 0 1)
void mul44_34(const double a[4][4], const double b[3][4], double out[4][4]) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.0);
}
inline f32 u8f(u8 v) { return (f32)v * (1.0f / 255.0f); }

// ---------------------------------------------------------------- set up
bool PicaBackend::init() {
    dvlb_ = DVLB_ParseFile((u32*)g3d_shbin, (u32)g3d_shbin_size);
    if (!dvlb_)
        return false;
    shaderProgramInit(&prog_);
    shaderProgramSetVsh(&prog_, &dvlb_->DVLE[0]);
    shaderInstance_s* vsh = prog_.vertexShader;
    auto at = [&](const char* name) { return (int)shaderInstanceGetUniformLocation(vsh, name); };
    R.mvp = at("mvp"), R.nmtx = at("nmtx"), R.matc = at("matc"), R.selmat = at("selmat"), R.ambc = at("ambc");
    R.selamb = at("selamb"), R.sellit = at("sellit"), R.lcfg = at("lcfg"), R.tsel = at("tsel"), R.tmtx = at("tmtx");
    R.lights = at("lights");
    // (bools are numbered from 0x68, integer vectors from 0x60)
    R.b_lit = at("lit") - 0x68, R.b_haslights = at("haslights") - 0x68, R.b_usenrm = at("usenrm") - 0x68;
    R.b_proj0 = at("proj0") - 0x68, R.b_tc1on = at("tc1on") - 0x68, R.b_tc2on = at("tc2on") - 0x68;
    const int floats[] = {R.mvp, R.nmtx, R.matc, R.selmat, R.ambc, R.selamb, R.sellit, R.lcfg, R.tsel, R.tmtx, R.lights};
    for (int reg : floats)
        if (reg < 0 || reg >= 95) {
            log("g3d pica: the shader's uniforms are not all there");
            return false;
        }
    if (R.b_lit < 0 || R.b_haslights < 0 || R.b_usenrm < 0 || R.b_proj0 < 0 || at("proj2") - 0x68 != R.b_proj0 + 2 ||
        R.b_tc1on < 0 || R.b_tc2on < 0 || at("nlights") != 0x60) {
        log("g3d pica: the shader's flags are not where expected");
        return false;
    }
    if (const char* v = getenv("NOMA_G3D_LOD_BIAS"))
        s_lod_bias_add = (f32)atof(v);
    if (const char* v = getenv("NOMA_G3D_STATS"))
        stats_every_ = std::max(1, atoi(v));
    // the fog ramp (A8, 8x8 in Z order: the alpha of column x is 255 x / 7, every row alike)
    if (!(s_fog_tex = linearAlloc(64)))
        return false;
    u8* t = (u8*)s_fog_tex;
    for (u32 y = 0; y < 8; y++)
        for (u32 x = 0; x < 8; x++)
            t[(x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3] =
                (u8)((255 * x + 3) / 7);
    GSPGPU_FlushDataCache(s_fog_tex, 64);
    s_fog_regs[0] = 0;
    s_fog_regs[1] = 8 | 8u << 16;
    s_fog_regs[2] = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                    GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE) |
                    GPU_TEXTURE_MODE(GPU_TEX_2D);
    s_fog_regs[3] = 0;
    s_fog_regs[4] = osConvertVirtToPhys(s_fog_tex) >> 3;
    s_fog_regs[5] = GPU_A8;
    log("g3d pica: backend ready (shader %u bytes)%s", (unsigned)g3d_shbin_size,
        stats_every_ > 0 ? "; NOMA_G3D_STATS on" : "");
    return true;
}

// ---------------------------------------------------------------- prepare / release
void PicaBackend::prepare(g3::Package& pkg) {
    if (pkg.backend_data)
        return;
    const u64 t0 = svcGetSystemTick();
    PkgRes* r = new PkgRes();
    pkg.backend_data = r;
    // ---- the vertex data in one block (each buffer where it was in the file), the indices in another
    const auto vbs = pkg.vertex_buffers();
    u32 lo = ~0u, hi = 0;
    for (const fmt::VertexBuffer& vb : vbs)
        if (vb.count)
            lo = std::min(lo, vb.data), hi = std::max(hi, vb.data + vb.count * vb.stride);
    const auto idx = pkg.indices();
    r->vtx_bytes = hi > lo ? (hi - lo + 15) & ~15u : 0;
    r->idx_bytes = (idx.count * 2 + 15) & ~15u;
    // (room for it all: the GX path's textures no recent frame drew make way -- the room's, while
    // the island loads)
    u32 need = r->vtx_bytes + r->idx_bytes;
    for (u32 i = 0; i < pkg.textures().count; i++)
        need += (pkg.texture_image(i).size + 127) & ~127u;
    pica_make_room(need);
    r->vtx = r->vtx_bytes ? linearAlloc(r->vtx_bytes) : nullptr;
    r->idx = r->idx_bytes ? linearAlloc(r->idx_bytes) : nullptr;
    if (!r->vtx || !r->idx) {
        log("g3d pica: no linear memory for the package's %u KB of vertices and %u KB of indices (%u KB free): "
            "not drawn", (unsigned)(r->vtx_bytes >> 10), (unsigned)(r->idx_bytes >> 10),
            (unsigned)(linearSpaceFree() >> 10));
        if (r->vtx)
            linearFree(r->vtx);
        if (r->idx)
            linearFree(r->idx);
        r->vtx = r->idx = nullptr;
        return;
    }
    memcpy(r->vtx, pkg.at(lo), hi - lo);
    memcpy(r->idx, idx.data, idx.count * 2);
    const u32 vpa = osConvertVirtToPhys(r->vtx), ipa = osConvertVirtToPhys(r->idx);
    r->base = std::min(vpa, ipa) & ~0xFu;
    r->idx_offset = ipa - r->base;
    // ---- each buffer's origin: its cell's centre (the first cell that draws it), else its box's
    r->vbs.resize(vbs.count);
    std::vector<u8> placed(vbs.count, 0);
    for (const fmt::Cell& c : pkg.cells())
        for (u32 l = 0; l < c.n_lods && l < (u32)fmt::MAX_CELL_LODS; l++)
            for (u32 i = 0; i < c.count[l]; i++) {
                const u32 v = pkg.batches()[pkg.cell_batches()[c.first[l] + i]].vbuf;
                if (placed[v])
                    continue;
                placed[v] = 1;
                for (int k = 0; k < 3; k++)
                    r->vbs[v].centre[k] = (c.bbox[k] + c.bbox[3 + k]) * 0.5f;
            }
    for (u32 v = 0; v < vbs.count; v++) {
        const fmt::VertexBuffer& vb = vbs[v];
        VbRes& vr = r->vbs[v];
        vr.ok = (vb.attrs & fmt::VA_POS) && vb.count;
        if (!vr.ok)
            continue;
        vr.offset = vpa + (vb.data - lo) - r->base;
        vr.layout = layout_of(vb.attrs, vb.flags, vb.stride);
        const bool f = vb.flags & fmt::VB_UV_FLOAT;
        vr.uv_scale[0] = f ? 1.0f : vb.uv_scale[0], vr.uv_scale[1] = f ? 1.0f : vb.uv_scale[1];
        u8* p = (u8*)r->vtx + (vb.data - lo);
        if (!placed[v]) {
            f32 b[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
            for (u32 i = 0; i < vb.count; i++) {
                f32 q[3];
                memcpy(q, p + i * vb.stride, 12);
                for (int k = 0; k < 3; k++)
                    b[k] = std::min(b[k], q[k]), b[3 + k] = std::max(b[3 + k], q[k]);
            }
            for (int k = 0; k < 3; k++)
                vr.centre[k] = (b[k] + b[3 + k]) * 0.5f;
        }
        for (u32 i = 0; i < vb.count; i++) {
            f32 q[3];
            memcpy(q, p + i * vb.stride, 12);
            for (int k = 0; k < 3; k++)
                q[k] -= vr.centre[k];
            memcpy(p + i * vb.stride, q, 12);
        }
    }
    GSPGPU_FlushDataCache(r->vtx, r->vtx_bytes);
    GSPGPU_FlushDataCache(r->idx, r->idx_bytes);
    // ---- the textures, each in a block of its own
    u32 kb_fmt[16] = {};
    r->texs.resize(pkg.textures().count);
    for (u32 i = 0; i < pkg.textures().count; i++) {
        const g3::Package::TextureImage img = pkg.texture_image(i);
        TexRes& t = r->texs[i];
        if (!img.texels || !img.size || img.format > fmt::TF_ETC1A4 || !(t.data = linearAlloc(img.size))) {
            r->tex_missing++;
            continue;
        }
        memcpy(t.data, img.texels, img.size);
        GSPGPU_FlushDataCache(t.data, img.size);
        t.bytes = img.size;
        t.addr = osConvertVirtToPhys(t.data) >> 3;
        t.dim = (u32)img.height | (u32)img.width << 16;
        t.format = img.format, t.levels = img.levels;
        t.sx = (f32)img.src_width / (f32)img.width, t.sy = (f32)img.src_height / (f32)img.height;
        r->tex_bytes += img.size;
        kb_fmt[img.format & 15] += img.size >> 10;
    }
    r->mats.resize(pkg.materials().count);
    r->ok = true;
    log("g3d pica: package prepared in %.1f ms: %u textures, %u KB (RGBA8 %u, RGB565 %u, RGBA5551 %u, LA8 %u, L8 %u, "
        "LA4 %u, L4 %u, A8 %u, ETC1 %u, ETC1A4 %u)%s; vertices %u KB in %u buffers, indices %u KB; %u KB of the "
        "linear heap free",
        (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11, (unsigned)(r->texs.size() - r->tex_missing),
        (unsigned)(r->tex_bytes >> 10), (unsigned)kb_fmt[fmt::TF_RGBA8], (unsigned)kb_fmt[fmt::TF_RGB565],
        (unsigned)kb_fmt[fmt::TF_RGBA5551], (unsigned)kb_fmt[fmt::TF_LA8], (unsigned)kb_fmt[fmt::TF_L8],
        (unsigned)kb_fmt[fmt::TF_LA4], (unsigned)kb_fmt[fmt::TF_L4], (unsigned)kb_fmt[fmt::TF_A8],
        (unsigned)kb_fmt[fmt::TF_ETC1], (unsigned)kb_fmt[fmt::TF_ETC1A4],
        r->tex_missing ? " (some not made: no memory)" : "", (unsigned)(r->vtx_bytes >> 10), (unsigned)vbs.count,
        (unsigned)(r->idx_bytes >> 10), (unsigned)(linearSpaceFree() >> 10));
}

void PicaBackend::release(g3::Package& pkg) {
    PkgRes* r = (PkgRes*)pkg.backend_data;
    if (!r)
        return;
    // (the GPU may still have draws of it to make: freed once the command lists so far are done)
    pica_free_after_frame(r->vtx);
    pica_free_after_frame(r->idx);
    for (TexRes& t : r->texs)
        pica_free_after_frame(t.data);
    delete r;
    pkg.backend_data = nullptr;
}

// ---------------------------------------------------------------- state, written directly
// a float uniform (x y z w; the GPU takes it w first), sent before the next draw if it changed
void PicaBackend::uni(int reg, const f32* v) {
    u32 w[4];
    memcpy(&w[3], &v[0], 4), memcpy(&w[2], &v[1], 4), memcpy(&w[1], &v[2], 4), memcpy(&w[0], &v[3], 4);
    if (!memcmp(w, uv_[reg], sizeof(w)))
        return;
    memcpy(uv_[reg], w, sizeof(w));
    if (reg < 64)
        dirty_lo_ |= 1ull << reg;
    else
        dirty_hi_ |= 1u << (reg - 64);
}

// the changed vectors, a run at a time (as gx_pica.cpp's upload_uniforms)
void PicaBackend::upload_uniforms() {
    u64 lo = dirty_lo_;
    u32 hi = dirty_hi_;
    auto bit = [&](u32 k) { return k < 64 ? (lo >> k) & 1 : (hi >> (k - 64)) & 1; };
    while (lo | hi) {
        const u32 i = lo ? (u32)__builtin_ctzll(lo) : 64 + (u32)__builtin_ctz(hi);
        u32 j = i;
        while (j < 96 && bit(j)) {
            if (j < 64)
                lo &= ~(1ull << j);
            else
                hi &= ~(1u << (j - 64));
            j++;
        }
        GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000 | i);
        GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, &uv_[i][0], 4 * (j - i));
        st_.uniform_vectors += j - i;
    }
    dirty_lo_ = 0, dirty_hi_ = 0;
}

void PicaBackend::set_bools(u32 bools) {
    if (bools == bools_)
        return;
    bools_ = bools;
    GPUCMD_AddWrite(GPUREG_VSH_BOOLUNIFORM, 0x7FFF0000 | bools);
}

// the light set's lights into the uniforms (the loop's count in i0: lights - 1, from 0, a light
// two vectors)
void PicaBackend::set_lights(const g3::Packet& p, int set) {
    if (set == light_set_)
        return;
    light_set_ = set;
    u32 n = 0;
    if (set >= 0 && (u32)set < p.light_sets.size())
        for (u16 l : p.light_sets[(u32)set].lights)
            if (l != fmt::NONE16 && l < p.lights.size() && n < (u32)MAX_LIGHTS) {
                uni(R.lights + 2 * (int)n, &lights_[8 * l]);
                uni(R.lights + 2 * (int)n + 1, &lights_[8 * l + 4]);
                n++;
            }
    n_lights_ = n;
    const u32 i0 = (n ? n - 1 : 0) | 2u << 16;
    if (i0 != int0_) {
        int0_ = i0;
        GPUCMD_AddWrite(GPUREG_VSH_INTUNIFORM_I0, i0);
    }
}

// a packet's start: the shader program and its constants, the attribute buffers' base, what the
// GPU has taken as unknown
void PicaBackend::begin_packet(const PkgRes& r) {
    shaderProgramConfigure(&prog_, true, false);
    shaderInstance_s* vsh = prog_.vertexShader;
    for (u32 i = 0; i < vsh->numFloat24Uniforms; i++)
        GPUCMD_AddIncrementalWrites(GPUREG_VSH_FLOATUNIFORM_CONFIG, (u32*)&vsh->float24Uniforms[i], 4);
    GPUCMD_AddWrite(GPUREG_ATTRIBBUFFERS_LOC, r.base >> 3);
    static const u32 k_zero[33] = {};  // (buffers 1-11: none)
    GPUCMD_AddIncrementalWrites(GPUREG_ATTRIBBUFFER1_OFFSET, k_zero, 33);
    GPUCMD_AddWrite(GPUREG_TEXENV_BUFFER_COLOR, 0xFFFFFFFF);
    // The depth, complemented. The GX path's draws output z (clip space) and the depth map makes the
    // buffer's depth -z/w (citro3d's default: scale -1, offset 0). Near 1, as most of the island is
    // (1 - near/distance, near 15 Mii units), that ratio of two nearly equal 24-bit floats is off by
    // up to ~7e-5 -- hundreds of the buffer's steps, half a package unit of distance 300 units away:
    // an overlay and the ground under it come out in either order. The native shader outputs
    // z' = -(z + w) instead, from one row the CPU sums in double precision (item_matrix), with 16
    // bits of precision of its own, and this depth map makes 1 + z'/w of it: the same -z/w, as
    // accurate as the buffer (and the same clipping: z' within [-w, 0] as z was). Checked on the
    // host with the GPU's 24-bit floats emulated, over the island's 21 camera views: a point
    // 0.03-0.4 units over a ground vertex and pulled as the translucent pass is came out behind the
    // ground at 23 % of 366,343 ground vertices with z, at none with z' (as with exact arithmetic).
    GPUCMD_AddWrite(GPUREG_DEPTHMAP_SCALE, f32tof24(1.0f));
    GPUCMD_AddWrite(GPUREG_DEPTHMAP_OFFSET, f32tof24(1.0f));
    memset(uv_, 0xFF, sizeof(uv_));  // (a NaN: no value compares equal to it)
    dirty_lo_ = 0, dirty_hi_ = 0;
    bools_ = int0_ = ~0u;
    memset(texenv_, 0xFF, sizeof(texenv_));
    texenv_buf_ = ~0u;
    memset(units_, 0xFF, sizeof(units_));
    unit_config_ = ~0u;
    memset(pixel_, 0xFF, sizeof(pixel_));
    layout_ = vbuf_ = -1;
    light_set_ = -2;
    mkey_ = MatrixKey();
    mat_key_ = ~0u;
    rows_vbuf_ = -1;
}

// the vertex buffer: its layout (the loaders, the shader inputs they feed, the fixed values) and
// its place
void PicaBackend::set_layout(const PkgRes& r, int vbuf) {
    const VbRes& vb = r.vbs[(u32)vbuf];
    const Layout& L = s_layouts[vb.layout];
    if (vb.layout != layout_) {
        layout_ = vb.layout;
        const u32 fmt_words[2] = {L.format_low, L.format_high};
        GPUCMD_AddIncrementalWrites(GPUREG_ATTRIBBUFFERS_FORMAT_LOW, fmt_words, 2);
        GPUCMD_AddMaskedWrite(GPUREG_VSH_INPUTBUFFER_CONFIG, 0xB, 0xA0000000 | 4);
        GPUCMD_AddWrite(GPUREG_VSH_NUM_ATTR, 4);
        const u32 perm[2] = {L.permutation, 0};
        GPUCMD_AddIncrementalWrites(GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW, perm, 2);
        for (u32 i = 0; i < L.n_fixed; i++) {
            GPUCMD_AddWrite(GPUREG_FIXEDATTRIB_INDEX, L.fixed_id[i]);
            C3D_ImmSendAttrib(L.fixed[i][0], L.fixed[i][1], L.fixed[i][2], L.fixed[i][3]);
        }
        vbuf_ = -1;
    }
    if (vbuf != vbuf_) {
        vbuf_ = vbuf;
        const u32 buf[3] = {vb.offset, L.buf_config1, L.buf_config2};
        GPUCMD_AddIncrementalWrites(GPUREG_ATTRIBBUFFER0_OFFSET, buf, 3);
    }
}

// the combiners (the stages that differ) and their buffer's update masks (fog off: the GPU's fog
// can't place a ramp 28,000 units away in a 24-bit depth buffer: see fog_pass)
void PicaBackend::set_texenv(const tev::Combiners& comb) {
    for (int i = 0; i < 6; i++) {
        u32 w[5];
        tev::combiner_words(comb.stage[i], w);
        if (!memcmp(w, texenv_[i], sizeof(w)))
            continue;
        memcpy(texenv_[i], w, sizeof(w));
        GPUCMD_AddIncrementalWrites(GPUREG_TEXENV0_SOURCE + (i >= 4 ? i + 2 : i) * 8, w, 5);
    }
    const u32 buf = (u32)(comb.buf_rgb & 0xF) << 8 | (u32)(comb.buf_a & 0xF) << 12;
    if (buf != texenv_buf_) {
        texenv_buf_ = buf;
        GPUCMD_AddMaskedWrite(GPUREG_TEXENV_UPDATE_BUFFER, 0x7, buf);
    }
}

// the texture units (the ones that differ), then which are on, with the texture cache cleared (as
// citro3d does when it binds a texture)
void PicaBackend::set_units(const UnitRes units[3]) {
    bool changed = false;
    u32 on = 0;
    for (int u = 0; u < 3; u++) {
        if (!units[u].on)
            continue;
        on |= 1u << u;
        if (!memcmp(units[u].regs, units_[u], sizeof(units_[u])))
            continue;
        memcpy(units_[u], units[u].regs, sizeof(units_[u]));
        GPUCMD_AddIncrementalWrites(k_unit_border[u], units[u].regs, 5);
        GPUCMD_AddWrite(k_unit_type[u], units[u].regs[5]);
        changed = true;
    }
    if (!changed && on == unit_config_)
        return;
    unit_config_ = on;
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0xB, 1u << 12 | on);
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, 1u << 16);
    GPUCMD_AddWrite(GPUREG_TEXUNIT0_SHADOW, 1);
}

// the pixel pipeline's registers that differ (as citro3d's effect binding writes them)
void PicaBackend::set_pixel(const u32 px[6]) {
    if (px[0] != pixel_[0])
        GPUCMD_AddWrite(GPUREG_FRAGOP_ALPHA_TEST, px[0]);
    if (px[1] != pixel_[1]) {
        GPUCMD_AddWrite(GPUREG_DEPTH_COLOR_MASK, px[1]);
        GPUCMD_AddMaskedWrite(GPUREG_GAS_DELTAZ_DEPTH, 0x8, (u32)GPU_MAKEGASDEPTHFUNC((px[1] >> 4) & 7) << 24);
    }
    if (px[2] != pixel_[2])
        GPUCMD_AddWrite(GPUREG_BLEND_FUNC, px[2]);
    if (px[3] != pixel_[3])
        GPUCMD_AddMaskedWrite(GPUREG_COLOR_OPERATION, 0x7, px[3]);
    if (px[4] != pixel_[4])
        GPUCMD_AddWrite(GPUREG_LOGIC_OP, px[4]);
    if (px[5] != pixel_[5])
        GPUCMD_AddWrite(GPUREG_FACECULLING_CONFIG, px[5]);
    memcpy(pixel_, px, sizeof(pixel_));
}

// the batch's triangles (as C3D_DrawElements draws GPU_TRIANGLES from u16 indices)
void PicaBackend::draw_batch(const PkgRes& r, const fmt::Batch& b) {
    upload_uniforms();
    pica_native_draw_noted();
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, GPU_GEOMETRY_PRIM);
    GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
    GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, (r.idx_offset + b.first_index * 2) | 1u << 31);
    GPUCMD_AddWrite(GPUREG_NUMVERTICES, b.n_indices);
    GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, 0);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0x100);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0x100);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
    GPUCMD_AddWrite(GPUREG_DRAWELEMENTS, 1);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0);
    GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
}

// ---------------------------------------------------------------- materials
void PicaBackend::make_material(PkgRes& r, u32 index, const g3::Material& m) {
    MatRes& mr = r.mats[index];
    mr = MatRes();
    mr.made = true;
    bool env = false;
    for (int u = 0; u < 3; u++) {
        const g3::TexUnit& tu = m.unit[u];
        UnitRes& ur = mr.unit[u];
        if (tu.texture == fmt::NONE16)
            continue;
        if (tu.texture >= r.texs.size() || !r.texs[tu.texture].data) {
            mr.drawable = false;
            continue;
        }
        const TexRes& t = r.texs[tu.texture];
        const bool proj = tu.source == g3::TexCoordSource::PROJECTION;
        // (only unit 0 can take a projection texture: units 1 and 2 divide per vertex)
        const GPU_TEXTURE_MODE_PARAM type = proj && u == 0 ? GPU_TEX_PROJECTION : GPU_TEX_2D;
        // levels: GX's min and max LOD within those the texture has; none but level 0 without a mip filter
        const bool mip = tu.mip != 0 && t.levels > 1;
        const int max_level = mip ? std::max(0, std::min<int>(t.levels - 1, (int)tu.max_lod)) : 0;
        const int min_level = mip ? std::min(max_level, std::max(0, (int)tu.min_lod)) : 0;
        const int bias = std::max(-0x1000, std::min(0xFFF, (int)lroundf((tu.lod_bias + s_lod_bias_add) * 256.0f)));
        ur.on = true;
        ur.regs[0] = 0;
        ur.regs[1] = t.dim;
        ur.regs[2] = GPU_TEXTURE_MAG_FILTER(tu.mag_linear ? GPU_LINEAR : GPU_NEAREST) |
                     GPU_TEXTURE_MIN_FILTER(tu.min_linear ? GPU_LINEAR : GPU_NEAREST) |
                     GPU_TEXTURE_MIP_FILTER(tu.mip == 2 ? GPU_LINEAR : GPU_NEAREST) |
                     GPU_TEXTURE_WRAP_S(tu.wrap_s) | GPU_TEXTURE_WRAP_T(tu.wrap_t) |
                     (t.format == fmt::TF_ETC1 ? GPU_TEXTURE_ETC1_PARAM : 0) | GPU_TEXTURE_MODE(type);
        ur.regs[3] = ((u32)bias & 0x1FFF) | (u32)max_level << 16 | (u32)min_level << 24;
        ur.regs[4] = t.addr;
        ur.regs[5] = t.format;
        ur.sx = t.sx, ur.sy = t.sy;
        if (proj)
            mr.bools |= 1u << (R.b_proj0 + u), mr.projects = true;
        env |= tu.source == g3::TexCoordSource::ENV_CAMERA;
        if (u == 1)
            mr.bools |= 1u << R.b_tc1on;
        if (u == 2)
            mr.bools |= 1u << R.b_tc2on;
    }
    const g3::Lighting& L = m.light;
    if (L.lit || L.alpha_lit)
        mr.bools |= 1u << R.b_lit;
    if (env || L.lit || L.alpha_lit)
        mr.bools |= 1u << R.b_usenrm;
    // the pixel pipeline (PixelState is in the GPU's encodings; depth as the GX path sets it: the
    // test always on, ALWAYS for GX's "off", written only with it)
    const g3::PixelState& px = m.pixel;
    mr.pixel[0] = (u32)px.alpha_test | (u32)(px.alpha_func & 7) << 4 | (u32)px.alpha_ref << 8;
    const u32 mask = (px.color_write ? GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE : 0) |
                     (px.alpha_write ? GPU_WRITE_ALPHA : 0) | (px.depth_test && px.depth_write ? GPU_WRITE_DEPTH : 0);
    mr.pixel[1] = 1 | (u32)(px.depth_test ? px.depth_func & 7 : GPU_ALWAYS) << 4 | mask << 8;
    if (px.blend)
        mr.pixel[2] = (u32)px.blend_eq | (u32)px.blend_eq << 8 | (u32)px.blend_src << 16 | (u32)px.blend_dst << 20 |
                      (u32)px.blend_src << 24 | (u32)px.blend_dst << 28;
    else
        mr.pixel[2] =
            GPU_BLEND_ADD | GPU_BLEND_ADD << 8 | GPU_ONE << 16 | GPU_ZERO << 20 | GPU_ONE << 24 | GPU_ZERO << 28;
    mr.pixel[3] = px.logic ? 0xE40000 : 0xE40100;  // (logic op, else blending; GL fragment ops)
    mr.pixel[4] = px.logic ? (u32)px.logic_op : (u32)GPU_LOGICOP_COPY;
    mr.pixel[5] = px.cull & 3;
}

// the material's colours and lights: colour channel 0 as GX computes it (register or vertex
// material, the light set's ambient and lights, or the register's ambient)
void PicaBackend::material_uniforms(const g3::Packet& p, const g3::Material& m, const g3::MatDynamic* dyn) {
    const g3::Lighting& L = m.light;
    const u8* mc = dyn ? dyn->mat_color : L.mat_color;
    const u8* ac = dyn ? dyn->amb_color : L.amb_color;
    const int set = L.light_set >= 0 && (u32)L.light_set < p.light_sets.size() ? L.light_set : -1;
    if (set >= 0)
        ac = p.light_sets[(u32)set].ambient;
    const f32 mv = L.mat_from_vertex ? 1.0f : 0.0f, mva = L.alpha_mat_from_vertex ? 1.0f : 0.0f;
    const f32 av = L.amb_from_vertex ? 1.0f : 0.0f;
    uni(R.matc, (1 - mv) * u8f(mc[0]), (1 - mv) * u8f(mc[1]), (1 - mv) * u8f(mc[2]), (1 - mva) * u8f(mc[3]));
    uni(R.selmat, mv, mv, mv, mva);
    uni(R.ambc, (1 - av) * u8f(ac[0]), (1 - av) * u8f(ac[1]), (1 - av) * u8f(ac[2]), (1 - av) * u8f(ac[3]));
    uni(R.selamb, av, av, av, av);
    const f32 lc = L.lit ? 1.0f : 0.0f;
    uni(R.sellit, lc, lc, lc, L.alpha_lit ? 1.0f : 0.0f);
    // N.L clamped (2), signed (1), or none: 1 (0)
    if (L.diffuse == 0)
        uni(R.lcfg, 0, 0, 1, 0);
    else
        uni(R.lcfg, L.diffuse == 1 ? -1e30f : 0.0f, 1, 0, 0);
    if (L.lit || L.alpha_lit)
        set_lights(p, set);
}

// The texture units' rows. A source (uv0, uv1: its s16 scale folded in; the view normal: nw4r's
// camera env map (0.5 + 0.5 nx, 0.5 - 0.5 ny) folded in) as (s, t, 1, 1) through the material's
// 2x4 matrix; the position for a projection map, (0.5 w + 0.5 x, 0.5 w - 0.5 y, w) in GX's clip
// space through the matrix (q = w); then the texture's padding.
void PicaBackend::texture_rows(const g3::Material& m, const MatRes& mr, const g3::MatDynamic* dyn, const VbRes& vb) {
    for (int u = 0; u < 3; u++) {
        if (!mr.unit[u].on)
            continue;
        const g3::TexUnit& tu = m.unit[u];
        const f32(*T)[4] = tu.matrix >= 0 && tu.matrix < 3 ? (dyn ? dyn->tex_mtx[tu.matrix] : m.tex_mtx[tu.matrix])
                                                            : k_identity24;
        const f32 sc[2] = {mr.unit[u].sx, mr.unit[u].sy};
        f32 rows[3][4] = {};
        f32 sel[4] = {0, 0, 0, 0};
        switch (tu.source) {
        case g3::TexCoordSource::UV0:
        case g3::TexCoordSource::UV1: {
            const int s = tu.source == g3::TexCoordSource::UV1;
            sel[s] = 1;
            for (int i = 0; i < 2; i++) {
                rows[i][0] = sc[i] * T[i][0] * vb.uv_scale[s];
                rows[i][1] = sc[i] * T[i][1] * vb.uv_scale[s];
                rows[i][2] = sc[i] * T[i][2];
                rows[i][3] = sc[i] * T[i][3];
            }
            break;
        }
        case g3::TexCoordSource::ENV_CAMERA:
            sel[2] = 1;
            for (int i = 0; i < 2; i++) {
                rows[i][0] = sc[i] * 0.5f * T[i][0];
                rows[i][1] = sc[i] * -0.5f * T[i][1];
                rows[i][2] = sc[i] * T[i][2];
                rows[i][3] = sc[i] * (T[i][3] + 0.5f * (T[i][0] + T[i][1]));
            }
            break;
        case g3::TexCoordSource::PROJECTION: {
            double c[4][4];  // GX clip space, from the buffer's positions
            mul44_34(proj_, mv_, c);
            for (int j = 0; j < 4; j++) {
                const double S = 0.5 * (c[3][j] + c[0][j]), Tt = 0.5 * (c[3][j] - c[1][j]), Q = c[3][j];
                for (int i = 0; i < 2; i++)
                    rows[i][j] = (f32)(sc[i] * (T[i][0] * S + T[i][1] * Tt + (T[i][2] + T[i][3]) * Q));
                rows[2][j] = (f32)Q;
            }
            break;
        }
        }
        for (int i = 0; i < 3; i++)
            uni(R.tmtx + 3 * u + i, rows[i]);
        uni(R.tsel + u, sel);
    }
}

// ---------------------------------------------------------------- matrices
// model (the buffer's positions, from its origin) -> view: the view (guest units) x S(k) x the
// item's matrix x T(the origin); then the clip matrix and the GX projection; the normal matrix (the
// cofactors: the inverse transpose up to a scale the shader normalises away)
void PicaBackend::item_matrix(const g3::Packet& p, const PkgRes& r, const g3::DrawItem& it, int vbuf, bool sky,
                              bool pulled) {
    const f32* M = &p.matrices[12 * (size_t)it.matrix];
    const f32* c = r.vbs[(u32)vbuf].centre;
    const double k = p.world_scale;
    double A[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            A[i][j] = k * M[4 * i + j];
        A[i][3] = k * ((double)M[4 * i + 3] + (double)M[4 * i] * c[0] + (double)M[4 * i + 1] * c[1] +
                       (double)M[4 * i + 2] * c[2]);
    }
    const double s = pulled ? 1.0 - g3::TRANSLUCENT_DEPTH_PULL : 1.0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            mv_[i][j] = s * ((double)p.view[i][0] * A[0][j] + (double)p.view[i][1] * A[1][j] +
                             (double)p.view[i][2] * A[2][j] + (j == 3 ? (double)p.view[i][3] : 0.0));
    double mvp[4][4];
    mul44_34(cp_, mv_, mvp);
    // the depth out as z' = -(z + w) (begin_packet's depth map takes it back to -z/w: see there)
    for (int j = 0; j < 4; j++)
        mvp[2][j] = -(mvp[2][j] + mvp[3][j]);
    if (sky)  // (drawn without depth: kept at depth 0.5, so that neither near nor far clip it)
        for (int j = 0; j < 4; j++)
            mvp[2][j] = -0.5 * mvp[3][j];
    for (int i = 0; i < 4; i++)
        uni(R.mvp + i, (f32)mvp[i][0], (f32)mvp[i][1], (f32)mvp[i][2], (f32)mvp[i][3]);
    // the cofactors (row i: the cross product of the other two rows), signed by the
    // determinant so that a mirroring matrix keeps its normals outward)
    const double(*m)[4] = mv_;
    double cof[3][3];
    for (int i = 0; i < 3; i++) {
        const int a = (i + 1) % 3, b = (i + 2) % 3;
        for (int j = 0; j < 3; j++) {
            const int c = (j + 1) % 3, d = (j + 2) % 3;
            cof[i][j] = m[a][c] * m[b][d] - m[a][d] * m[b][c];
        }
    }
    const double det = m[0][0] * cof[0][0] + m[0][1] * cof[0][1] + m[0][2] * cof[0][2];
    const double sg = det < 0 ? -1.0 : 1.0;
    for (int i = 0; i < 3; i++)
        uni(R.nmtx + i, (f32)(sg * cof[i][0]), (f32)(sg * cof[i][1]), (f32)(sg * cof[i][2]), 0);
}

// ---------------------------------------------------------------- fog
// GX's linear fog (PERSP_LIN: by view depth, between the fog's start and end) can't go through the
// PICA's fog unit: its 128-entry table is indexed by the depth buffer's value, and with the near
// plane at 1.5 package units a ramp 27,978-65,527 units away is depth 0.999965-0.999996, the last
// half-percent of the last entry, whose value and slope (at most 2 an entry) can't make it. And the
// fogged materials (the sea's) have no combiner stage or texture unit to spare. So the item is drawn
// a second time over itself: the depth test as its own and no depth or alpha written, its colour
// the fog's, its alpha the fog amount from the ramp texture -- s, affine in the position,
// interpolated per pixel with perspective, is the view depth's place between start and end
// exactly -- blended over what the first pass left: (1 - f) colour + f fog, GX's fog of an opaque
// surface (of a translucent one, f of what is behind it is fogged too). An item wholly nearer than
// the fog's start gets none.
bool PicaBackend::fog_rows(const g3::Packet& p, const g3::GxView& view, const g3::Material& m, const fmt::Batch& b,
                           const VbRes& vb, bool pulled, f32 rows[3][4], u32* color) {
    if (!m.fog || view.orthographic || m.fog_index >= p.fogs.size())
        return false;
    const fmt::Fog& f = p.fogs[m.fog_index];
    if (f.type != fmt::FOG_LINEAR || !(f.end > f.start))
        return false;
    const double k = p.world_scale, start = f.start * k, end = f.end * k;
    // the view's z row (a translucent item's model-view is pulled toward the eye: not its fog)
    const double unpull = pulled ? 1.0 / (1.0 - g3::TRANSLUCENT_DEPTH_PULL) : 1.0;
    double zrow[4];
    for (int j = 0; j < 4; j++)
        zrow[j] = mv_[2][j] * unpull;
    double zmax = -1e300;  // the box's furthest corner (view depth: -z)
    for (int i = 0; i < 8; i++) {
        const double x = (double)b.bbox[i & 1 ? 3 : 0] - vb.centre[0], y = (double)b.bbox[i & 2 ? 4 : 1] - vb.centre[1],
                     z = (double)b.bbox[i & 4 ? 5 : 2] - vb.centre[2];
        zmax = std::max(zmax, -(zrow[0] * x + zrow[1] * y + zrow[2] * z + zrow[3]));
    }
    if (zmax < start)
        return false;
    // s = 1/16 + 14/16 (depth - start) / (end - start), depth = -(the view's z row . position)
    const double a = (14.0 / 16.0) / (end - start);
    for (int j = 0; j < 4; j++)
        rows[0][j] = (f32)(-a * zrow[j]);
    rows[0][3] += (f32)(1.0 / 16.0 - a * start);
    const f32 r1[4] = {0, 0, 0, 0.5f}, r2[4] = {0, 0, 0, 1};
    memcpy(rows[1], r1, sizeof(r1)), memcpy(rows[2], r2, sizeof(r2));
    *color = (u32)f.rgba[0] | (u32)f.rgba[1] << 8 | (u32)f.rgba[2] << 16 | 0xFF000000u;
    return true;
}

void PicaBackend::fog_pass(const g3::Material& m, const fmt::Batch& b, const PkgRes& r, const f32 rows[3][4],
                           u32 color) {
    // the combiners: the fog's colour, the ramp's alpha; the rest pass it on
    tev::Combiners comb;
    for (int i = 0; i < 6; i++) {
        tev::CombinerStage& s = comb.stage[i];
        memset(&s, 0, sizeof(s));
        for (int k = 0; k < 3; k++)
            s.src_rgb[k] = s.src_a[k] = tev::SRC_PREVIOUS;
        s.func_rgb = s.func_a = tev::FUNC_REPLACE;
    }
    comb.stage[0].src_rgb[0] = tev::SRC_CONSTANT, comb.stage[0].src_a[0] = tev::SRC_TEXTURE0;
    comb.stage[0].konst = color;
    comb.buf_rgb = comb.buf_a = 0;
    set_texenv(comb);
    UnitRes units[3];
    units[0].on = true;
    memcpy(units[0].regs, s_fog_regs, sizeof(s_fog_regs));
    set_units(units);
    const g3::PixelState& px = m.pixel;
    const u32 pixel[6] = {
        0,  // no alpha test
        1 | (u32)(px.depth_test ? px.depth_func & 7 : GPU_ALWAYS) << 4 |
            (u32)(GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE) << 8,
        GPU_BLEND_ADD | GPU_BLEND_ADD << 8 | GPU_SRC_ALPHA << 16 | GPU_ONE_MINUS_SRC_ALPHA << 20 | GPU_ZERO << 24 |
            (u32)GPU_ONE << 28,
        0xE40100, GPU_LOGICOP_COPY, (u32)(px.cull & 3)};
    set_pixel(pixel);
    for (int i = 0; i < 3; i++)
        uni(R.tmtx + i, rows[i]);
    set_bools(1u << R.b_proj0);
    draw_batch(r, b);
    // (the next item sends its material's state and rows again: the per-register caches skip what
    // is the same)
    mat_key_ = ~0u;
    rows_vbuf_ = -1;
}

// ---------------------------------------------------------------- draw
void PicaBackend::draw(const g3::Packet& p, const g3::GxView& view) {
    PkgRes* r = p.package ? (PkgRes*)p.package->backend_data : nullptr;
    if (!r || !r->ok || p.items.empty() || !p.materials)
        return;
    if (r->materials != p.materials) {  // (another World's materials: made again)
        for (MatRes& mr : r->mats)
            mr.made = false;
        r->materials = p.materials;
    }
    f32 clip[4][4];
    pica_native_begin(clip);
    const u64 t0 = svcGetSystemTick();
    u32 *cmd, cmd_size, cmd_start;
    GPUCMD_GetBuffer(&cmd, &cmd_size, &cmd_start);
    // the packet's: clip x projection; the lights toward them in view space
    double cl[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            cl[i][j] = clip[i][j], proj_[i][j] = view.projection[i][j];
    mul44(cl, proj_, cp_);
    lights_.assign(8 * p.lights.size(), 0.0f);
    for (size_t i = 0; i < p.lights.size(); i++) {
        const g3::Packet::Light& l = p.lights[i];
        f32 v[3];
        for (int j = 0; j < 3; j++)
            v[j] = -(p.view[j][0] * l.dir[0] + p.view[j][1] * l.dir[1] + p.view[j][2] * l.dir[2]);
        const f32 n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        f32* o = &lights_[8 * i];
        for (int j = 0; j < 3; j++)
            o[j] = n > 0 ? v[j] / n : 0.0f;
        for (int j = 0; j < 4; j++)
            o[4 + j] = u8f(l.rgba[j]);
    }
    begin_packet(*r);
    const auto batches = p.package->batches();
    for (size_t n = 0; n < p.items.size(); n++) {
        const g3::DrawItem& it = p.items[n];
        u32 *b0, sz, off;
        GPUCMD_GetBuffer(&b0, &sz, &off);
        if (sz - off < ITEM_WORDS + GX_RESERVE_WORDS) {
            st_.items_skipped += (u32)(p.items.size() - n);
            if (!skipped_logged_)
                log("g3d pica: the command list is full: %u of the packet's %u items not drawn",
                    (unsigned)(p.items.size() - n), (unsigned)p.items.size());
            skipped_logged_ = true;
            break;
        }
        if (it.batch >= batches.count || 12 * ((size_t)it.matrix + 1) > p.matrices.size())
            continue;
        const fmt::Batch& b = batches[it.batch];
        if (b.vbuf >= r->vbs.size() || !r->vbs[b.vbuf].ok || !b.n_indices || b.material >= r->mats.size())
            continue;
        const g3::Material& m = p.materials[b.material];
        MatRes& mr = r->mats[b.material];
        if (!mr.made)
            make_material(*r, b.material, m);
        if (!mr.drawable)
            continue;
        const g3::MatDynamic* dyn = it.mat_state < p.mat_states.size() ? &p.mat_states[it.mat_state] : nullptr;
        const bool sky = it.flags & g3::DRAW_SKY;
        const bool pulled = p.pass == g3::Pass::TRANSLUCENT && !sky;
        const VbRes& vb = r->vbs[b.vbuf];
        set_layout(*r, b.vbuf);
        // its matrices
        MatrixKey key;
        key.matrix = it.matrix, key.flags = it.flags, key.pulled = pulled;
        memcpy(key.centre, vb.centre, sizeof(key.centre));
        const bool moved = !(key == mkey_);
        if (moved) {
            item_matrix(p, *r, it, b.vbuf, sky, pulled);
            mkey_ = key;
            st_.matrices++;
        }
        // its material
        const u32 mkey = (u32)b.material << 16 | it.mat_state;
        const bool new_mat = mkey != mat_key_;
        if (new_mat) {
            set_texenv(dyn ? dyn->comb : m.comb);
            set_units(mr.unit);
            material_uniforms(p, m, dyn);
            mat_key_ = mkey;
            st_.mat_changes++;
        }
        u32 pixel[6];
        memcpy(pixel, mr.pixel, sizeof(pixel));
        if (sky)  // (no depth test or write)
            pixel[1] = 1 | (u32)GPU_ALWAYS << 4 | (pixel[1] & 0xF00);
        set_pixel(pixel);
        set_bools(mr.bools | (n_lights_ && (mr.bools >> R.b_lit & 1) ? 1u << R.b_haslights : 0));
        if (new_mat || b.vbuf != rows_vbuf_ || (moved && mr.projects)) {
            texture_rows(m, mr, dyn, vb);
            rows_vbuf_ = b.vbuf;
        }
        draw_batch(*r, b);
        st_.draws++, st_.triangles += b.n_indices / 3, st_.indices += b.n_indices;
        f32 rows[3][4];
        u32 fog_color;
        if (!sky && fog_rows(p, view, m, b, vb, pulled, rows, &fog_color)) {
            fog_pass(m, b, *r, rows, fog_color);
            st_.fog_draws++;
        }
    }
    // unit 0 left an ordinary 2D texture (a projection one samples by texcoord0w, which the GX
    // shader doesn't write)
    if (units_[0][2] != ~0u && ((units_[0][2] >> 28) & 7) == GPU_TEX_PROJECTION)
        GPUCMD_AddWrite(GPUREG_TEXUNIT0_PARAM, units_[0][2] & ~GPU_TEXTURE_MODE(7));
    // the GX path's depth map (citro3d's default, which it never changes)
    GPUCMD_AddWrite(GPUREG_DEPTHMAP_SCALE, f32tof24(-1.0f));
    GPUCMD_AddWrite(GPUREG_DEPTHMAP_OFFSET, f32tof24(0.0f));
    u32 cmd_end;
    GPUCMD_GetBuffer(&cmd, &cmd_size, &cmd_end);
    pica_native_end(&prog_);
    stats_packet(p, *r, svcGetSystemTick() - t0, cmd_end - cmd_start);
}

// ---------------------------------------------------------------- NOMA_G3D_STATS
// (a frame: the World emits one OPAQUE packet a frame, the TRANSLUCENT one after it)
void PicaBackend::stats_packet(const g3::Packet& p, const PkgRes& r, u64 ticks, u32 words) {
    if (stats_every_ <= 0)
        return;
    st_.ticks += ticks, st_.cmd_words += words;
    if (p.pass == g3::Pass::OPAQUE) {
        st_.ticks_max_frame = std::max(st_.ticks_max_frame, st_.ticks_frame);
        st_.ticks_frame = 0;
        st_.frames++;
    }
    st_.ticks_frame += ticks;
    if (p.pass != g3::Pass::TRANSLUCENT || st_.frames < (u32)stats_every_)
        return;
    const double f = st_.frames, ms = 1000.0 / SYSCLOCK_ARM11;
    u32 textures = 0;
    for (const TexRes& t : r.texs)
        textures += t.data != nullptr;
    const double max_ms = (double)std::max(st_.ticks_max_frame, st_.ticks_frame) * ms;
    log("g3d pica: %u frames: %.0f draws (%.0f fog passes), %.0f triangles, %.0f indices a frame; %.2f ms a "
        "frame in the backend (at most %.2f); %.0f material changes, %.0f matrices, %.0f uniform vectors, %.0f "
        "command words a frame; %u items not drawn (command list full; the last frame's list %.0f %% of it); "
        "resident: %u textures %u KB, vertices %u KB, indices %u KB; %u KB of the linear heap free",
        (unsigned)st_.frames, st_.draws / f, st_.fog_draws / f, st_.triangles / f, st_.indices / f,
        (double)st_.ticks * ms / f, max_ms, st_.mat_changes / f, st_.matrices / f, st_.uniform_vectors / f,
        st_.cmd_words / f, (unsigned)st_.items_skipped, (double)C3D_GetCmdBufUsage() * 100.0, (unsigned)textures,
        (unsigned)(r.tex_bytes >> 10), (unsigned)(r.vtx_bytes >> 10), (unsigned)(r.idx_bytes >> 10),
        (unsigned)(linearSpaceFree() >> 10));
    st_ = Stats();
}

PicaBackend s_backend;
bool s_backend_made, s_backend_ok;

}  // namespace

engine::g3d::Backend* g3d_pica_backend() {
    if (!s_backend_made) {
        s_backend_made = true;
        s_backend_ok = s_backend.init();
        if (!s_backend_ok)
            log("g3d pica: the backend couldn't be made: native draws aren't drawn");
    }
    return s_backend_ok ? &s_backend : nullptr;
}

}  // namespace guest
