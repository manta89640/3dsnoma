// The transform unit's work for the vertex shader: planning a draw (its uniforms, what its
// vertices need), the palette of matrix rows, and the vertices themselves. See tnl.h.
#include "tnl.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "pica_vsh.h"
#include "regs.h"
#include "vertex_read.h"

namespace guest {

namespace {

u32 s_approximated;
GxTnlPalStats s_pal_stats;

// a light's rows for the shader: (cosatt, r) (distatt, g) (position, b) (direction, a)
void light_rows(u32 l, f32 rows[4][4]) {
    u32 base = XF_MEM_LIGHT + 0x10 * l;
    u32 c = g_gx.xf[base + 3];
    const f32 color[4] = {(f32)(c >> 24) / 255.0f, (f32)((c >> 16) & 255) / 255.0f, (f32)((c >> 8) & 255) / 255.0f,
                          (f32)(c & 255) / 255.0f};
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < 3; i++)
            rows[r][i] = xf_f32(base + 4 + 3 * r + i);
        rows[r][3] = color[r];
    }
}

// ---------------------------------------------------------------- the palette
enum : u8 { PAL_R3 = 1, PAL_R3N = 2, PAL_R2 = 3, PAL_CONT = 4 };
const u32 PAL_GROUPS = TNL_PAL_SLOTS / 3;

// the rows an entry of this kind and source holds now
void pal_source(u8 kind, u8 xf_row, f32 rows[6][4]) {
    const u32* xf = g_gx.xf;
    if (kind == PAL_R2) {
        memcpy(rows, &xf[xf_row * 4u], 32);
        rows[2][0] = rows[2][1] = rows[2][2] = 0.0f, rows[2][3] = 1.0f;
        return;
    }
    memcpy(rows, &xf[xf_row * 4u], 48);
    if (kind == PAL_R3N) {
        u32 n = XF_MEM_NORMAL + (xf_row & 31u) * 3;
        for (int r = 0; r < 3; r++) {
            memcpy(rows[3 + r], &xf[n + 3 * r], 12);
            rows[3 + r][3] = 0.0f;
        }
    }
}

u32 pal_groups_of(u8 kind) { return kind == PAL_R3N ? 2 : 1; }

void pal_fill(GxTnlPalette& pal, u32 g, u8 kind, u8 xf_row) {
    s_pal_stats.fills++;
    GxTnlPalette::Entry& e = pal.entries[g];
    e.first = (u8)(3 * g), e.n = (u8)(3 * pal_groups_of(kind)), e.kind = kind, e.xf_row = xf_row;
    pal.by_row[kind - 1][xf_row & 63] = (u8)g;
    e.gen = g_gx.xf_gen;
    pal_source(kind, xf_row, (f32(*)[4]) & pal.rows[e.first]);
    pal.dirty |= ((1ull << e.n) - 1) << e.first;
    for (u32 k = 0; k < pal_groups_of(kind); k++)
        pal.slot_entry[3 * (g + k)] = pal.slot_entry[3 * (g + k) + 1] = pal.slot_entry[3 * (g + k) + 2] = (u8)g;
    if (kind == PAL_R3N) {
        GxTnlPalette::Entry& c = pal.entries[g + 1];
        c.kind = PAL_CONT, c.first = (u8)(3 * g + 3), c.n = 3;
    }
}

// the entry's rows are as XF memory has them: none written since they were copied
bool pal_current(const GxTnlPalette& pal, u32 g) {
    const GxTnlPalette::Entry& e = pal.entries[g];
    const u32* rg = &g_gx.xf_row_gen[e.xf_row & 63];
    u32 newest = std::max(rg[0], rg[1]);
    if (e.kind != PAL_R2)
        newest = std::max(newest, rg[2]);
    if (e.kind == PAL_R3N) {
        const u32* ng = &g_gx.xf_nrm_gen[e.xf_row & 31];
        newest = std::max(newest, std::max(ng[0], std::max(ng[1], ng[2])));
    }
    s_pal_stats.checks++;
    if (newest > e.gen) {
        s_pal_stats.stale++;
        return false;
    }
    return true;
}

u64 slots_mask(u32 first, u32 n) { return ((1ull << n) - 1) << first; }

// entry g if it holds the rows of `kind` from XF row `xf_row` (made current if it can be):
// its first slot, else -1
int pal_try(GxTnlPalette& pal, u32 g, u8 kind, u8 xf_row) {
    GxTnlPalette::Entry& e = pal.entries[g];
    if (!e.kind || e.kind == PAL_CONT || e.xf_row != xf_row)
        return -1;
    if (!(e.kind == kind || (kind == PAL_R3 && e.kind == PAL_R3N)))
        return -1;
    if (!pal_current(pal, g)) {
        // stale: written again where no pending vertex reads it
        if (pal.used & slots_mask(e.first, e.n))
            return -1;
        pal_fill(pal, g, e.kind, xf_row);
    }
    pal.ref |= (u16)(((1u << (e.n / 3)) - 1) << g);
    pal.used |= slots_mask(e.first, 3 * pal_groups_of(kind));
    return e.first;
}

// the first slot of the rows of `kind` from XF row `xf_row`, in the palette already or made;
// -1 when every slot that could hold them is in use by the pending batch. `hint`: the slot they
// had last time (-1: none), tried first.
int pal_lookup(GxTnlPalette& pal, u8 kind, u8 xf_row, int hint = -1) {
    if (hint >= 0 && hint < (int)TNL_PAL_SLOTS && pal.slot_entry[hint] != 0xFF) {
        u32 g = pal.slot_entry[hint];
        if (pal.entries[g].first == hint) {
            int s = pal_try(pal, g, kind, xf_row);
            if (s >= 0)
                return s;
        }
    }
    // the entry holding these rows, by row (an R3's rows may sit in an R3N entry)
    u32 g = pal.by_row[kind - 1][xf_row & 63];
    if (g != 0xFF) {
        int s = pal_try(pal, g, kind, xf_row);
        if (s >= 0)
            return s;
    }
    if (kind == PAL_R3 && (g = pal.by_row[PAL_R3N - 1][xf_row & 63]) != 0xFF) {
        int s = pal_try(pal, g, kind, xf_row);
        if (s >= 0)
            return s;
    }
    // room: the groups [g, g + need) and the whole of any entry they cut into (an R3N's two
    // groups go together), none in use by the pending batch, from the clock's hand on; a group
    // used since the hand last passed is passed over once (its bit cleared)
    const u32 need = pal_groups_of(kind);
    s_pal_stats.scans++;
    int best = -1;
    u32 best_lo = 0, best_hi = 0;
    for (u32 step = 0; step < 2 * PAL_GROUPS + 2; step++) {
        u32 g = pal.hand;
        pal.hand = (u8)(g + need < PAL_GROUPS ? g + 1 : 0);
        if (g + need > PAL_GROUPS)
            continue;
        u32 lo = g, hi = g + need;
        if (lo > 0 && pal.entries[lo].kind == PAL_CONT)
            lo--;
        if (pal.entries[hi - 1].kind == PAL_R3N)
            hi++;
        if (hi > PAL_GROUPS || (pal.used & slots_mask(3 * lo, 3 * (hi - lo))))
            continue;
        const u16 mask = (u16)(((1u << (hi - lo)) - 1) << lo);
        if (pal.ref & mask) {
            pal.ref &= (u16)~mask;
            continue;
        }
        best = (int)g, best_lo = lo, best_hi = hi;
        break;
    }
    if (best < 0)
        return -1;
    for (u32 k = best_lo; k < best_hi; k++) {
        GxTnlPalette::Entry& e = pal.entries[k];
        if (e.kind && e.kind != PAL_CONT && pal.by_row[e.kind - 1][e.xf_row & 63] == k)
            pal.by_row[e.kind - 1][e.xf_row & 63] = 0xFF;
        e = GxTnlPalette::Entry();
        pal.slot_entry[3 * k] = pal.slot_entry[3 * k + 1] = pal.slot_entry[3 * k + 2] = 0xFF;
    }
    pal_fill(pal, (u32)best, kind, xf_row);
    pal.used |= slots_mask(3 * best, 3 * need);
    pal.ref |= (u16)(((1u << need) - 1) << best);
    return 3 * best;
}

}  // namespace

// ---------------------------------------------------------------- planning a draw
namespace {

// a plan is kept while what it was made from stays the same: the vertex format, the XF registers,
// the lights, the post matrices, the renderer's setup and its clip transform
struct PlanKey {
    u32 fmt_serial, reg_serial, light_serial, post_serial, setup_serial, clip_serial;
};
struct PlanSlot {
    bool valid = false;
    u32 age = 0;
    PlanKey key;
    GxTnlPlan plan;
};
PlanSlot s_plans[4];
u32 s_plan_clock, s_plan_misses;

void make_plan(const GxVertexFormat& f, const GxDrawSetup& setup, const f32 clip[4][4], GxTnlPlan& p);

}  // namespace

const GxTnlPlan& gx_tnl_plan(u32 vat, const GxDrawSetup& setup, u32 setup_serial, const f32 clip[4][4],
                             u32 clip_serial) {
    const GxVertexFormat& f = gx_vertex_format(vat);
    const PlanKey key = {f.serial, g_gx.xf_reg_serial, g_gx.light_serial, g_gx.xf_post_serial, setup_serial, clip_serial};
    PlanSlot* victim = &s_plans[0];
    for (PlanSlot& ps : s_plans) {
        if (ps.valid && ps.key.fmt_serial == key.fmt_serial && ps.key.reg_serial == key.reg_serial &&
            ps.key.light_serial == key.light_serial && ps.key.post_serial == key.post_serial &&
            ps.key.setup_serial == key.setup_serial && ps.key.clip_serial == key.clip_serial) {
            ps.age = ++s_plan_clock;
            return ps.plan;
        }
        if (!ps.valid || ps.age < victim->age)
            victim = &ps;
    }
    victim->valid = true;
    victim->age = ++s_plan_clock;
    victim->key = key;
    s_plan_misses++;
    make_plan(f, setup, clip, victim->plan);
    victim->plan.serial = s_plan_misses;
    return victim->plan;
}

u32 gx_tnl_plan_misses() { return s_plan_misses; }

namespace {

void make_plan(const GxVertexFormat& f, const GxDrawSetup& setup, const f32 clip[4][4], GxTnlPlan& p) {
    const u32* xf = g_gx.xf;
    p.fmt = &f;
    p.approximated = 0;
    p.need_nrm = p.nrm_eye = false;
    GxTnlConsts& c = p.consts;
    memset(&c, 0, sizeof(c));
    const u32 mia = xf[XF_MATINDEX_A], mib = xf[XF_MATINDEX_B];
    p.def_pos_index = (u8)bits(mia, 0, 6);

    // projection (on eye (x, y, z, 1)), then the platform's clip-space transform
    f32 pr[6];
    for (int i = 0; i < 6; i++)
        pr[i] = xf_f32(XF_PROJECTION + i);
    f32 P[4][4] = {};
    if (xf[XF_PROJECTION + 6]) {  // orthographic
        P[0][0] = pr[0], P[0][3] = pr[1];
        P[1][1] = pr[2], P[1][3] = pr[3];
        P[2][2] = pr[4], P[2][3] = pr[5];
        P[3][3] = 1.0f;
    } else {
        P[0][0] = pr[0], P[0][2] = pr[1];
        P[1][1] = pr[2], P[1][2] = pr[3];
        P[2][2] = pr[4], P[2][3] = pr[5];
        P[3][2] = -1.0f;
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            c.proj[i][j] = clip[i][0] * P[0][j] + clip[i][1] * P[1][j] + clip[i][2] * P[2][j] + clip[i][3] * P[3][j];

    // color channel 0: material and ambient from the registers or the vertex, the lights
    const bool on = xf[XF_NUMCOLORS] > 0;
    const u32 cc = xf[XF_COLOR0CNTRL], ac = xf[XF_ALPHA0CNTRL];
    u32 mat = xf[XF_MATERIAL0], amb = xf[XF_AMBIENT0];
    auto byte01 = [](u32 v, int k) { return (f32)((v >> (24 - 8 * k)) & 255) / 255.0f; };
    const bool rgb_lit = on && (cc & 2), a_lit = on && (ac & 2);
    for (int k = 0; k < 4; k++) {
        u32 ctrl = k < 3 ? cc : ac;
        bool lit = k < 3 ? rgb_lit : a_lit;
        c.selmat[k] = on && (ctrl & 1) ? 1.0f : 0.0f;
        c.matc[k] = on && !(ctrl & 1) ? byte01(mat, k) : 0.0f;
        c.selamb[k] = lit && (ctrl & 0x40) ? 1.0f : 0.0f;
        c.ambc[k] = lit && !(ctrl & 0x40) ? byte01(amb, k) : 0.0f;
        c.sellit[k] = lit ? 1.0f : 0.0f;
    }
    u32 bools = 0;
    if (rgb_lit || a_lit) {
        bools |= TNL_LIT;
        const u32 ctrl = rgb_lit ? cc : ac;  // the channel whose lights are summed
        const u32 mask = bits(ctrl, 2, 4) | (bits(ctrl, 11, 4) << 4);
        const u32 diffuse = bits(ctrl, 7, 2), attn = bits(ctrl, 9, 2);
        u32 nl = 0;
        for (u32 l = 0; l < 8; l++)
            if (mask & (1u << l))
                light_rows(l, c.lights[nl++]);
        c.nlights = nl;
        if (nl) {
            bools |= TNL_HASLIGHTS;
            if (attn == 3)
                bools |= TNL_SPOT;
            else if (attn == 1)
                p.approximated |= 1;  // specular attenuation: as none
            p.need_nrm = p.nrm_eye = true;
        }
        c.lcfg[0] = diffuse == 1 ? -1e30f : 0.0f;
        c.lcfg[1] = diffuse ? 1.0f : 0.0f;
        c.lcfg[2] = diffuse ? 0.0f : 1.0f;
        f32 alphaw = a_lit ? 1.0f : 0.0f;
        if (rgb_lit && a_lit) {
            const u32 amask = bits(ac, 2, 4) | (bits(ac, 11, 4) << 4);
            if (amask == 0)
                alphaw = 0.0f;
            else if (amask != mask || bits(ac, 7, 2) != diffuse || bits(ac, 9, 2) != attn)
                p.approximated |= 2;  // the alpha channel's own lighting: as the color's
        }
        c.lcfg[3] = alphaw;
    }

    // texgens of the texture units
    const u32 ntg = xf[XF_NUMTEXGENS];
    const bool dual = xf[XF_DUALTEX] & 1;
    static const u32 k_identity[12] = {0x3F800000, 0, 0, 0, 0, 0x3F800000, 0, 0, 0, 0, 0x3F800000, 0};
    for (u32 k = 0; k < 3; k++) {
        GxTnlPlan::Tc& t = p.tc[k];
        t = GxTnlPlan::Tc();
        const s8 g = setup.texgen[k];
        f32* scale = &c.tcscale[k >> 1][(k & 1) * 2];
        if (g < 0 || (u32)g >= ntg) {
            t.src = TNL_SRC_ZERO;  // (an output the combiners don't sample, or a texgen the XF hasn't)
            scale[0] = scale[1] = 0.0f;
            continue;
        }
        scale[0] = setup.sx[k], scale[1] = setup.sy[k];
        if (k == 1)
            bools |= TNL_TG1;
        if (k == 2)
            bools |= TNL_TG2;
        const u32 info = xf[XF_TEXMTXINFO + g];
        const u32 type = bits(info, 4, 3), row = bits(info, 7, 5);
        t.stq = bits(info, 1, 1);
        const bool abc1 = bits(info, 2, 1);
        if (type != 0) {
            p.approximated |= 4;  // emboss and color texgens: zero
            t.src = TNL_SRC_ZERO;
        } else if (row == 0) {
            t.src = TNL_SRC_POS, t.z = abc1;
        } else if (row == 1) {
            t.src = TNL_SRC_NRM, t.z = abc1;
            p.need_nrm = true;
        } else if (row >= 5 && row <= 12) {
            t.src = TNL_SRC_TEX, t.tex = (u8)(row - 5);
        } else {
            p.approximated |= 8;  // binormal, tangent, color sources: zero
            t.src = TNL_SRC_ZERO;
        }
        t.mtx_off = f.tex_mtx[g];
        t.def_index = (u8)(g < 4 ? bits(mia, 6 + 6 * g, 6) : bits(mib, 6 * (g - 4), 6));
        if (t.stq)
            bools |= TNL_STQ0 << k;
        if (dual) {
            const u32 post = xf[XF_POSTMTXINFO + g];
            const u32 at = XF_MEM_POST + bits(post, 0, 6) * 4u;
            const bool norm = bits(post, 8, 1);
            if (norm || memcmp(&xf[at], k_identity, sizeof(k_identity)) != 0) {
                bools |= TNL_POST0 << k;
                if (norm)
                    bools |= TNL_PNRM0 << k;
                memcpy(c.post[k], &xf[at], 48);
            }
        }
    }
    c.bools = bools;
    if (p.approximated)
        s_approximated++;
}

}  // namespace

u32 gx_tnl_approximated() { return s_approximated; }
GxTnlPalStats gx_tnl_pal_stats() { return s_pal_stats; }

// ---------------------------------------------------------------- the vertices
namespace {

// the slots of one draw's matrices, as the vertices name them or the draw's defaults: looked up
// once per distinct row (the caches), -1 from a lookup ends the vertices early
struct SlotMaps {
    s16 pos_slot[64], tc_slot[3][64];
    s16 fixed_pos = -1, fixed_tc[3] = {0, 0, 0};
    u8 pos_kind = PAL_R3;
    bool vary_pos = false, vary_tc[3] = {false, false, false};
    // the draw's fixed slots (hint: the slots the draw had last time); false when the palette is full
    bool init(const GxTnlPlan& p, GxTnlPalette& pal, const GxTnlSlots* hint = nullptr) {
        const GxVertexFormat& f = *p.fmt;
        pos_kind = p.nrm_eye ? PAL_R3N : PAL_R3;
        vary_pos = f.pos_mtx >= 0;
        if (!vary_pos) {
            int s = pal_lookup(pal, pos_kind, p.def_pos_index, hint ? hint->s[0] : -1);
            if (s < 0)
                return false;
            fixed_pos = (s16)s;
        } else {
            memset(pos_slot, -1, sizeof(pos_slot));
        }
        for (u32 k = 0; k < 3; k++) {
            const GxTnlPlan::Tc& t = p.tc[k];
            vary_tc[k] = t.src != TNL_SRC_ZERO && t.mtx_off >= 0;
            if (t.src == TNL_SRC_ZERO)
                continue;
            if (!vary_tc[k]) {
                int s = pal_lookup(pal, t.stq ? PAL_R3 : PAL_R2, t.def_index, hint ? hint->s[1 + k] : -1);
                if (s < 0)
                    return false;
                fixed_tc[k] = (s16)s;
            } else {
                memset(tc_slot[k], -1, sizeof(tc_slot[k]));
            }
        }
        return true;
    }
    // the slots of a draw whose matrices are fixed
    void fixed(GxTnlSlots& o) const {
        o.s[0] = (u8)fixed_pos;
        for (u32 k = 0; k < 3; k++)
            o.s[1 + k] = (u8)fixed_tc[k];
    }
    // a vertex's slots from its rows; false when the palette is full
    bool slots(const GxTnlPlan& p, GxTnlPalette& pal, const u8 rows[4], GxTnlSlots& o) {
        if (!vary_pos) {
            o.s[0] = (u8)fixed_pos;
        } else {
            u32 r = rows[0] & 63;
            if (pos_slot[r] < 0) {
                int s = pal_lookup(pal, pos_kind, (u8)r);
                if (s < 0)
                    return false;
                pos_slot[r] = (s16)s;
            }
            o.s[0] = (u8)pos_slot[r];
        }
        for (u32 k = 0; k < 3; k++) {
            if (!vary_tc[k]) {
                o.s[1 + k] = (u8)fixed_tc[k];
                continue;
            }
            u32 r = rows[1 + k] & 63;
            if (tc_slot[k][r] < 0) {
                int s = pal_lookup(pal, p.tc[k].stq ? PAL_R3 : PAL_R2, (u8)r);
                if (s < 0)
                    return false;
                tc_slot[k][r] = (s16)s;
            }
            o.s[1 + k] = (u8)tc_slot[k][r];
        }
        return true;
    }
};

// a texture coordinate source read from the vertex: (s, t, 1)
inline void read_tex_source(const GxVertexFormat& f, u32 tex, const u8* v, f32* out) {
    const GxAttrFormat& a = f.tex[tex];
    out[0] = out[1] = 0.0f, out[2] = 1.0f;
    if (a.mode)
        gx_read_comps(gx_attr_data(a, 4 + tex, v), a.type, a.n, a.scale, out);
}

}  // namespace

bool gx_tnl_rows_vary(const GxTnlPlan& p) {
    if (p.fmt->pos_mtx >= 0)
        return true;
    for (u32 k = 0; k < 3; k++)
        if (p.tc[k].src != TNL_SRC_ZERO && p.tc[k].mtx_off >= 0)
            return true;
    return false;
}

bool gx_tnl_fixed_slots(const GxTnlPlan& p, GxTnlPalette& pal, const GxTnlSlots* hint, GxTnlSlots* out) {
    SlotMaps m;
    if (!m.init(p, pal, hint))
        return false;
    const u8 none[4] = {0, 0, 0, 0};
    return m.slots(p, pal, none, *out);
}

// The commonest immediate draws, the layouts' quads and glyphs: float positions in the stream,
// no normal, a direct RGBA8 color or none, texgen sources from direct float, u16 or s16 texture
// coordinates, the position, or none; one matrix each. Nothing to read but the fields themselves.
struct SimpleTc {
    u8 kind;    // 0 none, 1 position, 2 f32, 3 u16, 4 s16
    u8 n;       // texture coordinate components
    u8 offset;  // its stream offset
    f32 scale;
    f32 z;      // the third component (1, or the position's z)
};
bool simple_plan(const GxTnlPlan& p, SimpleTc tcs[3]) {
    const GxVertexFormat& f = *p.fmt;
    if (p.need_nrm || f.pos_mtx >= 0 || f.pos.mode != 1 || f.pos.type != 4)
        return false;
    if (f.color[0].mode == 1 ? f.color[0].type != 5 : f.color[0].mode != 0)
        return false;
    for (u32 k = 0; k < 3; k++) {
        const GxTnlPlan::Tc& t = p.tc[k];
        SimpleTc& st = tcs[k];
        st.kind = 0, st.n = 0, st.offset = 0, st.scale = 1.0f, st.z = 1.0f;
        if (t.src == TNL_SRC_ZERO)
            continue;
        if (t.mtx_off >= 0)
            return false;
        if (t.src == TNL_SRC_POS) {
            st.kind = 1;
            continue;
        }
        if (t.src != TNL_SRC_TEX)
            return false;
        const GxAttrFormat& a = f.tex[t.tex];
        if (!a.mode) {
            continue;  // (zeros)
        }
        if (a.mode != 1 || (a.type != 4 && a.type != 2 && a.type != 3))
            return false;
        st.kind = a.type == 4 ? 2 : a.type == 2 ? 3 : 4;
        st.n = a.n, st.offset = a.offset, st.scale = a.scale;
    }
    return true;
}

u32 vertices_simple(const GxTnlPlan& p, const u8* data, u32 first, u32 n, const SlotMaps& m, const SimpleTc tcs[3],
                    GxTnlVertex* out, GxTnlSlots* slots) {
    const GxVertexFormat& f = *p.fmt;
    const u8* v = data + first * f.size;
    GxTnlSlots fixed;
    m.fixed(fixed);
    const u32 poff = f.pos.offset, pos_n = f.pos.n;
    const bool color = f.color[0].mode == 1;
    const u32 coff = f.color[0].offset;
    for (u32 i = 0; i < n; i++, v += f.size) {
        GxTnlVertex& o = out[i];
        slots[i] = fixed;
        o.pos[0] = gx_be_f32(v + poff), o.pos[1] = gx_be_f32(v + poff + 4), o.pos[2] = pos_n == 3 ? gx_be_f32(v + poff + 8) : 0.0f;
        o.nrm[0] = o.nrm[1] = o.nrm[2] = 0.0f;
        if (color)
            memcpy(o.color, v + coff, 4);
        else
            o.color[0] = o.color[1] = o.color[2] = o.color[3] = 0;
        for (u32 k = 0; k < 3; k++) {
            const SimpleTc& st = tcs[k];
            f32* tc = o.tc[k];
            const u8* d = v + st.offset;
            switch (st.kind) {
            case 1:
                tc[0] = o.pos[0], tc[1] = o.pos[1], tc[2] = p.tc[k].z ? o.pos[2] : 1.0f;
                break;
            case 2:
                tc[0] = gx_be_f32(d), tc[1] = st.n == 2 ? gx_be_f32(d + 4) : 0.0f, tc[2] = 1.0f;
                break;
            case 3:
                tc[0] = (f32)gx_be16(d) * st.scale, tc[1] = st.n == 2 ? (f32)gx_be16(d + 2) * st.scale : 0.0f, tc[2] = 1.0f;
                break;
            case 4:
                tc[0] = (f32)(s16)gx_be16(d) * st.scale, tc[1] = st.n == 2 ? (f32)(s16)gx_be16(d + 2) * st.scale : 0.0f, tc[2] = 1.0f;
                break;
            default:
                tc[0] = tc[1] = 0.0f, tc[2] = 1.0f;
                break;
            }
        }
    }
    return n;
}

u32 gx_tnl_vertices(const GxTnlPlan& p, const u8* data, u32 first, u32 n, GxTnlPalette& pal, GxTnlVertex* out,
                    GxTnlSlots* slots, GxTnlRows* rows) {
    const GxVertexFormat& f = *p.fmt;
    SlotMaps m;
    if (!m.init(p, pal))
        return 0;
    SimpleTc tcs[3];
    if (!rows && simple_plan(p, tcs))
        return vertices_simple(p, data, first, n, m, tcs, out, slots);
    const u8* v = data + first * f.size;
    for (u32 i = 0; i < n; i++, v += f.size) {
        GxTnlVertex& o = out[i];
        // the matrices' rows, their slots
        u8 r[4] = {0, 0, 0, 0};
        if (m.vary_pos)
            r[0] = v[f.pos_mtx];
        for (u32 k = 0; k < 3; k++)
            if (m.vary_tc[k])
                r[1 + k] = v[p.tc[k].mtx_off];
        if (rows)
            memcpy(rows[i].r, r, 4);
        if (!m.slots(p, pal, r, slots[i]))
            return i;
        // position
        o.pos[0] = o.pos[1] = o.pos[2] = 0.0f;
        if (f.pos.mode)
            gx_read_comps(gx_attr_data(f.pos, 0, v), f.pos.type, f.pos.n, f.pos.scale, o.pos);
        // normal
        o.nrm[0] = o.nrm[1] = o.nrm[2] = 0.0f;
        if (p.need_nrm && f.nrm.mode)
            gx_read_comps(gx_attr_data(f.nrm, 1, v), f.nrm.type, 3, f.nrm.scale, o.nrm);
        // color 0
        if (f.color[0].mode)
            gx_read_color(gx_attr_data(f.color[0], 2, v), f.color[0].type, o.color);
        else
            o.color[0] = o.color[1] = o.color[2] = o.color[3] = 0;
        // texgen sources
        for (u32 k = 0; k < 3; k++) {
            const GxTnlPlan::Tc& t = p.tc[k];
            f32* tc = o.tc[k];
            switch (t.src) {
            case TNL_SRC_POS:
                tc[0] = o.pos[0], tc[1] = o.pos[1], tc[2] = t.z ? o.pos[2] : 1.0f;
                break;
            case TNL_SRC_NRM:
                tc[0] = o.nrm[0], tc[1] = o.nrm[1], tc[2] = t.z ? o.nrm[2] : 1.0f;
                break;
            case TNL_SRC_TEX:
                read_tex_source(f, t.tex, v, tc);
                break;
            default:
                tc[0] = tc[1] = 0.0f, tc[2] = 1.0f;
                break;
            }
        }
    }
    return n;
}

u32 gx_tnl_slots(const GxTnlPlan& p, const GxTnlRows* rows, u32 n, GxTnlPalette& pal, GxTnlSlots* slots) {
    SlotMaps m;
    if (!m.init(p, pal))
        return 0;
    for (u32 i = 0; i < n; i++)
        if (!m.slots(p, pal, rows[i].r, slots[i]))
            return i;
    return n;
}

u32 gx_tnl_array_ranges(const GxVertexFormat& f, const u8* data, u32 count, GxTnlArrayRange out[12]) {
    u32 n = 0;
    auto range = [&](const GxAttrFormat& a, u32 attr, u32 elem_bytes) {
        if (a.mode < 2 || !count)
            return;
        u32 lo = ~0u, hi = 0;
        const u8* v = data + a.offset;
        if (a.mode == 2) {
            for (u32 i = 0; i < count; i++, v += f.size)
                lo = std::min<u32>(lo, *v), hi = std::max<u32>(hi, *v);
        } else {
            for (u32 i = 0; i < count; i++, v += f.size) {
                u32 x = gx_be16(v);
                lo = std::min(lo, x), hi = std::max(hi, x);
            }
        }
        u32 base = g_gx.cp[CP_ARRAY_BASE + attr], stride = g_gx.cp[CP_ARRAY_STRIDE + attr];
        out[n].attr = attr;
        out[n].addr = 0x80000000 | ((base + lo * stride) & 0x1FFFFFFF);
        out[n].size = (hi - lo) * stride + elem_bytes;
        n++;
    };
    range(f.pos, 0, f.pos.n * gx_comp_bytes(f.pos.type));
    range(f.nrm, 1, (f.nbt ? 9 : 3) * gx_comp_bytes(f.nrm.type));
    static const u32 k_color_bytes[8] = {2, 3, 4, 2, 3, 4, 4, 4};
    for (u32 c = 0; c < 2; c++)
        range(f.color[c], 2 + c, k_color_bytes[f.color[c].type & 7]);
    for (u32 t = 0; t < 8; t++)
        range(f.tex[t], 4 + t, f.tex[t].n * gx_comp_bytes(f.tex[t].type));
    return n;
}

// ---------------------------------------------------------------- the shader on the host
// NOMA_CHECK_GX_DRAW=pica: every draw through gx_tnl_plan/gx_tnl_vertices and the shader binary
// (NOMA_GX_SHBIN=path, picasso's output for gx.v.pica), compared with gx_transform's results.
namespace {

struct ShaderCheck {
    PicaShader* sh = nullptr;
    int o_pos = -1, o_clr = -1, o_tc[3] = {-1, -1, -1};
    int u_proj = -1, u_pal = -1, u_nlights = -1;
    bool tried = false;
};

bool shader_check_init(ShaderCheck& s) {
    if (s.tried)
        return s.sh != nullptr;
    s.tried = true;
    const char* path = getenv("NOMA_GX_SHBIN");
    if (!path) {
        log("gx check: NOMA_GX_SHBIN=path (picasso -o path platform/3ds/shaders/gx.v.pica) names the shader");
        return false;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        log("gx check: can't read %s", path);
        return false;
    }
    static std::vector<u8> bytes;
    fseek(f, 0, SEEK_END);
    bytes.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t got = fread(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    s.sh = got == bytes.size() ? pica_shader_load(bytes.data(), bytes.size()) : nullptr;
    if (!s.sh) {
        log("gx check: %s is not a shader binary", path);
        return false;
    }
    s.o_pos = pica_shader_output(s.sh, 0), s.o_clr = pica_shader_output(s.sh, 2);
    s.o_tc[0] = pica_shader_output(s.sh, 3), s.o_tc[1] = pica_shader_output(s.sh, 5), s.o_tc[2] = pica_shader_output(s.sh, 6);
    s.u_proj = pica_shader_uniform(s.sh, "proj"), s.u_pal = pica_shader_uniform(s.sh, "pal");
    s.u_nlights = pica_shader_uniform(s.sh, "nlights");
    if (s.u_proj != 0 || s.u_pal != (int)TNL_PAL_BASE || s.u_nlights != 0x60 || s.o_pos < 0 || s.o_clr < 0 ||
        s.o_tc[0] < 0) {
        log("gx check: the shader's uniforms are not where tnl.h expects them (proj %d, pal %d, nlights %d)",
            s.u_proj, s.u_pal, s.u_nlights);
        pica_shader_free(s.sh);
        s.sh = nullptr;
        return false;
    }
    log("gx check: running %s on every draw", path);
    return true;
}

}  // namespace

// The check keeps the palette across draws as the renderer does (slots a pending batch uses stay
// until a flush), and judges each pending vertex at the flush, with the palette as the GPU would
// read it then: a slot given away too early shows up as a wrong matrix.
namespace {

struct PendingVertex {
    f32 in[16][4];
    GxOutVertex ref;
    u32 bools;
    s8 texgen[3];
    f32 sx[3], sy[3];
    u32 vat, index, count;
    u32 first;
};

struct CheckState {
    std::vector<PendingVertex> pending;
    std::vector<f32> consts;  // the constants each pending vertex was planned with (53 vectors)
    GxTnlPalette pal;
    PicaUniforms u;
    bool consts_set = false;
    u64 draws = 0, vertices = 0, bad_draws = 0, bad_vertices = 0, flushes = 0, approx = 0;
};

bool close_f(f32 a, f32 b) {
    if (a == b || (std::isnan(a) && std::isnan(b)))
        return true;
    f32 d = std::fabs(a - b), m = std::max(std::fabs(a), std::fabs(b));
    return d <= 2e-4f * m || d <= 2e-5f;
}

// judge the pending vertices with the palette as it is now, then free it
void check_flush(ShaderCheck& s, CheckState& c) {
    memcpy(c.u.f[TNL_PAL_BASE], c.pal.rows, sizeof(c.pal.rows));
    u32 bad_this = 0;
    for (size_t i = 0; i < c.pending.size(); i++) {
        const PendingVertex& v = c.pending[i];
        memcpy(c.u.f[0], &c.consts[i * TNL_CONST_VECTORS * 4], TNL_CONST_VECTORS * 16);
        const GxTnlConsts* k = (const GxTnlConsts*)&c.consts[i * TNL_CONST_VECTORS * 4];
        (void)k;
        c.u.b = v.bools;
        f32 out[16][4];
        memset(out, 0, sizeof(out));
        pica_vsh_run(s.sh, c.u, v.in, out);
        const GxOutVertex& r = v.ref;
        bool ok = true;
        for (int q = 0; q < 4; q++)
            ok &= close_f(r.clip[q], out[s.o_pos][q]);
        for (int q = 0; q < 4; q++) {
            f32 col = r.color[0][q];
            f32 want = (col < 0 ? 0 : col > 255 ? 255 : col) / 255.0f;
            ok &= std::fabs(want - out[s.o_clr][q]) <= 1.5f / 255.0f;
        }
        for (int q = 0; q < 3; q++) {
            if (s.o_tc[q] < 0 || v.texgen[q] < 0)
                continue;
            const f32* t = r.tex[v.texgen[q]];
            f32 qq = t[2] != 0 ? t[2] : 1.0f;
            f32 ws = t[0] / qq * v.sx[q], wt = t[1] / qq * v.sy[q];
            if (v.texgen[q] >= 8 || v.texgen[q] < 0)
                ws = wt = 0.0f;
            ok &= close_f(ws, out[s.o_tc[q]][0]) && close_f(wt, out[s.o_tc[q]][1]);
        }
        if (!ok) {
            if (c.bad_vertices++ < 24)
                log("gx check: vat %u vertex %u/%u (bools %03X, slots %g %g %g %g): ref (%g %g %g %g) c %g %g %g %g t0 %g %g "
                    "| shader (%g %g %g %g) c %g %g %g %g t0 %g %g",
                    v.vat, v.index, v.count, v.bools, v.in[3][0], v.in[3][1], v.in[3][2], v.in[3][3], r.clip[0],
                    r.clip[1], r.clip[2], r.clip[3], r.color[0][0], r.color[0][1], r.color[0][2], r.color[0][3],
                    r.tex[v.first][0], r.tex[v.first][1], out[s.o_pos][0], out[s.o_pos][1], out[s.o_pos][2],
                    out[s.o_pos][3], out[s.o_clr][0] * 255, out[s.o_clr][1] * 255, out[s.o_clr][2] * 255,
                    out[s.o_clr][3] * 255, out[s.o_tc[0]][0], out[s.o_tc[0]][1]);
            bad_this++;
        }
    }
    c.bad_draws += bad_this != 0;  // (counted per flush)
    c.pending.clear();
    c.consts.clear();
    c.pal.flushed();
    c.flushes++;
}

}  // namespace

void gx_check_tnl_draw(u32 vat, u32 count, const u8* data, const GxOutVertex* ref) {
    static ShaderCheck s;
    if (!shader_check_init(s))
        return;
    static CheckState c;
    static std::vector<GxTnlVertex> verts;
    static std::vector<GxTnlSlots> vslots;
    if (!c.consts_set) {
        memset(&c.u, 0, sizeof(c.u));
        pica_shader_constants(s.sh, c.u);
        c.u.i[0][1] = 0, c.u.i[0][2] = 4, c.u.i[0][3] = 0;
        c.consts_set = true;
    }
    GxDrawSetup setup;
    u32 first = (u32)(c.draws % 6);
    for (int k = 0; k < 3; k++)
        setup.texgen[k] = (s8)(first + k), setup.sx[k] = 1.0f + k, setup.sy[k] = 1.0f / (1 + k);
    static const f32 ident[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    const GxTnlPlan& plan = gx_tnl_plan(vat, setup, (u32)(c.draws % 6), ident, 1);
    c.approx += plan.approximated != 0;
    // (the loop count is one uniform: a draw with a different count flushes, as the renderer's
    // constants change would)
    s32 nl = plan.consts.nlights ? (s32)plan.consts.nlights - 1 : 0;
    if (nl != c.u.i[0][0] && !c.pending.empty())
        check_flush(s, c);
    c.u.i[0][0] = nl;
    verts.resize(count);
    vslots.resize(count);
    const u32 ntg = g_gx.xf[XF_NUMTEXGENS];
    for (u32 done = 0; done < count;) {
        u32 n = gx_tnl_vertices(plan, data, done, count - done, c.pal, verts.data() + done, vslots.data() + done, nullptr);
        if (!n) {
            check_flush(s, c);
            n = gx_tnl_vertices(plan, data, done, count - done, c.pal, verts.data() + done, vslots.data() + done, nullptr);
            if (!n) {
                log("gx check: vat %u: a vertex's matrices don't fit the palette", vat);
                return;
            }
        }
        for (u32 i = done; i < done + n; i++) {
            const GxTnlVertex& v = verts[i];
            PendingVertex pv;
            memset(pv.in, 0, sizeof(pv.in));
            for (int k = 0; k < 3; k++)
                pv.in[0][k] = v.pos[k], pv.in[1][k] = v.nrm[k], pv.in[4][k] = v.tc[0][k], pv.in[5][k] = v.tc[1][k],
                pv.in[6][k] = v.tc[2][k];
            pv.in[0][3] = pv.in[1][3] = pv.in[4][3] = pv.in[5][3] = pv.in[6][3] = 1.0f;
            for (int k = 0; k < 4; k++)
                pv.in[2][k] = (f32)v.color[k], pv.in[3][k] = (f32)vslots[i].s[k];
            pv.ref = ref[i];
            pv.bools = plan.consts.bools;
            for (int k = 0; k < 3; k++) {
                pv.texgen[k] = (u32)setup.texgen[k] < ntg ? setup.texgen[k] : (s8)(setup.texgen[k] < 0 ? -1 : 8);
                pv.sx[k] = setup.sx[k], pv.sy[k] = setup.sy[k];
            }
            pv.vat = vat, pv.index = i, pv.count = count, pv.first = first;
            c.pending.push_back(pv);
            const f32* cf = (const f32*)&plan.consts;
            c.consts.insert(c.consts.end(), cf, cf + TNL_CONST_VECTORS * 4);
        }
        done += n;
    }
    c.draws++;
    c.vertices += count;
    if (c.pending.size() > 4000)  // (the renderer's batches end far sooner)
        check_flush(s, c);
    if (c.draws % 20000 == 0)
        log("gx check: %llu draws (%llu vertices) through the shader: %llu flushes with differences (%llu vertices); "
            "%llu approximated, %llu palette flushes",
            (unsigned long long)c.draws, (unsigned long long)c.vertices, (unsigned long long)c.bad_draws,
            (unsigned long long)c.bad_vertices, (unsigned long long)c.approx, (unsigned long long)c.flushes);
}

}  // namespace guest
