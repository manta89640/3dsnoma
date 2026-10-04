// The GPU thread's per-draw work (runtime/hw/gx/tnl.cpp) as the 3DS build compiles it, driven under
// Unicorn by armbench.py --gx: draws like the room's, for instruction counts per draw and vertex.
//   kind 0: a layout quad (positions, color and texture coordinates in the stream, floats), a new
//           position matrix for each draw, built as an immediate draw
//   kind 1: a model's draw (8-bit indices: s16 positions, s8 normals, u16 texture coordinates; lit
//           by one spot light), built into the list cache (with its matrix rows)
//   kind 2: kind 1 drawn again from the cache: only its fixed slots (a new matrix in the same XF row)
//   kind 3: a skinned draw from the cache (a matrix index per vertex, three matrices): its slots
// tex(direct, fmt, w, h, hash) converts a w x h texture of GX format fmt (random bytes and palette) into PICA
// texture memory: decoded to RGBA8 then tiled (direct 0), or straight from the GX tiles (direct 1,
// platform/3ds/pica_tex.h), returning a hash of the result (hash 1) or nothing.
// gx(kind, draws, vertices, flush_every) runs `draws` such draws of `vertices` vertices, the pending
// batch submitted (the palette's slots freed) every flush_every draws, and returns a hash of what
// was built (to compare builds of tnl.cpp).
#include <cstdarg>
#include <cstring>

#include "platform/3ds/pica_tex.h"
#include "runtime/hw/gx/regs.h"
#include "runtime/hw/gx/texture.h"
#include "runtime/hw/gx/tnl.h"

namespace guest {
uintptr_t g_page[1u << (32 - PAGE_BITS)];
u32 g_sp;
FnSlot g_fn_cache[4096];
s32 g_loop_budget = 20000;
GxState g_gx;
void loop_poll() { g_loop_budget = 20000; }
u32 mmio_read(u32, int) { return 0; }
void mmio_write(u32, u32, int) {}
void* lookup_fn_slow(u32) { return nullptr; }
void fatal(const char*, ...) {
    for (;;) {
    }
}
void log(const char*, ...) {}
void gx_check_transform_draw(u32, u32, const u8*, const GxOutVertex*) {}
u32 mem_generation(u32, u32) { return 0; }
u32 mem_mark_written(u32, u32) { return 0; }
void read_g(void*, u32, u32) {}
}  // namespace guest

using namespace guest;

namespace {

// guest memory for the vertex arrays (0x80100000)
const u32 ARRAYS = 0x80100000;
u8 s_arrays[0x10000] __attribute__((aligned(32)));
u8 s_stream[0x4000];
GxTnlVertex s_out[0x400];
GxTnlSlots s_slots[0x400];
GxTnlRows s_rows[0x400];
GxTnlPalette s_pal;

void put_be16(u8* p, u32 v) { p[0] = (u8)(v >> 8), p[1] = (u8)v; }
void put_be32(u8* p, u32 v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }
void put_f32(u8* p, f32 f) {
    u32 b;
    memcpy(&b, &f, 4);
    put_be32(p, b);
}
void xf_set(u32 addr, u32 v) {
    g_gx.xf[addr] = v;
    gx_xf_written(g_gx, addr, 1);
}
void xf_setf(u32 addr, f32 f) {
    u32 b;
    memcpy(&b, &f, 4);
    xf_set(addr, b);
}
// a 3x4 matrix into XF rows [row, row + 3), different each time (seed)
void load_matrix(u32 row, u32 seed) {
    for (u32 i = 0; i < 12; i++)
        xf_setf(row * 4 + i, (i % 5 == 0 ? 1.0f : 0.0f) + 0.001f * (f32)((seed * 7 + i) % 97));
}

void setup(u32 kind) {
    memset(&g_gx, 0, sizeof(g_gx));
    g_page[ARRAYS >> PAGE_BITS] = (uintptr_t)s_arrays - ARRAYS;
    s_pal = GxTnlPalette();
    u32* cp = g_gx.cp;
    xf_set(XF_NUMCOLORS, 1);
    xf_set(XF_NUMTEXGENS, 1);
    xf_set(XF_TEXMTXINFO, 5u << 7);  // texgen 0: texture coordinate 0, 2x4, matrix row 0 (default)
    xf_set(XF_MATINDEX_A, 0 | 30u << 6);  // position matrix row 0, texgen 0 matrix row 30
    for (u32 i = 0; i < 8; i++)
        xf_setf(120 + i, i % 5 == 0 ? 1.0f : 0.0f);  // the texture matrix (row 30)
    xf_setf(XF_PROJECTION + 0, 1.5f), xf_setf(XF_PROJECTION + 2, 2.0f), xf_setf(XF_PROJECTION + 4, -1.0f);
    xf_setf(XF_PROJECTION + 5, -0.1f);
    if (kind == 0) {
        // position direct f32 xyz, color 0 direct RGBA8, texture coordinate 0 direct f32 st
        cp[CP_VCD_LO] = 1u << 9 | 1u << 13;
        cp[CP_VCD_HI] = 1u;
        cp[CP_VAT_A] = 1u | 4u << 1 | 5u << 14 | 1u << 21 | 4u << 22;
        xf_set(XF_COLOR0CNTRL, 1), xf_set(XF_ALPHA0CNTRL, 1);  // material from the vertex, unlit
        return;
    }
    // 8-bit indices: position s16 xyz (shift 8), normal s8, texture coordinate 0 u16 st (shift 10)
    cp[CP_VCD_LO] = 2u << 9 | 2u << 11;
    cp[CP_VCD_HI] = 2u;
    cp[CP_VAT_A] = 1u | 3u << 1 | 8u << 4 | 1u << 10 | 1u << 21 | 2u << 22 | 10u << 25;
    cp[CP_ARRAY_BASE + 0] = ARRAYS, cp[CP_ARRAY_STRIDE + 0] = 6;
    cp[CP_ARRAY_BASE + 1] = ARRAYS + 0x4000, cp[CP_ARRAY_STRIDE + 1] = 3;
    cp[CP_ARRAY_BASE + 4] = ARRAYS + 0x8000, cp[CP_ARRAY_STRIDE + 4] = 4;
    for (u32 i = 0; i < 256; i++) {
        for (u32 k = 0; k < 3; k++)
            put_be16(s_arrays + i * 6 + 2 * k, (i * 37 + k * 11) & 0x7FFF);
        for (u32 k = 0; k < 3; k++)
            s_arrays[0x4000 + i * 3 + k] = (u8)(i * 13 + k);
        put_be16(s_arrays + 0x8000 + i * 4, i * 5), put_be16(s_arrays + 0x8000 + i * 4 + 2, i * 3);
    }
    // lit: material from the register, ambient from the register, light 0, spot attenuation
    xf_set(XF_MATERIAL0, 0xFFFFFFFF), xf_set(XF_AMBIENT0, 0x404040FF);
    xf_set(XF_COLOR0CNTRL, 2u | 1u << 2 | 2u << 7 | 3u << 9);
    xf_set(XF_ALPHA0CNTRL, 0);
    for (u32 i = 4; i < 16; i++)
        xf_setf(XF_MEM_LIGHT + i, 0.25f * (f32)i);
    xf_set(XF_MEM_LIGHT + 3, 0xFFFFFFFF);
    if (kind == 3) {  // a position matrix index in each vertex, before the attributes
        cp[CP_VCD_LO] |= 1u;
    }
}

// a draw's stream: kind 0 quads with floats, else 8-bit indices
u32 make_stream(u32 kind, u32 n) {
    u8* p = s_stream;
    for (u32 i = 0; i < n; i++) {
        if (kind == 0) {
            put_f32(p, (f32)(i & 1) * 16.0f), put_f32(p + 4, (f32)(i >> 1) * 16.0f), put_f32(p + 8, 0.0f);
            p[12] = 255, p[13] = 200, p[14] = 100, p[15] = 255;
            put_f32(p + 16, (f32)(i & 1)), put_f32(p + 20, (f32)(i >> 1));
            p += 24;
        } else {
            if (kind == 3)
                *p++ = (u8)((i % 3) * 3);  // matrix rows 0, 3, 6
            p[0] = (u8)(i * 7), p[1] = (u8)(i * 5), p[2] = (u8)(i * 3);
            p += 3;
        }
    }
    return (u32)(p - s_stream);
}

u32 hash_out(u32 h, const void* p, u32 n) {
    const u8* b = (const u8*)p;
    for (u32 i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

}  // namespace

extern "C" u32 gx(u32 kind, u32 draws, u32 vertices, u32 flush_every) {
    setup(kind);
    make_stream(kind, vertices);
    GxDrawSetup su;
    su.texgen[0] = 0;
    static const f32 clip[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    u32 h = 2166136261u;
    // kinds 2 and 3 draw vertices built before: built once here
    if (kind >= 2) {
        const GxTnlPlan& plan = gx_tnl_plan(0, su, 1, clip, 1);
        gx_tnl_vertices(plan, s_stream, 0, vertices, s_pal, s_out, s_slots, s_rows);
        s_pal.flushed();
    }
    GxTnlSlots fixed = {};
    for (u32 d = 0; d < draws; d++) {
        load_matrix(0, d);
        if (kind == 3)
            load_matrix(3, d + 1), load_matrix(6, d + 2);
        if (flush_every && d % flush_every == 0)
            s_pal.flushed();
        const GxTnlPlan& plan = gx_tnl_plan(0, su, 1, clip, 1);
        u32 n;
        if (kind == 0)
            n = gx_tnl_vertices(plan, s_stream, 0, vertices, s_pal, s_out, s_slots, nullptr);
        else if (kind == 1)
            n = gx_tnl_vertices(plan, s_stream, 0, vertices, s_pal, s_out, s_slots, s_rows);
        else if (kind == 2)
            n = gx_tnl_fixed_slots(plan, s_pal, &fixed, &fixed) ? vertices : 0;
        else
            n = gx_tnl_slots(plan, s_rows, vertices, s_pal, s_slots);
        if (n < vertices) {  // (the palette was full: as the renderer, submit and go on)
            s_pal.flushed();
            continue;
        }
        h = hash_out(h, s_slots, vertices * sizeof(GxTnlSlots));
        if (kind < 2)
            h = hash_out(h, s_out, vertices * sizeof(GxTnlVertex));
    }
    return h;
}

namespace {
u8 s_texsrc[256 * 256 * 4];
u8 s_texout[256 * 256 * 4];
u32 s_rgba[256 * 256];
pica_tex::Fmt pica_format(u32 f) {
    switch (f) {
    case 0: case 2: return pica_tex::LA4;
    case 1: case 3: case 8: case 9: return pica_tex::LA8;
    case 4: return pica_tex::RGB565;
    case 14: return pica_tex::RGBA5551;
    default: return pica_tex::RGBA8;
    }
}
}  // namespace

extern "C" u32 tex(u32 direct, u32 fmt, u32 w, u32 h, u32 hash) {
    static bool filled;
    if (!filled) {
        u32 x = 12345;
        for (u8& b : s_texsrc)
            b = (u8)((x = x * 1103515245 + 12345) >> 16);
        u8* tmem = gx_tmem_for_tests();
        for (u32 i = 0; i < 0x1000; i++)
            tmem[i] = (u8)((x = x * 1103515245 + 12345) >> 16);
        filled = true;
    }
    u32 W = 8, H = 8;
    while (W < w) W <<= 1;
    while (H < h) H <<= 1;
    pica_tex::Fmt pf = pica_format(fmt);
    if (direct)
        pica_tex::from_gx(s_texsrc, fmt, w, h, gx_tmem(), GX_TMEM_MASK, 0, 0, s_texout, W, H, pf);
    else {
        gx_decode_texture_tlut(s_texsrc, fmt, w, h, 0, 0, s_rgba);
        pica_tex::from_rgba(s_rgba, w, h, s_texout, W, H, pf, false);
    }
    return hash ? hash_out(2166136261u, s_texout, W * H * pica_tex::texel_bytes(pf)) : 0;
}
