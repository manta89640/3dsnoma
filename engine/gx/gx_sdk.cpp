// The GX library's hottest calls (GXTev.c, GXTexture.c, GXTransform.c, GXAttr.c, GXGeometry.c),
// native: the command stream they write and the __GXData state they keep are the original's,
// byte for byte, but the commands go straight to the write-gather FIFO (the decompiled code's
// stores to 0xCC008000 each took the MMIO path) and the state is read once instead of per store.
#include "gx_sdk.h"

#include "noma_decomp.h"
#include "hw/hw.h"

namespace engine::gx {

namespace {

// the write-gather pipe
inline void put8(u32 v) { guest::gx_fifo_write(0, v & 0xFF, 1); }
inline void put32(u32 v) { guest::gx_fifo_write(0, v, 4); }

inline u32 gxdata() { return guest::load32(GA___GXData); }

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

void set_su_tex_regs() {
    u32 gx = gxdata();
    if (guest::load32(gx + D_TEX_MAP_ID_NOT_SCALED) == 0xFF)
        return;
    u32 gen_mode = guest::load32(gx + D_GEN_MODE);
    u32 n_ind = (gen_mode >> 16) & 7;
    u32 n_tev = ((gen_mode >> 10) & 0xF) + 1;
    // indirect stages: their coordinates and maps from the IREF register
    u32 coord = 0, map = 0;
    for (u32 i = 0; i < n_ind; i++) {
        if (i < 4) {
            u32 iref = guest::load32(gx + D_IREF);
            coord = (iref >> (6 * i)) & 7;
            map = (iref >> (6 * i + 3)) & 7;
        }
        if (!(guest::load32(gx + D_TEX_MAP_ID_NOT_SCALED) & bit(map)))
            g::__SetSURegs(coord, map);
    }
    // TEV stages
    u32 gx2 = gxdata();
    for (u32 k = 0; k < n_tev; k++) {
        u32 tref = guest::load32(gx2 + D_TREF + (k / 2) * 4);
        u32 tc = guest::load32(gx2 + D_TEXCOORD_FOR_STAGE + 4 * k) & 0xFFFFFEFF;
        u32 m = (k & 1) ? (tref >> 15) & 7 : (tref >> 3) & 7;
        if (tc != 0xFF && !(guest::load32(gx + D_TEX_MAP_ID_NOT_SCALED) & bit(m)) &&
            (guest::load32(gx2 + D_TEV_TC_ENAB) & bit(k)))
            g::__SetSURegs(tc, m);
    }
}

void set_vcd() {
    put8(0x08);
    u32 gx = gxdata();
    put8(0x50);
    put32(guest::load32(gx + D_VCD_LO));
    put8(0x08);
    put8(0x60);
    put32(guest::load32(gx + D_VCD_HI));
    u32 nbt = guest::load8(gx + D_HAS_BI_NRMS) ? 2 : (guest::load8(gx + D_HAS_NBTS) != 0);
    u32 colors = clz32((guest::load32(gx + D_VCD_LO) >> 13) & 0xF);
    u32 texs = clz32(guest::load32(gx + D_VCD_HI) & 0xFFFF);
    put8(0x10);
    put32(0x1008);  // XF: the vertex's colors, normals and texture coordinates
    put32((((0x21 - texs) * 8) & 0xFFFFFFF0) | ((0x21 - colors) >> 1) | (nbt * 4));
    guest::store16(gx + D_BP_SENT_NOT, 1);
}

void set_vat() {
    u32 gx = gxdata();
    u32 dirty = guest::load8(gx + D_DIRTY_VAT);
    u32 p = gx, k = 0;
    do {
        if (dirty & 1) {
            put8(0x08), put8(k | 0x70), put32(guest::load32(p + D_VAT_A));
            put8(0x08), put8(k | 0x80), put32(guest::load32(p + D_VAT_A + 0x20));
            put8(0x08), put8(k | 0x90), put32(guest::load32(p + D_VAT_A + 0x40));
        }
        dirty >>= 1;
        k++;
        p += 4;
    } while (dirty);
    put8(0x00);
    guest::store8(gx + D_DIRTY_VAT, 0);
}

void calculate_vlim() {
    u32 gx = gxdata();
    if (guest::load16(gx + D_VNUM) == 0)
        return;
    u32 lo = guest::load32(gx + D_VCD_LO), hi = guest::load32(gx + D_VCD_HI);
    u32 nrm = VLIM_TBL3[(lo >> 11) & 3];
    u32 n = 0;
    for (u32 b = 0; b < 9; b++)  // the matrix indices
        n += (lo >> b) & 1;
    n += VLIM_TBL3[(lo >> 9) & 3];  // position
    u32 nbt = ((guest::load32(gx + D_VAT_A) >> 9) & 1) == 1 ? 3 : 1;
    n += nrm * nbt;
    n += VLIM_TBL1[(lo >> 13) & 3] + VLIM_TBL1[(lo >> 15) & 3];
    for (u32 t = 0; t < 8; t++)
        n += VLIM_TBL2[(hi >> (2 * t)) & 3];
    guest::store16(gx + D_VLIM, (u16)n);
}

}  // namespace

void set_matrix_index(u32 which) {
    bool a = (s32)which < 5;
    put8(0x08);
    u32 gx = gxdata();
    put8(a ? 0x30 : 0x40);
    u32 field = a ? D_MATIDX_A : D_MATIDX_B;
    put32(guest::load32(gx + field));
    put8(0x10);
    put32(a ? 0x1018 : 0x1019);
    put32(guest::load32(gx + field));
    guest::store16(gx + D_BP_SENT_NOT, 1);
}

void set_dirty_state() {
    u32 gx = gxdata();
    u32 dirty = guest::load32(gx + D_DIRTY_STATE);
    if (dirty & 1)
        set_su_tex_regs();
    // dirty & 2: __GXUpdateBPMask, empty in this SDK
    if (dirty & 4) {
        put8(0x61);
        u32 gx2 = gxdata();
        put32(guest::load32(gx2 + D_GEN_MODE));
        guest::store16(gx2 + D_BP_SENT_NOT, 0);
    }
    if (dirty & 8)
        set_vcd();
    if (dirty & 0x10)
        set_vat();
    if (dirty & 0x18)
        calculate_vlim();
    u32 xf = dirty & 0xFFFFFF00;
    if (!xf) {
        guest::store32(gx + D_DIRTY_STATE, 0);
        return;
    }
    // channel colors (XF 0x100A-0x100D)
    for (u32 k = 0; k < 4; k++)
        if (xf & (0x100u << k)) {
            put8(0x10);
            put32(0x100A + k);
            put32(guest::load32(gx + D_AMB_COLOR + 4 * k));
        }
    // the number of color channels, then the channel controls (XF 0x1009, 0x100E-0x1011)
    u32 chans = (xf & 0x01000000) | (xf & 0xF000);
    if (chans) {
        if (chans & 0x01000000) {
            u32 gen_mode = guest::load32(gx + D_GEN_MODE);
            put8(0x10), put32(0x1009), put32((gen_mode >> 4) & 7);
        }
        u32 reg = 0x100E, p = gx;
        for (u32 bits = (chans >> 12) & 0xF; bits; bits >>= 1, p += 4, reg++)
            if (bits & 1)
                put8(0x10), put32(reg), put32(guest::load32(p + D_CHAN_CTRL));
    }
    // the number of texture coordinates, then the texture coordinate generators (XF 0x103F,
    // 0x1040+, 0x1050+)
    u32 gens = xf & 0x02FF0000;
    if (gens) {
        if (gens & 0x02000000) {
            u32 gen_mode = guest::load32(gx + D_GEN_MODE);
            put8(0x10), put32(0x103F), put32(gen_mode & 0xF);
        }
        u32 reg = 0x1040, p = gx;
        for (u32 bits = (gens >> 16) & 0xFF; bits; bits >>= 1, reg++, p += 4)
            if (bits & 1) {
                put8(0x10), put32(reg), put32(guest::load32(p + D_TEXGEN));
                put8(0x10), put32(reg + 0x10), put32(guest::load32(p + D_TEXGEN + 0x20));
            }
    }
    if (xf & 0x04000000) {
        set_matrix_index(0);
        set_matrix_index(5);
    }
    if (xf & 0x10000000)
        g::__GXSetViewport();
    if (xf & 0x08000000)
        g::__GXSetProjection();
    guest::store16(gx + D_BP_SENT_NOT, 1);
    guest::store32(gx + D_DIRTY_STATE, 0);
}

u64 load_pos_mtx_imm(u32 mtx, u32 id) {
    put8(0x10);
    put32((id * 4) | 0xB0000);  // XF: 12 words at the matrix
    for (u32 i = 0; i < 12; i++)
        put32(guest::load32(mtx + 4 * i));
    return (u64)0xCC008000u << 32 | mtx;  // r3: the matrix, r4: the pipe (as the original leaves them)
}

}  // namespace engine::gx

// ---- the guest ABI (decl.h)
namespace g {

using namespace engine::gx;

void __GXSetDirtyState() {
    set_dirty_state();
}

void __GXSetSUTexRegs() {
    set_su_tex_regs();
}

void __GXSetVCD() {
    set_vcd();
}

void __GXSetVAT() {
    set_vat();
}

void __GXCalculateVLim() {
    calculate_vlim();
}

void __GXSetMatrixIndex(u32 which) {
    set_matrix_index(which);
}

// GXSetTevColorS10(id, color): a TEV register's signed 10-bit RGBA (BP 0xE0 + 2*id: R and A,
// 0xE1 + 2*id: B and G; the B/G write goes three times, as the hardware needs)
void GXSetTevColorS10(u32 id, u32 color) {
    u32 rg = guest::load32(color), ba = guest::load32(color + 4);  // GXColorS10 {s16 r, g, b, a}
    u32 reg = id * 2;
    put8(0x61);
    put32((((((reg + 0xE0) << 24) & ~0x7FFu) | ((rg >> 16) & 0x7FF)) & ~0x7FF000u) | ((ba << 12) & 0x7FF000));
    u32 gx = gxdata();
    u32 w = (((((reg + 0xE1) << 24) & ~0x7FFu) | ((ba >> 16) & 0x7FF)) & ~0x7FF000u) | ((rg << 12) & 0x7FF000);
    for (int i = 0; i < 3; i++)
        put8(0x61), put32(w);
    guest::store16(gx + D_BP_SENT_NOT, 0);
}

// GXSetTevKColor(id, color): a TEV constant color (BP 0xE0 + 2*id with the konst bit)
void GXSetTevKColor(u32 id, u32 color) {
    u32 c = guest::load32(color);  // RGBA
    u32 reg = id * 2;
    put8(0x61);
    u32 gx = gxdata();
    // BP 0xE0+2id: R (bits 0-7), A (12-19); 0xE1+2id: B, G; bit 23: the constant color
    u32 lo = (((reg + 0xE0) << 24) & ~0xFFu) | (c >> 24);
    lo = (lo & ~0xFF000u) | ((c << 12) & 0xFF000);
    put32((lo & ~0xF00000u) | 0x800000);
    put8(0x61);
    u32 hi = (((reg + 0xE1) << 24) & ~0xFFu) | ((c >> 8) & 0xFF);
    hi = (hi & ~0xFF000u) | ((c >> 4) & 0xFF000);
    put32((hi & ~0xF00000u) | 0x800000);
    guest::store16(gx + D_BP_SENT_NOT, 0);
}

// GXSetTevSwapModeTable(table, r, g, b, a): one of the four swap tables (two KSEL registers)
u32 GXSetTevSwapModeTable(u32 table, u32 red, u32 green, u32 blue, u32 alpha) {
    u32 gx = gxdata();
    u32 ksel = gx + D_TEV_KSEL;
    u32 a = ksel + table * 8, b = ksel + (table * 2 + 1) * 4;
    guest::store32(a, set_field(set_field(guest::load32(a), 0, 2, red), 2, 2, green));
    put8(0x61);
    put32(guest::load32(a));
    guest::store32(b, set_field(set_field(guest::load32(b), 0, 2, blue), 2, 2, alpha));
    put8(0x61);
    u32 v = guest::load32(b);
    put32(v);
    guest::store16(gx + D_BP_SENT_NOT, 0);
    return v;
}

// GXLoadTexMtxImm(mtx, id, type): a 3x4 (GX_MTX3x4, 0) or 2x4 (GX_MTX2x4, 1) texture matrix into
// XF matrix memory (ids from 0x40 are the post-transform matrices at 0x500). (Any other type
// announces 12 words and sends 8, as the original.)
void GXLoadTexMtxImm(u32 mtx, u32 id, u32 type) {
    u32 addr = id >= 0x40 ? (id - 0x40) * 4 + 0x500 : id * 4;
    put8(0x10);
    put32(addr | ((type == 1 ? 8 - 1 : 12 - 1) << 16));
    u32 words = type == 0 ? 12 : 8;
    for (u32 i = 0; i < words; i++)
        put32(guest::load32(mtx + 4 * i));
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
    put8(0x61), put32(mode0);
    put8(0x61), put32(mode1);
    put8(0x61), put32(image0);
    put8(0x61), put32(image1);
    put8(0x61), put32(image2);
    put8(0x61);
    guest::store32(obj, mode0);
    guest::store32(obj + 4, mode1);
    guest::store32(obj + 8, image0);
    guest::store32(region, image1);
    guest::store32(region + 4, image2);
    guest::store32(obj + 0xC, image3);
    put32(image3);
    if (!(flags & 2)) {
        // a color-indexed texture: its TLUT's region, from the callback the game registered
        u32 gx = gxdata();
        u32 tlut = gcall<u32>(guest::load32(gx + D_TLUT_REGION_CB), guest::load32(obj + 0x18), gx);
        u32 v = (guest::load32(tlut + 4) & 0x00FFFFFF) | tex_id(GX_TEX_TLUT_IDS, GA_GX_TEX_TLUT_IDS, map) << 24;
        guest::store32(tlut + 4, v);
        put8(0x61);
        put32(guest::load32(tlut + 4));
    }
    u32 gx = gxdata();
    guest::store32(gx + D_TEX_IMAGE0 + map * 4, guest::load32(obj + 8));
    guest::store32(gx + D_TEX_MODE0 + map * 4, guest::load32(obj));
    u32 dirty = guest::load32(gx + D_DIRTY_STATE) | 1;
    guest::store32(gx + D_DIRTY_STATE, dirty);
    guest::store16(gx + D_BP_SENT_NOT, 0);
    return dirty;
}

}  // namespace g
