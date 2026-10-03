// Fields of the GPU registers, read from the state the command stream built (g_gx).
//   XF: transform unit registers (0x1000-0x1057) and memory (matrices, lights)
//   BP: rasterizer, texture, TEV and pixel engine registers
#pragma once
#include <cstring>

#include "gx.h"

namespace guest {

static inline u32 bits(u32 v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
static inline f32 xf_f32(u32 addr) {
    f32 f;
    memcpy(&f, &g_gx.xf[addr], 4);
    return f;
}

// ---- XF registers
enum : u32 {
    XF_INVTXSPEC = 0x1008,
    XF_NUMCOLORS = 0x1009,
    XF_AMBIENT0 = 0x100A,   // + channel
    XF_MATERIAL0 = 0x100C,  // + channel
    XF_COLOR0CNTRL = 0x100E,
    XF_ALPHA0CNTRL = 0x1010,
    XF_DUALTEX = 0x1012,
    XF_MATINDEX_A = 0x1018,
    XF_MATINDEX_B = 0x1019,
    XF_VIEWPORT = 0x101A,    // scale x y z, offset x y z
    XF_PROJECTION = 0x1020,  // 6 parameters, type
    XF_NUMTEXGENS = 0x103F,
    XF_TEXMTXINFO = 0x1040,  // + texgen
    XF_POSTMTXINFO = 0x1050,
    XF_MEM_NORMAL = 0x400,
    XF_MEM_POST = 0x500,
    XF_MEM_LIGHT = 0x600,
};

// ---- BP registers
enum : u32 {
    BP_GENMODE = 0x00,
    BP_IND_MTX = 0x06,
    BP_IND_IMASK = 0x0F,
    BP_IND_CMD = 0x10,
    BP_SCISSOR_TL = 0x20,
    BP_SCISSOR_BR = 0x21,
    BP_LINEPTWIDTH = 0x22,
    BP_RAS1_SS0 = 0x25,
    BP_IREF = 0x27,
    BP_TREF = 0x28,
    BP_SU_SSIZE = 0x30,
    BP_ZMODE = 0x40,
    BP_CMODE0 = 0x41,
    BP_CMODE1 = 0x42,
    BP_ZCONTROL = 0x43,
    BP_EFB_TL = 0x49,
    BP_EFB_WH = 0x4A,
    BP_EFB_ADDR = 0x4B,
    BP_EFB_STRIDE = 0x4D,
    BP_DISP_YSCALE = 0x4E,
    BP_CLEAR_AR = 0x4F,
    BP_CLEAR_GB = 0x50,
    BP_CLEAR_Z = 0x51,
    BP_TRIGGER_COPY = 0x52,
    BP_SCISSOR_OFFSET = 0x59,
    BP_LOADTLUT0 = 0x64,
    BP_LOADTLUT1 = 0x65,
    BP_TX_SETMODE0 = 0x80,
    BP_TX_SETMODE1 = 0x84,
    BP_TX_SETIMAGE0 = 0x88,
    BP_TX_SETIMAGE1 = 0x8C,
    BP_TX_SETIMAGE2 = 0x90,
    BP_TX_SETIMAGE3 = 0x94,
    BP_TX_SETTLUT = 0x98,
    BP_TEV_COLOR_ENV = 0xC0,  // + 2 * stage; alpha env at + 1
    BP_TEV_REGISTER_L = 0xE0,  // + 2 * register
    BP_FOG_PARAM3 = 0xF1,
    BP_FOG_COLOR = 0xF2,
    BP_ALPHACOMPARE = 0xF3,
    BP_TEV_KSEL = 0xF6,
};

// texture map n (0-7): its register in a group of four (0x80-0x83 for maps 0-3, 0xA0-0xA3 for 4-7)
static inline u32 bp_tex(u32 base, u32 map) { return base + (map & 3) + ((map & 4) ? 0x20 : 0); }

}  // namespace guest
