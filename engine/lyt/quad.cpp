// nw4r::lyt's quads (lyt_drawInfo/lyt_common: detail::DrawQuad), native: every pane, picture and
// window part of a layout is one GX quad of four 2D vertices, each with its color (when the pane
// has vertex colors) and up to eight texture coordinate pairs. The decompiled code loaded each
// float through an out-of-line accessor and polled in every loop; here the vertices go into the
// write-gather buffer straight from guest memory. The FIFO bytes are the original's
// (tools/armbench/armbench.py --lyt runs them against the decompiled functions on random input).
#include "noma_decomp.h"

namespace engine::lyt {

namespace {

inline f32 loadf(u32 a) {
    u32 b = guest::load32(a);
    f32 f;
    ::memcpy(&f, &b, 4);
    return f;
}
inline void putf(f32 f) {
    u32 b;
    ::memcpy(&b, &f, 4);
    guest::gather_put<4>(b);
}

// one vertex: position, color (colors: 4 RGBA words, or none), texture coordinate pair `corner`
// (0 top left, 8 top right, 0x10 bottom left, 0x18 bottom right) of each of n sets
inline void vertex(f32 x, f32 y, const u32* colors, u32 color, u32 tc, s32 n, u32 corner) {
    putf(x), putf(y);
    if (colors)
        guest::gather_put<4>(colors[color]);
    for (s32 k = 0; k < n; k++, tc += 0x20)
        guest::gather_put<4>(guest::load32(tc + corner)), guest::gather_put<4>(guest::load32(tc + corner + 4));
}

}  // namespace

// DrawQuad(basePt, size, texCoordNum, texCoords, vtxColors): the quad's corners from the top left,
// clockwise
u32 draw_quad(u32 base, u32 size, u32 n_tex, u32 tex_coords, const u32* colors) {
    g::GXBegin(0x80, 0, 4);
    const s32 n = (s32)n_tex;
    vertex(loadf(base), loadf(base + 4), colors, 0, tex_coords, n, 0x0);
    vertex(loadf(base) + loadf(size), loadf(base + 4), colors, 1, tex_coords, n, 0x8);
    vertex(loadf(base) + loadf(size), (f32)(loadf(base + 4) - loadf(size + 4)), colors, 3, tex_coords, n, 0x18);
    vertex(loadf(base), (f32)(loadf(base + 4) - loadf(size + 4)), colors, 2, tex_coords, n, 0x10);
    return 0xCC010000;  // (r3 as the original leaves it)
}

}  // namespace engine::lyt

// ---- the guest ABI (decl.h)
namespace g {

u32 nw4r_lyt_detail_DrawQuad(u32 base, u32 size, u32 n_tex, u32 tex_coords, u32 colors) {
    u32 c[4];
    if (colors)
        for (u32 i = 0; i < 4; i++)
            c[i] = guest::load32(colors + 4 * i);
    return engine::lyt::draw_quad(base, size, n_tex, tex_coords, colors ? c : nullptr);
}

// DrawQuad(basePt, size, texCoordNum, texCoords, vtxColors, alpha): the colors' alpha times alpha / 255
// (rounded down, as the original's multiply by 0x80808081)
u32 nw4r_lyt_detail_DrawQuad_Alpha(u32 base, u32 size, u32 n_tex, u32 tex_coords, u32 colors, u32 alpha) {
    u32 c[4];
    if (colors)
        for (u32 i = 0; i < 4; i++) {
            u32 v = guest::load32(colors + 4 * i);
            if (alpha != 0xFF) {
                s32 t = (s32)((v & 0xFF) * alpha);
                s32 q = (s32)(((s64)(s32)0x80808081 * t) >> 32) + t;
                q >>= 7;
                v = (v & ~0xFFu) | (u8)(q + (s32)((u32)q >> 31));
            }
            c[i] = v;
        }
    return engine::lyt::draw_quad(base, size, n_tex, tex_coords, colors ? c : nullptr);
}

}  // namespace g
