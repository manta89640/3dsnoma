// Pictures made at run time for the Sound Player's 2D parts, in the channel's own texture style:
// a coverage image drawn on the host, the two-tone IA4 textures of the room bar made from it, text
// set from the channel's font, and the guest side (the texels in guest memory, a layout picture's
// texture map). Header-only: the bar button (bar_button.cpp) uses it.
#pragma once

#include "engine/soundplayer/lyt.h"
#include "noma_decomp.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine::soundplayer::art {

// How much of each pixel a shape covers, 0 to 1, rows from the top.
struct Coverage {
    u32 w = 0, h = 0;
    std::vector<f32> c;

    Coverage(u32 width, u32 height) : w(width), h(height), c((size_t)width * height, 0.0f) {}
    f32& at(u32 x, u32 y) { return c[(size_t)y * w + x]; }
    f32 at(u32 x, u32 y) const { return c[(size_t)y * w + x]; }
    void clear() { std::fill(c.begin(), c.end(), 0.0f); }
};

// ---- the two-tone style of the room bar's icons and labels (Move_Btn_Icon_*.tpl,
// Move_Btn_Text_*.tpl): IA4 textures (a byte a texel: alpha in the high nibble, intensity in the
// low one) holding a shape at intensity 0 over an outline at intensity 15. Their materials turn
// intensity 0 into TEV colour 0 and 15 into colour 1, and the buttons' animations change those
// colours (at rest a dark grey shape with a white outline, focused a white one with a cyan
// outline), so a texture in the same two tones looks and changes as the others do.
// The outline is the shape grown by a radius, its rim anti-aliased, at an opacity. Measured on the
// channel's textures (fitting this model to them): icons grow by 1.5 to 1.9 px at full opacity,
// labels by 1.5 px at 0.8 to 0.9.

// the bytes of a w x h IA4 texture: GX's 8x4 texel tiles, 32 bytes each
inline u32 ia4_size(u32 w, u32 h) { return ((w + 7) / 8) * ((h + 3) / 4) * 32; }

inline std::vector<u8> two_tone_ia4(const Coverage& shape, f32 radius, f32 opacity) {
    const u32 w = shape.w, h = shape.h;
    // the outline: the shape grown by a disc whose rim is half a pixel wide
    Coverage outline(w, h);
    const s32 reach = (s32)std::ceil(radius + 0.5f);
    for (s32 dy = -reach; dy <= reach; dy++)
        for (s32 dx = -reach; dx <= reach; dx++) {
            f32 weight = std::min(1.0f, radius + 0.5f - std::sqrt((f32)(dx * dx + dy * dy)));
            if (weight <= 0.0f)
                continue;
            for (u32 y = 0; y < h; y++) {
                s32 sy = (s32)y + dy;
                if (sy < 0 || sy >= (s32)h)
                    continue;
                for (u32 x = 0; x < w; x++) {
                    s32 sx = (s32)x + dx;
                    if (sx >= 0 && sx < (s32)w)
                        outline.at(x, y) = std::max(outline.at(x, y), shape.at((u32)sx, (u32)sy) * weight);
                }
            }
        }
    // the shape over its outline, in texels
    std::vector<u8> texels(ia4_size(w, h), 0);
    const u32 tiles_across = (w + 7) / 8;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            f32 body = shape.at(x, y), rim = outline.at(x, y) * opacity;
            f32 alpha = body + (1.0f - body) * rim;
            if (alpha <= 0.0f)
                continue;
            f32 intensity = (1.0f - body) * rim / alpha;
            u32 a = (u32)std::lround(alpha * 15.0f), i = (u32)std::lround(intensity * 15.0f);
            texels[((y / 4) * tiles_across + x / 8) * 32 + (y % 4) * 8 + x % 8] = (u8)(a << 4 | i);
        }
    return texels;
}

// ---- text in the channel's font

// The channel's font, wbf1.brfna (the Wii's system font, which every text box of the channel's
// layouts uses): an nw4r::ut::ArchiveFont that the font manager (lbl_80882E60) holds at +0x20.
// fn_800DE11C (the manager's load) reads wbf1.brfna from the channel's content, builds the font
// over it with all its glyph sheets unpacked (fn_8048B020, ArchiveFont::Construct), registers it
// for the layouts under that name and sets +0x174. 0 while it isn't loaded.
inline u32 system_font() {
    u32 manager = guest::load32(GA_lbl_80882E60);
    if (!manager || !guest::load8(manager + 0x174))
        return 0;
    return manager + 0x20;
}

// A character's glyph, from the font's nw4r::ut::Font::GetGlyph (its virtual at +0x50; the
// ArchiveFont's is fn_8048B0F0, through ArchiveFontBase::GetGlyphFromIndex fn_8048B140), which fills
// a Glyph: +0x0 the glyph sheet (a texture), +0x4 the ink's offset from the pen (s8), +0x5 the
// ink's width, +0x6 the advance (s8), +0x7 the cell's height, +0x8 the sheet's GX format, +0xC the
// sheet's width (u16), +0x10/+0x12 the cell's place on the sheet (u16). A character the font
// doesn't have gets its alternative character's glyph.
struct Glyph {
    u32 sheet = 0, format = 0, sheet_width = 0;
    u32 cell_x = 0, cell_y = 0, cell_h = 0;
    s32 left = 0, advance = 0;
    u32 width = 0;

    // a texel of the glyph, 0 to 1 (the system font's sheets are I4: 8x8 tiles, two texels a
    // byte, the first in the high nibble); outside the ink 0
    f32 texel(u32 x, u32 y) const {
        if (x >= width || y >= cell_h)
            return 0.0f;
        u32 sx = cell_x + x, sy = cell_y + y;
        u32 at = sheet + ((sy / 8) * ((sheet_width + 7) / 8) + sx / 8) * 32 + (sy % 8) * 4 + (sx % 8) / 2;
        u8 b = guest::load8(at);
        return (f32)((sx & 1) ? b & 0xF : b >> 4) / 15.0f;
    }
};

const u32 GX_TF_I4 = 0, GX_TF_IA4 = 2;

inline Glyph glyph(u32 font, u32 code) {
    GFRAME(0x20);
    u32 g = (u32)&GS(u8, 0x8);
    gcall<u32>(guest::load32(guest::load32(font) + 0x50), font, g, code);
    Glyph r;
    r.sheet = guest::load32(g);
    r.left = (s8)guest::load8(g + 0x4);
    r.width = guest::load8(g + 0x5);
    r.advance = (s8)guest::load8(g + 0x6);
    r.cell_h = guest::load8(g + 0x7);
    r.format = guest::load32(g + 0x8);
    r.sheet_width = guest::load16(g + 0xC);
    r.cell_x = guest::load16(g + 0x10);
    r.cell_y = guest::load16(g + 0x12);
    return r;
}

// the font's baseline, from the top of its cells (nw4r::ut::FontTextureGlyph's baselinePos: the
// font's FontInformation at +0x14, its glyph sheet header at +0x8 of that, the baseline at +0x2)
inline s32 baseline(u32 font) { return (s8)guest::load8(guest::load32(guest::load32(font + 0x14) + 0x8) + 0x2); }

// How text is set: glyphs scaled by `scale` (font pixels to texture pixels), their pens `advance`
// pixels apart, their ink made heavier by `weight` (a factor on its coverage: text this small
// from a bitmap font comes out lighter than the same font drawn at its size).
struct TextStyle {
    f32 scale, advance, weight;
};

// the ink's extent of a line, from its first pen: [left, right)
inline void measure_text(u32 font, const u16* text, u32 n, const TextStyle& style, f32& left, f32& right) {
    left = right = 0.0f;
    for (u32 i = 0; i < n; i++) {
        Glyph g = glyph(font, text[i]);
        f32 x0 = (f32)i * style.advance + (f32)g.left * style.scale, x1 = x0 + (f32)g.width * style.scale;
        left = i ? std::min(left, x0) : x0;
        right = i ? std::max(right, x1) : x1;
    }
}

namespace detail {

// the pixels a texel covers along one axis once scaled (scale <= 1: one or two pixels): the first
// one and how much of each
struct Span {
    s32 first;
    f32 w0, w1;
};
inline Span span(f32 at, f32 size) {
    s32 first = (s32)std::floor(at);
    f32 edge = (f32)(first + 1);
    if (at + size <= edge)
        return {first, size, 0.0f};
    return {first, edge - at, at + size - edge};
}

// a glyph box-filtered onto img (added): every texel spread over the pixels it covers, its cell's
// top left at (x, y)
inline void draw_glyph(Coverage& img, const Glyph& g, f32 x, f32 y, f32 scale) {
    std::vector<Span> cols(g.width), rows(g.cell_h);
    for (u32 i = 0; i < g.width; i++)
        cols[i] = span(x + (f32)i * scale, scale);
    for (u32 j = 0; j < g.cell_h; j++)
        rows[j] = span(y + (f32)j * scale, scale);
    auto add = [&img](s32 px, s32 py, f32 v) {
        if (px >= 0 && py >= 0 && px < (s32)img.w && py < (s32)img.h)
            img.at((u32)px, (u32)py) += v;
    };
    for (u32 j = 0; j < g.cell_h; j++)
        for (u32 i = 0; i < g.width; i++) {
            f32 t = g.texel(i, j);
            if (t <= 0.0f)
                continue;
            const Span &c = cols[i], &r = rows[j];
            add(c.first, r.first, t * c.w0 * r.w0);
            if (c.w1 > 0.0f)
                add(c.first + 1, r.first, t * c.w1 * r.w0);
            if (r.w1 > 0.0f) {
                add(c.first, r.first + 1, t * c.w0 * r.w1);
                if (c.w1 > 0.0f)
                    add(c.first + 1, r.first + 1, t * c.w1 * r.w1);
            }
        }
}

// how blurred a glyph comes out: the coverage that is neither ink nor background
inline f32 blur(const Coverage& img, f32 weight) {
    f32 sum = 0.0f;
    for (f32 v : img.c) {
        f32 c = std::min(1.0f, v * weight);
        sum += c * (1.0f - c);
    }
    return sum;
}

}  // namespace detail

// A line of text onto img, the first pen at x and the baseline at y. A bitmap font scaled down
// blurs where its strokes fall between pixels: each glyph moves by up to half a pixel (in quarter
// pixels, the nearest of the sharpest places), as a font's hinting would place it. Only the
// system font's I4 sheets are read: other glyphs are left out (false).
inline bool draw_text(Coverage& img, u32 font, const u16* text, u32 n, f32 x, f32 y, const TextStyle& style) {
    bool all = true;
    const f32 top = y - (f32)baseline(font) * style.scale;
    Coverage sum(img.w, img.h), trial(img.w, img.h), best(img.w, img.h);
    for (u32 i = 0; i < n; i++) {
        Glyph g = glyph(font, text[i]);
        if (!g.sheet || g.format != GX_TF_I4) {
            all = false;
            continue;
        }
        const f32 gx = x + (f32)i * style.advance + (f32)g.left * style.scale;
        f32 best_score = 0.0f;
        for (s32 qy = -2; qy < 2; qy++)
            for (s32 qx = -2; qx < 2; qx++) {
                trial.clear();
                detail::draw_glyph(trial, g, gx + (f32)qx * 0.25f, top + (f32)qy * 0.25f, style.scale);
                f32 score = detail::blur(trial, style.weight) + 0.0025f * (f32)(std::abs(qx) + std::abs(qy));
                if ((qx == -2 && qy == -2) || score < best_score) {
                    best_score = score;
                    std::swap(best.c, trial.c);
                }
            }
        for (size_t k = 0; k < sum.c.size(); k++)
            sum.c[k] += best.c[k];
    }
    for (size_t k = 0; k < img.c.size(); k++)
        img.c[k] = std::min(1.0f, img.c[k] + sum.c[k] * style.weight);
    return all;
}

// ---- the guest side

// texels into guest memory, flushed (DCFlushRange: GX's texture caches go by it)
inline void write_texels(u32 at, const std::vector<u8>& texels) {
    guest::write_g(at, texels.data(), (u32)texels.size());
    g::DCFlushRange(at, (u32)texels.size());
}

// a picture's first texture map: its material's +0x40 points at its texture maps (TexMap[0] first)
inline u32 pane_texmap(u32 pane) { return guest::load32(lyt::material_of(pane) + 0x40); }

// nw4r::lyt::TexMap (0x1C bytes; what nw4r_lyt_TexMap_Set fills from a TPL): +0x0 the texels,
// +0x4 the palette, +0x8/+0xA width and height (u16), +0x16 the palette's entries, +0x18 the format
// in bits 31..28 (the palette's format in 14..13, the wrap and filter modes below)
inline void set_texmap(u32 texmap, u32 texels, u32 w, u32 h, u32 format) {
    guest::store32(texmap + 0x0, texels);
    guest::store32(texmap + 0x4, 0);
    guest::store16(texmap + 0x8, (u16)w);
    guest::store16(texmap + 0xA, (u16)h);
    guest::store16(texmap + 0x16, 0);
    guest::store32(texmap + 0x18, (guest::load32(texmap + 0x18) & 0x0FFF9FFFu) | format << 28);
}

}  // namespace engine::soundplayer::art
