// The GPU's command stream: decoding, register state, display lists.
//
//   0x00        NOP
//   0x08 r v32  LOAD_CP_REG   vertex descriptors/formats, array bases and strides
//   0x10 h d..  LOAD_XF_REG   h = (count - 1) << 16 | address; count data words
//   0x20-0x38   LOAD_INDX_A-D XF memory from the CP arrays 12-15: index << 16 | (n - 1) << 12 | addr
//   0x40 a s    CALL_DL       execute the display list at physical a, s bytes
//   0x44, 0x48  (metrics), INVALIDATE_VTX_CACHE
//   0x50 id32   (ours)        a native draw: GxRenderer::native(id) (engine/g3d/native_draw.h)
//   0x51 s8 x16 y16 w16 h16 (ours) what follows goes onto 3DS screen s: GxRenderer::screen (gx.h)
//   0x61 v32    LOAD_BP_REG   register in the top byte, 24-bit value
//   0x80-0xBF   draw          primitive in bits 3-7, vertex format in bits 0-2; u16 count; vertices
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gx.h"
#include "regs.h"
#include "texture.h"
#include "vertex.h"
#include "engine/lyt/draw.h"

namespace guest {

static GxRenderer s_null_renderer;
static GxRenderer* s_renderer = &s_null_renderer;
void gx_set_renderer(GxRenderer* r) { s_renderer = r ? r : &s_null_renderer; }
void gx_sync();
void gx_end_frame_for_applet() {
    gx_sync();  // (the GPU's thread then leaves the renderer alone: nothing new reaches it)
    s_renderer->suspend();
}

static u32 s_bp_mask = 0xFFFFFF;

// statistics for NOMA_DEBUG_GX
struct GxStats {
    u32 draws, vertices, bp, cp, xf, dl, copies, finish, tokens, unknown, native;
};
static GxStats s_stats;
GxDecodeCounts gx_decode_counts() {
    return {s_stats.draws, s_stats.vertices, s_stats.bp, s_stats.cp, s_stats.xf, s_stats.dl, s_stats.native};
}
static bool s_debug = getenv("NOMA_DEBUG_GX") != nullptr;

static u32 be16(const u8* p) { return (u32)p[0] << 8 | p[1]; }
static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

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
}

// the part of a renderer's state each BP register is part of (GX_BP_*; 0: none)
static u8 s_bp_group[256];
static void init_bp_groups() {
    s_bp_group[0x00] = GX_BP_TEV;                     // GENMODE
    for (u32 r = 0x28; r < 0x30; r++)                 // TREF
        s_bp_group[r] = GX_BP_TEV;
    for (u32 r = 0xC0; r < 0xE8; r++)                 // TEV stages, registers
        s_bp_group[r] = GX_BP_TEV;
    for (u32 r = 0xF6; r < 0xFE; r++)                 // KSEL
        s_bp_group[r] = GX_BP_TEV;
    for (u32 r = 0x60; r <= 0x65; r++)                // TMEM preloads, palette loads
        s_bp_group[r] = GX_BP_TEX;
    for (u32 r = 0x80; r < 0xBC; r++)                 // texture modes, images, palettes
        s_bp_group[r] = GX_BP_TEX;
    s_bp_group[0x40] = s_bp_group[0x41] = s_bp_group[0x43] = GX_BP_PIXEL;  // ZMODE, CMODE0, ZCONTROL
    s_bp_group[0xF3] = GX_BP_PIXEL;                   // ALPHACOMPARE
    s_bp_group[0x20] = s_bp_group[0x21] = GX_BP_SCISSOR;
}

bool gx_debug_frame();
void platform_backtrace(const char* why);

// NOMA_DEBUG_GX_NAN: the first NaNs written to a matrix (XF memory below 0x500, the projection)
// are reported with the host stack: the guest code that computed them (a FIFO burst runs as soon
// as it lands), or for a display list the guest address of the word (for NOMA_WATCH)
static u32 s_dl_addr;        // the display list running (guest address) ...
static const u8* s_dl_host;  // ... and our copy of it
static u32 s_cur_prim;       // the draw command being executed (for the draw path's statistics)
static u32 s_dl_size, s_dl_offset;  // the list's size, the draw command's offset in it
bool gx_in_display_list() { return s_dl_host != nullptr; }
u32 gx_current_prim() { return s_cur_prim; }
u32 gx_display_list_addr() { return s_dl_addr; }
u32 gx_display_list_size() { return s_dl_size; }
u32 gx_display_list_offset() { return s_dl_offset; }
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

// XF words from the stream at p; what changed is reported once
static void load_xf(u32 addr, u32 count, const u8* p, bool in_display_list) {
    s_stats.xf += count;
    u32 lo = ~0u, hi = 0;
    for (u32 i = 0; i < count; i++, p += 4) {
        u32 a = addr + i, v = be32(p);
        check_nan(a, v, in_display_list ? p : nullptr);
        if (a < sizeof(g_gx.xf) / 4 && g_gx.xf[a] != v) {
            g_gx.xf[a] = v;
            lo = std::min(lo, a), hi = a + 1;
        }
    }
    if (lo < hi)
        gx_xf_written(g_gx, lo, hi - lo);
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
    // (a palette load or a TMEM preload changes what a texture register means: always a change)
    if (g_gx.bp[reg] != v || reg == 0x65 || reg == 0x63)
        g_gx.bp_dirty |= s_bp_group[reg];
    g_gx.bp[reg] = v;
    if (reg >= 0xE0 && reg < 0xE8) {  // TEV color/konst registers: low (R, A) or high (B, G) half
        s32* dst = ((v >> 23) & 1) ? g_gx.tev_konst[(reg - 0xE0) / 2] : g_gx.tev_reg[(reg - 0xE0) / 2];
        auto sext11 = [](u32 x) { return (s32)(x << 21) >> 21; };
        if (reg & 1)
            dst[2] = sext11(v & 0x7FF), dst[1] = sext11((v >> 12) & 0x7FF);
        else
            dst[0] = sext11(v & 0x7FF), dst[3] = sext11((v >> 12) & 0x7FF);
    }
    switch (reg) {
    case 0x45:  // draw done
        if ((v & 0xFF) == 2) {
            NOMA_PROF_SCOPE("(gx draw done)");
            s_stats.finish++;
            s_renderer->draw_done();
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
    case 0x65: {  // load a palette into TMEM
        NOMA_PROF_SCOPE("(gx tmem)");
        gx_load_tlut(g_gx.bp[0x64], v);
        break;
    }
    case 0x63:  // preload a texture into TMEM (0: GXTexModeSync, no load)
        if (v) {
            NOMA_PROF_SCOPE("(gx tmem)");
            gx_preload(g_gx.bp[0x60], g_gx.bp[0x61], g_gx.bp[0x62], v);
        }
        break;
    case 0x52: {  // EFB copy
        NOMA_PROF_SCOPE("(gx efb copy)");
        s_stats.copies++;
        s_renderer->efb_copy(v);
        if (s_debug)
            dump_stats();
        break;
    }
    default: break;
    }
}

static void load_indexed(u32 which, u32 v) {
    // the array registers 12-15 (CP 0xAC-0xAF) hold the base and stride for A-D
    u32 index = v >> 16, count = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    u32 base = g_gx.cp[CP_ARRAY_BASE + 12 + which], stride = g_gx.cp[CP_ARRAY_STRIDE + 12 + which];
    u32 src = 0x80000000 | ((base + index * stride) & 0x1FFFFFFF);
    u32 lo = ~0u, hi = 0;  // the words that changed
    for (u32 i = 0; i < count; i++) {
        u32 w = load32(src + 4 * i);
        check_nan(addr + i, w);
        if (addr + i < sizeof(g_gx.xf) / 4 && g_gx.xf[addr + i] != w) {
            g_gx.xf[addr + i] = w;
            lo = std::min(lo, addr + i), hi = addr + i + 1;
        }
    }
    if (lo < hi)
        gx_xf_written(g_gx, lo, hi - lo);
}

static void call_display_list(u32 addr, u32 size) {
    s_stats.dl++;
    if (size > 0x400000) {
        log("gx: display list at %08X has an unlikely size (%u bytes)", addr, size);
        return;
    }
    // decoded where it is when the guest's memory holds it in one piece, else from a copy
    static std::vector<u8> s_list;  // (lists don't nest: CALL_DL inside one is ignored)
    const u32 a = 0x80000000 | (addr & 0x1FFFFFFF);
    const u8* list = size ? host_ptr(a) : nullptr;
    for (u32 pg = (a >> PAGE_BITS) + 1; list && pg <= (a + size - 1) >> PAGE_BITS; pg++)
        if (host_ptr(pg << PAGE_BITS) != list + ((pg << PAGE_BITS) - a))
            list = nullptr;
    if (!list) {
        s_list.resize(size);
        read_g(s_list.data(), a, size);
        list = s_list.data();
    }
    s_dl_addr = a, s_dl_host = list, s_dl_size = size;
    // (a truncated command at the end of the list is dropped, as on hardware)
    gx_decode(list, size, true);
    s_dl_host = nullptr;
}

u32 gx_decode(const u8* p, u32 n, bool in_display_list) {
    static bool s_groups_ready;
    if (!s_groups_ready) {
        init_bp_groups();
        s_groups_ready = true;
    }
    u32 done = 0;
    while (done < n) {
        const u8* q = p + done;
        const u32 left = n - done;
        u32 cmd = q[0];
        u32 len;
        switch (cmd) {
        case 0x00:
            len = 1;
            break;
        case 0x08:
            if (left < 6)
                return done;
            load_cp(q[1], be32(q + 2));
            len = 6;
            break;
        case 0x10: {
            if (left < 5)
                return done;
            u32 h = be32(q + 1);
            u32 count = ((h >> 16) & 0xF) + 1, addr = h & 0xFFFF;
            len = 5 + 4 * count;
            if (left < len)
                return done;
            NOMA_PROF_SCOPE("(gx xf)");
            load_xf(addr, count, q + 5, in_display_list);
            break;
        }
        case 0x20: case 0x28: case 0x30: case 0x38:
            if (left < 5)
                return done;
            load_indexed((cmd - 0x20) >> 3, be32(q + 1));
            len = 5;
            break;
        case 0x40:
            if (left < 9)
                return done;
            if (in_display_list) {
                s_dl_offset = done;
            } else {
                NOMA_PROF_SCOPE("(gx display list)");
                call_display_list(be32(q + 1), be32(q + 5));
            }
            len = 9;
            break;
        case 0x44:
        case 0x48:
            len = 1;
            break;
        case 0x50:  // a native draw (ours)
            if (left < 5)
                return done;
            s_stats.native++;
            {
                NOMA_PROF_SCOPE("(gx native)");
                s_renderer->native(be32(q + 1));
            }
            len = 5;
            break;
        case 0x51:  // the 3DS screen what follows goes onto (ours)
            if (left < 10)
                return done;
            s_renderer->screen(q[1], (u32)q[2] << 8 | q[3], (u32)q[4] << 8 | q[5], (u32)q[6] << 8 | q[7],
                               (u32)q[8] << 8 | q[9]);
            len = 10;
            break;
        case 0x52:  // a layout drawn natively (ours)
            if (left < 5)
                return done;
            {
                NOMA_PROF_SCOPE("(gx lyt)");
                engine::lyt::decode(be32(q + 1), *s_renderer);
            }
            len = 5;
            break;
        case 0x61: {
            if (left < 5)
                return done;
            NOMA_PROF_SCOPE("(gx bp)");
            load_bp(be32(q + 1));
            len = 5;
            break;
        }
        default:
            if (cmd & 0x80) {
                if (left < 3)
                    return done;
                u32 vat = cmd & 7, prim = cmd & 0xF8;
                u32 count = be16(q + 1);
                len = 3 + count * gx_vertex_format(vat).size;
                if (left < len)
                    return done;
                s_stats.draws++;
                s_stats.vertices += count;
                if (count) {
                    NOMA_PROF_SCOPE("(gx draw)");
                    s_cur_prim = prim;
                    s_dl_offset = done;
                    s_renderer->draw(prim, vat, count, q + 3);
                }
                if (s_debug)
                    dump_stats();
                break;
            }
            // an unknown command: the hardware would hang; skip the byte so a desynchronised
            // stream shows up in the statistics instead
            if (s_stats.unknown++ < 16)
                log("gx: unknown command %02X", cmd);
            len = 1;
            break;
        }
        done += len;
    }
    return done;
}

}  // namespace guest
