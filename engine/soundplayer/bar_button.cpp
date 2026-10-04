// The Sound Player's button on the room's bottom bar. runtime/patches/menus.cpp makes it as the bar
// makes its others, from いろんな間's layout (Move_Btn_Ironna_Btn.lex): the same face, frame,
// animations and sounds. Here it gets its own icon and label, in the bar's two-tone style
// (art.h): a pair of beamed eighth notes drawn here, and サウンド set from the channel's font,
// both measured against the bar's own textures (Move_Btn_Icon_*.tpl, Move_Btn_Text_*.tpl).
// The textures are drawn once, the first time the room is built, into a block reserved at the
// start (the room is built again every time it comes back; see below), and never written again.
#include "engine/soundplayer/sound_player.h"

#include "engine/soundplayer/art.h"
#include "engine/system/host_scene.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine::soundplayer {

namespace {

using art::Coverage;

// ---- the button's panes

// layout::Button: +0x14 its layout::Layout, whose +0xC is the nw4r::lyt::Layout (+0x10 its root
// pane). A pane of it by name, or 0: nw4r::lyt::Pane::FindPaneByName on the root pane (its virtual
// at +0x3C, fn_80458C20; recursive), the name on the guest stack.
u32 find_pane(u32 button, const char* name) {
    GFRAME(0x20);
    u32 s = (u32)&GS(u8, 0x8);
    for (u32 i = 0; i < 0x17; i++) {
        guest::store8(s + i, (u8)name[i]);
        if (!name[i])
            break;
    }
    u32 root = guest::load32(guest::load32(button + 0x14 + 0xC) + 0x10);
    return gcall<u32>(guest::load32(guest::load32(root) + 0x3C), root, s, 1u);
}

// ---- the icon

// The bar's icons are 50x50 textures (いろんな間's 50x48) drawn at 45x45 (Pic_Icon), a chunky
// silhouette reaching to about 2 px from the texture's edges, outlined 1.5 to 1.9 px. The notes
// below are as heavy: strokes of 5 to 6 px, the ink within 3 px of the edges.
const u32 ICON_SIZE = 50;
const f32 ICON_PANE_SIZE = 45.0f;
const f32 ICON_OUTLINE = 1.7f;

// A pair of eighth notes joined by a beam (♫): two note heads (ellipses tilted up to the right),
// their stems up from the heads' right sides, and the beam across the stems' tops, rising to the
// right. Sampled 4x4 times a pixel.
Coverage draw_icon() {
    struct Ellipse {
        f32 cx, cy, rx, ry, cos_a, sin_a;
        bool inside(f32 x, f32 y) const {
            f32 dx = x - cx, dy = y - cy;
            f32 u = (dx * cos_a + dy * sin_a) / rx, v = (dy * cos_a - dx * sin_a) / ry;
            return u * u + v * v <= 1.0f;
        }
    };
    const f32 tilt = -22.0f * 3.14159265f / 180.0f;
    const f32 head_rx = 9.6f, head_ry = 7.2f, stem = 5.5f, beam = 9.5f;
    const Ellipse heads[2] = {{12.0f, 39.0f, head_rx, head_ry, std::cos(tilt), std::sin(tilt)},
                              {37.5f, 35.0f, head_rx, head_ry, std::cos(tilt), std::sin(tilt)}};
    // the stems' right edges, just inside the heads' right ends
    const f32 stem_right[2] = {heads[0].cx + head_rx * 0.93f, heads[1].cx + head_rx * 0.93f};
    // the beam's top edge, from the left stem's left edge to the right stem's right edge
    const f32 beam_x0 = stem_right[0] - stem, beam_x1 = stem_right[1], beam_y0 = 6.5f, beam_y1 = 2.5f;
    auto beam_top = [&](f32 x) { return beam_y0 + (beam_y1 - beam_y0) * (x - beam_x0) / (beam_x1 - beam_x0); };
    auto inside = [&](f32 x, f32 y) {
        if (heads[0].inside(x, y) || heads[1].inside(x, y))
            return true;
        if (x < beam_x0 || x > beam_x1 || y < beam_top(x))
            return false;
        if (y <= beam_top(x) + beam)
            return true;
        for (u32 i = 0; i < 2; i++)
            if (x >= stem_right[i] - stem && x <= stem_right[i] && y <= heads[i].cy)
                return true;
        return false;
    };
    const u32 SAMPLES = 4;
    Coverage icon(ICON_SIZE, ICON_SIZE);
    for (u32 y = 0; y < ICON_SIZE; y++)
        for (u32 x = 0; x < ICON_SIZE; x++) {
            u32 n = 0;
            for (u32 sy = 0; sy < SAMPLES; sy++)
                for (u32 sx = 0; sx < SAMPLES; sx++)
                    n += inside((f32)x + ((f32)sx + 0.5f) / SAMPLES, (f32)y + ((f32)sy + 0.5f) / SAMPLES);
            icon.at(x, y) = (f32)n / (SAMPLES * SAMPLES);
        }
    return icon;
}

// ---- the label

// The bar's labels are 20 px high and 18 px a character wide (いろんな間 90, ホームシアター 126),
// drawn 1:1 (Pic_Text), outlined 1.5 px at 0.8 to 0.9. Fitting the system font's glyphs to them:
// the font's 29 px em at 19 px, the characters set a pixel tighter at 18 px, the baseline 17 px
// from the top, the ink a fifth heavier (the labels look drawn from an outline font; the system
// font is a bitmap one, which comes out lighter scaled down).
const u16 LABEL[] = {0x30B5, 0x30A6, 0x30F3, 0x30C9};  // サウンド
const u32 LABEL_LENGTH = sizeof(LABEL) / sizeof(LABEL[0]);
const u32 LABEL_HEIGHT = 20;
const f32 LABEL_EM = 19.0f, FONT_EM = 29.0f, LABEL_ADVANCE = 18.0f, LABEL_BASELINE = 17.0f, LABEL_WEIGHT = 1.2f;
const f32 LABEL_MARGIN = 2.0f;  // the outline's room at each end
const f32 LABEL_OUTLINE = 1.5f, LABEL_OUTLINE_OPACITY = 0.85f;

// the label's coverage, its width that of the bar's labels of as many characters (wider if its ink
// and outline need it), the ink centred in it; empty if the font isn't there
Coverage draw_label(u32 font) {
    const art::TextStyle style = {LABEL_EM / FONT_EM, LABEL_ADVANCE, LABEL_WEIGHT};
    f32 left, right;
    art::measure_text(font, LABEL, LABEL_LENGTH, style, left, right);
    u32 w = std::max((u32)(LABEL_ADVANCE * LABEL_LENGTH), (u32)std::ceil(right - left + 2.0f * LABEL_MARGIN));
    Coverage label(w, LABEL_HEIGHT);
    f32 x = ((f32)w - (right - left)) / 2.0f - left;
    if (!art::draw_text(label, font, LABEL, LABEL_LENGTH, x, LABEL_BASELINE, style))
        guest::log("soundplayer: bar button: the system font has no I4 glyph for part of its label");
    return label;
}

// The textures' block: made at the start (the room is made again every time it comes back, and a
// block of its own each time would keep the room's memory under it from being given back: the
// heaps are stacks), drawn the first time the bar is made (the font is loaded by then), and used by
// every bar after that. The label gets the room it needs, up to LABEL_MAX_W.
const u32 LABEL_MAX_W = 128;
u32 s_texels = 0;
u32 s_label_w = 0;  // 0: not drawn yet

}  // namespace

void reserve_bar_button() {
    s_texels = engine::system::lasting_alloc(art::ia4_size(ICON_SIZE, ICON_SIZE) + art::ia4_size(LABEL_MAX_W, LABEL_HEIGHT),
                                             32);
}

void dress_bar_button(u32 button) {
    u32 icon_pane = find_pane(button, "Pic_Icon"), label_pane = find_pane(button, "Pic_Text");
    u32 font = art::system_font();
    if (!s_texels || !icon_pane || !label_pane || !font) {
        guest::log("soundplayer: bar button %08X: no textures, icon or label pane, or font (%08X %08X %08X %08X)",
                   button, s_texels, icon_pane, label_pane, font);
        return;
    }
    // the label's texels follow the icon's (whole 32-byte tiles: they stay aligned)
    u32 label_texels = s_texels + art::ia4_size(ICON_SIZE, ICON_SIZE);
    if (!s_label_w) {
        Coverage label_shape = draw_label(font);
        if (label_shape.w > LABEL_MAX_W) {
            guest::log("soundplayer: bar button: its label is %u pixels wide, more than %u", label_shape.w, LABEL_MAX_W);
            return;
        }
        art::write_texels(s_texels, art::two_tone_ia4(draw_icon(), ICON_OUTLINE, 1.0f));
        art::write_texels(label_texels, art::two_tone_ia4(label_shape, LABEL_OUTLINE, LABEL_OUTLINE_OPACITY));
        s_label_w = label_shape.w;
        guest::log("soundplayer: bar button: icon %ux%u, label %ux%u (texels at %08X)", ICON_SIZE, ICON_SIZE,
                   s_label_w, LABEL_HEIGHT, s_texels);
    }
    art::set_texmap(art::pane_texmap(icon_pane), s_texels, ICON_SIZE, ICON_SIZE, art::GX_TF_IA4);
    art::set_pane_size(icon_pane, ICON_PANE_SIZE, ICON_PANE_SIZE);
    art::set_texmap(art::pane_texmap(label_pane), label_texels, s_label_w, LABEL_HEIGHT, art::GX_TF_IA4);
    art::set_pane_size(label_pane, (f32)s_label_w, (f32)LABEL_HEIGHT);
}

}  // namespace engine::soundplayer
