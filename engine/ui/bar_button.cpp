// The room bar's buttons the port adds: their icons and labels (see bar_button.h).
#include "engine/ui/bar_button.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/system/host_scene.h"
#include "engine/ui/lyt.h"

namespace engine::ui {

namespace {

using art::Coverage;

// the icon's pane (Pic_Icon, 45x45 in the bar's layouts but the TV programmes' 46x46): 45, the
// texture's 50 texels at 0.9 a pixel, as the bar's own icons, so the outlines weigh the same
const f32 ICON_PANE_SIZE = 45.0f;
const f32 ICON_OUTLINE = 1.7f;

// The bar's labels are 20 px high and 18 px a character wide (いろんな間 90, ホームシアター 126),
// drawn 1:1 (Pic_Text), outlined 1.5 px at 0.8 to 0.9. Fitting the system font's glyphs to them:
// the font's 29 px em at 19 px, the characters set a pixel tighter at 18 px, the baseline 17 px
// from the top, the ink a fifth heavier (the labels look drawn from an outline font; the system
// font is a bitmap one, which comes out lighter scaled down).
const u32 LABEL_HEIGHT = 20;
const f32 LABEL_EM = 19.0f, FONT_EM = 29.0f, LABEL_ADVANCE = 18.0f, LABEL_BASELINE = 17.0f, LABEL_WEIGHT = 1.2f;
const f32 LABEL_MARGIN = 2.0f;  // the outline's room at each end
const f32 LABEL_OUTLINE = 1.5f, LABEL_OUTLINE_OPACITY = 0.85f;
// the widest label the block has room for
const u32 LABEL_MAX_W = 128;

}  // namespace

// the label's coverage, its width that of the bar's labels of as many characters (wider if its ink
// and outline need it), the ink centred in it (font must be non-zero: dress checks)
Coverage BarButtonArt::draw_label(u32 font) const {
    const art::TextStyle style = {LABEL_EM / FONT_EM, LABEL_ADVANCE, LABEL_WEIGHT};
    const u16* text = label_.data();
    const u32 n = (u32)label_.size();
    f32 left, right;
    art::measure_text(font, text, n, style, left, right);
    u32 w = std::max((u32)(LABEL_ADVANCE * (f32)n), (u32)std::ceil(right - left + 2.0f * LABEL_MARGIN));
    Coverage label(w, LABEL_HEIGHT);
    f32 x = ((f32)w - (right - left)) / 2.0f - left;
    if (!art::draw_text(label, font, text, n, x, LABEL_BASELINE, style))
        guest::log("%s: bar button: the system font has no I4 glyph for part of its label", log_name_);
    return label;
}

void BarButtonArt::reserve() {
    texels_ = engine::system::lasting_alloc(
        art::ia4_size(BAR_ICON_SIZE, BAR_ICON_SIZE) + art::ia4_size(LABEL_MAX_W, LABEL_HEIGHT), 32);
}

void BarButtonArt::dress(u32 button) {
    // the panes of the button's own layout (lyt::find_pane: LayoutWrapper::FindPane, recursive)
    u32 layout = lyt::button_layout(button);
    u32 icon_pane = lyt::find_pane(layout, "Pic_Icon"), label_pane = lyt::find_pane(layout, "Pic_Text");
    u32 font = art::system_font();
    if (!texels_ || !icon_pane || !label_pane || !font) {
        guest::log("%s: bar button %08X: no textures, icon or label pane, or font (%08X %08X %08X %08X)", log_name_,
                   (unsigned)button, (unsigned)texels_, (unsigned)icon_pane, (unsigned)label_pane, (unsigned)font);
        return;
    }
    // the label's texels follow the icon's (whole 32-byte tiles: they stay aligned)
    u32 label_texels = texels_ + art::ia4_size(BAR_ICON_SIZE, BAR_ICON_SIZE);
    if (!label_w_) {
        Coverage label_shape = draw_label(font);
        if (label_shape.w > LABEL_MAX_W) {
            guest::log("%s: bar button: its label is %u pixels wide, more than %u", log_name_, (unsigned)label_shape.w,
                       (unsigned)LABEL_MAX_W);
            return;
        }
        art::write_texels(texels_, art::two_tone_ia4(icon_(), ICON_OUTLINE, 1.0f));
        art::write_texels(label_texels, art::two_tone_ia4(label_shape, LABEL_OUTLINE, LABEL_OUTLINE_OPACITY));
        label_w_ = label_shape.w;
        guest::log("%s: bar button: icon %ux%u, label %ux%u (texels at %08X)", log_name_, (unsigned)BAR_ICON_SIZE,
                   (unsigned)BAR_ICON_SIZE, (unsigned)label_w_, (unsigned)LABEL_HEIGHT, (unsigned)texels_);
    }
    art::set_texmap(art::pane_texmap(icon_pane), texels_, BAR_ICON_SIZE, BAR_ICON_SIZE, art::GX_TF_IA4);
    lyt::resize(icon_pane, ICON_PANE_SIZE, ICON_PANE_SIZE);
    art::set_texmap(art::pane_texmap(label_pane), label_texels, label_w_, LABEL_HEIGHT, art::GX_TF_IA4);
    lyt::resize(label_pane, (f32)label_w_, (f32)LABEL_HEIGHT);
}

}  // namespace engine::ui
