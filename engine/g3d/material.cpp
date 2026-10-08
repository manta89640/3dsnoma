// Materials of the native g3d at run time (material.h).
#include "engine/g3d/material.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/g3d/anim.h"

namespace engine::g3d {

namespace {

inline u32 bits(u32 v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

// GX compare functions (never, less, equal, lequal, greater, nequal, gequal, always) -> GPU_TESTFUNC
const u8 k_test_func[8] = {0 /*never*/, 4 /*less*/, 2 /*equal*/, 5 /*lequal*/,
                           6 /*greater*/, 3 /*notequal*/, 7 /*gequal*/, 1 /*always*/};

// GX blend factors -> GPU_BLENDFACTOR (as gx_pica.cpp's blend_factor: GX's "colour" factor is the
// other side's colour)
u8 blend_factor(u32 f, bool src) {
    switch (f) {
    case 0: return 0;               // zero
    case 1: return 1;               // one
    case 2: return src ? 4 : 2;     // dst colour / src colour
    case 3: return src ? 5 : 3;     // 1 - dst colour / 1 - src colour
    case 4: return 6;               // src alpha
    case 5: return 7;               // 1 - src alpha
    case 6: return 8;               // dst alpha
    default: return 9;              // 1 - dst alpha
    }
}

// GX logic ops -> GPU_LOGICOP (as gx_pica.cpp's table)
const u8 k_logic_op[16] = {0 /*clear*/, 1 /*and*/, 2 /*and reverse*/, 3 /*copy*/, 13 /*and inverted*/, 6 /*noop*/,
                           11 /*xor*/, 9 /*or*/, 10 /*nor*/, 12 /*equiv*/, 7 /*invert*/, 14 /*or reverse*/,
                           5 /*copy inverted*/, 15 /*or inverted*/, 8 /*nand*/, 4 /*set*/};

void pixel_state(const fmt::Material& m, PixelState& p) {
    // depth (BP 0x40): enable, function, update
    p.depth_test = m.zmode & 1;
    p.depth_func = p.depth_test ? k_test_func[bits(m.zmode, 1, 3)] : 1;
    p.depth_write = p.depth_test && (m.zmode & 0x10);
    // blending (BP 0x41): blend or logic op, the write masks
    const u32 cm = m.blend;
    p.color_write = cm & 0x08;
    p.alpha_write = cm & 0x10;
    p.blend = cm & 1;
    p.logic = !p.blend && (cm & 2);
    if (p.blend && (cm & 0x800)) {
        p.blend_eq = 2;  // reverse subtract: dst - src, factors one
        p.blend_src = p.blend_dst = 1;
    } else if (p.blend) {
        p.blend_eq = 0;
        p.blend_src = blend_factor(bits(cm, 8, 3), true);
        p.blend_dst = blend_factor(bits(cm, 5, 3), false);
    } else {
        p.blend_eq = 0, p.blend_src = 1, p.blend_dst = 0;
    }
    p.logic_op = k_logic_op[bits(cm, 12, 4)];
    p.const_alpha = m.const_alpha & 0x100;
    p.const_alpha_value = (u8)(m.const_alpha & 0xFF);
    // alpha test (BP 0xF3): the PICA has one comparison; keep the one that isn't "always" (as
    // gx_pica.cpp: exact for the island's GEQUAL 128 AND LEQUAL 255)
    const u32 ac = m.alpha_compare;
    const u32 c0 = bits(ac, 16, 3), c1 = bits(ac, 19, 3), op = bits(ac, 22, 2);
    const u32 r0 = bits(ac, 0, 8), r1 = bits(ac, 8, 8);
    p.alpha_test = true;
    if (c0 == 7 && c1 == 7)
        p.alpha_test = false, p.alpha_func = 1, p.alpha_ref = 0;
    else if (op == 0 && c1 == 7)
        p.alpha_func = k_test_func[c0], p.alpha_ref = (u8)r0;
    else if (op == 0 && c0 == 7)
        p.alpha_func = k_test_func[c1], p.alpha_ref = (u8)r1;
    else if (op == 1 && c1 == 0)
        p.alpha_func = k_test_func[c0], p.alpha_ref = (u8)r0;
    else if (op == 1 && c0 == 0)
        p.alpha_func = k_test_func[c1], p.alpha_ref = (u8)r1;
    else
        p.alpha_func = k_test_func[c0], p.alpha_ref = (u8)r0;
    // culling: GX front faces are counter-clockwise on the screen, like the PICA's (gx_pica.cpp).
    // The material holds GXSetCullMode's value (GX_CULL_FRONT 1, GX_CULL_BACK 2), which the GX
    // library writes into GENMODE swapped (front as 2, back as 1); gx_pica.cpp maps GENMODE's 1 to
    // GPU_CULL_FRONT_CCW and 2 to GPU_CULL_BACK_CCW under the same clip transform the native draws
    // use, so GX_CULL_BACK is the PICA's FRONT_CCW here too (the soft backend reads cull_gx).
    p.cull_gx = m.cull;
    static const u8 k_cull[4] = {0, 2, 1, 0};
    p.cull = k_cull[m.cull & 3];
}

void lighting(const fmt::Material& m, Lighting& l) {
    const u32 c = m.chan_color, a = m.chan_alpha;
    l.lit = m.n_chans > 0 && (c & 2);
    l.mat_from_vertex = c & 1;
    l.amb_from_vertex = c & 0x40;
    l.alpha_lit = m.n_chans > 0 && (a & 2);
    l.alpha_mat_from_vertex = a & 1;
    l.light_mask = (u8)(bits(c, 2, 4) | bits(c, 11, 4) << 4);
    l.diffuse = (u8)bits(c, 7, 2);
    l.spot_attn = (c & 0x200) && (c & 0x400);
    l.light_set = (m.flags & fmt::MAT_LIGHT_SET) ? (s16)m.light_set : (s16)-1;
    memcpy(l.mat_color, m.mat_color, 4);
    memcpy(l.amb_color, m.amb_color, 4);
    // prelit: the channel's lighting is in the vertex colours (the stager's static sun): unlit, the
    // channel's colour and alpha the vertex's
    if ((m.flags & fmt::MAT_PRELIT) && m.n_chans > 0) {
        l.lit = l.alpha_lit = false;
        l.mat_from_vertex = l.alpha_mat_from_vertex = true;
        l.amb_from_vertex = false;
        l.light_mask = 0;
        l.light_set = -1;
    }
}

}  // namespace

u8 pica_wrap(u8 gx_wrap) { return gx_wrap == 1 ? 2 : gx_wrap == 2 ? 3 : 0; }

engine::gx::TevRegs tev_regs(const fmt::Material& m, const s16 (*reg)[4], const s16 (*konst)[4]) {
    engine::gx::TevRegs r;
    r.n_stages = m.n_stages;
    for (int i = 0; i < 16; i++)
        r.color_env[i] = m.color_env[i] & 0xFFFFFF, r.alpha_env[i] = m.alpha_env[i] & 0xFFFFFF;
    for (int i = 0; i < 8; i++)
        r.tref[i] = m.tref[i] & 0xFFFFFF, r.ksel[i] = m.ksel[i] & 0xFFFFFF;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++)
            r.reg[i][k] = reg[i][k], r.konst[i][k] = konst[i][k];
    return r;
}

void build_material(const Package& pkg, u32 index, Material* out) {
    const fmt::Material& m = pkg.materials()[index];
    Material& r = *out;
    r = Material();
    r.index = (u16)index;
    engine::gx::tev_to_combiners(tev_regs(m, m.tev_reg, m.tev_konst), r.comb);
    r.approximated += r.comb.unsupported;
    // texture units: what the combiners sample
    for (int u = 0; u < 3; u++) {
        const engine::gx::CombinerUnit cu = r.comb.unit[u];
        if (cu.map < 0)
            continue;
        TexUnit& t = r.unit[u];
        const fmt::TexMap& map = m.maps[cu.map];
        t.texture = map.texture;
        t.wrap_s = pica_wrap(map.wrap_s), t.wrap_t = pica_wrap(map.wrap_t);
        t.mag_linear = map.mag_filter != 0;
        // GX's minification filters: near, linear, then {near, linear} x {mip near, mip linear}
        static const u8 k_min_linear[6] = {0, 1, 0, 1, 0, 1}, k_mip[6] = {0, 0, 1, 1, 2, 2};
        t.min_linear = k_min_linear[map.min_filter];
        t.mip = k_mip[map.min_filter];
        t.lod_bias = map.lod_bias, t.min_lod = map.min_lod, t.max_lod = map.max_lod;
        if (t.texture != fmt::NONE16) {
            const u8 f = pkg.texture_image(t.texture).format;
            if (f == fmt::TF_L4 || f == fmt::TF_L8)
                r.lum_units |= 1u << u;
        }
        // its coordinates: the texgen, and what its matrix maps from
        const u32 g = (u32)cu.coord;
        if (g >= m.n_texgens) {
            r.approximated++;  // (no texgen: GX would read an undefined coordinate)
            continue;
        }
        const fmt::TexGen& tg = m.texgens[g];
        t.normalize = tg.normalize != 0;
        t.post_matrix = tg.post_mtx != fmt::NONE16 ? (s8)tg.post_mtx : (s8)-1;
        u8 map_mode = 0;
        if (tg.matrix != fmt::NONE16) {
            if (tg.matrix < 3)
                t.matrix = (s8)tg.matrix;
            else
                r.approximated++;  // (only the first three matrices are kept: identity)
            map_mode = pkg.tex_mtx()[m.tex_mtx_first + tg.matrix].map_mode;
        }
        if (map_mode == 1 || map_mode == 3 || map_mode == 4) {
            t.source = TexCoordSource::ENV_CAMERA;
            r.approximated += map_mode != 1;  // (light and specular maps: as the camera's)
        } else if (map_mode == 2) {
            t.source = TexCoordSource::PROJECTION;
            t.projective = true;
        } else if (tg.type <= 1 && tg.source == 4) {
            t.source = TexCoordSource::UV0;
        } else if (tg.type <= 1 && tg.source == 5) {
            t.source = TexCoordSource::UV1;
        } else if (tg.type <= 1 && tg.source == 0) {
            t.source = TexCoordSource::PROJECTION;
            t.projective = tg.type == 0;
        } else if (tg.type <= 1 && tg.source == 1) {
            t.source = TexCoordSource::ENV_CAMERA;
        } else {
            t.source = TexCoordSource::UV0;  // (bump mapping, colours as coordinates, TEX2-7)
            r.approximated++;
        }
    }
    if (r.lum_units)
        engine::gx::remap_luminance(r.comb.stage, r.lum_units);
    // the texture matrices: SRT as nw4r computes it
    for (u32 i = 0; i < 3; i++) {
        f32(&mtx)[2][4] = r.tex_mtx[i];
        if (i >= m.n_tex_mtx) {
            mtx[0][0] = 1, mtx[0][1] = 0, mtx[0][2] = 0, mtx[0][3] = 0;
            mtx[1][0] = 0, mtx[1][1] = 1, mtx[1][2] = 0, mtx[1][3] = 0;
            continue;
        }
        const fmt::TexMtx& tm = pkg.tex_mtx()[m.tex_mtx_first + i];
        bool approx = false;
        tex_srt_matrix(tm.mode, tm.scale[0], tm.scale[1], tm.rotate, tm.translate[0], tm.translate[1], mtx, &approx);
        r.approximated += approx;
    }
    lighting(m, r.light);
    pixel_state(m, r.pixel);
    r.fog = m.flags & fmt::MAT_FOG;
    r.fog_index = m.fog;
    r.translucent = m.flags & fmt::MAT_XLU;
    r.priority = m.priority;
    r.indirect = (m.flags & fmt::MAT_INDIRECT) || m.n_ind;
    r.prelit = (m.flags & fmt::MAT_PRELIT) && m.n_chans > 0;
    r.shadow_receiver = (m.flags & fmt::MAT_SHADOW_RECEIVER) && shadow_receiver_fits(r, r.comb);
    r.shadow_decal = (m.flags & fmt::MAT_SHADOW_DECAL) && shadow_decal_fits(r, r.comb);
}

namespace {

// whether a stage reads a source (colour or alpha)
bool reads(const engine::gx::CombinerStage& st, u8 src) {
    for (int i = 0; i < 3; i++)
        if ((st.src_rgb[i] & 15) == src || (st.src_a[i] & 15) == src)
            return true;
    return false;
}

engine::gx::CombinerStage pass_on() {
    engine::gx::CombinerStage s;
    memset(&s, 0, sizeof(s));
    for (int k = 0; k < 3; k++)
        s.src_rgb[k] = s.src_a[k] = engine::gx::SRC_PREVIOUS;
    s.func_rgb = s.func_a = engine::gx::FUNC_REPLACE;
    return s;
}

}  // namespace

bool shadow_receiver_fits(const Material& m, const engine::gx::Combiners& comb) {
    const u32 n = std::max<u32>(1, comb.n_stages);
    if (n > 4 || m.unit[2].texture != fmt::NONE16 || m.unit[0].source == TexCoordSource::PROJECTION)
        return false;
    for (u32 i = 0; i < n; i++)
        if (reads(comb.stage[i], engine::gx::SRC_TEXTURE2))
            return false;
    return true;
}

void shadow_receiver_combiners(const engine::gx::Combiners& in, f32 darkness, engine::gx::Combiners* out) {
    using namespace engine::gx;
    *out = in;
    const u32 n = std::max<u32>(1, in.n_stages);
    for (u32 i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            if ((out->stage[i].src_rgb[k] & 15) == SRC_TEXTURE0)
                out->stage[i].src_rgb[k] = SRC_TEXTURE2;
            if ((out->stage[i].src_a[k] & 15) == SRC_TEXTURE0)
                out->stage[i].src_a[k] = SRC_TEXTURE2;
        }
    // the material's result into the buffer (read two stages on: the PICA's buffer is a stage behind)
    out->buf_rgb |= (u8)(1u << (n - 1));
    const u32 k = (u32)std::lround(std::max(0.0f, std::min(1.0f, darkness)) * 255.0f);
    // f = TEXTURE0 x darkness + (1 - darkness)
    CombinerStage& f = out->stage[n];
    f = pass_on();
    f.src_rgb[0] = SRC_TEXTURE0, f.src_rgb[1] = SRC_CONSTANT, f.src_rgb[2] = SRC_CONSTANT;
    f.op_rgb[2] = OP_RGB_ONE_MINUS_SRC_COLOR;
    f.func_rgb = FUNC_MULTIPLY_ADD;
    f.konst = k | k << 8 | k << 16 | 0xFF000000u;
    // the material's colour (the buffer) x f, its alpha passed on
    CombinerStage& c = out->stage[n + 1];
    c = pass_on();
    c.src_rgb[0] = SRC_PREVIOUS, c.src_rgb[1] = SRC_PREVIOUS_BUFFER;
    c.func_rgb = FUNC_MODULATE;
    for (u32 i = n + 2; i < 6; i++)
        out->stage[i] = pass_on();
    out->n_stages = (u8)(n + 2);
}

bool shadow_decal_fits(const Material& m, const engine::gx::Combiners& comb) {
    const u32 n = std::max<u32>(1, comb.n_stages);
    if (n > 5 || m.unit[1].texture != fmt::NONE16)
        return false;
    for (u32 i = 0; i < n; i++)
        if (reads(comb.stage[i], engine::gx::SRC_TEXTURE1))
            return false;
    return true;
}

void shadow_decal_combiners(const engine::gx::Combiners& in, engine::gx::Combiners* out) {
    using namespace engine::gx;
    *out = in;
    const u32 n = std::max<u32>(1, in.n_stages);
    // the alpha times the fade window's (TEXTURE1's alpha), the colour passed on
    CombinerStage& a = out->stage[n];
    a = pass_on();
    a.src_a[0] = SRC_PREVIOUS, a.src_a[1] = SRC_TEXTURE1;
    a.func_a = FUNC_MODULATE;
    for (u32 i = n + 1; i < 6; i++)
        out->stage[i] = pass_on();
    out->n_stages = (u8)(n + 1);
}

}  // namespace engine::g3d
