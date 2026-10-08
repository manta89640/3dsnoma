// The native g3d on the 3DS GPU: engine/g3d's Backend (native_draw.h) for the PICA200. A World's
// packets are drawn into the GX renderer's frame (gx_pica.cpp) at the point of the GX stream their
// command is at, into the depth buffer the GX draws (the Mii) use and through the GX projection set
// there, so that the two meet as if one renderer drew both.
//
// prepare() puts a package where the GPU reads it, in linear memory: each texture with all its
// levels (packages are baked: PICA formats, tiled, the levels one after the other, as the GPU
// samples them), the vertex data in one block and the u16 indices in another, flushed from the
// CPU's cache. The attribute buffers' offsets count from the lower of the two blocks (they have 28
// bits; the GX path's base is its own buffers'). Positions are relative to an origin of their
// buffer's, which the draw's matrix puts back: compact ones (fmt::VA_POS16, the version 2 island's)
// are s16 with the buffer's own origin and scale, read by the GPU as shorts, exact in its 24-bit
// floats; f32 ones are moved at upload to an origin -- a cell's buffers to the cell's centre, a
// model's to its box's: the GPU's floats keep 16 bits of mantissa, a step of 0.06-0.25 package units
// at the island's coordinates (0.016-0.03 from a cell's centre), and the buffers of a cell share
// their origin so that an overlay and the ground under it (_blend layers) round alike. (An s16
// position quantised with its buffer's scale is off by at most half a step, which the stager keeps
// far below the 0.03 units an overlay lies above the ground.)
//
// draw() writes the GPU's registers directly, its own shader program (shaders/g3d.v.pica)
// included, between gx_pica.cpp's pica_native_begin and pica_native_end, which bring the GX path's
// frame up to date before and take nothing it kept about the GPU as still true after. Per item:
// the vertex buffer's attribute layout and offset; the uniforms (the model-view-projection, the
// normal matrix -- only for a material that reads normals: not a prelit one --, the material's
// colours and its light set's lights in view space, the texture matrices with the coordinates' scale
// and nw4r's env and projection mappings folded in); the material's combiners (engine/gx/tev_combiner:
// the GX path's own translation), textures (each unit with the material's filters, wrap and LOD),
// the pixel pipeline (alpha test, blending, depth, culling); the draw. The matrices are made once a
// packet for each (item matrix, buffer origin and scale): the model-view in double precision (its
// translation is the island's coordinates less the eye's), the clip rows from it in float (the
// complemented depth's row made in double once a packet: see begin_packet), kept in a small table
// that the cell's other items find them in (the World groups the opaque items by material, so a
// cell's items aren't next to each other). Nothing the GPU already has from the item before is sent
// again. Fog is a second pass of the item (fog_pass), for the materials that have it (the version 2
// island's have none). The command list is gx_pica's, and an item is drawn only with room left in it
// for the GX draws to come (ITEM_WORDS).
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
extern "C" const uint8_t g3d_sea_shbin[];
extern "C" const size_t g3d_sea_shbin_size;

namespace guest {

namespace {

namespace g3 = engine::g3d;
namespace fmt = engine::g3d::fmt;
namespace tev = engine::gx;

// ---------------------------------------------------------------- vertex layouts
// The layouts the format allows (fmt::VertexBuffer: attrs, flags, stride) as the attribute loaders
// take them: the attributes the buffer has, in its order (position f32 x3 or s16 x4 -- the compact
// one's pad as w, which the shader doesn't read --, normal s8 x4 -- its pad byte as w, unread --,
// colour u8 x4, uv0 and uv1 s16 or f32 x2), then fixed values for the shader inputs it lacks (the
// normal up, the colour white, the coordinates 0).
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
    const bool pos16 = attrs & fmt::VA_POS16;
    const In inputs[5] = {
        {pos16 ? (u8)fmt::VA_POS16 : (u8)fmt::VA_POS, pos16 ? (u8)GPU_SHORT : (u8)GPU_FLOAT, pos16 ? (u8)4 : (u8)3,
         {0, 0, 0, 1}},
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
    f32 origin[3] = {};      // its positions are from here (package units) ...
    f32 scale = 1;           // ... in steps of this (compact positions: the buffer's; f32 ones: 1)
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
    u32 pixel[8] = {};        // alpha test, depth and colour mask, blend, colour operation, logic op, cull,
                              // stencil test and operation (off: the sea's items set them)
};
struct PkgRes {
    bool ok = false;
    void* vtx = nullptr;
    void* idx = nullptr;
    u32 vtx_bytes = 0, idx_bytes = 0;
    u32 base = 0;             // the attribute buffers' base (physical)
    u32 idx_offset = 0;       // the indices' offset from it
    // the shadow map's clear (shadow_pass): a quad over the whole target, f32 positions in clip space,
    // after the package's vertices (its offset from the base) and indices (its first index)
    u32 quad_offset = 0, quad_first = 0;
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
// the sea's wave grid's program's (shaders/g3d_sea.v.pica)
struct SeaRegs {
    int mvp, tmtx, wkx, wkz, wph, wamp, wfa, wfb, eye, sun, sunr, shade;
} SR;
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

// The commands, written in place: libctru's GPUCMD_Add, a call and a copy loop for every register
// written (26 calls an item; with newlib's byte-wise memcmp in the caches' compares, nearly half of the
// backend's instructions on the 3DS), as inline writes of the same words -- the first parameter, the
// header with the count less one, the other parameters, a zero to an even count, more than 256
// parameters split as GPUCMD_Add splits them -- and the same stop (svcBreak) when the list has no room.
inline void cmd_check(u32 n) {
    if (gpuCmdBufOffset + n + 1 > gpuCmdBufSize)
        svcBreak(USERBREAK_PANIC);
}
// one register (mask: its bytes written)
inline void cmd_one(u32 header, u32 value) {
    cmd_check(1);
    u32* b = gpuCmdBuf + gpuCmdBufOffset;
    b[0] = value, b[1] = header;
    gpuCmdBufOffset += 2;
}
inline void cmd_write(u32 reg, u32 value) { cmd_one(GPUCMD_HEADER(0, 0xF, reg), value); }
inline void cmd_masked(u32 reg, u32 mask, u32 value) { cmd_one(GPUCMD_HEADER(0, mask, reg), value); }
// n parameters (n >= 1) to one register, or to it and the ones after it (incremental)
inline void cmd_add(u32 header, const u32* param, u32 n) {
    do {
        const u32 k = n > 256 ? 256 : n;
        cmd_check(k);
        u32* b = gpuCmdBuf + gpuCmdBufOffset;
        b[0] = param[0];
        b[1] = header | (k - 1) << 20;
        for (u32 i = 1; i < k; i++)
            b[1 + i] = param[i];
        u32 len = k + 1;
        if (!(k & 1))
            b[len++] = 0;
        gpuCmdBufOffset += len;
        param += k, n -= k;
        if (header >> 31)
            header += k;
    } while (n);
}
inline void cmd_writes(u32 reg, const u32* values, u32 n) { cmd_add(GPUCMD_HEADER(0, 0xF, reg), values, n); }
inline void cmd_incremental(u32 reg, const u32* values, u32 n) { cmd_add(GPUCMD_HEADER(1, 0xF, reg), values, n); }
// the state caches' compares, a word at a time
template <u32 N>
inline bool same_words(const u32 (&a)[N], const u32 (&b)[N]) {
    u32 d = 0;
    for (u32 i = 0; i < N; i++)
        d |= a[i] ^ b[i];
    return d == 0;
}

// The command list is gx_pica's, one for the frame (C3D_Init's 2 MB), and a full one stops the
// program (cmd_check, as GPUCMD_Add): an item isn't drawn unless the list keeps room for the most
// words an item takes (its state, uniforms, draw and fog pass, ~600) and for the GX draws still to
// come in the frame (the room's whole frame fits in far less). The island's packets take at most 62,000
// words, 24,000 a frame on average (host check, 21 views). NOMA_G3D_STATS counts the items not
// drawn; the first is logged.
const u32 ITEM_WORDS = 2048, GX_RESERVE_WORDS = 64u << 10;

// The vertex data made each frame (the sea's: the wave grid's amplitudes, the boats' trails: native_draw.h
// DRAW_SEA_GRID, DRAW_DYNAMIC), copied into a ring in linear memory where the GPU reads it, flushed
// from the CPU's cache. A frame takes about 14 KB (9 KB of the grid's amplitudes, 4.6 KB of trails), so
// a part of the ring is written again five frames later at the soonest: the GPU has drawn the frame
// that read it by then (gx_pica.cpp's frame start waits for the frame before).
const u32 RING_BYTES = 64u << 10;

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
    // the sea's wave grid's program, and the ring the frame's vertex data goes into
    DVLB_s* sea_dvlb_ = nullptr;
    shaderProgram_s sea_prog_;
    bool sea_ok_ = false;
    u8* ring_ = nullptr;
    u32 ring_at_ = 0;
    u32 base_ = 0;         // the attribute buffers' base now (the package's, or lower for the ring's data)
    u32 idx_base_ = 0;     // the package's indices from it

    // ---- what the GPU has in this packet (sent again only when it changes; ~0: unknown)
    u32 uv_[96][4];        // float uniforms as the GPU takes them (w, z, y, x)
    u64 dirty_lo_ = 0;     // c0-c63
    u32 dirty_hi_ = 0;     // c64-c95
    u32 bools_ = ~0u, int0_ = ~0u;
    u32 texenv_[6][5];
    u32 texenv_buf_ = ~0u;
    u32 units_[3][6];
    u32 unit_config_ = ~0u;
    u32 tex_shadow_ = ~0u;  // GPUREG_TEXUNIT0_SHADOW
    u32 pixel_[8];
    int layout_ = -1, vbuf_ = -1;
    int light_set_ = -2;   // whose lights the uniforms hold
    u32 n_lights_ = 0;
    // ---- the matrices: what they are made from (the item's matrix, its buffer's origin and scale,
    // which a cell's buffers share, the sky's flag, an overlay's pull) ...
    struct MatrixKey {
        int matrix = -1;
        f32 origin[3] = {};
        f32 scale = 1;
        u16 flags = 0;
        double pull = 0;  // an overlay's (native_draw.h overlay_pull)
        bool operator==(const MatrixKey& o) const {
            return matrix == o.matrix && origin[0] == o.origin[0] && origin[1] == o.origin[1] &&
                   origin[2] == o.origin[2] && scale == o.scale && flags == o.flags && pull == o.pull;
        }
        u32 hash() const {
            u32 h = (u32)matrix * 0x9E3779B1u ^ flags;
            u32 b[4];
            memcpy(b, origin, 12), memcpy(&b[3], &scale, 4);
            for (u32 v : b)
                h = (h ^ v) * 0x01000193u;
            return h ^ (h >> 15);
        }
    };
    // ... and what they are: model (the buffer's positions) -> view in the guest's units (double),
    // the clip rows (the complemented depth's in row 2), the normal rows (made when first wanted)
    struct MatrixEntry {
        MatrixKey key;
        bool valid = false, has_normals = false;
        double mv[3][4];
        f32 mvp[4][4];
        f32 nmtx[3][4];
    };
    static constexpr u32 MATRIX_TABLE = 64;  // (a packet's cells and nodes, hashed; a clash makes one again)
    MatrixEntry mtable_[MATRIX_TABLE];
    MatrixEntry* mcur_ = nullptr;  // the item's (what the uniforms hold)
    MatrixKey mkey_;               // ... made for this key
    u64 mat_key_ = ~0ull;  // material << 16 | its state, and its shadow variant << 32
    f32 rows_uv_[2] = {};  // the coordinates' scale the texture rows were made with

    // ---- the packet's: the clip matrix times the GX projection (double, and in float with the
    // complemented depth's row), the GX projection, the lights
    double cp_[4][4], proj_[4][4];
    f32 cpf_[4][4];
    std::vector<f32> lights_;     // per packet light: direction toward it (view space), colour
    // ---- the item's: model (the buffer's positions) -> view, in the guest's units
    double mv_[3][4];

    // ---- the shadow map (native_draw.h ShadowMap): a shadow texture (in VRAM if there is room) the
    // opaque packet's casters are drawn into, its frame buffer; the map's parameters and whether this
    // frame's is drawn; the decals' fade window (an A8 texture), the units' registers for both
    C3D_Tex shadow_tex_;
    C3D_FrameBuf shadow_fb_;
    u32 shadow_size_ = 0;          // (the texture's; 0: none made)
    bool shadow_failed_ = false;
    bool shadow_ready_ = false;
    g3::ShadowMap smap_;
    UnitRes shadow_unit_, fade_unit_;
    u32 shadow_word_ = 1;          // GPUREG_TEXUNIT0_SHADOW for the receivers: orthographic, the bias
    void* fade_tex_ = nullptr;

    // ---- NOMA_G3D_STATS
    int stats_every_ = 0;
    struct Stats {
        u32 frames, draws, fog_draws, prelit_draws, triangles, indices, items_skipped, sea_draws, dynamic_draws;
        u32 mat_changes, matrices, matrices_made, uniform_vectors, cmd_words;
        u32 shadow_maps, shadow_draws, shadow_triangles, shadow_receivers, shadow_faded;
        u64 ticks, ticks_max_frame, ticks_frame, shadow_ticks;
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
    void configure(shaderProgram_s* prog);
    u32 ring_put(const void* data, u32 size);
    void use_base(u32 base, const PkgRes& r);
    bool draw_sea_grid(const g3::Packet& p, const PkgRes& r, const g3::DrawItem& it, const g3::Material& m,
                       MatRes& mr);
    void projection_rows(const f32 (*T)[4], f32 sx, f32 sy, f32 rows[3][4]) const;
    void set_layout(const PkgRes& r, int vbuf, u32 dynamic_pa = 0);
    void set_texenv(const tev::Combiners& comb);
    void set_units(const UnitRes units[3], u32 tex_shadow = 1);
    void set_pixel(const u32 pixel[8]);
    void draw_batch(const PkgRes& r, const fmt::Batch& b) { draw_indices(r, b.first_index, b.n_indices); }
    void draw_indices(const PkgRes& r, u32 first, u32 count);
    void forget_state();
    bool make_shadow(u32 size);
    void shadow_pass(const g3::Packet& p, PkgRes& r);
    void position_rows(const g3::Packet& p, int matrix, const VbRes& vb, const f32 (*rows)[4], u32 n, int slot);
    void make_material(PkgRes& r, u32 index, const g3::Material& m);
    MatrixEntry& matrices(const g3::Packet& p, const MatrixKey& key);
    void normal_rows(MatrixEntry& e);
    void material_uniforms(const g3::Packet& p, const g3::Material& m, const g3::MatDynamic* dyn);
    void texture_rows(const g3::Material& m, const MatRes& mr, const g3::MatDynamic* dyn, const VbRes& vb,
                      const int slot[3] = nullptr);
    bool fog_rows(const g3::Packet& p, const g3::GxView& view, const g3::Material& m, const fmt::Batch& b,
                  const VbRes& vb, double pull, f32 rows[3][4], u32* color);
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
    // the sea's program (without it, or without the ring, the sea's grid isn't drawn: the flat sea is)
    sea_dvlb_ = DVLB_ParseFile((u32*)g3d_sea_shbin, (u32)g3d_sea_shbin_size);
    ring_ = (u8*)linearAlloc(RING_BYTES);
    if (sea_dvlb_ && ring_) {
        shaderProgramInit(&sea_prog_);
        shaderProgramSetVsh(&sea_prog_, &sea_dvlb_->DVLE[0]);
        shaderInstance_s* sv = sea_prog_.vertexShader;
        auto sat = [&](const char* name) { return (int)shaderInstanceGetUniformLocation(sv, name); };
        SR.mvp = sat("mvp"), SR.tmtx = sat("tmtx"), SR.wkx = sat("wkx"), SR.wkz = sat("wkz"), SR.wph = sat("wph");
        SR.wamp = sat("wamp"), SR.wfa = sat("wfa"), SR.wfb = sat("wfb"), SR.eye = sat("eye"), SR.sun = sat("sun");
        SR.sunr = sat("sunr"), SR.shade = sat("shade");
        sea_ok_ = true;
        for (int reg : {SR.mvp, SR.tmtx, SR.wkx, SR.wkz, SR.wph, SR.wamp, SR.wfa, SR.wfb, SR.eye, SR.sun, SR.sunr, SR.shade})
            sea_ok_ &= reg >= 0 && reg < 95;
        if (!sea_ok_)
            log("g3d pica: the sea's program's uniforms are not all there: the sea is drawn flat");
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
    // the decals' fade window (native_draw.h shadow_fade_alpha): A8, SHADOW_FADE_TEXELS square in 8x8
    // tiles of Z order (it is the same flipped and turned: the tiles' and the rows' order don't matter)
    const u32 fn = g3::SHADOW_FADE_TEXELS;
    if (!(fade_tex_ = linearAlloc(fn * fn)))
        return false;
    u8* ft = (u8*)fade_tex_;
    for (u32 y = 0; y < fn; y++)
        for (u32 x = 0; x < fn; x++) {
            const u32 tile = (y >> 3) * (fn >> 3) + (x >> 3);
            ft[tile * 64 + ((x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3)] =
                (u8)lroundf(255.0f * g3::shadow_fade_alpha(((f32)x + 0.5f) / fn, ((f32)y + 0.5f) / fn));
        }
    GSPGPU_FlushDataCache(fade_tex_, fn * fn);
    fade_unit_.on = true;
    fade_unit_.regs[0] = 0;
    fade_unit_.regs[1] = fn | fn << 16;
    fade_unit_.regs[2] = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                         GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE) |
                         GPU_TEXTURE_MODE(GPU_TEX_2D);
    fade_unit_.regs[3] = 0;
    fade_unit_.regs[4] = osConvertVirtToPhys(fade_tex_) >> 3;
    fade_unit_.regs[5] = GPU_A8;
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
    const u32 vtx_data = hi > lo ? (hi - lo + 15) & ~15u : 0, idx_data = (idx.count * 2 + 15) & ~15u;
    r->vtx_bytes = vtx_data + 64;  // (the quad's 4 vertices, 12 bytes each)
    r->idx_bytes = idx_data + 16;  // (its 6 indices)
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
    if (hi > lo)
        memcpy(r->vtx, pkg.at(lo), hi - lo);
    memcpy(r->idx, idx.data, idx.count * 2);
    static const f32 k_quad[12] = {-1, -1, -0.5f, 1, -1, -0.5f, 1, 1, -0.5f, -1, 1, -0.5f};
    static const u16 k_quad_idx[6] = {0, 1, 2, 0, 2, 3};
    memcpy((u8*)r->vtx + vtx_data, k_quad, sizeof(k_quad));
    memcpy((u8*)r->idx + idx_data, k_quad_idx, sizeof(k_quad_idx));
    const u32 vpa = osConvertVirtToPhys(r->vtx), ipa = osConvertVirtToPhys(r->idx);
    r->base = std::min(vpa, ipa) & ~0xFu;
    r->idx_offset = ipa - r->base;
    r->quad_offset = vpa + vtx_data - r->base;
    r->quad_first = idx_data / 2;
    // ---- each buffer's origin: a compact one's own; for f32 positions its cell's centre (the first
    // cell that draws it), else its box's
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
                    r->vbs[v].origin[k] = (c.bbox[k] + c.bbox[3 + k]) * 0.5f;
            }
    u32 compact = 0;
    for (u32 v = 0; v < vbs.count; v++) {
        const fmt::VertexBuffer& vb = vbs[v];
        VbRes& vr = r->vbs[v];
        vr.ok = (vb.attrs & (fmt::VA_POS | fmt::VA_POS16)) && vb.count;
        if (!vr.ok)
            continue;
        vr.offset = vpa + (vb.data - lo) - r->base;
        vr.layout = layout_of(vb.attrs, vb.flags, vb.stride);
        const bool f = vb.flags & fmt::VB_UV_FLOAT;
        vr.uv_scale[0] = f ? 1.0f : vb.uv_scale[0], vr.uv_scale[1] = f ? 1.0f : vb.uv_scale[1];
        if (vb.attrs & fmt::VA_POS16) {  // (as they are: s16 x scale + origin)
            memcpy(vr.origin, vb.pos_origin, sizeof(vr.origin));
            vr.scale = vb.pos_scale;
            compact++;
            continue;
        }
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
                vr.origin[k] = (b[k] + b[3 + k]) * 0.5f;
        }
        for (u32 i = 0; i < vb.count; i++) {
            f32 q[3];
            memcpy(q, p + i * vb.stride, 12);
            for (int k = 0; k < 3; k++)
                q[k] -= vr.origin[k];
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
    u32 prelit = 0;
    for (const fmt::Material& m : pkg.materials())
        prelit += (m.flags & fmt::MAT_PRELIT) != 0;
    log("g3d pica: package prepared in %.1f ms: %u textures, %u KB (RGBA8 %u, RGB565 %u, RGBA5551 %u, LA8 %u, L8 %u, "
        "LA4 %u, L4 %u, A8 %u, ETC1 %u, ETC1A4 %u)%s; vertices %u KB in %u buffers (%u compact), indices %u KB; "
        "%u of %u materials prelit; %u KB of the linear heap free",
        (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11, (unsigned)(r->texs.size() - r->tex_missing),
        (unsigned)(r->tex_bytes >> 10), (unsigned)kb_fmt[fmt::TF_RGBA8], (unsigned)kb_fmt[fmt::TF_RGB565],
        (unsigned)kb_fmt[fmt::TF_RGBA5551], (unsigned)kb_fmt[fmt::TF_LA8], (unsigned)kb_fmt[fmt::TF_L8],
        (unsigned)kb_fmt[fmt::TF_LA4], (unsigned)kb_fmt[fmt::TF_L4], (unsigned)kb_fmt[fmt::TF_A8],
        (unsigned)kb_fmt[fmt::TF_ETC1], (unsigned)kb_fmt[fmt::TF_ETC1A4],
        r->tex_missing ? " (some not made: no memory)" : "", (unsigned)(r->vtx_bytes >> 10), (unsigned)vbs.count,
        (unsigned)compact, (unsigned)(r->idx_bytes >> 10), (unsigned)prelit, (unsigned)pkg.materials().count,
        (unsigned)(linearSpaceFree() >> 10));
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
    if (same_words(w, uv_[reg]))
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
        cmd_write(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000 | i);
        cmd_writes(GPUREG_VSH_FLOATUNIFORM_DATA, &uv_[i][0], 4 * (j - i));
        st_.uniform_vectors += j - i;
    }
    dirty_lo_ = 0, dirty_hi_ = 0;
}

void PicaBackend::set_bools(u32 bools) {
    if (bools == bools_)
        return;
    bools_ = bools;
    cmd_write(GPUREG_VSH_BOOLUNIFORM, 0x7FFF0000 | bools);
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
        cmd_write(GPUREG_VSH_INTUNIFORM_I0, i0);
    }
}

// a packet's start: the shader program and its constants, the attribute buffers' base, what the
// GPU has taken as unknown
void PicaBackend::begin_packet(const PkgRes& r) {
    configure(&prog_);
    cmd_write(GPUREG_ATTRIBBUFFERS_LOC, r.base >> 3);
    base_ = r.base, idx_base_ = r.idx_offset;
    static const u32 k_zero[33] = {};  // (buffers 1-11: none)
    cmd_incremental(GPUREG_ATTRIBBUFFER1_OFFSET, k_zero, 33);
    cmd_write(GPUREG_TEXENV_BUFFER_COLOR, 0xFFFFFFFF);
    // Both renderers use the complemented clip depth supplied by gx_pica's set_xform. With the
    // old clip depth and citro3d's default map (-1, 0), near 1, as most of the island is
    // (1 - near/distance, near 15 Mii units), that ratio of two nearly equal 24-bit floats is off by
    // up to ~7e-5 -- hundreds of the buffer's steps, half a package unit of distance 300 units away:
    // an overlay and the ground under it come out in either order. The shared clip transform outputs
    // z' = -(z + w) instead (draw: cpf_'s row 2, the GX projection's depth row, applied to the
    // model-view in matrices()), with 16 bits of precision
    // of its own, and this depth map makes 1 + z'/w of it: the same -z/w, as
    // accurate as the buffer (and the same clipping: z' within [-w, 0] as z was). Checked on the
    // host with the GPU's 24-bit floats emulated, over the island's 21 camera views: a point
    // 0.03-0.4 units over a ground vertex and pulled 1/1024 toward the eye came out behind the
    // ground at 23 % of 366,343 ground vertices with z, at none with z' (as with exact arithmetic);
    // overlays are pulled so, at most 4 units at their furthest corner (native_draw.h overlay_pull).
    cmd_write(GPUREG_DEPTHMAP_SCALE, f32tof24(1.0f));
    cmd_write(GPUREG_DEPTHMAP_OFFSET, f32tof24(1.0f));
    forget_state();
}

// nothing the GPU has taken as known (a packet's start; after the shadow pass)
void PicaBackend::forget_state() {
    memset(uv_, 0xFF, sizeof(uv_));  // (a NaN: no value compares equal to it)
    dirty_lo_ = 0, dirty_hi_ = 0;
    bools_ = int0_ = ~0u;
    memset(texenv_, 0xFF, sizeof(texenv_));
    texenv_buf_ = ~0u;
    memset(units_, 0xFF, sizeof(units_));
    unit_config_ = ~0u;
    tex_shadow_ = ~0u;
    memset(pixel_, 0xFF, sizeof(pixel_));
    layout_ = vbuf_ = -1;
    light_set_ = -2;
    for (MatrixEntry& e : mtable_)
        e.valid = false;
    mcur_ = nullptr;
    mkey_ = MatrixKey();
    mat_key_ = ~0ull;
}

// a program: its code, entry point and outputs (shaderProgramConfigure), and its constants
void PicaBackend::configure(shaderProgram_s* prog) {
    shaderProgramConfigure(prog, true, false);
    shaderInstance_s* vsh = prog->vertexShader;
    for (u32 i = 0; i < vsh->numFloat24Uniforms; i++)
        cmd_incremental(GPUREG_VSH_FLOATUNIFORM_CONFIG, (u32*)&vsh->float24Uniforms[i], 4);
}

// the frame's vertex data into the ring (16-byte aligned, from its start again when it doesn't fit),
// flushed: where the GPU reads it (physical), 0 if it can't take it
u32 PicaBackend::ring_put(const void* data, u32 size) {
    if (!ring_ || !size || size > RING_BYTES)
        return 0;
    if (ring_at_ + size > RING_BYTES)
        ring_at_ = 0;
    u8* at = ring_ + ring_at_;
    memcpy(at, data, size);
    GSPGPU_FlushDataCache(at, size);
    ring_at_ = (ring_at_ + size + 15) & ~15u;
    return osConvertVirtToPhys(at);
}

// The attribute buffers' base: the package's, or lower, under the ring's data (the buffers' offsets
// are unsigned, from it); the package's indices from it, its buffers' offsets sent again.
void PicaBackend::use_base(u32 base, const PkgRes& r) {
    if (base == base_)
        return;
    base_ = base;
    idx_base_ = r.base + r.idx_offset - base;
    cmd_write(GPUREG_ATTRIBBUFFERS_LOC, base >> 3);
    vbuf_ = -1;
}

// the vertex buffer: its layout (the loaders, the shader inputs they feed, the fixed values) and
// its place (the package's buffer, or the frame's data at dynamic_pa: DRAW_DYNAMIC)
void PicaBackend::set_layout(const PkgRes& r, int vbuf, u32 dynamic_pa) {
    const VbRes& vb = r.vbs[(u32)vbuf];
    if (dynamic_pa)
        use_base(std::min(r.base, dynamic_pa) & ~0xFu, r);
    else
        use_base(r.base, r);
    const Layout& L = s_layouts[vb.layout];
    if (vb.layout != layout_) {
        layout_ = vb.layout;
        const u32 fmt_words[2] = {L.format_low, L.format_high};
        cmd_incremental(GPUREG_ATTRIBBUFFERS_FORMAT_LOW, fmt_words, 2);
        cmd_masked(GPUREG_VSH_INPUTBUFFER_CONFIG, 0xB, 0xA0000000 | 4);
        cmd_write(GPUREG_VSH_NUM_ATTR, 4);
        const u32 perm[2] = {L.permutation, 0};
        cmd_incremental(GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW, perm, 2);
        for (u32 i = 0; i < L.n_fixed; i++) {
            cmd_write(GPUREG_FIXEDATTRIB_INDEX, L.fixed_id[i]);
            C3D_ImmSendAttrib(L.fixed[i][0], L.fixed[i][1], L.fixed[i][2], L.fixed[i][3]);
        }
        vbuf_ = -1;
    }
    if (dynamic_pa || vbuf != vbuf_) {
        vbuf_ = dynamic_pa ? -1 : vbuf;
        const u32 buf[3] = {dynamic_pa ? dynamic_pa - base_ : r.base + vb.offset - base_, L.buf_config1, L.buf_config2};
        cmd_incremental(GPUREG_ATTRIBBUFFER0_OFFSET, buf, 3);
    }
}

// the combiners (the stages that differ) and their buffer's update masks (fog off: the GPU's fog
// can't place a ramp 28,000 units away in a 24-bit depth buffer: see fog_pass)
void PicaBackend::set_texenv(const tev::Combiners& comb) {
    for (int i = 0; i < 6; i++) {
        u32 w[5];
        tev::combiner_words(comb.stage[i], w);
        if (same_words(w, texenv_[i]))
            continue;
        memcpy(texenv_[i], w, sizeof(w));
        cmd_incremental(GPUREG_TEXENV0_SOURCE + (i >= 4 ? i + 2 : i) * 8, w, 5);
    }
    const u32 buf = (u32)(comb.buf_rgb & 0xF) << 8 | (u32)(comb.buf_a & 0xF) << 12;
    if (buf != texenv_buf_) {
        texenv_buf_ = buf;
        cmd_masked(GPUREG_TEXENV_UPDATE_BUFFER, 0x7, buf);
    }
}

// the texture units (the ones that differ), then which are on, with the texture cache cleared (as
// citro3d does when it binds a texture)
void PicaBackend::set_units(const UnitRes units[3], u32 tex_shadow) {
    bool changed = tex_shadow != tex_shadow_;
    u32 on = 0;
    for (int u = 0; u < 3; u++) {
        if (!units[u].on)
            continue;
        on |= 1u << u;
        if (same_words(units[u].regs, units_[u]))
            continue;
        memcpy(units_[u], units[u].regs, sizeof(units_[u]));
        cmd_incremental(k_unit_border[u], units[u].regs, 5);
        cmd_write(k_unit_type[u], units[u].regs[5]);
        changed = true;
    }
    if (!changed && on == unit_config_)
        return;
    unit_config_ = on;
    cmd_masked(GPUREG_TEXUNIT_CONFIG, 0xB, 1u << 12 | on);
    cmd_masked(GPUREG_TEXUNIT_CONFIG, 0x4, 1u << 16);
    cmd_write(GPUREG_TEXUNIT0_SHADOW, tex_shadow);
    tex_shadow_ = tex_shadow;
}

// the pixel pipeline's registers that differ (as citro3d's effect binding writes them)
void PicaBackend::set_pixel(const u32 px[8]) {
    if (px[0] != pixel_[0])
        cmd_write(GPUREG_FRAGOP_ALPHA_TEST, px[0]);
    if (px[1] != pixel_[1]) {
        cmd_write(GPUREG_DEPTH_COLOR_MASK, px[1]);
        cmd_masked(GPUREG_GAS_DELTAZ_DEPTH, 0x8, (u32)GPU_MAKEGASDEPTHFUNC((px[1] >> 4) & 7) << 24);
    }
    if (px[2] != pixel_[2])
        cmd_write(GPUREG_BLEND_FUNC, px[2]);
    if (px[3] != pixel_[3])
        cmd_masked(GPUREG_COLOR_OPERATION, 0x7, px[3]);
    if (px[4] != pixel_[4])
        cmd_write(GPUREG_LOGIC_OP, px[4]);
    if (px[5] != pixel_[5])
        cmd_write(GPUREG_FACECULLING_CONFIG, px[5]);
    if (px[6] != pixel_[6])
        cmd_write(GPUREG_STENCIL_TEST, px[6]);
    if (px[7] != pixel_[7])
        cmd_write(GPUREG_STENCIL_OP, px[7]);
    memcpy(pixel_, px, sizeof(pixel_));
}

// triangles from u16 indices (as C3D_DrawElements draws GPU_TRIANGLES): a batch's, a caster's run
void PicaBackend::draw_indices(const PkgRes& r, u32 first, u32 count) {
    upload_uniforms();
    pica_native_draw_noted();
    cmd_masked(GPUREG_PRIMITIVE_CONFIG, 2, GPU_GEOMETRY_PRIM);
    cmd_write(GPUREG_RESTART_PRIMITIVE, 1);
    cmd_write(GPUREG_INDEXBUFFER_CONFIG, (idx_base_ + first * 2) | 1u << 31);
    cmd_write(GPUREG_NUMVERTICES, count);
    cmd_write(GPUREG_VERTEX_OFFSET, 0);
    cmd_masked(GPUREG_GEOSTAGE_CONFIG, 2, 0x100);
    cmd_masked(GPUREG_GEOSTAGE_CONFIG2, 2, 0x100);
    cmd_masked(GPUREG_START_DRAW_FUNC0, 1, 0);
    cmd_write(GPUREG_DRAWELEMENTS, 1);
    cmd_masked(GPUREG_START_DRAW_FUNC0, 1, 1);
    cmd_masked(GPUREG_GEOSTAGE_CONFIG, 2, 0);
    cmd_masked(GPUREG_GEOSTAGE_CONFIG2, 2, 0);
    cmd_write(GPUREG_VTX_FUNC, 1);
    cmd_masked(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    cmd_masked(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
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
    mr.pixel[6] = (u32)GPU_ALWAYS << 4;  // (stencil test off)
    mr.pixel[7] = 0;
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
    // N.L clamped (2), signed (1), or none: 1 (0). (Signed: a floor below N.L's -1 that the GPU's
    // 24-bit floats hold -- their largest is ~2^64, and how it takes a uniform beyond is unknown.)
    if (L.diffuse == 0)
        uni(R.lcfg, 0, 0, 1, 0);
    else
        uni(R.lcfg, L.diffuse == 1 ? -2.0f : 0.0f, 1, 0, 0);
    if (L.lit || L.alpha_lit)
        set_lights(p, set);
}

// The texture units' rows. A source (uv0, uv1: its s16 scale folded in; the view normal: nw4r's
// camera env map (0.5 + 0.5 nx, 0.5 - 0.5 ny) folded in) as (s, t, 1, 1) through the material's
// 2x4 matrix; the position for a projection map, (0.5 w + 0.5 x, 0.5 w - 0.5 y, w) in GX's clip
// space through the matrix (q = w); then the texture's padding.
void PicaBackend::texture_rows(const g3::Material& m, const MatRes& mr, const g3::MatDynamic* dyn, const VbRes& vb,
                               const int slot[3]) {
    for (int u = 0; u < 3; u++) {
        if (!mr.unit[u].on || (slot && slot[u] < 0))
            continue;
        const int k = slot ? slot[u] : u;  // (the unit it is sampled on: a receiver's 0 is on 2)
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
        case g3::TexCoordSource::PROJECTION:
            projection_rows(T, sc[0], sc[1], rows);
            break;
        }
        for (int i = 0; i < 3; i++)
            uni(R.tmtx + 3 * k + i, rows[i]);
        uni(R.tsel + k, sel);
    }
}

// A unit's rows of coordinates from the position (the shadow map's s, t and depth, or the fade
// window's s, t and q = 1): world rows times the item's matrix, its buffer's origin and step, in double
// (the island's coordinates less the map's are a few hundred units: what the GPU's 24-bit floats get is
// within a hundredth of a texel)
void PicaBackend::position_rows(const g3::Packet& p, int matrix, const VbRes& vb, const f32 (*rows)[4], u32 n,
                                int slot) {
    const f32* M = &p.matrices[12 * (size_t)matrix];
    for (u32 i = 0; i < 3; i++) {
        f32 out[4] = {0, 0, 0, 1};
        if (i < n) {
            double w[4];
            for (int c = 0; c < 4; c++)
                w[c] = (double)rows[i][0] * M[c] + (double)rows[i][1] * M[4 + c] + (double)rows[i][2] * M[8 + c] +
                       (c == 3 ? (double)rows[i][3] : 0.0);
            w[3] += w[0] * vb.origin[0] + w[1] * vb.origin[1] + w[2] * vb.origin[2];
            for (int c = 0; c < 3; c++)
                out[c] = (f32)(w[c] * vb.scale);
            out[3] = (f32)w[3];
        }
        uni(R.tmtx + 3 * slot + (int)i, out);
    }
    uni(R.tsel + slot, 0, 0, 0, 0);
}

// a camera projection's rows (s, t, q) through the texture matrix T, of the positions the item's
// model-view (mv_) takes: (0.5 w + 0.5 x, 0.5 w - 0.5 y, w) in GX's clip space; the padding's scale
void PicaBackend::projection_rows(const f32 (*T)[4], f32 sx, f32 sy, f32 rows[3][4]) const {
    double c[4][4];  // GX clip space, from the buffer's positions
    mul44_34(proj_, mv_, c);
    const f32 sc[2] = {sx, sy};
    for (int j = 0; j < 4; j++) {
        const double S = 0.5 * (c[3][j] + c[0][j]), Tt = 0.5 * (c[3][j] - c[1][j]), Q = c[3][j];
        for (int i = 0; i < 2; i++)
            rows[i][j] = (f32)(sc[i] * (T[i][0] * S + T[i][1] * Tt + (T[i][2] + T[i][3]) * Q));
        rows[2][j] = (f32)Q;
    }
}

// ---------------------------------------------------------------- the sea's wave grid
// The grid's item (native_draw.h DRAW_SEA_GRID) with its own program (shaders/g3d_sea.v.pica): its
// positions (the package's buffer) and the packet's amplitudes (into the ring) as two attribute buffers;
// its uniforms -- the clip rows of its matrix, the waves (sea.h SeaWaves: the phases moved to the
// buffer's origin, a hair from the grid's centre), the eye and the sun about the centre, the shading's
// weights, and the texture rows: unit 0 the camera projection as any item's, units 1 and 2 the sea's
// texture coordinates at a position (fmt::Sea::uv_map at the centre, its constant term in double and
// brought within a repeat of the texture) through the material's matrices --; the material's
// combiners, units and pixel state, marking what it draws in the stencil buffer (the packet's value:
// the flat sea's items, DRAW_SEA_MASKED, aren't drawn there). Then the g3d program again, with nothing
// it had in the float uniforms, the bools or the attribute layout taken as still there. False (not
// drawn: the flat sea shows) without the program, the ring or the packet's amplitudes.
bool PicaBackend::draw_sea_grid(const g3::Packet& p, const PkgRes& r, const g3::DrawItem& it, const g3::Material& m,
                                MatRes& mr) {
    const fmt::Sea* sp = p.package->sea();
    const fmt::Batch& b = p.package->batches()[it.batch];
    const VbRes& vb = r.vbs[b.vbuf];
    const u32 n = p.package->vertex_buffers()[b.vbuf].count;
    if (!sea_ok_ || !sp || vb.scale != 1.0f || p.sea.amp.size() < 4 * (size_t)n)
        return false;
    const u32 amp_pa = ring_put(p.sea.amp.data(), 4 * n);
    if (!amp_pa)
        return false;
    const g3::MatDynamic* dyn = it.mat_state < p.mat_states.size() ? &p.mat_states[it.mat_state] : nullptr;
    configure(&sea_prog_);
    // ---- the attributes: buffer 0 the positions (f32 x 3: v0), buffer 1 the amplitudes (u8 x 4: v1)
    use_base(std::min(r.base, amp_pa) & ~0xFu, r);
    const u32 fmt_words[2] = {(u32)(2 << 2 | GPU_FLOAT) | (u32)(3 << 2 | GPU_UNSIGNED_BYTE) << 4, (0xFFFu & ~3u) << 16 | 1u << 28};
    cmd_incremental(GPUREG_ATTRIBBUFFERS_FORMAT_LOW, fmt_words, 2);
    cmd_masked(GPUREG_VSH_INPUTBUFFER_CONFIG, 0xB, 0xA0000000 | 1);
    cmd_write(GPUREG_VSH_NUM_ATTR, 1);
    const u32 perm[2] = {0x10, 0};
    cmd_incremental(GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW, perm, 2);
    const u32 bufs[6] = {r.base + vb.offset - base_, 0, 12u << 16 | 1u << 28, amp_pa - base_, 1, 4u << 16 | 1u << 28};
    cmd_incremental(GPUREG_ATTRIBBUFFER0_OFFSET, bufs, 6);
    // ---- the uniforms
    MatrixKey key;
    key.matrix = it.matrix, key.scale = 1.0f;
    memcpy(key.origin, vb.origin, sizeof(key.origin));
    const MatrixEntry& e = matrices(p, key);
    memcpy(mv_, e.mv, sizeof(mv_));
    for (int i = 0; i < 4; i++)
        uni(SR.mvp + i, e.mvp[i]);
    const g3::SeaWaves& w = p.sea.waves;
    f32 ph[4];
    for (int i = 0; i < 4; i++)
        ph[i] = w.phase[i] + w.kx[i] * vb.origin[0] + w.kz[i] * vb.origin[2];
    uni(SR.wkx, w.kx), uni(SR.wkz, w.kz), uni(SR.wph, ph), uni(SR.wamp, w.amp);
    uni(SR.wfa, w.fade_a), uni(SR.wfb, w.fade_b);
    uni(SR.eye, p.sea.eye[0] - vb.origin[0], p.sea.eye[1] - vb.origin[1], p.sea.eye[2] - vb.origin[2], 0);
    uni(SR.sun, sp->sun[0], sp->sun[1], sp->sun[2], 0);
    uni(SR.sunr, -sp->sun[0], sp->sun[1], -sp->sun[2], 0);
    uni(SR.shade, sp->shade);
    f32 rows[7][4] = {};
    const double ox = (double)p.sea.origin[0] + vb.origin[0], oz = (double)p.sea.origin[2] + vb.origin[2];
    for (int u = 0; u < 3; u++) {
        if (!mr.unit[u].on)
            continue;
        const g3::TexUnit& tu = m.unit[u];
        const f32(*T)[4] = tu.matrix >= 0 && tu.matrix < 3 ? (dyn ? dyn->tex_mtx[tu.matrix] : m.tex_mtx[tu.matrix])
                                                            : k_identity24;
        if (u == 0) {
            if (tu.source == g3::TexCoordSource::PROJECTION)
                projection_rows(T, mr.unit[0].sx, mr.unit[0].sy, rows);
            continue;
        }
        // (s, t) = T (u, v, 1, 1), (u, v) = uv_map (x, z, 1) at the centre plus the position
        const double (*U)[3] = nullptr;
        double um[2][3];
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 3; j++)
                um[i][j] = sp->uv_map[i][j];
        U = um;
        const double cu = U[0][0] * ox + U[0][1] * oz + U[0][2], cv = U[1][0] * ox + U[1][1] * oz + U[1][2];
        const f32 sc[2] = {mr.unit[u].sx, mr.unit[u].sy};
        const u8 wrap[2] = {tu.wrap_s, tu.wrap_t};
        for (int i = 0; i < 2; i++) {
            double c = T[i][0] * cu + T[i][1] * cv + T[i][2] + T[i][3];
            const double period = wrap[i] == 2 ? 1.0 : wrap[i] == 3 ? 2.0 : 0.0;  // (repeat, mirrored repeat)
            if (period > 0)
                c -= period * std::floor(c / period);
            f32* row = rows[1 + 2 * u + i];
            row[0] = (f32)(sc[i] * (T[i][0] * U[0][0] + T[i][1] * U[1][0]));
            row[1] = 0;
            row[2] = (f32)(sc[i] * (T[i][0] * U[0][1] + T[i][1] * U[1][1]));
            row[3] = (f32)(sc[i] * c);
        }
    }
    for (int i = 0; i < 7; i++)
        uni(SR.tmtx + i, rows[i]);
    // ---- the material, marking its pixels in the stencil buffer (where the depth test passes)
    set_texenv(dyn ? dyn->comb : m.comb);
    set_units(mr.unit);
    u32 pixel[8];
    memcpy(pixel, mr.pixel, sizeof(pixel));
    pixel[6] = 1 | (u32)GPU_ALWAYS << 4 | 0xFFu << 8 | (u32)p.sea.stencil << 16 | 0xFFu << 24;
    pixel[7] = GPU_STENCIL_KEEP | GPU_STENCIL_KEEP << 4 | GPU_STENCIL_REPLACE << 8;
    set_pixel(pixel);
    draw_batch(r, b);
    // ---- back: buffer 1 none, the g3d program, what it had taken as unknown
    static const u32 k_none[3] = {};
    cmd_incremental(GPUREG_ATTRIBBUFFER1_OFFSET, k_none, 3);
    configure(&prog_);
    memset(uv_, 0xFF, sizeof(uv_));
    dirty_lo_ = 0, dirty_hi_ = 0;
    bools_ = int0_ = ~0u;
    layout_ = vbuf_ = -1;
    light_set_ = -2;
    mcur_ = nullptr;
    mkey_ = MatrixKey();
    mat_key_ = ~0u;
    rows_uv_[0] = rows_uv_[1] = 0;
    return true;
}

// ---------------------------------------------------------------- matrices
// The matrices of a key (made once a packet, then found in the table): model (the buffer's positions:
// from its origin, in steps of its scale) -> view, the view (guest units) x S(k) x the item's matrix x
// T(origin) x S(scale), in double -- its translation is the island's coordinates less the eye's, tens
// of thousands of units that cancel to the few the eye is from the cell, which a float would keep to
// a hundredth of a unit --, an overlay's pulled toward the eye (native_draw.h overlay_pull); then the
// clip rows in float (cpf_: the 3DS's clip transform and the GX projection, the complemented depth's
// row made in double: begin_packet); the sky's depth held at 0.5 (neither near nor far clips it).
// What a float costs here is a relative 2^-24 of rows the GPU takes at 2^-17 (its 24-bit floats).
PicaBackend::MatrixEntry& PicaBackend::matrices(const g3::Packet& p, const MatrixKey& key) {
    MatrixEntry& e = mtable_[key.hash() % MATRIX_TABLE];
    if (e.valid && e.key == key)
        return e;
    st_.matrices_made++;
    e.key = key, e.valid = true, e.has_normals = false;
    const f32* M = &p.matrices[12 * (size_t)key.matrix];
    const double k = p.world_scale, sc = key.scale;
    const f32* o = key.origin;
    double A[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            A[i][j] = k * sc * M[4 * i + j];
        A[i][3] = k * ((double)M[4 * i + 3] + (double)M[4 * i] * o[0] + (double)M[4 * i + 1] * o[1] +
                       (double)M[4 * i + 2] * o[2]);
    }
    const double s = 1.0 - key.pull;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            e.mv[i][j] = s * ((double)p.view[i][0] * A[0][j] + (double)p.view[i][1] * A[1][j] +
                              (double)p.view[i][2] * A[2][j] + (j == 3 ? (double)p.view[i][3] : 0.0));
    f32 mv[3][4];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            mv[i][j] = (f32)e.mv[i][j];
    for (int r = 0; r < 4; r++)
        for (int j = 0; j < 4; j++)
            e.mvp[r][j] = cpf_[r][0] * mv[0][j] + cpf_[r][1] * mv[1][j] + cpf_[r][2] * mv[2][j] + (j == 3 ? cpf_[r][3] : 0.0f);
    if (key.flags & g3::DRAW_SKY)
        for (int j = 0; j < 4; j++)
            e.mvp[2][j] = -0.5f * e.mvp[3][j];
    return e;
}

// the normal rows: the model-view's cofactors (the inverse transpose up to a scale the shader
// normalises away; row i: the cross product of the other two rows), signed by the determinant so that
// a mirroring matrix keeps its normals outward
void PicaBackend::normal_rows(MatrixEntry& e) {
    if (e.has_normals)
        return;
    e.has_normals = true;
    f32 m[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] = (f32)e.mv[i][j];
    f32 cof[3][3];
    for (int i = 0; i < 3; i++) {
        const int a = (i + 1) % 3, b = (i + 2) % 3;
        for (int j = 0; j < 3; j++) {
            const int c = (j + 1) % 3, d = (j + 2) % 3;
            cof[i][j] = m[a][c] * m[b][d] - m[a][d] * m[b][c];
        }
    }
    const f32 det = m[0][0] * cof[0][0] + m[0][1] * cof[0][1] + m[0][2] * cof[0][2];
    const f32 sg = det < 0 ? -1.0f : 1.0f;
    for (int i = 0; i < 3; i++) {
        e.nmtx[i][0] = sg * cof[i][0], e.nmtx[i][1] = sg * cof[i][1], e.nmtx[i][2] = sg * cof[i][2];
        e.nmtx[i][3] = 0;
    }
}

// ---------------------------------------------------------------- fog
// GX's linear fog (PERSP_LIN: by view depth, between the fog's start and end) can't go through the
// PICA's fog unit: its 128-entry table is indexed by the depth buffer's value, and with the near
// plane at 1.5 package units a ramp 27,978-65,527 units away is depth 0.999965-0.999996, the last
// half-percent of the last entry, whose value and slope (at most 2 an entry) can't make it. And the
// fogged materials (the sea's) have no combiner stage or texture unit to spare. So the item is drawn
// a second time over itself: no depth or alpha written, its colour the fog's, its alpha the fog
// amount from the ramp texture -- s, affine in the position, interpolated per pixel with
// perspective, is the view depth's place between start and end exactly -- blended over what the
// first pass left: (1 - f) colour + f fog, GX's fog of an opaque surface (of a translucent one, f of
// what is behind it is fogged too). Its depth test (fog_pass): where the item wrote its depth, equal
// -- the second pass's depth is the first's, bit for bit, so exactly the pixels the item drew, not
// those its alpha test cut out (a tree's leaves) or a nearer surface hid; else the item's own test
// with equality let through (LESS would refuse every pixel). An item wholly nearer than the fog's
// start, or that writes no colour, gets none.
bool PicaBackend::fog_rows(const g3::Packet& p, const g3::GxView& view, const g3::Material& m, const fmt::Batch& b,
                           const VbRes& vb, double pull, f32 rows[3][4], u32* color) {
    if (!m.fog || !m.pixel.color_write || view.orthographic || m.fog_index >= p.fogs.size())
        return false;
    const fmt::Fog& f = p.fogs[m.fog_index];
    if (f.type != fmt::FOG_LINEAR || !(f.end > f.start))
        return false;
    const double k = p.world_scale, start = f.start * k, end = f.end * k;
    // the view's z row (an overlay's model-view is pulled toward the eye: not its fog)
    const double unpull = 1.0 / (1.0 - pull);
    double zrow[4];
    for (int j = 0; j < 4; j++)
        zrow[j] = mv_[2][j] * unpull;
    double zmax = -1e300;  // the box's furthest corner (view depth: -z; the box in the buffer's steps)
    const double inv = 1.0 / vb.scale;
    for (int i = 0; i < 8; i++) {
        const double x = ((double)b.bbox[i & 1 ? 3 : 0] - vb.origin[0]) * inv,
                     y = ((double)b.bbox[i & 2 ? 4 : 1] - vb.origin[1]) * inv,
                     z = ((double)b.bbox[i & 4 ? 5 : 2] - vb.origin[2]) * inv;
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
    u32 func = GPU_ALWAYS;  // (see fog_rows)
    if (px.depth_test && px.depth_write)
        func = GPU_EQUAL;
    else if (px.depth_test && px.depth_func == GPU_LESS)
        func = GPU_LEQUAL;
    else if (px.depth_test && px.depth_func == GPU_GREATER)
        func = GPU_GEQUAL;
    else if (px.depth_test)
        func = px.depth_func & 7u;
    const u32 pixel[8] = {
        0,  // no alpha test
        1 | func << 4 | (u32)(GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE) << 8,
        GPU_BLEND_ADD | GPU_BLEND_ADD << 8 | GPU_SRC_ALPHA << 16 | GPU_ONE_MINUS_SRC_ALPHA << 20 | GPU_ZERO << 24 |
            (u32)GPU_ONE << 28,
        0xE40100, GPU_LOGICOP_COPY, (u32)(px.cull & 3), (u32)GPU_ALWAYS << 4, 0};
    set_pixel(pixel);
    for (int i = 0; i < 3; i++)
        uni(R.tmtx + i, rows[i]);
    set_bools(1u << R.b_proj0);
    draw_batch(r, b);
    // (the next item sends its material's state and rows again: the per-register caches skip what
    // is the same)
    mat_key_ = ~0ull;
}

// ---------------------------------------------------------------- the shadow map
// The map's texture: the size the first map asks for (a later size takes the same texture: what a
// texel covers is the World's choice, the receivers sample whatever the texture has), in VRAM if there
// is room (the GPU draws into it and reads it back every frame), else in linear memory; RGBA8 as the
// GPU's shadow mode keeps its texels (depth in 24 bits, intensity in 8); no depth buffer.
bool PicaBackend::make_shadow(u32 size) {
    if (shadow_size_)
        return true;
    if (shadow_failed_ || size < 8 || size > 1024 || (size & (size - 1)))
        return false;
    bool vram = C3D_TexInitShadow(&shadow_tex_, (u16)size, (u16)size);
    if (!vram && !C3D_TexInitWithParams(&shadow_tex_, nullptr,
                                        (C3D_TexInitParams){(u16)size, (u16)size, 0, GPU_RGBA8, GPU_TEX_SHADOW_2D, false})) {
        log("g3d pica: no memory for a %u-texel shadow map: no shadows", (unsigned)size);
        shadow_failed_ = true;
        return false;
    }
    C3D_FrameBufTex(&shadow_fb_, &shadow_tex_, GPU_TEXFACE_2D, 0);
    C3D_FrameBufDepth(&shadow_fb_, nullptr, GPU_RB_DEPTH24);
    shadow_size_ = size;
    UnitRes& u = shadow_unit_;
    u.on = true;
    u.regs[0] = 0xFFFFFFFF;  // (off the map: nothing in front: lit)
    u.regs[1] = size | size << 16;
    u.regs[2] = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_BORDER) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_BORDER) |
                GPU_TEXTURE_MODE(GPU_TEX_SHADOW_2D);
    u.regs[3] = 0;
    u.regs[4] = osConvertVirtToPhys(shadow_tex_.data) >> 3;
    u.regs[5] = GPU_RGBA8;
    log("g3d pica: shadow map %u x %u in %s (%u KB)", (unsigned)size, (unsigned)size, vram ? "VRAM" : "linear memory",
        (unsigned)(size * size * 4 >> 10));
    return true;
}

// rows (affine, 4: the last (0 0 0 1)) of the world times an item's matrix, its buffer's origin and
// step, into float rows for the positions the GPU reads
void compose_rows(const f32* M, const VbRes& vb, const double rows[4][4], f32 out[4][4]) {
    for (int i = 0; i < 4; i++) {
        double w[4];
        for (int c = 0; c < 4; c++)
            w[c] = rows[i][0] * M[c] + rows[i][1] * M[4 + c] + rows[i][2] * M[8 + c] + (c == 3 ? rows[i][3] : 0.0);
        w[3] += w[0] * vb.origin[0] + w[1] * vb.origin[1] + w[2] * vb.origin[2];
        for (int c = 0; c < 3; c++)
            out[i][c] = (f32)(w[c] * vb.scale);
        out[i][3] = (f32)w[3];
    }
}

// The map, before the opaque packet's items: the frame's target swapped for the map's texture
// (gx_pica.cpp pica_native_target); the texture cleared by a quad over it in the ordinary fragment mode
// (white: depth 0xFFFFFF, intensity 255 -- nothing in front of anything); then, in the GPU's shadow
// mode, each caster's triangles (no culling: both faces cast), clip x, y = 2 s - 1, 2 t - 1 and z = -depth
// (w 1, an orthographic view), depth mapped by scale -1 to the map's 0..1, the GPU keeping the nearer
// depth itself (no depth test); their combiners' green 0 (a hard shadow, no penumbra) and alpha the
// material's texture's where it is alpha-tested (a tree's leaves cut out), tested as the material
// tests it. Then the frame's target again, and nothing kept about the GPU's state.
void PicaBackend::shadow_pass(const g3::Packet& p, PkgRes& r) {
    shadow_ready_ = false;
    if (!make_shadow(p.shadow.size))
        return;
    const u64 t0 = svcGetSystemTick();
    smap_ = p.shadow;
    const u32 iBias = std::min((u32)((1u << 24) - 1), (u32)lroundf(smap_.bias * (f32)(1u << 24)));
    shadow_word_ = (iBias & ~1u) | 1u;  // (orthographic: s, t used as they are; C3D_TexShadowParams)
    pica_native_target(&shadow_fb_);
    tev::Combiners comb;
    for (int i = 0; i < 6; i++) {
        tev::CombinerStage& st = comb.stage[i];
        memset(&st, 0, sizeof(st));
        for (int k = 0; k < 3; k++)
            st.src_rgb[k] = st.src_a[k] = tev::SRC_PREVIOUS;
        st.func_rgb = st.func_a = tev::FUNC_REPLACE;
    }
    comb.buf_rgb = comb.buf_a = 0;
    // ---- the clear
    {
        const u8 lay = layout_of(fmt::VA_POS, 0, 12);
        const Layout& L = s_layouts[lay];
        const u32 fmt_words[2] = {L.format_low, L.format_high};
        cmd_incremental(GPUREG_ATTRIBBUFFERS_FORMAT_LOW, fmt_words, 2);
        cmd_masked(GPUREG_VSH_INPUTBUFFER_CONFIG, 0xB, 0xA0000000 | 4);
        cmd_write(GPUREG_VSH_NUM_ATTR, 4);
        const u32 perm[2] = {L.permutation, 0};
        cmd_incremental(GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW, perm, 2);
        for (u32 i = 0; i < L.n_fixed; i++) {
            cmd_write(GPUREG_FIXEDATTRIB_INDEX, L.fixed_id[i]);
            C3D_ImmSendAttrib(L.fixed[i][0], L.fixed[i][1], L.fixed[i][2], L.fixed[i][3]);
        }
        const u32 buf[3] = {r.quad_offset, L.buf_config1, L.buf_config2};
        cmd_incremental(GPUREG_ATTRIBBUFFER0_OFFSET, buf, 3);
        layout_ = vbuf_ = -1;
        comb.stage[0].src_rgb[0] = comb.stage[0].src_a[0] = tev::SRC_CONSTANT;
        comb.stage[0].konst = 0xFFFFFFFF;
        set_texenv(comb);
        UnitRes none[3];
        set_units(none, 1);
        const u32 pixel[6] = {0, (u32)GPU_WRITE_COLOR << 8,
                              GPU_BLEND_ADD | GPU_BLEND_ADD << 8 | GPU_ONE << 16 | GPU_ZERO << 20 | GPU_ONE << 24 |
                                  (u32)GPU_ZERO << 28,
                              0xE40100, GPU_LOGICOP_COPY, 0};
        set_pixel(pixel);
        set_bools(0);
        for (int i = 0; i < 4; i++)
            uni(R.mvp + i, i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f, i == 3 ? 1.0f : 0.0f);
        draw_indices(r, r.quad_first, 6);
    }
    // ---- the casters
    cmd_write(GPUREG_FRAGOP_SHADOW, 0x3C00u | 0x8000u << 16);  // (C3D_FragOpShadow(0, 1): no penumbra in use)
    cmd_write(GPUREG_DEPTHMAP_SCALE, f32tof24(-1.0f));
    cmd_write(GPUREG_DEPTHMAP_OFFSET, f32tof24(0.0f));
    double clip[4][4];
    for (int j = 0; j < 4; j++) {
        clip[0][j] = 2.0 * smap_.light[0][j] - (j == 3 ? 1.0 : 0.0);
        clip[1][j] = 2.0 * smap_.light[1][j] - (j == 3 ? 1.0 : 0.0);
        clip[2][j] = -(double)smap_.light[2][j];
        clip[3][j] = j == 3 ? 1.0 : 0.0;
    }
    const auto batches = p.package->batches();
    u32 tris = 0;
    for (const g3::ShadowCaster& c : p.casters) {
        if (c.batch >= batches.count || 12 * ((size_t)c.matrix + 1) > p.matrices.size())
            continue;
        const fmt::Batch& b = batches[c.batch];
        if (b.vbuf >= r.vbs.size() || !r.vbs[b.vbuf].ok || b.material >= r.mats.size())
            continue;
        const g3::Material& m = p.materials[b.material];
        MatRes& mr = r.mats[b.material];
        if (!mr.made)
            make_material(r, b.material, m);
        const VbRes& vb = r.vbs[b.vbuf];
        set_layout(r, b.vbuf);
        f32 mvp[4][4];
        compose_rows(&p.matrices[12 * (size_t)c.matrix], vb, clip, mvp);
        for (int i = 0; i < 4; i++)
            uni(R.mvp + i, mvp[i]);
        // the alpha test's texture: unit 0's, at uv0 or uv1
        const g3::TexUnit& tu = m.unit[0];
        const bool cut = m.pixel.alpha_test && m.pixel.alpha_func != GPU_ALWAYS && mr.drawable && mr.unit[0].on &&
                         (tu.source == g3::TexCoordSource::UV0 || tu.source == g3::TexCoordSource::UV1);
        comb.stage[0].src_rgb[0] = tev::SRC_CONSTANT;
        comb.stage[0].src_a[0] = cut ? tev::SRC_TEXTURE0 : tev::SRC_CONSTANT;
        comb.stage[0].op_a[0] = cut && (m.lum_units & 1) ? tev::OP_A_SRC_R : tev::OP_A_SRC_ALPHA;
        comb.stage[0].konst = 0xFF000000;
        set_texenv(comb);
        UnitRes units[3];
        if (cut) {
            units[0] = mr.unit[0];
            static const int k_slot0[3] = {0, -1, -1};
            texture_rows(m, mr, nullptr, vb, k_slot0);
        }
        set_units(units, 1);
        const u32 pixel[6] = {cut ? mr.pixel[0] : 0, (u32)GPU_WRITE_COLOR << 8,
                              GPU_BLEND_ADD | GPU_BLEND_ADD << 8 | GPU_ONE << 16 | GPU_ZERO << 20 | GPU_ONE << 24 |
                                  (u32)GPU_ZERO << 28,
                              0xE40103, GPU_LOGICOP_COPY, 0};
        set_pixel(pixel);
        set_bools(0);
        draw_indices(r, c.first_index, c.n_indices);
        tris += c.n_indices / 3;
    }
    // ---- the frame's target again
    cmd_write(GPUREG_DEPTHMAP_SCALE, f32tof24(1.0f));
    cmd_write(GPUREG_DEPTHMAP_OFFSET, f32tof24(1.0f));
    pica_native_target(nullptr);
    forget_state();
    shadow_ready_ = true;
    st_.shadow_maps++, st_.shadow_draws += (u32)p.casters.size(), st_.shadow_triangles += tris;
    st_.shadow_ticks += svcGetSystemTick() - t0;
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
    // The shared clip matrix already complements depth. Apply it once: complementing row 2 again
    // would reverse the island's depth while its material tests still expect smaller values nearer
    // the eye, so distant water would cover nearer land. The GX and native draws share this mapping.
    double cl[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            cl[i][j] = clip[i][j], proj_[i][j] = view.projection[i][j];
    mul44(cl, proj_, cp_);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            cpf_[i][j] = (f32)cp_[i][j];
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
    // the shadow map: drawn by the opaque packet (when it has casters), sampled by both
    if (!p.shadow.size)
        shadow_ready_ = false;
    else if (p.pass == g3::Pass::OPAQUE) {
        shadow_ready_ = false;
        if (!p.casters.empty())
            shadow_pass(p, *r);
    }
    const u32 tex_shadow = shadow_ready_ ? shadow_word_ : 1;
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
        const VbRes& vb = r->vbs[b.vbuf];
        // the sea's wave grid: its own program (draw_sea_grid)
        if (it.flags & g3::DRAW_SEA_GRID) {
            if (draw_sea_grid(p, *r, it, m, mr)) {
                st_.draws++, st_.sea_draws++, st_.triangles += b.n_indices / 3, st_.indices += b.n_indices;
            }
            continue;
        }
        // (a dynamic item's vertices: the packet's, into the ring)
        u32 dynamic_pa = 0;
        if (it.flags & g3::DRAW_DYNAMIC) {
            const fmt::VertexBuffer& fvb = p.package->vertex_buffers()[b.vbuf];
            if (p.dynamic.size() < (size_t)fvb.count * fvb.stride ||
                !(dynamic_pa = ring_put(p.dynamic.data(), fvb.count * fvb.stride)))
                continue;
            st_.dynamic_draws++;
        }
        set_layout(*r, b.vbuf, dynamic_pa);
        // its matrices (an overlay pulled toward the eye by its box's furthest corner: native_draw.h
        // overlay_pull, from the model-view in package units)
        MatrixKey key;
        key.matrix = it.matrix, key.flags = it.flags & g3::DRAW_SKY, key.scale = vb.scale;
        memcpy(key.origin, vb.origin, sizeof(key.origin));
        if (!sky && g3::is_overlay(m)) {
            const MatrixEntry& base = matrices(p, key);
            double mv[3][4];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 4; j++)
                    mv[i][j] = j < 3 ? base.mv[i][j] / vb.scale : base.mv[i][j];
            key.pull = g3::overlay_pull(mv, b.bbox, vb.origin, p.world_scale);
        }
        // (found again even when it hasn't moved: an overlay's own lookup above may have taken its place
        // in the table)
        const bool moved = !(key == mkey_);
        mcur_ = &matrices(p, key);
        if (moved) {
            memcpy(mv_, mcur_->mv, sizeof(mv_));
            for (int i = 0; i < 4; i++)
                uni(R.mvp + i, mcur_->mvp[i]);
            mkey_ = key;
            st_.matrices++;
        }
        // its material, as a receiver of the shadow map (its unit 0 on unit 2, the map on 0: material.h)
        // or a decal faded where the map covers (the fade window on unit 1)
        const bool recv = shadow_ready_ && (it.flags & g3::DRAW_SHADOW_RECEIVER) && m.shadow_receiver;
        const bool fade = shadow_ready_ && !recv && (it.flags & g3::DRAW_SHADOW_FADE) && m.shadow_decal;
        const u64 mkey = (u64)(recv ? 1 : fade ? 2 : 0) << 32 | (u32)b.material << 16 | it.mat_state;
        const bool new_mat = mkey != mat_key_;
        if (new_mat) {
            const tev::Combiners& base = dyn ? dyn->comb : m.comb;
            if (recv) {
                tev::Combiners c2;
                g3::shadow_receiver_combiners(base, smap_.darkness, &c2);
                set_texenv(c2);
                const UnitRes u3[3] = {shadow_unit_, mr.unit[1], mr.unit[0]};
                set_units(u3, tex_shadow);
            } else if (fade) {
                tev::Combiners c2;
                g3::shadow_decal_combiners(base, &c2);
                set_texenv(c2);
                const UnitRes u3[3] = {mr.unit[0], fade_unit_, mr.unit[2]};
                set_units(u3, tex_shadow);
            } else {
                set_texenv(base);
                set_units(mr.unit, tex_shadow);
            }
            material_uniforms(p, m, dyn);
            mat_key_ = mkey;
            st_.mat_changes++;
        }
        st_.shadow_receivers += recv, st_.shadow_faded += fade;
        // the normal rows, for a material that reads normals (lit, or mapping the environment)
        if (mr.bools >> R.b_usenrm & 1) {
            normal_rows(*mcur_);
            for (int i = 0; i < 3; i++)
                uni(R.nmtx + i, mcur_->nmtx[i]);
        }
        u32 pixel[8];
        memcpy(pixel, mr.pixel, sizeof(pixel));
        if (sky)  // (no depth test or write)
            pixel[1] = 1 | (u32)GPU_ALWAYS << 4 | (pixel[1] & 0xF00);
        if (it.flags & g3::DRAW_SEA_MASKED)  // (not where the wave grid marked: native_draw.h)
            pixel[6] = 1 | (u32)GPU_NOTEQUAL << 4 | (u32)p.sea.stencil << 16 | 0xFFu << 24;
        set_pixel(pixel);
        u32 bools = mr.bools;
        if (recv)  // (the map's coordinates from the position on unit 0, the material's unit 0 on unit 2)
            bools |= 1u << R.b_proj0 | 1u << R.b_tc2on;
        else if (fade)  // (the fade window's from the position on unit 1)
            bools |= 1u << (R.b_proj0 + 1) | 1u << R.b_tc1on;
        set_bools(bools | (n_lights_ && (mr.bools >> R.b_lit & 1) ? 1u << R.b_haslights : 0));
        if (new_mat || vb.uv_scale[0] != rows_uv_[0] || vb.uv_scale[1] != rows_uv_[1] ||
            (moved && (mr.projects || recv || fade))) {
            static const int k_receiver_slots[3] = {2, 1, -1};
            texture_rows(m, mr, dyn, vb, recv ? k_receiver_slots : nullptr);
            if (recv)
                position_rows(p, it.matrix, vb, smap_.light, 3, 0);
            else if (fade)
                position_rows(p, it.matrix, vb, smap_.fade, 2, 1);
            rows_uv_[0] = vb.uv_scale[0], rows_uv_[1] = vb.uv_scale[1];
        }
        draw_batch(*r, b);
        st_.draws++, st_.triangles += b.n_indices / 3, st_.indices += b.n_indices;
        st_.prelit_draws += m.prelit;
        f32 rows[3][4];
        u32 fog_color;
        if (!sky && m.fog && fog_rows(p, view, m, b, vb, key.pull, rows, &fog_color)) {
            fog_pass(m, b, *r, rows, fog_color);
            st_.fog_draws++;
        }
    }
    // the stencil test off again (the GX path never uses it), the package's base
    if (pixel_[6] != ~0u && (pixel_[6] & 1)) {
        cmd_write(GPUREG_STENCIL_TEST, (u32)GPU_ALWAYS << 4);
        cmd_write(GPUREG_STENCIL_OP, 0);
    }
    use_base(r->base, *r);
    // unit 0 left an ordinary 2D texture (a projection one samples by texcoord0w, which the GX
    // shader doesn't write; a shadow one compares), its shadow register as citro3d keeps it
    if (units_[0][2] != ~0u && (((units_[0][2] >> 28) & 7) == GPU_TEX_PROJECTION ||
                                ((units_[0][2] >> 28) & 7) == GPU_TEX_SHADOW_2D))
        cmd_write(GPUREG_TEXUNIT0_PARAM, units_[0][2] & ~GPU_TEXTURE_MODE(7));
    if (tex_shadow_ != 1)
        cmd_write(GPUREG_TEXUNIT0_SHADOW, 1);
    // the GX path uses the same complemented clip depth
    cmd_write(GPUREG_DEPTHMAP_SCALE, f32tof24(1.0f));
    cmd_write(GPUREG_DEPTHMAP_OFFSET, f32tof24(1.0f));
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
    log("g3d pica: %u frames: %.0f draws (%.0f prelit, %.0f fog passes, %.1f wave grids, %.1f trails), %.0f triangles, "
        "%.0f indices a frame; "
        "%.2f ms a frame in the backend (at most %.2f); %.0f material changes, %.0f matrix changes (%.0f made), "
        "%.0f uniform vectors, %.0f command words a frame; %u items not drawn (command list full; the last frame's "
        "list %.0f %% of it); resident: %u textures %u KB, vertices %u KB, indices %u KB; %u KB of the linear heap "
        "free; shadow map %u: %.0f casters, %.0f triangles, %.2f ms in its pass, %.0f receiving items, %.0f decals "
        "faded a frame (%u maps)",
        (unsigned)st_.frames, st_.draws / f, st_.prelit_draws / f, st_.fog_draws / f, st_.sea_draws / f,
        st_.dynamic_draws / f, st_.triangles / f,
        st_.indices / f, (double)st_.ticks * ms / f, max_ms, st_.mat_changes / f, st_.matrices / f,
        st_.matrices_made / f, st_.uniform_vectors / f, st_.cmd_words / f, (unsigned)st_.items_skipped,
        (double)C3D_GetCmdBufUsage() * 100.0, (unsigned)textures,
        (unsigned)(r.tex_bytes >> 10), (unsigned)(r.vtx_bytes >> 10), (unsigned)(r.idx_bytes >> 10),
        (unsigned)(linearSpaceFree() >> 10), (unsigned)shadow_size_, st_.shadow_draws / f, st_.shadow_triangles / f,
        (double)st_.shadow_ticks * ms / f, st_.shadow_receivers / f, st_.shadow_faded / f, (unsigned)st_.shadow_maps);
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
