// nw4r::lyt's Material::SetupGX as state, for the native layout draw (draw.h): the values the GX
// registers hold after the decompiled function's forty-odd GX calls (nw4r_lyt_Material_SetupGX_orig;
// engine/lyt/material.cpp sends the same commands), worked out from the material in guest memory with
// no GX call and no guest call: the TEV stages (the material's own, or the default ones for its
// number of textures) with its colour registers, konst colours and swap tables; the texture maps as
// GXLoadTexObj loads them, palette ones with the palette load GXLoadTlut sends before them; the
// texture coordinate generators with the matrices XF holds for them (CalcTextureMtx's, by the
// generator's matrix number); the lighting channel's colour sources and material colour; the alpha
// compare and the blend mode.
//
// What a packet can't carry goes back to the caller (false: the pane is drawn through the guest):
// more maps or coordinates than the PICA has, indirect texturing, and whatever makes the GX path's
// result depend on GX state the material doesn't set (a texture matrix it doesn't load, a texture
// map past the ones it loads, a material colour register it doesn't set).
#include "engine/lyt/draw.h"

#include <cstring>

#include "noma_decomp.h"

namespace engine::lyt {

namespace {

// __GXData (the fields this reads)
enum : u32 {
    D_TEX_REGION_CB = 0x518,          // the texture region callback (GXSetTexRegionCallback)
    D_TLUT_REGION_CB = 0x51C,         // the TLUT region callback (GXSetTlutRegionCallback)
    // GXInit's texture cache regions, eight of each (0x10 bytes: the SETIMAGE1 and SETIMAGE2 words):
    // for textures without mipmaps and palette ones, for mipmapped ones and RGBA8 ones, for
    // mipmapped RGBA8 ones
    D_TEX_REGIONS = 0x258,
    D_TEX_REGIONS_MIP = 0x2D8,
    D_TEX_REGIONS_RGBA8_MIP = 0x358,
    // GXInit's twenty TLUT regions (0x10 bytes: the LOADTLUT1 word with the region's TMEM address
    // and size, then GXLoadTlut's copy of the palette last loaded there: its SETTLUT word, its
    // LOADTLUT0 word, its entry count)
    D_TLUT_REGIONS = 0x3D8,
};

inline f32 loadf(u32 a) {
    const u32 b = guest::load32(a);
    f32 f;
    ::memcpy(&f, &b, 4);
    return f;
}

// a material's GX block (+0x40): where each part is, by the capacities (+0x38, which also say which
// of the optional parts it has), and how many of each SetupGX uses, by the counts (+0x3C)
struct Mat {
    u32 cap, num, res;
    u32 n_tex_maps() const { return num >> 28; }
    u32 n_tex_srts() const { return (num >> 24) & 0xF; }
    u32 n_tex_gens() const { return (num >> 20) & 0xF; }
    u32 n_ind_stages() const { return (num >> 15) & 7; }
    u32 n_tev_stages() const { return (num >> 9) & 0x1F; }
    bool has_swap_table() const { return (cap >> 14) & 1; }
    bool has_chan_ctrl() const { return (cap >> 8) & 1; }
    bool has_mat_color() const { return (cap >> 7) & 1; }
    bool has_alpha_compare() const { return (cap >> 6) & 1; }
    bool has_blend_mode() const { return (cap >> 5) & 1; }
    u32 tex_map(u32 i) const { return res + i * 0x1C; }
    u32 tex_srt(u32 j) const { return res + (cap >> 28) * 0x1C + j * 0x14; }
    u32 tex_gens() const { return tex_srt((cap >> 24) & 0xF); }
    u32 chan_ctrl() const { return tex_gens() + ((cap >> 18) & 0x3C); }
    u32 mat_color() const { return chan_ctrl() + ((cap >> 6) & 4); }
    u32 swap_table() const { return mat_color() + ((cap >> 5) & 4); }
    u32 alpha_compare() const { return swap_table() + ((cap >> 12) & 4); }
    u32 blend_mode() const { return alpha_compare() + ((cap >> 4) & 4); }
    // past the indirect stages and the indirect SRTs
    u32 tev_stages() const { return blend_mode() + ((cap >> 3) & 4) + ((cap >> 13) & 0x1C) + ((cap >> 18) & 3) * 0x14; }
};

// The TEV registers as the GX library's setters (GXTev.c) leave them: the same fields of the same
// words. SetupGX sets every field of each stage's two combiner words, so what the words held before
// doesn't show in the result.
struct Tev {
    gx::TevRegs& r;

    // GXSetTevOrder: the stage's half of its TREF word
    void order(u32 s, u32 coord, u32 map, u32 color) const {
        const u32 m = map & ~0x100u;
        const u32 tmap = m >= 8 ? 0 : m;
        if (coord >= 8)
            coord = 0;
        // the colour channel as the rasterizer's input, through GXTev.c's table (GXTevChannelToRas:
        // proposed GXTev_c2r)
        const u32 chan = color == 0xFF ? 7 : guest::load32(GA_GXTevChannelToRas + color * 4);
        const u32 enable = map != 0xFF && !(map & 0x100);
        const u32 half = (tmap & 7) | ((coord << 3) & 0x38) | (enable << 6) | ((chan << 7) & 0x380);
        const u32 shift = (s & 1) * 12;
        r.tref[s / 2] = (r.tref[s / 2] & ~(0x3FFu << shift)) | half << shift;
    }
    // GXSetTevSwapMode: the swap tables the stage's rasterized colour and texture go through
    void swap_mode(u32 s, u32 ras, u32 tex) const {
        r.alpha_env[s] = (r.alpha_env[s] & ~0xFu) | (ras & 3) | ((tex << 2) & 0xC);
    }
    void color_in(u32 s, u32 a, u32 b, u32 c, u32 d) const {
        r.color_env[s] = (r.color_env[s] & ~0xFFFFu) | ((a << 12) & 0xF000) | ((b << 8) & 0xF00) | ((c << 4) & 0xF0) | (d & 0xF);
    }
    void alpha_in(u32 s, u32 a, u32 b, u32 c, u32 d) const {
        r.alpha_env[s] =
            (r.alpha_env[s] & ~0xFFF0u) | ((a << 13) & 0xE000) | ((b << 10) & 0x1C00) | ((c << 7) & 0x380) | ((d << 4) & 0x70);
    }
    // GXSetTevColorOp / GXSetTevAlphaOp on a combiner word (the compare operations keep their
    // operation where the others have the scale, and the bias field all set)
    static u32 op(u32 v, u32 op, u32 bias, u32 scale, u32 clamp, u32 out) {
        v = (v & ~0x40000u) | ((op << 18) & 0x40000);
        if ((s32)op <= 1)
            v = (v & ~0x330000u) | ((scale << 20) & 0x300000) | ((bias << 16) & 0x30000);
        else
            v = (v & ~0x330000u) | ((op << 19) & 0x300000) | 0x30000;
        return (v & ~0xC80000u) | ((clamp << 19) & 0x80000) | ((out << 22) & 0xC00000);
    }
    void color_op(u32 s, u32 o, u32 bias, u32 scale, u32 clamp, u32 out) const {
        r.color_env[s] = op(r.color_env[s], o, bias, scale, clamp, out);
    }
    void alpha_op(u32 s, u32 o, u32 bias, u32 scale, u32 clamp, u32 out) const {
        r.alpha_env[s] = op(r.alpha_env[s], o, bias, scale, clamp, out);
    }
    void kcolor_sel(u32 s, u32 sel) const {
        u32& w = r.ksel[s / 2];
        w = s & 1 ? (w & ~0x7C000u) | ((sel << 14) & 0x7C000) : (w & ~0x1F0u) | ((sel << 4) & 0x1F0);
    }
    void kalpha_sel(u32 s, u32 sel) const {
        u32& w = r.ksel[s / 2];
        w = s & 1 ? (w & ~0xF80000u) | ((sel << 19) & 0xF80000) : (w & ~0x3E00u) | ((sel << 9) & 0x3E00);
    }
    // GXSetTevSwapModeTable: table t in the low bits of KSEL 2t (red, green) and 2t + 1 (blue, alpha)
    void swap_table(u32 t, u32 red, u32 green, u32 blue, u32 alpha) const {
        r.ksel[t * 2] = (r.ksel[t * 2] & ~0xFu) | (red & 3) | ((green << 2) & 0xC);
        r.ksel[t * 2 + 1] = (r.ksel[t * 2 + 1] & ~0xFu) | (blue & 3) | ((alpha << 2) & 0xC);
    }
};

// a TEV colour register's component as the decoder takes it from what GXSetTevColorS10 sends: the
// s16's low 11 bits, signed
inline s32 sext11(u32 v) { return (s32)(v << 21) >> 21; }

// nw4r::lyt's CalcTextureMtx (0x8045F230), the first two rows (what GXLoadTexMtxImm loads as a
// GX_MTX2x4): scale and rotation about the texture's centre (0.5, 0.5), then the translation. The
// expressions are the decompiled function's, operand for operand (-s * sy in double, as there), so
// that the host compiler fuses the same products into the same sums and the floats come out the
// same; the sine and cosine are the native functions it calls.
void texture_mtx(u32 srt, f32 m[2][4]) {
    const f32 center = noma_kf32(0x3F000000u);
    const f32 rotate = loadf(srt + 0x8);  // degrees, to FIdx (256 a turn)
    const f64 c = g::nw4r_math_CosFIdx(0, 0, (f64)(noma_kf32(0x3F360B61u) * rotate));
    const f64 s = g::nw4r_math_SinFIdx((f64)(noma_kf32(0x3F360B61u) * rotate));
    const f32 sx = loadf(srt + 0xC), sy = loadf(srt + 0x10);
    const f32 s_sx = (f32)s * sx, c_sx = (f32)c * sx;
    const f32 neg_s_sy = -s * sy, c_sy = (f32)c * sy;
    const f32 tx = loadf(srt) + center, ty = loadf(srt + 0x4) + center;
    m[0][0] = c_sx, m[0][1] = neg_s_sy, m[0][2] = 0.0f;
    m[0][3] = (neg_s_sy * -center) + (tx + (c_sx * -center));
    m[1][0] = s_sx, m[1][1] = c_sy, m[1][2] = 0.0f;
    m[1][3] = (c_sy * -center) + (ty + (s_sx * -center));
}

// The GXTexObj nw4r::lyt's TexMap::Get (0x80461F40) makes from a TexMap (0x1C bytes: image, palette,
// width, height, min/max LOD, LOD bias x 256, palette entries, the format/wrap/filter bits), as far
// as GXLoadTexObj reads it: GXInitTexObj (or GXInitTexObjCI) then GXInitTexObjLOD, whose filter and
// LOD fields replace the ones GXInitTexObj puts in the first two words.
struct TexObj {
    u32 mode0, mode1, image0, image3;  // its first four words, without the register numbers
    u32 fmt;                           // +0x14
    u32 mipmap;                        // +0x1F bit 0
};
TexObj texmap_obj(u32 tm) {
    const u32 bits = guest::load32(tm + 0x18);
    const u32 fmt = bits >> 28, wrap_s = (bits >> 25) & 3, wrap_t = (bits >> 23) & 3;
    const u32 min_filter = (bits >> 20) & 7, mag_filter = (bits >> 19) & 1, bias_clamp = (bits >> 18) & 1,
              edge_lod = (bits >> 17) & 1, aniso = (bits >> 15) & 3;
    const u32 w = guest::load16(tm + 0x8), h = guest::load16(tm + 0xA);
    TexObj o;
    o.fmt = fmt;
    o.mipmap = (bits >> 27) & 1;
    o.image0 = ((w - 1) & 0x3FF) | (((h - 1) << 10) & 0xFFC00) | ((fmt << 20) & 0xF00000);
    o.image3 = (guest::load32(tm) >> 5) & 0xFFFFFF;
    // GXInitTexObjLOD: the bias (TexMap::Get passes it as the u16 / 256) to -4 .. 3.99, the LODs to
    // 0 .. 10, as the decompiled function clamps and converts them
    f64 bias = (f64)((f32)(u32)guest::load16(tm + 0x14) * noma_kf32(0x3B800000u));
    if (bias < (f64)noma_kf32(0xC0800000u))
        bias = (f64)noma_kf32(0xC0800000u);
    else if (bias >= (f64)noma_kf32(0x40800000u))
        bias = (f64)noma_kf32(0x407F5C29u);
    f64 min_lod = (f64)loadf(tm + 0xC), max_lod = (f64)loadf(tm + 0x10);
    if (min_lod < (f64)noma_kf32(0x00000000u))
        min_lod = (f64)noma_kf32(0x00000000u);
    else if (min_lod > (f64)noma_kf32(0x41200000u))
        min_lod = (f64)noma_kf32(0x41200000u);
    if (max_lod < (f64)noma_kf32(0x00000000u))
        max_lod = (f64)noma_kf32(0x00000000u);
    else if (max_lod > (f64)noma_kf32(0x41200000u))
        max_lod = (f64)noma_kf32(0x41200000u);
    // the minification filter through the GX library's table (GX_NEAR .. GX_LIN_MIP_LIN to the
    // register's encoding; GX2HWFiltConv: proposed GX2HWFiltConv), the magnification filter and edge
    // LOD as single bits (edge LOD inverted)
    o.mode0 = (wrap_s & 3) | ((wrap_t << 2) & 0xC) | ((mag_filter << 4) & 0x10) |
              ((guest::load8(GA_GX2HWFiltConv + min_filter) << 5) & 0xE0) | (((edge_lod ^ 1) << 8) & 0x100) |
              ((NOMA_FCTIWZ((noma_kf32(0x42000000u) * (f32)bias)) << 9) & 0x1FE00) | ((aniso << 19) & 0x180000) |
              ((bias_clamp << 21) & 0x200000);
    o.mode1 = (NOMA_FCTIWZ((noma_kf32(0x41800000u) * (f32)min_lod)) & 0xFF) |
              ((NOMA_FCTIWZ((noma_kf32(0x41800000u) * (f32)max_lod)) << 8) & 0xFF00);
    return o;
}

inline bool is_palette(u32 fmt) { return fmt - 8 <= 2; }  // GX_TF_C4, C8, C14X2

// Whether the GX library's region callbacks are GXInit's (__GXDefaultTexRegionCallback,
// __GXDefaultTlutRegionCallback): regions by texture map and by TLUT name, without side effects,
// worked out below. The game registers no others (GXSetTexRegionCallback and
// GXSetTlutRegionCallback are called only by GXInit).
bool default_regions(u32 gxd) {
    return guest::load32(gxd + D_TEX_REGION_CB) == GA___GXDefaultTexRegionCallback &&
           guest::load32(gxd + D_TLUT_REGION_CB) == GA___GXDefaultTlutRegionCallback;
}
// __GXDefaultTexRegionCallback: the cache region of texture map `map` for the texture's kind
u32 tex_region(u32 gxd, u32 fmt, u32 mipmap, u32 map) {
    u32 base = D_TEX_REGIONS;
    if (!is_palette(fmt)) {
        if (fmt == 6)  // GX_TF_RGBA8
            base = mipmap ? D_TEX_REGIONS_RGBA8_MIP : D_TEX_REGIONS_MIP;
        else if (mipmap)
            base = D_TEX_REGIONS_MIP;
    }
    return gxd + base + (map & 7) * 0x10;
}
// __GXDefaultTlutRegionCallback: TLUT region `name` (none past the twentieth)
u32 tlut_region(u32 gxd, u32 name) { return name < 20 ? gxd + D_TLUT_REGIONS + name * 0x10 : 0; }

// what GXLoadTexObjPreLoaded sends for the texture object, with its texture region's words
LytTex tex_words(const TexObj& o, u32 region) {
    LytTex t;
    t.mode0 = o.mode0, t.mode1 = o.mode1, t.image0 = o.image0, t.image3 = o.image3;
    t.image1 = guest::load32(region) & 0xFFFFFF;
    t.image2 = guest::load32(region + 4) & 0xFFFFFF;
    return t;
}

}  // namespace

bool material_state(u32 material, bool modulate, u8 alpha, LytMaterial& out) {
    NOMA_PROF_SCOPE("(lyt material)");
    const Mat t{guest::load32(material + 0x38), guest::load32(material + 0x3C), guest::load32(material + 0x40)};
    const u32 n_maps = t.n_tex_maps(), n_gens = t.n_tex_gens(), n_stages = t.n_tev_stages();
    // More texture maps or coordinates than the packet has; indirect stages; more TEV stages than GX
    // has (the count would wrap in GENMODE); more texture SRTs than the ten SetupGX keeps flags for
    // (SRTs past the tenth go by bytes of the decompiled function's stack left from earlier calls,
    // and the eleventh's matrix would replace XF's identity, GX_IDENTITY).
    if (n_maps > LYT_MAX_MAPS || n_gens > LYT_MAX_COORDS || t.n_ind_stages() || n_stages > 16 || t.n_tex_srts() > 10)
        return false;
    out = LytMaterial{};
    LytState& st = out.state;
    st.n_chans = 1;  // GXSetNumChans(1)

    // The lighting channel (lighting off): the colour and the alpha from the vertex or from the
    // material colour register. A source other than GX_SRC_REG and GX_SRC_VTX puts its low bit in the
    // register but leaves the material colour unset or the vertex colours unsent.
    bool use_vtx = true, use_mat = false;
    out.color_from_vtx = out.alpha_from_vtx = true;
    if (t.has_chan_ctrl()) {
        const u32 p = t.chan_ctrl(), c = guest::load8(p), a = guest::load8(p + 1);
        if (c > 1 || a > 1)
            return false;
        out.color_from_vtx = c == 1, out.alpha_from_vtx = a == 1;
        use_vtx = c == 1 || a == 1;
        use_mat = c == 0 || a == 0;
    }
    bool modulate_tev = use_vtx && modulate;
    // the material colour (white without one), its alpha times the pane's (detail::MultipleAlpha:
    // rounded down); GXSetChanMatColor gets it where a channel takes the register
    u32 color = t.has_mat_color() ? guest::load32(t.mat_color()) : 0xFFFFFFFF;
    if (alpha != 0xFF)
        color = (color & ~0xFFu) | ((color & 0xFF) * alpha / 255);
    out.mat_color = color;
    if (use_mat)
        modulate_tev = modulate_tev || color != 0xFFFFFFFF;

    // Texture coordinates: GX_TG_MTX2x4 generators from a texture coordinate set, through XF's
    // identity (GX_IDENTITY, which GXInit loads) or texture matrix 30 + 3j, which SetupGX loads with
    // texture SRT j's matrix (by the generator's matrix number, not its index). Another matrix
    // number holds whatever XF had; other kinds of generator aren't in the packet.
    st.n_texgens = out.n_gens = n_gens;
    for (u32 k = 0, p = t.tex_gens(); k < n_gens; k++, p += 4) {
        const u32 type = guest::load8(p), src = guest::load8(p + 1), mtx = guest::load8(p + 2);
        if (type != 1 || src - 4 > 7)  // GX_TG_MTX2x4, GX_TG_TEX0-7
            return false;
        LytTexGen& gen = out.gen[k];
        gen.src = src - 4;
        if (mtx == 60) {
            gen.m[0][0] = gen.m[1][1] = 1.0f;
        } else {
            if (mtx < 30 || (mtx - 30) % 3 || (mtx - 30) / 3 >= t.n_tex_srts())
                return false;
            texture_mtx(t.tex_srt((mtx - 30) / 3), gen.m);
        }
    }

    // The textures, map i from TexMap i. A palette texture's TLUT is loaded into TLUT region
    // GX_TLUT0 + k (C4, C8: the k-th of the material's) or GX_BIGTLUT0 + k (C14X2) from the TexMap's
    // palette, before the texture: LOADTLUT0 the palette's address, LOADTLUT1 the region's TMEM
    // address and size, and the texture's SETTLUT the palette format with the region's TMEM address.
    st.n_maps = n_maps;
    if (n_maps) {
        const u32 gxd = guest::load32(GA___GXData);
        if (!default_regions(gxd))
            return false;
        u32 n_tlut = 0, n_big_tlut = 0x10;
        for (u32 i = 0; i < n_maps; i++) {
            const u32 tm = t.tex_map(i);
            const TexObj o = texmap_obj(tm);
            LytTex& tex = st.tex[i] = tex_words(o, tex_region(gxd, o.fmt, o.mipmap, i));
            if (is_palette(o.fmt)) {
                const u32 load1 = guest::load32(tlut_region(gxd, o.fmt == 10 ? n_big_tlut++ : n_tlut++)) & 0xFFFFFF;
                tex.tlut = (((guest::load32(tm + 0x18) >> 13) & 3) << 10) | (load1 & 0x3FF);
                tex.tlut_load[0] = (guest::load32(tm + 0x4) >> 5) & 0xFFFFFF;
                tex.tlut_load[1] = load1;
            }
        }
    }

    // TEV colour registers C0-C2 and the konst colours, as the decoder takes what GXSetTevColorS10
    // and GXSetTevKColor send
    gx::TevRegs& regs = st.tev;
    for (u32 r = 1; r <= 3; r++)
        for (u32 k = 0; k < 4; k++)
            regs.reg[r][k] = sext11(guest::load16(material + 0x8 + r * 8 + k * 2));
    for (u32 r = 0; r < 4; r++) {
        const u32 rgba = guest::load32(material + 0x28 + r * 4);
        regs.konst[r][0] = rgba >> 24, regs.konst[r][1] = (rgba >> 16) & 0xFF;
        regs.konst[r][2] = (rgba >> 8) & 0xFF, regs.konst[r][3] = rgba & 0xFF;
    }
    const Tev tev{regs};
    if (t.has_swap_table()) {
        for (u32 k = 0, p = t.swap_table(); k < 4; k++) {
            const u32 s = guest::load8(p + k);
            tev.swap_table(k, s & 3, (s >> 2) & 3, (s >> 4) & 3, (s >> 6) & 3);
        }
    } else {
        tev.swap_table(0, 0, 1, 2, 3);
        tev.swap_table(1, 0, 0, 0, 3);
        tev.swap_table(2, 1, 1, 1, 3);
        tev.swap_table(3, 2, 2, 2, 3);
    }

    // the TEV stages: the material's own, or the default ones for its number of textures
    if (n_stages) {
        for (u32 s = 0, p = t.tev_stages(); s < n_stages; s++, p += 0x10) {
            u8 b[16];
            for (u32 k = 0; k < 16; k += 4) {
                const u32 w = guest::load32(p + k);
                b[k] = w >> 24, b[k + 1] = w >> 16, b[k + 2] = w >> 8, b[k + 3] = w;
            }
            // an indirect matrix (the stage's offsets from an indirect texture)
            if ((b[13] >> 3) & 0xF)
                return false;
            const u32 map = b[2] | ((b[3] << 8) & 0x100);
            // a texture map past the material's holds the texture of an earlier draw
            if (map != 0xFF && !(map & 0x100) && (map >= 8 ? 0 : map) >= n_maps)
                return false;
            tev.order(s, b[0], map, b[1]);
            tev.swap_mode(s, (b[3] >> 1) & 3, (b[3] >> 3) & 3);
            tev.color_in(s, b[4] & 0xF, b[4] >> 4, b[5] & 0xF, b[5] >> 4);
            tev.color_op(s, b[6] & 0xF, (b[6] >> 4) & 3, (b[6] >> 6) & 3, b[7] & 1, (b[7] >> 1) & 3);
            tev.kcolor_sel(s, (b[7] >> 3) & 0x1F);
            tev.alpha_in(s, b[8] & 0xF, b[8] >> 4, b[9] & 0xF, b[9] >> 4);
            tev.alpha_op(s, b[10] & 0xF, (b[10] >> 4) & 3, (b[10] >> 6) & 3, b[11] & 1, (b[11] >> 1) & 3);
            tev.kalpha_sel(s, (b[11] >> 3) & 0x1F);
        }
        regs.n_stages = n_stages;
        modulate_tev = true;
    } else {
        u32 s = 0;
        if (n_maps == 0) {  // C1 times the rasterized colour
            tev.order(0, 0xFF, 0xFF, 4);
            tev.color_in(0, 0xF, 4, 0xA, 0xF);
            tev.alpha_in(0, 7, 2, 5, 7);
            modulate_tev = true;
            s = 1;
        } else {
            if (n_maps == 1) {  // the texture between C0 and C1
                tev.order(0, 0, 0, 0xFF);
                tev.color_in(0, 2, 4, 8, 0xF);
                tev.alpha_in(0, 1, 2, 4, 7);
                s = 1;
            } else {
                // the konst selections come from nw4r::lyt's tables (nw4r_lyt_Material_kColSels, nw4r_lyt_Material_kAlpSels:
                // proposed nw4r_lyt_kColSels, nw4r_lyt_kAlpSels)
                if (n_maps == 2) {  // texture 1 to texture 0 by K3's alpha
                    tev.order(0, 0, 0, 0xFF);
                    tev.color_in(0, 0xF, 0xF, 0xF, 8);
                    tev.alpha_in(0, 7, 7, 7, 4);
                    tev.order(1, 1, 1, 0xFF);
                    tev.color_in(1, 8, 0, 0xE, 0xF);
                    tev.alpha_in(1, 4, 0, 6, 7);
                    tev.kcolor_sel(1, guest::load32(GA_nw4r_lyt_Material_kColSels));
                    tev.kalpha_sel(1, guest::load32(GA_nw4r_lyt_Material_kAlpSels));
                    s = 2;
                } else {  // the textures summed, each times a konst component
                    for (u32 i = 0; i < n_maps; i++, s++) {
                        tev.order(s, i, i, 0xFF);
                        tev.color_in(s, 0xF, 8, 0xE, i == 0 ? 0xF : 0);
                        tev.alpha_in(s, 7, 4, 6, i == 0 ? 7 : 0);
                        tev.kcolor_sel(s, guest::load32(GA_nw4r_lyt_Material_kColSels + i * 4));
                        tev.kalpha_sel(s, guest::load32(GA_nw4r_lyt_Material_kAlpSels + i * 4));
                    }
                }
                // the result between C0 and C1, unless they're black and white (nw4r_lyt_Material_DefaultBlackColor, the
                // black compared with: proposed nw4r_lyt_DefaultBlackColor)
                bool plain = true;
                for (u32 k = 0; k < 4; k++)
                    plain = plain && (s16)guest::load16(material + 0x10 + k * 2) == (s16)guest::load16(GA_nw4r_lyt_Material_DefaultBlackColor + k * 2);
                for (u32 k = 0; plain && k < 4; k++)
                    plain = (s16)guest::load16(material + 0x18 + k * 2) == 0xFF;
                if (!plain) {
                    tev.order(s, 0xFF, 0xFF, 0xFF);
                    tev.color_in(s, 2, 4, 0, 0xF);
                    tev.alpha_in(s, 1, 2, 0, 7);
                    s++;
                }
            }
            if (modulate_tev) {  // the rasterized colour times the result
                tev.order(s, 0xFF, 0xFF, 4);
                tev.color_in(s, 0xF, 0, 0xA, 0xF);
                tev.alpha_in(s, 7, 0, 5, 7);
                s++;
            }
        }
        for (u32 k = 0; k < s; k++) {
            tev.color_op(k, 0, 0, 0, 1, 0);
            tev.alpha_op(k, 0, 0, 0, 1, 0);
            tev.swap_mode(k, 0, 0);
        }
        regs.n_stages = s;
    }

    // the alpha compare (BP 0xF3) and the blend (BP 0x41's blend and logic op bits)
    u32 comp0 = 7, ref0 = 0, op = 0, comp1 = 7, ref1 = 0;
    if (t.has_alpha_compare()) {
        const u32 p = t.alpha_compare(), c = guest::load8(p);
        comp0 = c & 0xF, ref0 = guest::load8(p + 2), op = guest::load8(p + 1), comp1 = (c >> 4) & 0xF, ref1 = guest::load8(p + 3);
    }
    st.alpha_compare =
        (ref0 & 0xFF) | ((ref1 << 8) & 0xFF00) | ((comp0 << 16) & 0x70000) | ((comp1 << 19) & 0x380000) | ((op << 22) & 0xC00000);
    u32 type = 1, src = 4, dst = 5, logic = 0xF;
    if (t.has_blend_mode()) {
        const u32 p = t.blend_mode();
        type = guest::load8(p), src = guest::load8(p + 1), dst = guest::load8(p + 2), logic = guest::load8(p + 3);
    }
    st.cmode0 = (type & 1) | (type == 2 ? 2 : 0) | ((dst << 5) & 0xE0) | ((src << 8) & 0x700) | (type == 3 ? 0x800 : 0) |
                ((logic << 12) & 0xF000);

    out.use_vtx = modulate_tev && use_vtx;
    return true;
}

LytTex tex_from_obj(u32 obj) {
    // The words as GXLoadTexObj sends them for texture map 0 (where the callers' texture objects go:
    // CharWriter's font sheets): image1 and image2 are its texture region's for that map.
    const u32 gxd = guest::load32(GA___GXData);
    const u32 fmt = guest::load32(obj + 0x14), flags = guest::load8(obj + 0x1F);
    TexObj o;
    o.mode0 = guest::load32(obj) & 0xFFFFFF;
    o.mode1 = guest::load32(obj + 0x4) & 0xFFFFFF;
    o.image0 = guest::load32(obj + 0x8) & 0xFFFFFF;
    o.image3 = guest::load32(obj + 0xC) & 0xFFFFFF;
    o.fmt = fmt;
    o.mipmap = flags & 1;
    // the regions from the callbacks: GXInit's worked out, any other called as GXLoadTexObj calls it
    const bool palette = !(flags & 2);  // GXInitTexObjCI clears it
    u32 region, tlut = 0;
    if (default_regions(gxd)) {
        region = tex_region(gxd, fmt, o.mipmap, 0);
        if (palette)
            tlut = tlut_region(gxd, guest::load32(obj + 0x18));
    } else {
        region = gcall<u32>(guest::load32(gxd + D_TEX_REGION_CB), obj, 0u, gxd);
        if (palette)
            tlut = gcall<u32>(guest::load32(gxd + D_TLUT_REGION_CB), guest::load32(obj + 0x18), gxd);
    }
    LytTex t = tex_words(o, region);
    // A palette texture's SETTLUT is its TLUT region's word, and its palette the one GXLoadTlut last
    // loaded into that region (the region keeps its LOADTLUT0 word: 0 if none was).
    if (tlut) {
        t.tlut = guest::load32(tlut + 4) & 0xFFFFFF;
        if (const u32 load0 = guest::load32(tlut + 8)) {
            t.tlut_load[0] = load0 & 0xFFFFFF;
            t.tlut_load[1] = guest::load32(tlut) & 0xFFFFFF;
        }
    }
    return t;
}

void material_colors(const LytMaterial& m, const u32* vc, u8 out[4][4]) {
    // colour channel 0 with lighting off (runtime/hw/gx/xform.cpp): each of colour and alpha from
    // the vertex or from the material register; a vertex without a colour attribute decodes as 0
    // (runtime/hw/gx/vertex.cpp)
    for (u32 k = 0; k < 4; k++) {
        const u32 v = vc ? vc[k] : 0;
        const u32 c = m.color_from_vtx ? v : m.mat_color, a = m.alpha_from_vtx ? v : m.mat_color;
        out[k][0] = (u8)(c >> 24), out[k][1] = (u8)(c >> 16), out[k][2] = (u8)(c >> 8), out[k][3] = (u8)a;
    }
}

}  // namespace engine::lyt
