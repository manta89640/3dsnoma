// A caption at the top of the screen (see caption.h).
#include "engine/ui/caption.h"

#include <algorithm>
#include <limits>

#include "engine/ui/lyt.h"

namespace engine::ui {

namespace {

const char* const TOP_MESSAGE_LEX = "layout/Common/Top_Message.lex";
const char* const WHITE_BG_LEX = "layout/Common/Common_White_Bg.lex";

// Top_Message (in layout units, y up): Text_Title's box 530 x 38 at y 169, its text hanging from
// the box's top (188) at 22.4 x 26.6; the rule (Pic_Line, 510 x 1) at y 160, just under the text.
// The second line hangs 6 under the rule (its box's top at 154), in the channel's smaller text
// (19.2 x 22.8, Progress_Update's Text_State).
const f32 TITLE_BOX_H = 38.0f, SUBTITLE_TOP = 154.0f;
const f32 SUBTITLE_FONT_W = 19.2f, SUBTITLE_FONT_H = 22.8f;
// the second line fades in this many frames after the title
const int SUBTITLE_DELAY = 8;
// the title at the font's own size (its glyph cells, 30 x 36: Top_Message has 22.4 x 26.6), its box
// grown with it about the same middle
const f32 TITLE_FONT_W = 30.0f, TITLE_FONT_H = 36.0f, TITLE_BOX_GROWN = 46.0f;
// The band (layout units, y up): across the whole screen, from over the title to under the rule
// (or under the second line), white at BAND_ALPHA of the title's own alpha
const f32 BAND_W = 640.0f, BAND_TOP = 198.0f, BAND_BOTTOM = 150.0f, BAND_BOTTOM_SUB = 124.0f;
const f32 BAND_ALPHA = 0.9f;

}  // namespace

Caption::Caption(const std::u16string& title, const std::u16string& subtitle, u32 chars) {
    chars_ = std::max<u32>(chars + 1, (u32)std::max(title.size(), subtitle.size()) + 1);
    for (Line* line : {&title_, &subtitle_}) {
        line->layout = lyt::layout_new(TOP_MESSAGE_LEX);
        line->fade_in = lyt::find_anim(line->layout, "FadeIn_All");
        line->fade_out = lyt::find_anim(line->layout, "FadeOut_All");
        lyt::pose(line->layout, line->fade_in, 0);  // transparent until shown
        u32 box = lyt::find_pane(line->layout, "Text_Title");
        const std::u16string& text = line == &title_ ? title : subtitle;
        lyt::text_reserve(box, chars_);
        lyt::set_text(box, text);
        if (line == &subtitle_) {
            lyt::show(lyt::find_pane(line->layout, "Pic_Line"), false);
            lyt::move(box, 0.0f, SUBTITLE_TOP - TITLE_BOX_H / 2);
            lyt::font_size(box, SUBTITLE_FONT_W, SUBTITLE_FONT_H);
        } else {
            lyt::font_size(box, TITLE_FONT_W, TITLE_FONT_H);
            lyt::resize(box, lyt::load_f32(box + 0x4C), TITLE_BOX_GROWN);
        }
    }
    band_ = lyt::layout_new(WHITE_BG_LEX);
    band_pic_ = lyt::find_pane(band_, "Pic_Bg");
    has_subtitle_ = !subtitle.empty();
    fit_band();
}

// the band from over the title to under the rule, or under the second line when there is one
void Caption::fit_band() {
    if (!band_pic_)
        return;
    const f32 bottom = has_subtitle_ ? BAND_BOTTOM_SUB : BAND_BOTTOM;
    lyt::resize(band_pic_, BAND_W, BAND_TOP - bottom);
    lyt::move(band_pic_, 0.0f, (BAND_TOP + bottom) / 2);
}

Caption::~Caption() {
    lyt::layout_delete(band_);
    lyt::layout_delete(subtitle_.layout);
    lyt::layout_delete(title_.layout);
}

void Caption::set_text(const std::u16string& title, const std::u16string& subtitle) {
    lyt::set_text(lyt::find_pane(title_.layout, "Text_Title"), title.substr(0, chars_ - 1));
    lyt::set_text(lyt::find_pane(subtitle_.layout, "Text_Title"), subtitle.substr(0, chars_ - 1));
    has_subtitle_ = !subtitle.empty();
    fit_band();
}

void Caption::show(int frames) {
    frame_ = 0;
    hide_at_ = frames == UNTIL_HIDDEN ? std::numeric_limits<int>::max() : frames;
    shown_ = sub_shown_ = hiding_ = false;
}

void Caption::hide() {
    if (frame_ >= 0 && !hiding_)
        hide_at_ = frame_;
}

void Caption::update() {
    if (frame_ >= 0) {
        if (frame_ == 0) {
            lyt::play(title_.layout, title_.fade_in);
            shown_ = true;
        }
        if (frame_ == SUBTITLE_DELAY && !hiding_) {
            lyt::play(subtitle_.layout, subtitle_.fade_in);
            sub_shown_ = true;
        }
        if (frame_ >= hide_at_ && !hiding_) {
            hiding_ = true;
            lyt::play(title_.layout, title_.fade_out);
            if (sub_shown_)
                lyt::play(subtitle_.layout, subtitle_.fade_out);
        }
        if (hiding_ && !lyt::animating(title_.layout) && !lyt::animating(subtitle_.layout)) {
            frame_ = -1;
            shown_ = sub_shown_ = hiding_ = false;
        } else {
            frame_++;
        }
    }
    // the band as seen as the title: its alpha (the FadeIn/FadeOut's, on the root or the text) times
    // BAND_ALPHA
    if (band_) {
        const u32 root = lyt::root_pane(title_.layout), text = lyt::find_pane(title_.layout, "Text_Title");
        const f32 a = (f32)guest::load8(root + 0xB8) * (text ? (f32)guest::load8(text + 0xB8) / 255.0f : 1.0f);
        lyt::set_alpha(lyt::root_pane(band_), (u8)(a * BAND_ALPHA));
        lyt::layout_update(band_);
    }
    lyt::layout_update(title_.layout);
    lyt::layout_update(subtitle_.layout);
}

void Caption::draw() {
    if (shown_ && band_)
        lyt::layout_draw(band_);
    if (shown_)
        lyt::layout_draw(title_.layout);
    if (sub_shown_)
        lyt::layout_draw(subtitle_.layout);
}

void Caption::place(f32 x, f32 y) {
    for (Line* line : {&title_, &subtitle_})
        lyt::move(lyt::root_pane(line->layout), x, y);
    if (band_)
        lyt::move(lyt::root_pane(band_), x, y);
}

}  // namespace engine::ui
