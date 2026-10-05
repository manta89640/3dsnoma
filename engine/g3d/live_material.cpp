// nw4r::g3d's material and shape setup, as GPU register writes (see live_material.h). Each step is
// the game's (the names, and the G3DState fields they keep, from the channel's g3d):
//   fog          G3DState::LoadFog (a fog's GXSetFog; not derived yet: the room's fogs are off)
//   z compare    GXSetZCompLoc(ResMatMisc +0)
//   TLUTs        GXLoadTlut (palettes into TMEM; not derived: the room's textures have none)
//   textures     GXLoadTexObj for each map the ResTexObj has (the mode and image registers), its
//                size kept for the texcoord scale
//   gen mode     kept, sent before the shape (BP 0x00 masked, XF 0x1009 and 0x103F)
//   TEV          ResTev's display list (its size by the stage count), then the texcoord scale
//                registers (BP 0x30+) from the sizes of the maps the texcoords sample
//   pixel, TEV colours, indirect, texgens: the material's display lists
//   light set    G3DState::LoadLightSet: the set's lights into GX light slots, their masks and the
//                set's ambient (the light cache: lights by slot; a light not in a slot loaded into
//                the first slot whose light the set doesn't use)
//   channels     ResMatChan: material colours, ambients scaled by the set's, controls with the
//                set's light masks (lighting off when the draw mode forces it)
//   texture SRTs per texcoord: identity, or the effect matrix and the SRT (Maya / XSI / Max) with a
//                mapping (environment, projection, specular) through the game's own functions,
//                into the post-transform matrices (XF 0x500 + 12 * texcoord)
// The arithmetic is the game's own functions where there is any (PSMTX*, the SRT and mapping
// functions), so the matrices are the same floats.
#include "engine/g3d/live_material.h"

#include <cstdlib>
#include <cstring>

#include "engine/g3d/live_res.h"
#include "noma_decomp.h"
#include "runtime/runtime.h"

namespace engine::g3d::live {

using ::guest::gp;

namespace {

u32 be32(const u8* p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

const u32 G3D_STATE = GA_nw4r_g3d_G3DState_sData;  // G3DState
const u32 LIGHT_CACHE = GA_nw4r_g3d_G3DState_sLightCache;
const u32 TEX_MTX_FUNCS = G3D_STATE + 0x240;  // {function, needs normal} by mapping mode
const u32 TEV_DL_SIZE = GA_nw4r_g3d_ResTev_sDlSize;      // ResTev's list's size by stage count - 1
const u32 CULL = GA_nw4r_g3d_sCullModeTable;              // GX cull mode by nw4r's

// a texture map's BP registers (maps 0-3, 4-7)
u32 tex_reg(u32 base03, u32 map) { return map < 4 ? base03 + map : base03 + 0x20 + (map - 4); }

void warn_once(const char* what) {
    static std::vector<const char*> s_said;
    for (const char* s : s_said)
        if (s == what)
            return;
    s_said.push_back(what);
    ::guest::log("g3d live: %s", what);
}

// GXLoadLightObjImm(obj, 1 << slot): the light's colour and 12 floats into its slot (words 3-15)
void load_light(u32 gx_obj, u32 slot, GxSink& r) {
    const u32 at = 0x600 + slot * 0x10;
    r.xf_write(at + 0, 0), r.xf_write(at + 1, 0), r.xf_write(at + 2, 0);
    r.xf_write(at + 3, load32(gx_obj + 0xC));
    for (u32 i = 0; i < 12; i++)
        r.xf_write(at + 4 + i, load32(gx_obj + 0x10 + 4 * i));
}

// G3DState::LoadLightSet(index): masks (colour 0, alpha 0, colour 1, alpha 1) and the ambient
void load_light_set(u32 index, LightCache& lc, u32 masks[4], u8 amb[4], GxSink& r) {
    if (index > 0x7F) {
        memset(masks, 0, 4 * sizeof(u32));
        memset(amb, 0xFF, 4);
        return;
    }
    const u32 set = LIGHT_CACHE + 0x242C + index * 0xC;
    const s8 ambient = (s8)load8(set + 8);
    for (int c = 0; c < 4; c++)
        amb[c] = ambient < 0 ? 0xFF : load8(LIGHT_CACHE + 0x222C + ambient * 4 + c);
    if (lc.current != (s32)index) {
        memset(lc.masks, 0, sizeof(lc.masks));
        lc.current = (s32)index;
        for (u32 k = 0; k < 8; k++) {
            const s8 li = (s8)load8(set + k);
            if (li < 0)
                continue;
            const u32 obj = LIGHT_CACHE + 0x2C + li * 0x44;
            const u32 flags = load32(obj);
            if (!(flags & 4))
                continue;
            int slot = -1;
            for (int s = 0; s < 8 && slot < 0; s++)
                if (lc.slots[s] == li)
                    slot = s;
            if (slot < 0) {
                for (int s = 0; s < 8 && slot < 0; s++)
                    if (lc.slots[s] == -1)
                        slot = s;  // (a free slot: taken, nothing loaded, as the game does)
                if (slot >= 0) {
                    lc.slots[slot] = li;
                } else {
                    // the first slot whose light the set doesn't use
                    for (int s = 0; s < 8 && slot < 0; s++) {
                        bool used = false;
                        for (u32 j = 0; j < 8; j++)
                            used |= (s8)load8(set + j) == lc.slots[s];
                        if (!used)
                            slot = s;
                    }
                    if (slot < 0)
                        continue;  // (the game's would be slot 8: no such light)
                    load_light(obj + 4, (u32)slot, r);
                    lc.slots[slot] = li;
                }
            }
            const u32 bit = 1u << slot;
            if (flags & 2) {
                if (!(flags & 0x10))
                    lc.masks[2] |= bit;
                if (!(flags & 0x20))
                    lc.masks[3] |= bit;
            } else {
                if (!(flags & 0x10))
                    lc.masks[0] |= bit;
                if (!(flags & 0x20))
                    lc.masks[1] |= bit;
            }
        }
    }
    memcpy(masks, lc.masks, sizeof(lc.masks));
    // the lights the masks name are in their slots (loaded now, or by the pass's SetLightSetting)
    const u32 all = masks[0] | masks[1] | masks[2] | masks[3];
    for (u32 s = 0; s < 8; s++)
        if ((all >> s) & 1 && lc.slots[s] >= 0)
            load_light(LIGHT_CACHE + 0x2C + lc.slots[s] * 0x44 + 4, s, r);
}

// a colour or alpha control with the light set's mask in its light mask bits (and the lighting
// enable cleared when it is forced off)
u32 chan_ctrl(u32 ctrl, u32 mask, bool off) {
    const u32 keep = off ? ~0x783Eu : ~0x783Cu;
    return (((ctrl & keep & ~0x3Cu) | ((mask << 2) & 0x3C)) & ~0x7800u) | ((mask << 7) & 0x7800);
}

// ResMatChan
void set_channels(u32 chan, const MatShared& sh, const u32 masks[4], const u8 amb[4], bool off, GxSink& r) {
    if (!chan)
        return;
    const u32 f0 = load32(chan);
    if (f0 & 3)
        r.xf_write(0x100C, load32(chan + 4));
    if (f0 & 0xC) {
        u32 a = 0;
        for (int c = 0; c < 4; c++) {
            const f32 v = 0.5f + 0.003921569f * (f32)(s32)(amb[c] * load8(chan + 8 + c));
            a |= (u32)(u8)(s32)v << (24 - 8 * c);
        }
        r.xf_write(0x100A, a);
    }
    static const bool s_mutate = getenv("NOMA_CHECK_G3D_LIVE_MUTATE") != nullptr;  // (the check's own test)
    if (f0 & 0x10)
        r.xf_write(0x100E, chan_ctrl(load32(chan + 0xC), s_mutate ? 0 : masks[0], off));
    if (f0 & 0x20)
        r.xf_write(0x1010, chan_ctrl(load32(chan + 0x10), masks[1], off));
    if (sh.chans == 2) {
        const u32 f1 = load32(chan + 0x14);
        if (f1 & 3)
            r.xf_write(0x100D, load32(chan + 0x18));
        if (f1 & 0xC)
            r.xf_write(0x100B, load32(chan + 0x1C));
        if (f1 & 0x10)
            r.xf_write(0x100F, chan_ctrl(load32(chan + 0x20), masks[2], off));
        if (f1 & 0x20)
            r.xf_write(0x1011, chan_ctrl(load32(chan + 0x24), masks[3], off));
    } else {
        r.xf_write(0x100F, 0);
        r.xf_write(0x1011, 0);
    }
}

// the gen mode's registers
void flush_genmode(const MatShared& sh, GxSink& r) {
    r.bp_write(0xFE07FC3Fu);
    r.bp_write(((u32)sh.ind << 16 & 0xFF0000) | (u32)load8(CULL + sh.cull) << 14 | ((u32)sh.stages - 1) << 10 |
               sh.texgens | ((u32)sh.chans * 16 & 0xFF0));
    r.xf_write(0x1009, sh.chans);
    r.xf_write(0x103F, sh.texgens);
}

// ResTexSrt: the post-transform matrices (and which texcoords the shape gives normal matrices)
void set_tex_srt(u32 srt, MatShared& sh, GxSink& r) {
    GFRAME(0xD0);
    const u32 m = (u32)&GS(u8, 0x68), tmp = (u32)&GS(u8, 0x38), ident = (u32)&GS(u8, 0x8);
    const u32 flags = srt ? load32(srt) : 0;
    for (u32 i = 0; i < 8; i++) {
        sh.needs[i] = 0;
        if (!srt || !(flags & (1u << (4 * i))))
            continue;
        const u32 eff = srt + 0xA8 + i * 0x34;  // TexMtxEffect: camera, light, mapping, flag, MTX34
        const u32 nib = (flags >> (4 * i)) & 0xF;
        bool identity = true;
        g::PSMTXIdentity(m, 0);
        u32 mode = load8(eff + 2);
        if (mode) {
            s32 cam = (s8)load8(eff), light = (s8)load8(eff + 1);
            if (cam >= 0x20)
                cam = -1;
            if (light >= 0x80)
                light = -1;
            gcall<u32>(load32(TEX_MTX_FUNCS + mode * 8), m, (u32)cam, (u32)light, mode);
            identity = false;
            sh.needs[i] = (u8)load32(TEX_MTX_FUNCS + mode * 8 + 4);
        }
        if (!(nib == 0xF && (load8(eff + 3) & 1))) {
            if (identity) {
                if (!(load8(eff + 3) & 1)) {
                    identity = false;
                    g::PSMTXCopy(eff + 4, m);
                }
            } else {
                g::PSMTXConcat(eff + 4, m, m);
            }
            const u32 one = srt + 8 + i * 0x14;
            if (mode == 0) {
                g::nw4r_g3d_CalcTexMtx(m, identity ? 1 : 0, one, nib, load32(srt + 4));
            } else {
                g::nw4r_g3d_CalcTexMtx(tmp, 1, one, nib, load32(srt + 4));
                // (projection: the translation into the third column)
                const u32 a = load32(tmp + 0x8), b = load32(tmp + 0xC), c = load32(tmp + 0x18), d = load32(tmp + 0x1C);
                ::guest::store32(tmp + 0x8, b), ::guest::store32(tmp + 0xC, a);
                ::guest::store32(tmp + 0x18, d), ::guest::store32(tmp + 0x1C, c);
                g::PSMTXConcat(tmp, m, m);
            }
            identity = false;
        }
        if (identity)
            g::PSMTXIdentity(ident, 0);
        const u32 src = identity ? ident : m;
        for (u32 k = 0; k < 12; k++)
            r.xf_write(0x500 + 12 * i + k, load32(src + 4 * k));
    }
}

}  // namespace

// ---- the register file

void GxRegs::clear() {
    memset(bp, 0, sizeof(bp)), memset(bp_known, 0, sizeof(bp_known));
    memset(tev_reg, 0, sizeof(tev_reg)), memset(tev_konst, 0, sizeof(tev_konst));
    memset(tev_reg_known, 0, sizeof(tev_reg_known)), memset(tev_konst_known, 0, sizeof(tev_konst_known));
    memset(xf, 0, sizeof(xf)), memset(xf_known, 0, sizeof(xf_known));
    memset(xf_mem, 0, sizeof(xf_mem)), memset(xf_mem_known, 0, sizeof(xf_mem_known));
    bp_mask = 0xFFFFFF;
}

void GxRegs::bp_write(u32 cmd) {
    if (log)
        log->push_back({0, 0, cmd});
    const u32 reg = cmd >> 24, v = cmd & 0xFFFFFF;
    if (reg == 0xFE) {
        bp_mask = v;
        return;
    }
    const u32 mask = bp_mask;
    bp_mask = 0xFFFFFF;
    bp[reg] = (bp[reg] & ~mask) | (v & mask);
    bp_known[reg] |= mask;
    if (reg >= 0xE0 && reg < 0xE8 && mask == 0xFFFFFF) {
        const bool konst = (v >> 23) & 1;
        s32* dst = konst ? tev_konst[(reg - 0xE0) / 2] : tev_reg[(reg - 0xE0) / 2];
        u8& known = konst ? tev_konst_known[(reg - 0xE0) / 2] : tev_reg_known[(reg - 0xE0) / 2];
        auto sext11 = [](u32 x) { return (s32)(x << 21) >> 21; };
        if (reg & 1)
            dst[2] = sext11(v & 0x7FF), dst[1] = sext11((v >> 12) & 0x7FF), known |= 2 | 4;
        else
            dst[0] = sext11(v & 0x7FF), dst[3] = sext11((v >> 12) & 0x7FF), known |= 1 | 8;
    }
}

void GxRegs::xf_write(u32 addr, u32 v) {
    if (log)
        log->push_back({1, (u16)addr, v});
    if (addr >= 0x1000 && addr < 0x1060)
        xf[addr - 0x1000] = v, xf_known[addr - 0x1000] = true;
    else if (addr < 0x700)
        xf_mem[addr] = v, xf_mem_known[addr] = true;
}

bool GxRegs::apply_dl(u32 addr, u32 size) {
    const u8* p = ::guest::host_ptr(addr);
    if (!p || ::guest::host_ptr(addr + size - 1) != p + size - 1)
        return false;
    for (u32 at = 0; at < size;) {
        const u8 cmd = p[at];
        if (cmd == 0x00 || cmd == 0x48) {
            at++;
        } else if (cmd == 0x61 && at + 5 <= size) {
            bp_write(be32(p + at + 1));
            at += 5;
        } else if (cmd == 0x10 && at + 5 <= size) {
            const u32 h = be32(p + at + 1), n = ((h >> 16) & 0xF) + 1, a = h & 0xFFFF;
            for (u32 i = 0; i < n && at + 9 + 4 * i <= size; i++)
                xf_write(a + i, be32(p + at + 5 + 4 * i));
            at += 5 + 4 * n;
        } else if (cmd == 0x08 && at + 6 <= size) {
            at += 6;
        } else {
            return false;
        }
    }
    return true;
}

// ---- the sources, the caches

MatSources mat_sources(u32 mt, u32 repl) {
    const u32 dls = mat::dls(mt), id = mat::id(mt);
    auto pick = [&](u32 field, u32 size, u32 own) {
        const u32 buf = repl ? load32(repl + field) : 0;
        return buf ? buf + id * size : own;
    };
    MatSources s;
    s.tex_obj = pick(repl::TEX_OBJ, repl::SZ_TEX_OBJ, mat::tex_obj(mt));
    s.tlut_obj = pick(repl::TLUT_OBJ, repl::SZ_TLUT_OBJ, mat::tlut_obj(mt));
    s.genmode = pick(repl::GENMODE, repl::SZ_GENMODE, mat::genmode(mt));
    s.tev = pick(repl::TEV, repl::SZ_TEV, mat::tev(mt));
    s.pix_dl = pick(repl::PIX_DL, repl::SZ_PIX_DL, dls ? dls : 0);
    s.tevcolor_dl = pick(repl::TEVCOLOR_DL, repl::SZ_TEVCOLOR_DL, dls ? dls + 0x20 : 0);
    s.ind_dl = pick(repl::IND_DL, repl::SZ_IND_DL, dls ? dls + 0xA0 : 0);
    s.chan = pick(repl::CHAN, repl::SZ_CHAN, mat::chan(mt));
    s.texgen_dl = pick(repl::TEXGEN_DL, repl::SZ_TEXGEN_DL, dls ? dls + 0xE0 : 0);
    s.misc = pick(repl::MISC, repl::SZ_MISC, mat::misc(mt));
    s.tex_srt = pick(repl::TEX_SRT, repl::SZ_TEX_SRT, mat::tex_srt(mt));
    return s;
}

LightCache light_cache_now() {
    LightCache lc;
    lc.current = (s32)load32(LIGHT_CACHE + 0x10);
    for (int i = 0; i < 4; i++)
        lc.masks[i] = load32(LIGHT_CACHE + 0x14 + 4 * i);
    for (int s = 0; s < 8; s++)
        lc.slots[s] = (s8)load8(LIGHT_CACHE + 0x24 + s);
    return lc;
}

void light_cache_store(const LightCache& lc) {
    ::guest::store32(LIGHT_CACHE + 0x10, (u32)lc.current);
    for (int i = 0; i < 4; i++)
        ::guest::store32(LIGHT_CACHE + 0x14 + 4 * i, lc.masks[i]);
    for (int s = 0; s < 8; s++)
        ::guest::store8(LIGHT_CACHE + 0x24 + s, (u8)lc.slots[s]);
}

MatShared mat_shared_now() {
    MatShared sh;
    sh.texgens = load8(G3D_STATE), sh.chans = load8(G3D_STATE + 1), sh.stages = load8(G3D_STATE + 2);
    sh.ind = load8(G3D_STATE + 3), sh.cull = load32(G3D_STATE + 4);
    for (int i = 0; i < 8; i++) {
        sh.needs[i] = (u8)load32(G3D_STATE + 0x210 + 4 * i);
        sh.map_w[i] = load16(G3D_STATE + 0xC + 4 + 4 * i), sh.map_h[i] = load16(G3D_STATE + 0xC + 6 + 4 * i);
    }
    return sh;
}

// ---- Draw1Mat1ShpDirectly's material setup

bool derive_material(u32 mt, const MatSources& s, u32 ctrl, LightCache& lc, MatShared& sh, GxSink& r) {
    if (mat::fur(mt)) {
        warn_once("a fur material: drawn by the game's own code");
        return false;
    }
    if (ctrl & 0x10)
        return true;
    if (s.misc && (load8(s.misc + 4) | load8(s.misc + 5) | load8(s.misc + 6) | load8(s.misc + 7))) {
        warn_once("an indirect normal map: drawn by the game's own code");
        return false;
    }
    // z compare (BP 0x43 bit 6: the other bits are the pixel format's)
    if (s.misc) {
        r.bp_write(0xFE000040u);
        r.bp_write(0x43000000u | (u32)(load8(s.misc) & 1) << 6);
    }
    // textures
    if (s.tex_obj) {
        const u32 used = load32(s.tex_obj);
        for (u32 i = 0; i < 8; i++) {
            if (!(used & (1u << i)))
                continue;
            const u32 o = s.tex_obj + 4 + i * 32;
            const u32 image0 = load32(o + 8) & 0xFFFFFF;
            r.bp_write(tex_reg(0x80, i) << 24 | (load32(o) & 0xFFFFFF));
            r.bp_write(tex_reg(0x84, i) << 24 | (load32(o + 4) & 0xFFFFFF));
            r.bp_write(tex_reg(0x88, i) << 24 | image0);
            r.bp_write(tex_reg(0x94, i) << 24 | (load32(o + 0xC) & 0xFFFFFF));
            sh.map_w[i] = (u16)((image0 & 0x3FF) + 1), sh.map_h[i] = (u16)(((image0 >> 10) & 0x3FF) + 1);
        }
    }
    // gen mode (sent with the texgens, below)
    if (s.genmode) {
        sh.texgens = load8(s.genmode), sh.chans = load8(s.genmode + 1), sh.stages = load8(s.genmode + 2);
        sh.ind = load8(s.genmode + 3), sh.cull = load32(s.genmode + 4);
    }
    // the TEV, then the texcoord scales from the sizes of the maps they sample
    if (s.tev) {
        const u32 n = load8(s.tev + 0xC);
        if (n < 1 || n > 16 || !r.apply_dl(s.tev + 0x20, load32(TEV_DL_SIZE + (n - 1) * 4)))
            warn_once("a TEV list that didn't decode");
        for (u32 t = 0; t < sh.texgens && t < 8; t++) {
            const u8 map = tev::texmap_of(s.tev, t);
            if (map == 0xFF || map > 7)
                continue;
            r.bp_write(0xFE03FFFFu);
            r.bp_write((0x30 + 2 * t) << 24 | (u32)(sh.map_w[map] - 1));
            r.bp_write(0xFE03FFFFu);
            r.bp_write((0x31 + 2 * t) << 24 | (u32)(sh.map_h[map] - 1));
        }
    }
    if (s.pix_dl)
        r.apply_dl(s.pix_dl, 0x20);
    if (s.tevcolor_dl)
        r.apply_dl(s.tevcolor_dl, 0x80);
    if (s.ind_dl && sh.ind)
        r.apply_dl(s.ind_dl, 0x40);
    // the light set, the channels
    u32 masks[4];
    u8 amb[4];
    load_light_set(s.misc ? (u32)(s32)(s8)load8(s.misc + 1) : ~0u, lc, masks, amb, r);
    set_channels(s.chan, sh, masks, amb, (ctrl >> 3) & 1, r);
    // texgens, the gen mode
    if (s.texgen_dl && sh.texgens)
        r.apply_dl(s.texgen_dl, sh.texgens < 2 ? 0x20 : sh.texgens < 4 ? 0x40 : sh.texgens < 8 ? 0x80 : 0xA0);
    flush_genmode(sh, r);
    // texture SRTs
    set_tex_srt(s.tex_srt, sh, r);
    return true;
}

// ---- the shape's matrix registers

bool derive_shape(u32 shp, u32 vpos, u32 vnrm, const MatShared& sh, GxSink& r) {
    const s32 cur = shp::cur_mtx(shp);
    u32 t[8];
    bool any = false;
    if (cur >= 0) {
        GFRAME(0x40);
        for (u32 i = 0; i < 8; i++) {
            if (sh.needs[i] == 1) {
                t[i] = 0x1E + 3 * i;
                // the normal matrix as a texture matrix: the view array's, or made from the position matrix
                u32 n = vnrm ? vnrm + (u32)cur * 0x24 : 0;
                if (!n) {
                    n = (u32)&GS(u8, 0x8);
                    g::nw4r_math_MTX34InvTranspose(n, vpos ? vpos + (u32)cur * 0x30 : 0);
                }
                const u32 at = t[i] * 4;
                for (u32 row = 0; row < 3; row++) {
                    for (u32 col = 0; col < 3; col++)
                        r.xf_write(at + row * 4 + col, load32(n + row * 0xC + col * 4));
                    r.xf_write(at + row * 4 + 3, 0);
                }
                any = true;
            } else if (sh.needs[i] == 2) {
                t[i] = 0;
                any = true;
            } else {
                t[i] = 0x3C;
            }
        }
    }
    if (any) {
        r.xf_write(0x1018, t[3] << 24 | t[2] << 18 | t[1] << 12 | t[0] << 6);
        r.xf_write(0x1019, t[7] << 18 | t[6] << 12 | t[5] << 6 | t[4]);
        return false;
    }
    r.xf_write(0x1018, 0x3CF3CF00u);
    r.xf_write(0x1019, 0x00F3CF3Cu);
    return true;
}

}  // namespace engine::g3d::live
