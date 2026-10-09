// nw4r::lyt's Material::SetupGX, native: every pane a layout draws first sets the GX state of its
// material (lighting channels, texture coordinates, textures, TEV colors, swap tables and stages,
// alpha compare, blending), about forty GX calls a pane. The decompiled function called the GX
// library for each, and each call read __GXData through a page lookup and put its command in the
// write-gather pipe a byte or a word at a time. Here the same commands go to the pipe and the same
// __GXData fields and dirty bits are written, worked out in one function on a host pointer.
// Materials with palette (TLUT) textures or indirect stages (none in the room) go to the decompiled
// function, nw4r_lyt_Material_SetupGX_orig. NOMA_CHECK_LYT_MATERIAL=1 compares the two on every setup.
#include <vector>

#include "noma_decomp.h"
#include "runtime.h"

namespace g {
u32 nw4r_lyt_Material_SetupGX_orig(u32 m, u32 modulate, u32 alpha);  // (the decompiled one)
}

namespace engine::lyt {

namespace {

// a register write (0x61 and the value) to the write-gather pipe, one bounds check for its 5 bytes
// where it fits before the buffer's end, else as the decompiled code's two stores
inline void bp(u32 v) {
    u32 n = guest::g_gather_n;
    if (__builtin_expect(n <= 32 - 5, 1)) {
        guest::g_gather[n] = 0x61;
        u32 w = __builtin_bswap32(v);
        ::memcpy(guest::g_gather + n + 1, &w, 4);
        guest::g_gather_n = n += 5;
        if (n == 32)
            guest::gx_gather_burst();
    } else {
        guest::gather_put<1>(0x61);
        guest::gather_put<4>(v);
    }
}

// slw: a shift by 32 or more gives 0
inline u32 slw(u32 v, u32 n) { return (n & 0x3F) < 32 ? v << (n & 0x3F) : 0; }

// __GXData through its host pointer, and the GX library's setters as the SDK writes them
struct Gx {
    u8* d;

    u32 r32(u32 o) const {
        u32 v;
        ::memcpy(&v, d + o, 4);
        return __builtin_bswap32(v);
    }
    void w32(u32 o, u32 v) const {
        v = __builtin_bswap32(v);
        ::memcpy(d + o, &v, 4);
    }
    void sent() const { d[2] = 0, d[3] = 0; }  // bpSentNot = 0 (a BP register went out)
    void dirty(u32 bits) const { w32(0x5FC, r32(0x5FC) | bits); }
    void field(u32 o, u32 mask, u32 v) const { w32(o, (r32(o) & ~mask) | (v & mask)); }

    // GXLight.c
    void num_chans(u32 n) const { field(0x254, 0x70, n << 4), dirty(0x01000004); }
    void chan_ctrl(u32 chan, u32 enable, u32 amb_src, u32 mat_src, u32 lights, u32 diff_fn, u32 attn_fn) const {
        const u32 diff = attn_fn == 0 ? 0 : diff_fn;
        const u32 i = chan & 3;
        const u32 v = ((enable << 1) & 2) | (mat_src & 1) | ((amb_src << 6) & 0x40) | ((diff << 7) & 0x180) |
                      ((((2 - attn_fn) | (attn_fn - 2)) >> 22) & 0x200) | ((((0u - attn_fn) | attn_fn) >> 21) & 0x400) |
                      ((lights << 2) & 0x3C) | ((lights << 7) & 0x7800);
        w32(0xB8 + i * 4, v);
        dirty(slw(0x1000, i));
        if (chan == 4)
            w32(0xC0, v), dirty(0x5000);
        else if (chan == 5)
            w32(0xC4, v), dirty(0xA000);
    }
    void chan_mat_color0a0(u32 color) const { dirty(0x400), w32(0xB0, color); }  // GXSetChanMatColor(GX_COLOR0A0)

    // GXAttr.c
    void num_tex_gens(u32 n) const { field(0x254, 0xF, n), dirty(0x02000004); }
    void tex_coord_gen2(u32 id, u32 func, u32 src, u32 mtx, u32 normalize, u32 post_mtx) const {
        u32 row = 5, form = 0;
        switch (src) {
        case 0: row = 0, form = 1; break;
        case 1: row = 1, form = 1; break;
        case 2: row = 3, form = 1; break;
        case 3: row = 4, form = 1; break;
        case 19:
        case 20: row = 2; break;
        case 4: row = 5; break;
        case 5: row = 6; break;
        case 6: row = 7; break;
        case 7: row = 8; break;
        case 8: row = 9; break;
        case 9: row = 10; break;
        case 10: row = 11; break;
        case 11: row = 12; break;
        }
        u32 v = 0;
        const u32 bump = func - 2;
        if (bump > 7) {
            if (func == 1)
                v = ((form << 2) & 4) | ((row << 7) & 0xF80);
            else if (func == 0)
                v = 2 | ((form << 2) & 4) | ((row << 7) & 0xF80);
            else if (func == 0xA)
                v = ((form << 2) & 4) | (src == 0x13 ? 0x20 : 0x30) | 0x100;
        } else {
            v = ((form << 2) & 4) | 0x10 | ((row << 7) & 0xF80) | (((src - 0xC) << 12) & 0x7000) | ((bump << 15) & 0x38000);
        }
        w32(0xC8 + id * 4, v);
        dirty(slw(0x10000, id));
        w32(0xE8 + id * 4, ((post_mtx - 0x40) & 0x3F) | ((normalize << 8) & 0x100));
        switch (id) {
        case 0: field(0x80, 0xFC0, mtx << 6); break;
        case 1: field(0x80, 0x3F000, mtx << 12); break;
        case 2: field(0x80, 0xFC0000, mtx << 18); break;
        case 3: field(0x80, 0x3F000000, mtx << 24); break;
        case 4: field(0x84, 0x3F, mtx); break;
        case 5: field(0x84, 0xFC0, mtx << 6); break;
        case 6: field(0x84, 0x3F000, mtx << 12); break;
        default: field(0x84, 0xFC0000, mtx << 18); break;
        }
        dirty(0x04000000);
    }

    // GXTev.c
    void num_tev_stages(u32 n) const { field(0x254, 0x3C00, (n - 1) << 10), dirty(4); }
    void tev_order(u32 stage, u32 coord, u32 map, u32 color) const {
        const u32 m = map & ~0x100u;
        const u32 tmap = m >= 8 ? 0 : m;
        w32(0x5A4 + stage * 4, map);
        const u32 o = 0x150 + (stage / 2) * 4;
        if ((s32)coord >= 8)
            coord = 0, w32(0x5E8, r32(0x5E8) & ~slw(1, stage));
        else
            w32(0x5E8, r32(0x5E8) | slw(1, stage));
        const u32 chan = color == 0xFF ? 7 : guest::load32(GA_GXTevChannelToRas + color * 4);
        const u32 enable = map != 0xFF && !(map & 0x100);
        u32 v = r32(o);
        if (stage & 1)
            v = (v & ~0x3FF000u) | ((tmap << 12) & 0x7000) | ((coord << 15) & 0x38000) | ((chan << 19) & 0x380000) |
                (enable << 18);
        else
            v = (v & ~0x3FFu) | (tmap & 7) | ((coord << 3) & 0x38) | ((chan << 7) & 0x380) | (enable << 6);
        w32(o, v);
        bp(v);
        sent();
        dirty(1);
    }
    void tev_swap_mode(u32 stage, u32 ras, u32 tex) const {
        const u32 o = 0x1C0 + stage * 4;
        const u32 v = (r32(o) & ~0xFu) | (ras & 3) | ((tex << 2) & 0xC);
        w32(o, v);
        bp(v);
        sent();
    }
    void tev_color_in(u32 stage, u32 a, u32 b, u32 c, u32 e) const {
        const u32 o = 0x180 + stage * 4;
        const u32 v = (r32(o) & ~0xFFFFu) | ((a << 12) & 0xF000) | ((b << 8) & 0xF00) | ((c << 4) & 0xF0) | (e & 0xF);
        bp(v);
        w32(o, v);
        sent();
    }
    void tev_alpha_in(u32 stage, u32 a, u32 b, u32 c, u32 e) const {
        const u32 o = 0x1C0 + stage * 4;
        const u32 v = (r32(o) & ~0xFFF0u) | ((a << 13) & 0xE000) | ((b << 10) & 0x1C00) | ((c << 7) & 0x380) | ((e << 4) & 0x70);
        bp(v);
        w32(o, v);
        sent();
    }
    // the color (0x180) or alpha (0x1C0) combiner's operation
    void tev_op(u32 base, u32 stage, u32 op, u32 bias, u32 scale, u32 clamp, u32 out) const {
        const u32 o = base + stage * 4;
        u32 v = (r32(o) & ~0x40000u) | ((op << 18) & 0x40000);
        if ((s32)op <= 1)
            v = (v & ~0x330000u) | ((scale << 20) & 0x300000) | ((bias << 16) & 0x30000);
        else
            v = (v & ~0x330000u) | ((op << 19) & 0x300000) | 0x30000;
        v = (v & ~0xC80000u) | ((clamp << 19) & 0x80000) | ((out << 22) & 0xC00000);
        bp(v);
        w32(o, v);
        sent();
    }
    void tev_kcolor_sel(u32 stage, u32 sel) const {
        const u32 o = 0x200 + (stage * 2 & ~3u);
        if (stage & 1)
            field(o, 0x7C000, sel << 14);
        else
            field(o, 0x1F0, sel << 4);
        bp(r32(o));
        sent();
    }
    void tev_kalpha_sel(u32 stage, u32 sel) const {
        const u32 o = 0x200 + (stage * 2 & ~3u);
        if (stage & 1)
            field(o, 0xF80000, sel << 19);
        else
            field(o, 0x3E00, sel << 9);
        bp(r32(o));
        sent();
    }
    void tev_swap_mode_table(u32 table, u32 r, u32 g, u32 b, u32 a) const {
        const u32 oa = 0x200 + table * 8, ob = 0x200 + (table * 2 + 1) * 4;
        field(oa, 0xF, (r & 3) | ((g << 2) & 0xC));
        bp(r32(oa));
        field(ob, 0xF, (b & 3) | ((a << 2) & 0xC));
        bp(r32(ob));
        sent();
    }
    void tev_indirect(u32 stage, u32 ind_stage, u32 format, u32 bias, u32 mtx, u32 wrap_s, u32 wrap_t, u32 add_prev,
                      u32 utc_lod, u32 alpha_sel) const {
        bp((ind_stage & 3) | ((format << 2) & 0xC) | ((bias << 4) & 0x70) | ((alpha_sel << 7) & 0x180) |
           ((mtx << 9) & 0x1E00) | ((wrap_s << 13) & 0xE000) | ((wrap_t << 16) & 0x70000) | (((utc_lod & 0xFF) << 19) & 0x80000) |
           ((add_prev << 20) & 0x100000) | ((stage + 0x10) << 24));
        sent();
    }
    void tev_direct(u32 stage) const { tev_indirect(stage, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
    void num_ind_stages(u32 n) const { field(0x254, 0x70000, n << 16), dirty(6); }
    // GXSetTevColorS10(id, color): color as its two words (r << 16 | g, b << 16 | a)
    void tev_color_s10(u32 id, u32 rg, u32 ba) const {
        bp(((id * 2 + 0xE0) << 24) | ((rg >> 16) & 0x7FF) | ((ba << 12) & 0x7FF000));
        const u32 v = ((id * 2 + 0xE1) << 24) | ((ba >> 16) & 0x7FF) | ((rg << 12) & 0x7FF000);
        bp(v), bp(v), bp(v);
        sent();
    }
    void tev_kcolor(u32 id, u32 rgba) const {
        bp(((id * 2 + 0xE0) << 24) | (rgba >> 24) | ((rgba << 12) & 0xFF000) | 0x800000);
        bp(((id * 2 + 0xE1) << 24) | ((rgba >> 8) & 0xFF) | ((rgba >> 4) & 0xFF000) | 0x800000);
        sent();
    }

    // GXPixel.c
    void alpha_compare(u32 comp0, u32 ref0, u32 op, u32 comp1, u32 ref1) const {
        bp(0xF3000000 | (ref0 & 0xFF) | ((ref1 << 8) & 0xFF00) | ((comp0 << 16) & 0x70000) | ((comp1 << 19) & 0x380000) |
           ((op << 22) & 0xC00000));
        sent();
    }
    void blend_mode(u32 type, u32 src, u32 dst, u32 op) const {
        u32 v = r32(0x220);
        v = (v & ~0xFFE3u) | (type == 3 ? 0x800 : 0) | (type & 1) | (type == 2 ? 2 : 0) | ((op << 12) & 0xF000) |
            ((src << 8) & 0x700) | ((dst << 5) & 0xE0);
        bp(v);
        w32(0x220, v);
        sent();
    }
};

// The material's parts (0x40: its resource block) by its flags (0x38, 0x3C): where each one is
struct Mat {
    u32 m, f38, f3C, res;
    u32 n_tex_maps() const { return f3C >> 28; }
    u32 n_tex_gens() const { return (f3C >> 20) & 0xF; }
    u32 n_tev_stages() const { return (f3C >> 9) & 0x1F; }
    u32 n_ind_stages() const { return (f3C >> 15) & 7; }
    u32 base() const { return res + (f38 >> 28) * 0x1C + ((f38 >> 24) & 0xF) * 0x14; }  // past the maps, the SRTs
    u32 chan_ctrl() const { return base() + ((f38 >> 18) & 0x3C); }
    u32 mat_color() const { return chan_ctrl() + ((f38 >> 6) & 4); }
    u32 tex_gens() const { return base(); }
    u32 swap_table() const { return mat_color() + ((f38 >> 5) & 4); }
    u32 tex_srts() const { return res + (f38 >> 28) * 0x1C; }  // past the maps' capacity
    u32 tev_stages() const {
        return swap_table() + ((f38 >> 12) & 4) + ((f38 >> 4) & 4) + ((f38 >> 3) & 4) + ((f38 >> 13) & 0x1C) +
               ((f38 >> 18) & 3) * 0x14;
    }
    u32 alpha_compare() const { return swap_table() + ((f38 >> 12) & 4); }
    u32 blend_mode() const { return alpha_compare() + ((f38 >> 4) & 4); }
};

// what the native path doesn't do: palette textures, indirect texturing, and texture matrices
// outside the ten the original keeps flags for
bool needs_original(const Mat& t) {
    for (u32 i = 0, p = t.tex_gens(); i < t.n_tex_gens(); i++, p += 4)
        if (u32 mtx = guest::load8(p + 2); guest::load8(p) == 1 && mtx != 0x3C && (mtx - 0x1E) / 3 >= 10)
            return true;
    if (((t.f3C >> 24) & 0xF) > 10)
        return true;
    for (u32 i = 0, p = t.res; i < t.n_tex_maps(); i++, p += 0x1C)
        if ((guest::load32(p + 0x18) >> 28) - 8 <= 2)
            return true;
    for (u32 i = 0, p = t.tev_stages(); i < t.n_tev_stages(); i++, p += 0x10)
        if (u32 m = (guest::load8(p + 0xD) >> 3) & 0xF; m >= 1 && m <= 3)
            return true;
    return t.n_ind_stages() != 0;
}

u32 s_original;  // setups that went to the decompiled function (for the check's log)

}  // namespace

// Material::SetupGX(modulateMatColor, alpha): whether the vertex colors are needed
u32 material_setup_gx(u32 m, u32 modulate, u32 alpha) {
    Mat t{m, guest::load32(m + 0x38), guest::load32(m + 0x3C), guest::load32(m + 0x40)};
    if (needs_original(t)) {
        s_original++;
        return g::nw4r_lyt_Material_SetupGX_orig(m, modulate, alpha);
    }
    if (--guest::g_loop_budget < 0)
        guest::loop_poll();
    const Gx gx{guest::host_ptr(guest::load32(GA___GXData))};

    // the lighting channels: material colors from the vertices or the material
    bool use_vtx = true, use_mat = false;
    gx.num_chans(1);
    if ((t.f38 >> 8) & 1) {
        const u32 p = t.chan_ctrl();
        const u32 c0 = guest::load8(p), a0 = guest::load8(p + 1);
        gx.chan_ctrl(0, 0, 0, c0, 0, 0, 2);
        gx.chan_ctrl(2, 0, 0, a0, 0, 0, 2);
        use_vtx = c0 == 1 || a0 == 1;
        use_mat = c0 == 0 || a0 == 0;
    } else {
        gx.chan_ctrl(4, 0, 0, 1, 0, 0, 2);
    }
    bool modulate_tev = use_vtx && modulate != 0;
    if (use_mat) {
        u32 color = (t.f38 >> 7) & 1 ? guest::load32(t.mat_color()) : 0xFFFFFFFF;
        if (alpha != 0xFF) {  // detail::MultipleAlpha: alpha * a / 255, rounded down
            s32 x = (s32)((color & 0xFF) * alpha);
            s32 q = ((s32)(((s64)(s32)0x80808081 * x) >> 32) + x) >> 7;
            color = (color & ~0xFFu) | (u8)(q + (s32)((u32)q >> 31));
        }
        gx.chan_mat_color0a0(color);
        modulate_tev = modulate_tev || color != 0xFFFFFFFF;
    }

    // texture coordinates, and the matrices of those made by one (each texture SRT's 2x4 matrix,
    // made by the game's own CalcTextureMtx, to XF as GXLoadTexMtxImm sends it)
    GuestFrame frame(0x60);
    gx.num_tex_gens(t.n_tex_gens());
    bool srt_used[10] = {}, any_srt = false;
    for (u32 i = 0, p = t.tex_gens(); i < t.n_tex_gens(); i++, p += 4) {
        const u32 type = guest::load8(p), mtx = guest::load8(p + 2);
        if (type == 1 && mtx != 0x3C)
            any_srt = true, srt_used[(mtx - 0x1E) / 3] = true;
        gx.tex_coord_gen2(i, type, guest::load8(p + 1), mtx, 0, 0x7D);
    }
    if (any_srt) {
        const u32 mtx = frame.sp + 0x8;
        // (the SRTs follow the maps' capacity, not their count: as the decompiled function and
        // TexMap's accessor read them; the two differ once the game has lowered the count)
        for (u32 i = 0, srt = t.tex_srts(); i < ((t.f3C >> 24) & 0xF); i++, srt += 0x14) {
            if (!srt_used[i])
                continue;
            g::nw4r_lyt_CalcTextureMtx(mtx, srt);
            guest::gather_put<1>(0x10);
            guest::gather_put<4>((0x1E + i * 3) * 4 | (8 - 1) << 16);
            for (u32 k = 0; k < 8; k++)
                guest::gather_put<4>(guest::load32(mtx + k * 4));
        }
    }

    // the textures: their texture objects made and loaded by the game's own code
    if (t.n_tex_maps()) {
        const u32 obj = frame.sp + 0x40;
        for (u32 i = 0, p = t.res; i < t.n_tex_maps(); i++, p += 0x1C) {
            g::nw4r_lyt_TexMap_Get(p, obj);
            g::GXLoadTexObj(obj, i);
        }
    }

    // TEV colors, constant colors, swap tables
    gx.tev_color_s10(1, guest::load32(m + 0x10), guest::load32(m + 0x14));
    gx.tev_color_s10(2, guest::load32(m + 0x18), guest::load32(m + 0x1C));
    gx.tev_color_s10(3, guest::load32(m + 0x20), guest::load32(m + 0x24));
    for (u32 k = 0; k < 4; k++)
        gx.tev_kcolor(k, guest::load32(m + 0x28 + k * 4));
    if ((t.f38 >> 14) & 1) {
        for (u32 k = 0, p = t.swap_table(); k < 4; k++) {
            const u32 s = guest::load8(p + k);
            gx.tev_swap_mode_table(k, s & 3, (s >> 2) & 3, (s >> 4) & 3, (s >> 6) & 3);
        }
    } else {
        gx.tev_swap_mode_table(0, 0, 1, 2, 3);
        gx.tev_swap_mode_table(1, 0, 0, 0, 3);
        gx.tev_swap_mode_table(2, 1, 1, 1, 3);
        gx.tev_swap_mode_table(3, 2, 2, 2, 3);
    }

    // the TEV stages: the material's own, or the default ones for its number of textures
    if (const u32 n = t.n_tev_stages()) {
        gx.num_tev_stages(n);
        for (u32 s = 0, p = t.tev_stages(); s < n; s++, p += 0x10) {
            u8 b[16];
            for (u32 k = 0; k < 16; k++)
                b[k] = guest::load8(p + k);
            gx.tev_order(s, b[0], b[2] | ((b[3] << 8) & 0x100), b[1]);
            gx.tev_swap_mode(s, (b[3] >> 1) & 3, (b[3] >> 3) & 3);
            gx.tev_color_in(s, b[4] & 0xF, b[4] >> 4, b[5] & 0xF, b[5] >> 4);
            gx.tev_op(0x180, s, b[6] & 0xF, (b[6] >> 4) & 3, (b[6] >> 6) & 3, b[7] & 1, (b[7] >> 1) & 3);
            gx.tev_kcolor_sel(s, (b[7] >> 3) & 0x1F);
            gx.tev_alpha_in(s, b[8] & 0xF, b[8] >> 4, b[9] & 0xF, b[9] >> 4);
            gx.tev_op(0x1C0, s, b[10] & 0xF, (b[10] >> 4) & 3, (b[10] >> 6) & 3, b[11] & 1, (b[11] >> 1) & 3);
            gx.tev_kalpha_sel(s, (b[11] >> 3) & 0x1F);
            gx.tev_indirect(s, b[12], b[15] & 3, b[13] & 7, (b[13] >> 3) & 0xF, b[14] & 7, (b[14] >> 3) & 7,
                            (b[15] >> 2) & 1, (b[15] >> 3) & 1, (b[15] >> 4) & 3);
        }
        modulate_tev = true;
    } else {
        const u32 maps = t.n_tex_maps();
        u32 s = 0;
        if (maps == 0) {
            gx.tev_order(0, 0xFF, 0xFF, 4);
            gx.tev_color_in(0, 0xF, 4, 0xA, 0xF);
            gx.tev_alpha_in(0, 7, 2, 5, 7);
            modulate_tev = true;
            s = 1;
        } else {
            if (maps == 1) {
                gx.tev_order(0, 0, 0, 0xFF);
                gx.tev_color_in(0, 2, 4, 8, 0xF);
                gx.tev_alpha_in(0, 1, 2, 4, 7);
                s = 1;
            } else {
                if (maps == 2) {
                    gx.tev_order(0, 0, 0, 0xFF);
                    gx.tev_color_in(0, 0xF, 0xF, 0xF, 8);
                    gx.tev_alpha_in(0, 7, 7, 7, 4);
                    gx.tev_order(1, 1, 1, 0xFF);
                    gx.tev_color_in(1, 8, 0, 0xE, 0xF);
                    gx.tev_alpha_in(1, 4, 0, 6, 7);
                    gx.tev_kcolor_sel(1, guest::load32(GA_nw4r_lyt_Material_kColSels));
                    gx.tev_kalpha_sel(1, guest::load32(GA_nw4r_lyt_Material_kAlpSels));
                    s = 2;
                } else {
                    for (u32 i = 0; i < maps; i++, s++) {
                        gx.tev_order(s & 0xFF, i, i, 0xFF);
                        gx.tev_color_in(s & 0xFF, 0xF, 8, 0xE, i == 0 ? 0xF : 0);
                        gx.tev_alpha_in(s & 0xFF, 7, 4, 6, i == 0 ? 7 : 0);
                        gx.tev_kcolor_sel(s & 0xFF, guest::load32(GA_nw4r_lyt_Material_kColSels + i * 4));
                        gx.tev_kalpha_sel(s & 0xFF, guest::load32(GA_nw4r_lyt_Material_kAlpSels + i * 4));
                    }
                }
                // the TEV colors 0 and 1 (black, white) blend the result unless they're the defaults
                bool plain = true;
                for (u32 k = 0; k < 4; k++)
                    plain = plain && (s16)guest::load16(m + 0x10 + k * 2) == (s16)guest::load16(GA_nw4r_lyt_Material_DefaultBlackColor + k * 2);
                for (u32 k = 0; plain && k < 4; k++)
                    plain = (s16)guest::load16(m + 0x18 + k * 2) == 0xFF;
                if (!plain) {
                    gx.tev_order(s & 0xFF, 0xFF, 0xFF, 0xFF);
                    gx.tev_color_in(s & 0xFF, 2, 4, 0, 0xF);
                    gx.tev_alpha_in(s & 0xFF, 1, 2, 0, 7);
                    s++;
                }
            }
            if (modulate_tev) {  // the rasterized color times the result
                gx.tev_order(s & 0xFF, 0xFF, 0xFF, 4);
                gx.tev_color_in(s & 0xFF, 0xF, 0, 0xA, 0xF);
                gx.tev_alpha_in(s & 0xFF, 7, 0, 5, 7);
                s++;
            }
        }
        s &= 0xFF;
        for (u32 k = 0; k < s; k++) {
            gx.tev_op(0x180, k, 0, 0, 0, 1, 0);
            gx.tev_op(0x1C0, k, 0, 0, 0, 1, 0);
            gx.tev_direct(k);
            gx.tev_swap_mode(k, 0, 0);
        }
        gx.num_tev_stages(s);
    }
    gx.num_ind_stages(0);

    if ((t.f38 >> 6) & 1) {
        const u32 p = t.alpha_compare();
        const u32 c = guest::load8(p);
        gx.alpha_compare(c & 0xF, guest::load8(p + 2), guest::load8(p + 1), (c >> 4) & 0xF, guest::load8(p + 3));
    } else {
        gx.alpha_compare(7, 0, 0, 7, 0);
    }
    if ((t.f38 >> 5) & 1) {
        const u32 p = t.blend_mode();
        gx.blend_mode(guest::load8(p), guest::load8(p + 1), guest::load8(p + 2), guest::load8(p + 3));
    } else {
        gx.blend_mode(1, 4, 5, 0xF);
    }
    return modulate_tev && use_vtx;
}

}  // namespace engine::lyt

namespace guest {
extern void (*g_gather_capture)(const u8* burst);
}

namespace engine::lyt {

namespace {

// NOMA_CHECK_LYT_MATERIAL=1: every material set up by the decompiled function and by the native one
// from the same state, their commands and __GXData compared (then the native one's kept)
struct Run {
    std::vector<u8> sent;  // the bursts, then what the pipe holds
    u8 data[0x600];
    u32 ret;
};
std::vector<u8>* s_capture;
void capture(const u8* burst) { s_capture->insert(s_capture->end(), burst, burst + 32); }

u32 checked_setup_gx(u32 m, u32 modulate, u32 alpha) {
    u8* gd = guest::host_ptr(guest::load32(GA___GXData));
    u8 data0[0x600], pipe0[32];
    ::memcpy(data0, gd, sizeof(data0));
    ::memcpy(pipe0, guest::g_gather, 32);
    const u32 n0 = guest::g_gather_n;
    Run run[2];
    for (int which = 0; which < 2; which++) {
        ::memcpy(gd, data0, sizeof(data0));
        ::memcpy(guest::g_gather, pipe0, 32);
        guest::g_gather_n = n0;
        s_capture = &run[which].sent;
        guest::g_gather_capture = capture;
        run[which].ret = which ? material_setup_gx(m, modulate, alpha) : g::nw4r_lyt_Material_SetupGX_orig(m, modulate, alpha);
        guest::g_gather_capture = nullptr;
        run[which].sent.insert(run[which].sent.end(), guest::g_gather, guest::g_gather + guest::g_gather_n);
        ::memcpy(run[which].data, gd, sizeof(data0));
    }
    static u32 s_checked, s_bad;
    s_checked++;
    const bool same_sent = run[0].sent == run[1].sent, same_data = !::memcmp(run[0].data, run[1].data, 0x600);
    if ((!same_sent || !same_data || run[0].ret != run[1].ret) && s_bad++ < 20) {
        u32 at = 0;
        while (at < run[0].sent.size() && at < run[1].sent.size() && run[0].sent[at] == run[1].sent[at])
            at++;
        u32 d = 0;
        while (d < 0x600 && run[0].data[d] == run[1].data[d])
            d++;
        guest::log("lyt material %08X (flags %08X %08X): commands %s (%zu/%zu bytes, first difference at %u), __GXData %s "
            "(first difference at %X), returns %u/%u", m, guest::load32(m + 0x38), guest::load32(m + 0x3C),
            same_sent ? "same" : "DIFFER", run[0].sent.size(), run[1].sent.size(), at, same_data ? "same" : "DIFFERS", d,
            run[0].ret, run[1].ret);
    }
    if (s_checked % 20000 == 0)
        guest::log("lyt material check: %u setups (%u by the decompiled function), %u differ", s_checked, s_original / 2, s_bad);
    // the native one's for real
    ::memcpy(gd, data0, sizeof(data0));
    ::memcpy(guest::g_gather, pipe0, 32);
    guest::g_gather_n = n0;
    return material_setup_gx(m, modulate, alpha);
}

}  // namespace

}  // namespace engine::lyt

// ---- the guest ABI (decl.h)
namespace g {

u32 nw4r_lyt_Material_SetupGX(u32 m, u32 modulate, u32 alpha) {
    static const bool s_check = ::getenv("NOMA_CHECK_LYT_MATERIAL") != nullptr;
    if (s_check)
        return engine::lyt::checked_setup_gx(m, modulate, alpha);
    return engine::lyt::material_setup_gx(m, modulate, alpha);
}

}  // namespace g
