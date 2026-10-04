// The GX library's hottest calls (GXTev.c, GXTexture.c, GXTransform.c, GXAttr.c, GXGeometry.c),
// native: the command stream they write and the __GXData state they keep are the original's,
// byte for byte (tools/armbench/armbench.py --gxsdk runs them against the decompiled functions on
// random states). What makes them cheaper than the decompiled code on the ARM11: __GXData is read
// through one host pointer (it lives in MEM1) instead of a page lookup per field, no poll at entry,
// and the values are worked out once.
#include "gx_sdk.h"

#include "noma_decomp.h"
#include "hw/hw.h"

namespace engine::gx {

namespace {

// __GXData (GXData in the SDK; the fields this uses)
enum : u32 {
    D_BP_SENT_NOT = 0x2,      // s16: 0 after BP writes, 1 after XF/CP writes
    D_VNUM = 0x4,             // u16: vertices in the current primitive (0: none)
    D_VLIM = 0x6,             // u16
    D_VCD_LO = 0x14,
    D_VCD_HI = 0x18,
    D_VAT_A = 0x1C,           // u32[8], then VAT B at 0x3C, VAT C at 0x5C
    D_MATIDX_A = 0x80,
    D_MATIDX_B = 0x84,
    D_AMB_COLOR = 0xA8,       // u32[2], then the material colors at 0xB0 (XF 0x100A-0x100D)
    D_CHAN_CTRL = 0xB8,       // u32[4] (XF 0x100E-0x1011)
    D_TEXGEN = 0xC8,          // u32[8] (XF 0x1040+), the post-transform ones at 0xE8 (XF 0x1050+)
    D_TREF = 0x150,           // u32[8]: two TEV orders per register
    D_IREF = 0x170,
    D_TEV_KSEL = 0x200,       // u32[8]: swap table at the low bits of each pair
    D_GEN_MODE = 0x254,
    D_TLUT_REGION_CB = 0x51C,
    D_HAS_NBTS = 0x524,       // u8
    D_HAS_BI_NRMS = 0x525,    // u8
    D_TEX_IMAGE0 = 0x564,     // u32[8]
    D_TEX_MODE0 = 0x584,      // u32[8]
    D_TEXCOORD_FOR_STAGE = 0x5A4,  // u32[16]
    D_TEX_MAP_ID_NOT_SCALED = 0x5E4,  // bits per texture map that need no scale registers
    D_TEV_TC_ENAB = 0x5E8,
    D_DIRTY_VAT = 0x5FB,      // u8: VATs to send
    D_DIRTY_STATE = 0x5FC,
};

// __GXData: its guest address and where it is in host memory (MEM1: one piece), read and written
// in place
struct Data {
    u32 a;
    u8* p;
    u32 w(u32 off) const {
        u32 v;
        ::memcpy(&v, p + off, 4);
        return __builtin_bswap32(v);
    }
    u32 h(u32 off) const { return (u32)p[off] << 8 | p[off + 1]; }
    u32 b(u32 off) const { return p[off]; }
    void set_w(u32 off, u32 v) const {
        v = __builtin_bswap32(v);
        ::memcpy(p + off, &v, 4);
    }
    void set_h(u32 off, u32 v) const { p[off] = (u8)(v >> 8), p[off + 1] = (u8)v; }
    void set_b(u32 off, u32 v) const { p[off] = (u8)v; }
};
inline Data gxdata() {
    u32 a = guest::load32(GA___GXData);
    return {a, guest::host_ptr(a)};
}

// commands for the write-gather pipe, appended to its buffer as they come (guest::gather_put: the
// decompiled code's own inline path); send() is kept for the call sites' shape
struct Pipe {
    void byte(u32 v) { guest::gather_put<1>(v & 0xFF); }
    void word(u32 v) { guest::gather_put<4>(v); }
    // a register write's 5 bytes with one bounds check where they fit before the buffer's end
    void bp(u32 v) {
        u32 n = guest::g_gather_n;
        if (__builtin_expect(n <= 32 - 5, 1)) {
            guest::g_gather[n] = 0x61;
            u32 w = __builtin_bswap32(v);
            ::memcpy(guest::g_gather + n + 1, &w, 4);
            guest::g_gather_n = n += 5;
            if (n == 32)
                guest::gx_gather_burst();
        } else {
            byte(0x61), word(v);
        }
    }
    void cp(u32 reg, u32 v) { byte(0x08), byte(reg), word(v); }
    void xf(u32 reg, u32 v) { byte(0x10), word(reg), word(v); }  // one XF register
    void send() {}
};

// GXTexture.c: each texture map's BP registers (GXTexMode0Ids ... GXTexTlutIds)
const u8 GX_TEX_MODE0_IDS[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
const u8 GX_TEX_MODE1_IDS[8] = {0x84, 0x85, 0x86, 0x87, 0xA4, 0xA5, 0xA6, 0xA7};
const u8 GX_TEX_IMAGE0_IDS[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};
const u8 GX_TEX_IMAGE1_IDS[8] = {0x8C, 0x8D, 0x8E, 0x8F, 0xAC, 0xAD, 0xAE, 0xAF};
const u8 GX_TEX_IMAGE2_IDS[8] = {0x90, 0x91, 0x92, 0x93, 0xB0, 0xB1, 0xB2, 0xB3};
const u8 GX_TEX_IMAGE3_IDS[8] = {0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7};
const u8 GX_TEX_TLUT_IDS[8] = {0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB};
// (the guest's copies, for a map id past the tables, read as the original would)
const u32 GA_GX_TEX_MODE0_IDS = 0x80881B68, GA_GX_TEX_MODE1_IDS = 0x80881B70, GA_GX_TEX_IMAGE0_IDS = 0x80881B78,
          GA_GX_TEX_IMAGE1_IDS = 0x80881B80, GA_GX_TEX_IMAGE2_IDS = 0x80881B88, GA_GX_TEX_IMAGE3_IDS = 0x80881B90,
          GA_GX_TEX_TLUT_IDS = 0x80881B98;
inline u32 tex_id(const u8* table, u32 guest_table, u32 map) {
    return map < 8 ? table[map] : guest::load8(guest_table + map);
}
// GXAttr.c, __GXCalculateVLim: bytes per attribute by its descriptor (none, direct, 8- and 16-bit
// index), for colors, texture coordinates, and positions/normals
const u8 VLIM_TBL1[4] = {0, 4, 1, 2};
const u8 VLIM_TBL2[4] = {0, 8, 1, 2};
const u8 VLIM_TBL3[4] = {0, 12, 1, 2};

inline u32 bit(u32 n) { return n < 32 ? 1u << n : 0; }  // PowerPC slw (n < 64 here)
inline u32 clz32(u32 x) { return x ? (u32)__builtin_clz(x) : 32; }

inline u32 set_field(u32 word, u32 shift, u32 bits, u32 v) {
    u32 mask = ((1u << bits) - 1) << shift;
    return (word & ~mask) | ((v << shift) & mask);
}

// __SetSURegs(map, coord): a texture coordinate's scale registers (SU_SSIZE, SU_TSIZE) from its
// map's size and wrap modes
void su_regs(const Data& d, u32 map, u32 coord) {
    // (an index past the tables reads and writes whatever guest memory is there, as the original)
    const bool in = map < 8 && coord < 8;
    auto rd = [&](u32 off) { return in ? d.w(off) : guest::load32(d.a + off); };
    const u32 image0 = rd(D_TEX_IMAGE0 + map * 4), mode0 = rd(D_TEX_MODE0 + map * 4);
    u32 s = (rd(0x108 + coord * 4) & ~0x1FFFFu) | (image0 & 0x3FF) | ((clz32((mode0 & 3) - 1) << 11) & 0x10000);
    if (in)
        d.set_w(0x108 + coord * 4, s);
    else
        guest::store32(d.a + 0x108 + coord * 4, s);
    u32 t = (rd(0x128 + coord * 4) & ~0x1FFFFu) | ((image0 >> 10) & 0x3FF) |
            ((clz32(((mode0 >> 2) & 3) - 1) << 11) & 0x10000);
    if (in)
        d.set_w(0x128 + coord * 4, t);
    else
        guest::store32(d.a + 0x128 + coord * 4, t);
    Pipe p;
    p.bp(s), p.bp(t);
    d.set_h(D_BP_SENT_NOT, 0);
}

void set_su_tex_regs() {
    const Data d = gxdata();
    if (d.w(D_TEX_MAP_ID_NOT_SCALED) == 0xFF)
        return;
    u32 gen_mode = d.w(D_GEN_MODE);
    u32 n_ind = (gen_mode >> 16) & 7;
    u32 n_tev = ((gen_mode >> 10) & 0xF) + 1;
    // indirect stages: their coordinates and maps from the IREF register (fields read again after
    // each __SetSURegs, as the original does)
    u32 coord = 0, map = 0;
    for (u32 i = 0; i < n_ind; i++) {
        if (i < 4) {
            u32 iref = d.w(D_IREF);
            coord = (iref >> (6 * i)) & 7;
            map = (iref >> (6 * i + 3)) & 7;
        }
        if (!(d.w(D_TEX_MAP_ID_NOT_SCALED) & bit(map)))
            su_regs(d, coord, map);
    }
    // TEV stages
    for (u32 k = 0; k < n_tev; k++) {
        u32 tref = d.w(D_TREF + (k / 2) * 4);
        u32 tc = d.w(D_TEXCOORD_FOR_STAGE + 4 * k) & 0xFFFFFEFF;
        u32 m = (k & 1) ? (tref >> 15) & 7 : (tref >> 3) & 7;
        if (tc != 0xFF && !(d.w(D_TEX_MAP_ID_NOT_SCALED) & bit(m)) && (d.w(D_TEV_TC_ENAB) & bit(k)))
            su_regs(d, tc, m);
    }
}

void set_vcd(const Data& d) {
    Pipe p;
    u32 lo = d.w(D_VCD_LO), hi = d.w(D_VCD_HI);
    p.cp(0x50, lo);
    p.cp(0x60, hi);
    u32 nbt = d.b(D_HAS_BI_NRMS) ? 2 : (d.b(D_HAS_NBTS) != 0);
    u32 colors = clz32((lo >> 13) & 0xF);
    u32 texs = clz32(hi & 0xFFFF);
    // XF: the vertex's colors, normals and texture coordinates
    p.xf(0x1008, (((0x21 - texs) * 8) & 0xFFFFFFF0) | ((0x21 - colors) >> 1) | (nbt * 4));
    p.send();
    d.set_h(D_BP_SENT_NOT, 1);
}

void set_vat(const Data& d) {
    Pipe p;
    u32 dirty = d.b(D_DIRTY_VAT);
    u32 k = 0;
    do {
        if (dirty & 1) {
            p.cp(k | 0x70, d.w(D_VAT_A + 4 * k));
            p.cp(k | 0x80, d.w(D_VAT_A + 0x20 + 4 * k));
            p.cp(k | 0x90, d.w(D_VAT_A + 0x40 + 4 * k));
        }
        dirty >>= 1;
        k++;
    } while (dirty);
    p.byte(0x00);
    p.send();
    d.set_b(D_DIRTY_VAT, 0);
}

void calculate_vlim(const Data& d) {
    if (d.h(D_VNUM) == 0)
        return;
    u32 lo = d.w(D_VCD_LO), hi = d.w(D_VCD_HI);
    u32 nrm = VLIM_TBL3[(lo >> 11) & 3];
    u32 n = (u32)__builtin_popcount(lo & 0x1FF);  // the matrix indices
    n += VLIM_TBL3[(lo >> 9) & 3];                 // position
    u32 nbt = ((d.w(D_VAT_A) >> 9) & 1) == 1 ? 3 : 1;
    n += nrm * nbt;
    n += VLIM_TBL1[(lo >> 13) & 3] + VLIM_TBL1[(lo >> 15) & 3];
    for (u32 t = 0; t < 8; t++)
        n += VLIM_TBL2[(hi >> (2 * t)) & 3];
    d.set_h(D_VLIM, n);
}

void matrix_index(const Data& d, Pipe& p, u32 which) {
    bool a = (s32)which < 5;
    u32 v = d.w(a ? D_MATIDX_A : D_MATIDX_B);
    p.cp(a ? 0x30 : 0x40, v);
    p.xf(a ? 0x1018 : 0x1019, v);
}

}  // namespace

void set_matrix_index(u32 which) {
    const Data d = gxdata();
    Pipe p;
    matrix_index(d, p, which);
    p.send();
    d.set_h(D_BP_SENT_NOT, 1);
}

void set_dirty_state() {
    const Data d = gxdata();
    u32 dirty = d.w(D_DIRTY_STATE);
    if (dirty & 1)
        set_su_tex_regs();
    // dirty & 2: __GXUpdateBPMask, empty in this SDK
    if (dirty & 4) {
        Pipe p;
        p.bp(d.w(D_GEN_MODE));
        p.send();
        d.set_h(D_BP_SENT_NOT, 0);
    }
    if (dirty & 8)
        set_vcd(d);
    if (dirty & 0x10)
        set_vat(d);
    if (dirty & 0x18)
        calculate_vlim(d);
    u32 xf = dirty & 0xFFFFFF00;
    if (!xf) {
        d.set_w(D_DIRTY_STATE, 0);
        return;
    }
    Pipe p;
    // channel colors (XF 0x100A-0x100D)
    for (u32 k = 0; k < 4; k++)
        if (xf & (0x100u << k))
            p.xf(0x100A + k, d.w(D_AMB_COLOR + 4 * k));
    // the number of color channels, then the channel controls (XF 0x1009, 0x100E-0x1011)
    u32 chans = (xf & 0x01000000) | (xf & 0xF000);
    if (chans) {
        if (chans & 0x01000000)
            p.xf(0x1009, (d.w(D_GEN_MODE) >> 4) & 7);
        u32 reg = 0x100E, off = D_CHAN_CTRL;
        for (u32 bits = (chans >> 12) & 0xF; bits; bits >>= 1, off += 4, reg++)
            if (bits & 1)
                p.xf(reg, d.w(off));
    }
    // the number of texture coordinates, then the texture coordinate generators (XF 0x103F,
    // 0x1040+, 0x1050+)
    u32 gens = xf & 0x02FF0000;
    if (gens) {
        if (gens & 0x02000000)
            p.xf(0x103F, d.w(D_GEN_MODE) & 0xF);
        u32 reg = 0x1040, off = D_TEXGEN;
        for (u32 bits = (gens >> 16) & 0xFF; bits; bits >>= 1, reg++, off += 4)
            if (bits & 1) {
                p.xf(reg, d.w(off));
                p.xf(reg + 0x10, d.w(off + 0x20));
            }
    }
    if (xf & 0x04000000) {
        matrix_index(d, p, 0);
        matrix_index(d, p, 5);
    }
    p.send();
    if (xf & 0x04000000)
        d.set_h(D_BP_SENT_NOT, 1);  // (as __GXSetMatrixIndex leaves it, for what follows)
    if (xf & 0x10000000)
        g::__GXSetViewport();
    if (xf & 0x08000000)
        g::__GXSetProjection();
    d.set_h(D_BP_SENT_NOT, 1);
    d.set_w(D_DIRTY_STATE, 0);
}

u64 load_pos_mtx_imm(u32 mtx, u32 id) {
    Pipe p;
    p.byte(0x10);
    p.word((id * 4) | 0xB0000);  // XF: 12 words at the matrix
    for (u32 i = 0; i < 12; i++)
        p.word(guest::load32(mtx + 4 * i));
    p.send();
    return (u64)0xCC008000u << 32 | mtx;  // r3: the matrix, r4: the pipe (as the original leaves them)
}

// GXBegin's flush of a primitive after BP writes (bpSentNot = 0): a dummy draw of vnum * vlim zero
// bytes, as words, in triangle strips of format 0
void begin_flush(const Data& d) {
    const u32 n = d.h(D_VNUM) * d.h(D_VLIM);
    Pipe p;
    p.byte(0x98);
    guest::gather_put<2>(d.h(D_VNUM));
    const u32 words = n ? (n + 3) >> 2 : 0;
    for (u32 i = 0; i < words; i++)
        p.word(0);
    if (words && (guest::g_loop_budget -= (s32)(words / 8 + 1)) < 0)
        guest::loop_poll();
    d.set_h(D_BP_SENT_NOT, 1);
}

// a __GXData word at an offset that may lie anywhere (an index the original doesn't check)
inline u32 any_w(const Data& d, u64 off) {
    return off + 4 <= 0x600 ? d.w((u32)off) : guest::load32(d.a + (u32)off);
}
inline void any_set_w(const Data& d, u64 off, u32 v) {
    if (off + 4 <= 0x600)
        d.set_w((u32)off, v);
    else
        guest::store32(d.a + (u32)off, v);
}

}  // namespace engine::gx

// ---- the guest ABI (decl.h)
namespace g {

using namespace engine::gx;

void __GXSetDirtyState() {
    set_dirty_state();
}

void __SetSURegs(u32 map, u32 coord) { engine::gx::su_regs(engine::gx::gxdata(), map, coord); }

void __GXSetSUTexRegs() {
    set_su_tex_regs();
}

void __GXSetVCD() {
    set_vcd(gxdata());
}

void __GXSetVAT() {
    set_vat(gxdata());
}

void __GXCalculateVLim() {
    calculate_vlim(gxdata());
}

void __GXSetMatrixIndex(u32 which) {
    set_matrix_index(which);
}

// GXSetTevColorS10(id, color): a TEV register's signed 10-bit RGBA (BP 0xE0 + 2*id: R and A,
// 0xE1 + 2*id: B and G; the B/G write goes three times, as the hardware needs)
void GXSetTevColorS10(u32 id, u32 color) {
    u32 rg = guest::load32(color), ba = guest::load32(color + 4);  // GXColorS10 {s16 r, g, b, a}
    u32 reg = id * 2;
    Pipe p;
    p.bp((((((reg + 0xE0) << 24) & ~0x7FFu) | ((rg >> 16) & 0x7FF)) & ~0x7FF000u) | ((ba << 12) & 0x7FF000));
    u32 w = (((((reg + 0xE1) << 24) & ~0x7FFu) | ((ba >> 16) & 0x7FF)) & ~0x7FF000u) | ((rg << 12) & 0x7FF000);
    p.bp(w), p.bp(w), p.bp(w);
    p.send();
    gxdata().set_h(D_BP_SENT_NOT, 0);
}

// GXSetTevKColor(id, color): a TEV constant color (BP 0xE0 + 2*id with the konst bit)
void GXSetTevKColor(u32 id, u32 color) {
    u32 c = guest::load32(color);  // RGBA
    u32 reg = id * 2;
    // BP 0xE0+2id: R (bits 0-7), A (12-19); 0xE1+2id: B, G; bit 23: the constant color
    u32 lo = (((reg + 0xE0) << 24) & ~0xFFu) | (c >> 24);
    lo = (lo & ~0xFF000u) | ((c << 12) & 0xFF000);
    u32 hi = (((reg + 0xE1) << 24) & ~0xFFu) | ((c >> 8) & 0xFF);
    hi = (hi & ~0xFF000u) | ((c >> 4) & 0xFF000);
    Pipe p;
    p.bp((lo & ~0xF00000u) | 0x800000);
    p.bp((hi & ~0xF00000u) | 0x800000);
    p.send();
    gxdata().set_h(D_BP_SENT_NOT, 0);
}

// GXSetTevSwapModeTable(table, r, g, b, a): one of the four swap tables (two KSEL registers)
u32 GXSetTevSwapModeTable(u32 table, u32 red, u32 green, u32 blue, u32 alpha) {
    const Data d = gxdata();
    u32 a = D_TEV_KSEL + table * 8, b = D_TEV_KSEL + (table * 2 + 1) * 4;
    u32 va = set_field(set_field(d.w(a), 0, 2, red), 2, 2, green);
    d.set_w(a, va);
    u32 vb = set_field(set_field(d.w(b), 0, 2, blue), 2, 2, alpha);
    d.set_w(b, vb);
    Pipe p;
    p.bp(va), p.bp(vb);
    p.send();
    d.set_h(D_BP_SENT_NOT, 0);
    return vb;
}

// GXLoadTexMtxImm(mtx, id, type): a 3x4 (GX_MTX3x4, 0) or 2x4 (GX_MTX2x4, 1) texture matrix into
// XF matrix memory (ids from 0x40 are the post-transform matrices at 0x500). (Any other type
// announces 12 words and sends 8, as the original.)
void GXLoadTexMtxImm(u32 mtx, u32 id, u32 type) {
    u32 addr = id >= 0x40 ? (id - 0x40) * 4 + 0x500 : id * 4;
    u32 words = type == 0 ? 12 : 8;
    Pipe p;
    p.byte(0x10);
    p.word(addr | ((type == 1 ? 8 - 1 : 12 - 1) << 16));
    for (u32 i = 0; i < words; i++)
        p.word(guest::load32(mtx + 4 * i));
    p.send();
}

// GXLoadTexObjPreLoaded(obj, region, map): a texture object's registers for a texture map, its
// TLUT's when it has one; the texture map's mode and image registers are remembered
u32 GXLoadTexObjPreLoaded(u32 obj, u32 region, u32 map) {
    u32 mode0 = (guest::load32(obj) & 0x00FFFFFF) | tex_id(GX_TEX_MODE0_IDS, GA_GX_TEX_MODE0_IDS, map) << 24;
    u32 mode1 = (guest::load32(obj + 4) & 0x00FFFFFF) | tex_id(GX_TEX_MODE1_IDS, GA_GX_TEX_MODE1_IDS, map) << 24;
    u32 image0 = (guest::load32(obj + 8) & 0x00FFFFFF) | tex_id(GX_TEX_IMAGE0_IDS, GA_GX_TEX_IMAGE0_IDS, map) << 24;
    u32 image1 = (guest::load32(region) & 0x00FFFFFF) | tex_id(GX_TEX_IMAGE1_IDS, GA_GX_TEX_IMAGE1_IDS, map) << 24;
    u32 image2 = (guest::load32(region + 4) & 0x00FFFFFF) | tex_id(GX_TEX_IMAGE2_IDS, GA_GX_TEX_IMAGE2_IDS, map) << 24;
    u32 flags = guest::load8(obj + 0x1F);
    u32 image3 = (guest::load32(obj + 0xC) & 0x00FFFFFF) | tex_id(GX_TEX_IMAGE3_IDS, GA_GX_TEX_IMAGE3_IDS, map) << 24;
    Pipe p;
    p.bp(mode0), p.bp(mode1), p.bp(image0), p.bp(image1), p.bp(image2), p.bp(image3);
    p.send();
    guest::store32(obj, mode0);
    guest::store32(obj + 4, mode1);
    guest::store32(obj + 8, image0);
    guest::store32(region, image1);
    guest::store32(region + 4, image2);
    guest::store32(obj + 0xC, image3);
    const Data d = gxdata();
    if (!(flags & 2)) {
        // a color-indexed texture: its TLUT's region, from the callback the game registered
        u32 tlut = gcall<u32>(d.w(D_TLUT_REGION_CB), guest::load32(obj + 0x18), d.a);
        u32 v = (guest::load32(tlut + 4) & 0x00FFFFFF) | tex_id(GX_TEX_TLUT_IDS, GA_GX_TEX_TLUT_IDS, map) << 24;
        guest::store32(tlut + 4, v);
        p.bp(guest::load32(tlut + 4));
        p.send();
    }
    d.set_w(D_TEX_IMAGE0 + map * 4, guest::load32(obj + 8));
    d.set_w(D_TEX_MODE0 + map * 4, guest::load32(obj));
    u32 dirty = d.w(D_DIRTY_STATE) | 1;
    d.set_w(D_DIRTY_STATE, dirty);
    d.set_h(D_BP_SENT_NOT, 0);
    return dirty;
}

// GXBegin(type, vtxfmt, nverts)
u32 GXBegin(u32 type, u32 fmt, u32 n) {
    const Data d = gxdata();
    if (d.w(D_DIRTY_STATE))
        set_dirty_state();
    if (d.w(0) == 0)
        begin_flush(d);
    guest::gather_put<1>((fmt | type) & 0xFF);
    guest::gather_put<2>(n & 0xFFFF);
    return 0xCC010000u;
}

// GXSetArray(attr, base, stride): the CP array registers
void GXSetArray(u32 attr, u32 base, u32 stride) {
    const u32 a = (attr == 0x19 ? 0xA : attr) - 9;
    Pipe p;
    p.cp((a | 0xA0) & 0xFF, base & 0x3FFFFFFF);
    p.cp((a | 0xB0) & 0xFF, stride);
}

// GXLoadTexObj(obj, map): the texture's region from the game's callback, then preloaded
u32 GXLoadTexObj(u32 obj, u32 map) {
    const Data d = gxdata();
    return GXLoadTexObjPreLoaded(obj, gcall<u32>(guest::load32(d.a + 0x518), obj, map, d.a), map);
}

// GXSetCurrentMtx(id)
void GXSetCurrentMtx(u32 id) {
    const Data d = gxdata();
    d.set_w(D_MATIDX_A, (d.w(D_MATIDX_A) & ~0x3Fu) | (id & 0x3F));
    d.set_w(D_DIRTY_STATE, d.w(D_DIRTY_STATE) | 0x04000000);
}

// GXCallDisplayList(list, size)
u32 GXCallDisplayList(u32 list, u32 size) {
    const Data d = gxdata();
    if (d.w(D_DIRTY_STATE))
        set_dirty_state();
    if (d.w(0) == 0)
        __GXSendFlushPrim();
    Pipe p;
    p.byte(0x40), p.word(list), p.word(size);
    return 0xCC010000u;
}

// GXLoadLightObjImm(obj, light): a light's 16 XF words (3 zeros, its color, 12 floats)
void GXLoadLightObjImm(u32 obj, u32 light, u32, u32) {
    Pipe p;
    p.byte(0x10);
    p.word((((0x1F - clz32(light)) * 0x10) & 0x70) + 0x600 | 0xF0000);
    p.word(0), p.word(0), p.word(0);
    p.word(guest::load32(obj + 0xC));
    for (u32 i = 0; i < 12; i++)
        p.word(guest::load32(obj + 0x10 + 4 * i));
    gxdata().set_h(D_BP_SENT_NOT, 1);
}

// GXLoadNrmMtxImm(mtx, id): a normal matrix's 3x3 part
void GXLoadNrmMtxImm(u32 mtx, u32 id) {
    Pipe p;
    p.byte(0x10);
    p.word((id * 3 + 0x400) | 0x80000);
    for (u32 r = 0; r < 3; r++)
        for (u32 c = 0; c < 3; c++)
            p.word(guest::load32(mtx + r * 0x10 + c * 4));
}

// GXSetVtxDesc(attr, type): r3 and r4 as the original leaves them
u64 GXSetVtxDesc(u32 attr, u32 type) {
    const Data d = gxdata();
    u32 r3 = d.a;
    auto lo = [&](u32 shift, u32 mask) { d.set_w(D_VCD_LO, (d.w(D_VCD_LO) & ~mask) | ((type << shift) & mask)); };
    auto hi = [&](u32 shift, u32 mask) { d.set_w(D_VCD_HI, (d.w(D_VCD_HI) & ~mask) | ((type << shift) & mask)); };
    switch (attr) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: lo(attr, 1u << attr); break;
    case 9: lo(9, 0x600); break;
    case 10:
    case 25:
        if (type != 0) {
            r3 = 1;
            d.set_b(attr == 10 ? D_HAS_NBTS : D_HAS_BI_NRMS, 1);
            d.set_b(attr == 10 ? D_HAS_BI_NRMS : D_HAS_NBTS, 0);
            d.set_w(0x520, type);
        } else {
            d.set_b(attr == 10 ? D_HAS_NBTS : D_HAS_BI_NRMS, 0);
        }
        break;
    case 11: lo(13, 0x6000); break;
    case 12: lo(15, 0x18000); break;
    case 13: case 14: case 15: case 16: case 17: case 18: case 19: case 20: hi((attr - 13) * 2, 3u << ((attr - 13) * 2)); break;
    default: r3 = attr; break;
    }
    if (d.b(D_HAS_NBTS) || d.b(D_HAS_BI_NRMS)) {
        r3 = (d.w(D_VCD_LO) & ~0x1800u) | ((d.w(0x520) << 11) & 0x1800);
        d.set_w(D_VCD_LO, r3);
    } else {
        d.set_w(D_VCD_LO, d.w(D_VCD_LO) & 0xFFFFE7FF);
    }
    d.set_w(D_DIRTY_STATE, d.w(D_DIRTY_STATE) | 8);
    return NOMA_U64(r3, d.a);
}

void GXClearVtxDesc() {
    const Data d = gxdata();
    d.set_w(D_VCD_LO, 0x200);
    d.set_w(D_VCD_HI, 0);
    d.set_b(D_HAS_NBTS, 0);
    d.set_b(D_HAS_BI_NRMS, 0);
    d.set_w(D_DIRTY_STATE, d.w(D_DIRTY_STATE) | 8);
}

// GXSetVtxAttrFmt(vtxfmt, attr, cnt, type, frac): r3 and r4 as the original leaves them
u64 GXSetVtxAttrFmt(u32 vf, u32 attr, u32 cnt, u32 type, u32 frac) {
    const Data d = gxdata();
    const u64 a = D_VAT_A + (u64)vf * 4, b = a + 0x20, c = a + 0x40;
    auto set = [&](u64 off, u32 mask, u32 v) { any_set_w(d, off, (any_w(d, off) & ~mask) | (v & mask)); };
    switch (attr) {
    case 9: set(a, 0x1FF, (cnt & 1) | ((type << 1) & 0xE) | ((frac << 4) & 0x1F0)); break;
    case 10:
    case 25:
        set(a, 0x1C00, type << 10);
        if (cnt == 2)
            any_set_w(d, a, any_w(d, a) | 0x200 | 0x80000000);
        else
            any_set_w(d, a, ((any_w(d, a) & ~0x200u) | ((cnt << 9) & 0x200)) & 0x7FFFFFFF);
        break;
    case 11: set(a, 0x1E000, ((cnt << 13) & 0x2000) | ((type << 14) & 0x1C000)); break;
    case 12: set(a, 0x1E0000, ((cnt << 17) & 0x20000) | ((type << 18) & 0x1C0000)); break;
    case 13: set(a, 0x3FE00000, ((cnt << 21) & 0x200000) | ((type << 22) & 0x1C00000) | ((frac << 25) & 0x3E000000)); break;
    case 14: set(b, 0x1FF, (cnt & 1) | ((type << 1) & 0xE) | ((frac << 4) & 0x1F0)); break;
    case 15: set(b, 0x3FE00, ((cnt << 9) & 0x200) | ((type << 10) & 0x1C00) | ((frac << 13) & 0x3E000)); break;
    case 16: set(b, 0x7FC0000, ((cnt << 18) & 0x40000) | ((type << 19) & 0x380000) | ((frac << 22) & 0x7C00000)); break;
    case 17:
        set(b, 0x78000000, ((cnt << 27) & 0x8000000) | ((type << 28) & 0x70000000));
        set(c, 0x1F, frac);
        break;
    case 18: set(c, 0x3FE0, ((cnt << 5) & 0x20) | ((type << 6) & 0x1C0) | ((frac << 9) & 0x3E00)); break;
    case 19: set(c, 0x7FC000, ((cnt << 14) & 0x4000) | ((type << 15) & 0x38000) | ((frac << 18) & 0x7C0000)); break;
    case 20: set(c, 0xFF800000, ((cnt << 23) & 0x800000) | ((type << 24) & 0x7000000) | (frac << 27)); break;
    }
    const u32 dirty = d.w(D_DIRTY_STATE);
    d.set_w(D_DIRTY_STATE, dirty | 0x10);
    const u32 vats = d.b(D_DIRTY_VAT);
    const u32 sh = vf & 0xFF & 0x3F;  // (slw: the low 6 bits of the count; 32 to 63 give 0)
    d.set_b(D_DIRTY_VAT, vats | (u8)(sh < 32 ? 1u << sh : 0));
    return NOMA_U64(vats, dirty);
}

}  // namespace g
