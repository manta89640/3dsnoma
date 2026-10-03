// The GPU's command stream: decoding, register state, display lists.
//
//   0x00        NOP
//   0x08 r v32  LOAD_CP_REG   vertex descriptors/formats, array bases and strides
//   0x10 h d..  LOAD_XF_REG   h = (count - 1) << 16 | address; count data words
//   0x20-0x38   LOAD_INDX_A-D XF memory from the CP arrays 12-15: index << 16 | (n - 1) << 12 | addr
//   0x40 a s    CALL_DL       execute the display list at physical a, s bytes
//   0x44, 0x48  (metrics), INVALIDATE_VTX_CACHE
//   0x61 v32    LOAD_BP_REG   register in the top byte, 24-bit value
//   0x80-0xBF   draw          primitive in bits 3-7, vertex format in bits 0-2; u16 count; vertices
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gx.h"
#include "regs.h"
#include "texture.h"

namespace guest {

static GxRenderer s_null_renderer;
static GxRenderer* s_renderer = &s_null_renderer;
void gx_set_renderer(GxRenderer* r) { s_renderer = r ? r : &s_null_renderer; }

static u32 s_bp_mask = 0xFFFFFF;

// statistics for NOMA_DEBUG_GX
struct GxStats {
    u32 draws, vertices, bp, cp, xf, dl, copies, finish, tokens, unknown;
};
static GxStats s_stats;
static bool s_debug = getenv("NOMA_DEBUG_GX") != nullptr;

static u32 be16(const u8* p) { return (u32)p[0] << 8 | p[1]; }
static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

// ---- vertex sizes
static const u32 k_comp_size[8] = {1, 1, 2, 2, 4, 0, 0, 0};       // u8 s8 u16 s16 f32
static const u32 k_color_size[8] = {2, 3, 4, 2, 3, 4, 0, 0};      // 565 888 888x 4444 6666 8888

static u32 index_size(u32 mode) { return mode == 2 ? 1 : 2; }

u32 gx_vertex_size(u32 vat) {
    const u32 lo = g_gx.cp[CP_VCD_LO], hi = g_gx.cp[CP_VCD_HI];
    const u32 a = g_gx.cp[CP_VAT_A + vat], b = g_gx.cp[CP_VAT_B + vat], c = g_gx.cp[CP_VAT_C + vat];
    u32 size = 0;
    // matrix indices, one byte each
    for (int i = 0; i < 9; i++)
        size += (lo >> i) & 1;
    // position
    if (u32 m = (lo >> 9) & 3)
        size += m == 1 ? ((a & 1) ? 3 : 2) * k_comp_size[(a >> 1) & 7] : index_size(m);
    // normal (N, or NBT: three vectors; indexed NBT may carry one index or three)
    if (u32 m = (lo >> 11) & 3) {
        bool nbt = (a >> 9) & 1;
        if (m == 1)
            size += (nbt ? 9 : 3) * k_comp_size[(a >> 10) & 7];
        else
            size += index_size(m) * ((nbt && (a >> 31)) ? 3 : 1);
    }
    // colors
    for (int i = 0; i < 2; i++) {
        u32 m = (lo >> (13 + 2 * i)) & 3;
        if (m)
            size += m == 1 ? k_color_size[(a >> (14 + 4 * i)) & 7] : index_size(m);
    }
    // texture coordinates: elements (S or ST) and format for each of the eight
    const u32 tc_fields[8][2] = {
        {(a >> 21) & 1, (a >> 22) & 7}, {(b >> 0) & 1, (b >> 1) & 7},   {(b >> 9) & 1, (b >> 10) & 7},
        {(b >> 18) & 1, (b >> 19) & 7}, {(b >> 27) & 1, (b >> 28) & 7}, {(c >> 5) & 1, (c >> 6) & 7},
        {(c >> 14) & 1, (c >> 15) & 7}, {(c >> 23) & 1, (c >> 24) & 7},
    };
    for (int i = 0; i < 8; i++) {
        u32 m = (hi >> (2 * i)) & 3;
        if (m)
            size += m == 1 ? (tc_fields[i][0] ? 2 : 1) * k_comp_size[tc_fields[i][1]] : index_size(m);
    }
    return size;
}

// ---- register loads
static void load_cp(u32 reg, u32 v) {
    s_stats.cp++;
    switch (reg & 0xF0) {
    case 0x50: reg = CP_VCD_LO; break;
    case 0x60: reg = CP_VCD_HI; break;
    case 0x70: case 0x80: case 0x90: reg = (reg & 0xF0) | (reg & 7); break;
    default: break;
    }
    g_gx.cp[reg & 0xFF] = v;
    s_renderer->cp_write(reg & 0xFF, v);
}

bool gx_debug_frame();
void platform_backtrace(const char* why);

// NOMA_DEBUG_GX_NAN: the first NaNs written to a matrix (XF memory below 0x500, the projection)
// are reported with the host stack: the guest code that computed them (a FIFO burst runs as soon
// as it lands), or for a display list the guest address of the word (for NOMA_WATCH)
static u32 s_dl_addr;        // the display list running (guest address) ...
static const u8* s_dl_host;  // ... and our copy of it
static void check_nan(u32 addr, u32 v, const u8* from = nullptr) {
    static bool s_on = getenv("NOMA_DEBUG_GX_NAN") != nullptr;
    static int s_left = 4;
    if (!s_on || s_left <= 0 || (v & 0x7F800000) != 0x7F800000 || !(v & 0x7FFFFF))
        return;
    if (addr >= 0x500 && (addr < XF_PROJECTION || addr > XF_PROJECTION + 6))
        return;
    s_left--;
    char why[96];
    if (from && s_dl_host)
        snprintf(why, sizeof(why), "gx: NaN (%08X) written to XF %04X by the display list word at %08X:", v,
                 addr, s_dl_addr + (u32)(from - s_dl_host));
    else
        snprintf(why, sizeof(why), "gx: NaN (%08X) written to XF %04X from:", v, addr);
    platform_backtrace(why);
}

static void load_xf(u32 addr, u32 v, const u8* from = nullptr) {
    s_stats.xf++;
    check_nan(addr, v, from);
    if (addr < sizeof(g_gx.xf) / 4)
        g_gx.xf[addr] = v;
    s_renderer->xf_write(addr, v);
}

static void dump_stats() {
    static u64 last;
    u64 now = host_time_us();
    if (last && now - last < 2000000)
        return;
    last = now;
    log("gx: %u draws (%u vertices), %u bp %u cp %u xf, %u display lists, %u copies, %u finish, "
        "%u tokens, %u unknown",
        s_stats.draws, s_stats.vertices, s_stats.bp, s_stats.cp, s_stats.xf, s_stats.dl, s_stats.copies,
        s_stats.finish, s_stats.tokens, s_stats.unknown);
}

static void load_bp(u32 cmd) {
    s_stats.bp++;
    u32 reg = cmd >> 24, v = cmd & 0xFFFFFF;
    if (reg == 0xFE) {  // mask for the next write
        s_bp_mask = v;
        return;
    }
    v = (g_gx.bp[reg] & ~s_bp_mask) | (v & s_bp_mask);
    s_bp_mask = 0xFFFFFF;
    g_gx.bp[reg] = v;
    if (reg >= 0xE0 && reg < 0xE8) {  // TEV color/konst registers: low (R, A) or high (B, G) half
        s32* dst = ((v >> 23) & 1) ? g_gx.tev_konst[(reg - 0xE0) / 2] : g_gx.tev_reg[(reg - 0xE0) / 2];
        auto sext11 = [](u32 x) { return (s32)(x << 21) >> 21; };
        if (reg & 1)
            dst[2] = sext11(v & 0x7FF), dst[1] = sext11((v >> 12) & 0x7FF);
        else
            dst[0] = sext11(v & 0x7FF), dst[3] = sext11((v >> 12) & 0x7FF);
    }
    s_renderer->bp_write(reg, v);
    switch (reg) {
    case 0x45:  // draw done
        if ((v & 0xFF) == 2) {
            s_stats.finish++;
            pe_finish();
            if (s_debug)
                dump_stats();
        }
        break;
    case 0x47:  // token
        s_stats.tokens++;
        pe_token((u16)v, false);
        break;
    case 0x48:  // token with interrupt
        s_stats.tokens++;
        pe_token((u16)v, true);
        break;
    case 0x65:  // load a palette into TMEM
        gx_load_tlut(g_gx.bp[0x64], v);
        break;
    case 0x52:  // EFB copy
        s_stats.copies++;
        s_renderer->efb_copy(v);
        if (s_debug)
            dump_stats();
        break;
    default: break;
    }
}

static void load_indexed(u32 which, u32 v) {
    // the array registers 12-15 (CP 0xAC-0xAF) hold the base and stride for A-D
    u32 index = v >> 16, count = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    u32 base = g_gx.cp[CP_ARRAY_BASE + 12 + which], stride = g_gx.cp[CP_ARRAY_STRIDE + 12 + which];
    u32 src = 0x80000000 | ((base + index * stride) & 0x1FFFFFFF);
    for (u32 i = 0; i < count; i++) {
        u32 w = load32(src + 4 * i);
        check_nan(addr + i, w);
        if (addr + i < sizeof(g_gx.xf) / 4)
            g_gx.xf[addr + i] = w;
    }
    s_renderer->xf_indexed_load(which, index, src, count);
}

static void call_display_list(u32 addr, u32 size) {
    s_stats.dl++;
    if (size > 0x400000) {
        log("gx: display list at %08X has an unlikely size (%u bytes)", addr, size);
        return;
    }
    static std::vector<u8> s_list;  // (lists don't nest: CALL_DL inside one is ignored)
    s_list.resize(size);
    read_g(s_list.data(), 0x80000000 | (addr & 0x1FFFFFFF), size);
    s_dl_addr = 0x80000000 | (addr & 0x1FFFFFFF), s_dl_host = s_list.data();
    u32 done = 0;
    while (done < size) {
        u32 used = gx_decode(s_list.data() + done, size - done, true);
        if (!used)
            break;  // a truncated command at the end of the list is dropped, as on hardware
        done += used;
    }
    s_dl_host = nullptr;
}

u32 gx_decode(const u8* p, u32 n, bool in_display_list) {
    if (n < 1)
        return 0;
    u32 cmd = p[0];
    switch (cmd) {
    case 0x00:
        return 1;
    case 0x08:
        if (n < 6)
            return 0;
        load_cp(p[1], be32(p + 2));
        return 6;
    case 0x10: {
        if (n < 5)
            return 0;
        u32 h = be32(p + 1);
        u32 count = ((h >> 16) & 0xF) + 1, addr = h & 0xFFFF;
        u32 len = 5 + 4 * count;
        if (n < len)
            return 0;
        NOMA_PROF_SCOPE("(gx xf)");
        for (u32 i = 0; i < count; i++)
            load_xf(addr + i, be32(p + 5 + 4 * i), in_display_list ? p + 5 + 4 * i : nullptr);
        return len;
    }
    case 0x20: case 0x28: case 0x30: case 0x38:
        if (n < 5)
            return 0;
        load_indexed((cmd - 0x20) >> 3, be32(p + 1));
        return 5;
    case 0x40:
        if (n < 9)
            return 0;
        if (!in_display_list) {
            NOMA_PROF_SCOPE("(gx display list)");
            call_display_list(be32(p + 1), be32(p + 5));
        }
        return 9;
    case 0x44:
    case 0x48:
        return 1;
    case 0x61: {
        if (n < 5)
            return 0;
        NOMA_PROF_SCOPE("(gx bp)");
        load_bp(be32(p + 1));
        return 5;
    }
    default:
        break;
    }
    if (cmd & 0x80) {
        if (n < 3)
            return 0;
        u32 vat = cmd & 7, prim = cmd & 0xF8;
        u32 count = be16(p + 1);
        u32 len = 3 + count * gx_vertex_size(vat);
        if (n < len)
            return 0;
        s_stats.draws++;
        s_stats.vertices += count;
        if (count) {
            NOMA_PROF_SCOPE("(gx draw)");
            s_renderer->draw(prim, vat, count, p + 3);
        }
        if (s_debug)
            dump_stats();
        return len;
    }
    // an unknown command: the hardware would hang; skip the byte so a desynchronised stream
    // shows up in the statistics instead
    if (s_stats.unknown++ < 16)
        log("gx: unknown command %02X", cmd);
    return 1;
}

}  // namespace guest
