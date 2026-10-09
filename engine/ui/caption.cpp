// A caption at the top of the screen (see caption.h).
#include "engine/ui/caption.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/ui/lyt.h"

namespace engine::ui {

namespace {

const char* const TOP_MESSAGE_LEX = "layout/Common/Top_Message.lex";

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
// The white outline: the line drawn white this far (layout units: on the top screen about 1.3
// pixels) to each of eight sides under the grey one
const f32 OUTLINE = 2.4f;
const int OUTLINE_SIDES = 8;
// the title lifted and the second line lowered off the rule, so the three stand apart
const f32 TITLE_LIFT = 10.0f, SUBTITLE_DROP = 14.0f;

}  // namespace

// a Top_Message for one line, its text's room made, its animations found, transparent until shown
Caption::Copy Caption::make_copy(const std::u16string& text, bool subtitle, f32 dx, f32 dy, bool white) const {
    Copy c;
    c.layout = lyt::layout_new(TOP_MESSAGE_LEX);
    c.fade_in = lyt::find_anim(c.layout, "FadeIn_All");
    c.fade_out = lyt::find_anim(c.layout, "FadeOut_All");
    c.dx = dx, c.dy = dy;
    lyt::pose(c.layout, c.fade_in, 0);
    const u32 box = lyt::find_pane(c.layout, "Text_Title");
    lyt::text_reserve(box, chars_);
    lyt::set_text(box, text);
    const u32 rule = lyt::find_pane(c.layout, "Pic_Line");
    if (subtitle) {
        lyt::show(rule, false);
        lyt::move(box, 0.0f, SUBTITLE_TOP - TITLE_BOX_H / 2 - SUBTITLE_DROP);
        lyt::font_size(box, SUBTITLE_FONT_W, SUBTITLE_FONT_H);
    } else {
        lyt::font_size(box, TITLE_FONT_W, TITLE_FONT_H);
        lyt::resize(box, lyt::load_f32(box + 0x4C), TITLE_BOX_GROWN);
        lyt::move(box, lyt::load_f32(box + 0x2C), lyt::load_f32(box + 0x30) + TITLE_LIFT);
    }
    if (white) {
        // (a text box's glyphs are its material's colours times its own text colours, top and bottom:
        // all white; the materials again at every update, whitened())
        guest::store32(box + 0xDC, 0xFFFFFFFFu);
        guest::store32(box + 0xE0, 0xFFFFFFFFu);
        c.white_box = box;
        c.white_rule = subtitle ? 0 : rule;
        whiten(c);
    }
    lyt::move(lyt::root_pane(c.layout), dx, dy);
    return c;
}

// Top_Message's FadeIn_All and FadeOut_All set the text's and the rule's material colour (tev colour
// 1) to the channel's grey (80, 80, 80) at every update, fading them by the panes' alpha alone: an
// outline copy's materials are made white again after its update, its alpha the animation's
void Caption::whiten(const Copy& c) {
    if (c.white_box)
        lyt::paint(c.white_box, lyt::WHITE);
    if (c.white_rule)
        lyt::paint(c.white_rule, lyt::WHITE);
}

Caption::Caption(const std::u16string& title, const std::u16string& subtitle, u32 chars) {
    chars_ = std::max<u32>(chars + 1, (u32)std::max(title.size(), subtitle.size()) + 1);
    for (Line* line : {&title_, &subtitle_}) {
        const bool sub = line == &subtitle_;
        const std::u16string& text = sub ? subtitle : title;
        for (int k = 0; k < OUTLINE_SIDES; k++) {
            const f32 a = (f32)k * 6.2831853f / OUTLINE_SIDES;
            line->copies.push_back(make_copy(text, sub, OUTLINE * std::cos(a), OUTLINE * std::sin(a), true));
        }
        line->copies.push_back(make_copy(text, sub, 0.0f, 0.0f, false));  // (the grey line, drawn last)
    }
}

Caption::~Caption() {
    for (Line* line : {&subtitle_, &title_})
        for (auto it = line->copies.rbegin(); it != line->copies.rend(); ++it)
            lyt::layout_delete(it->layout);
}

void Caption::set_text(const std::u16string& title, const std::u16string& subtitle) {
    for (Line* line : {&title_, &subtitle_})
        for (const Copy& c : line->copies)
            lyt::set_text(lyt::find_pane(c.layout, "Text_Title"),
                          (line == &title_ ? title : subtitle).substr(0, chars_ - 1));
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

// the line's animation played on each of its copies (the same layouts: the same poses)
void Caption::play(const Line& line, bool in) {
    for (const Copy& c : line.copies)
        lyt::play(c.layout, in ? c.fade_in : c.fade_out);
}

bool Caption::animating(const Line& line) { return lyt::animating(line.copies.back().layout); }

void Caption::update() {
    if (frame_ >= 0) {
        if (frame_ == 0) {
            play(title_, true);
            shown_ = true;
        }
        if (frame_ == SUBTITLE_DELAY && !hiding_) {
            play(subtitle_, true);
            sub_shown_ = true;
        }
        if (frame_ >= hide_at_ && !hiding_) {
            hiding_ = true;
            play(title_, false);
            if (sub_shown_)
                play(subtitle_, false);
        }
        if (hiding_ && !animating(title_) && !animating(subtitle_)) {
            frame_ = -1;
            shown_ = sub_shown_ = hiding_ = false;
        } else {
            frame_++;
        }
    }
    for (Line* line : {&title_, &subtitle_})
        for (const Copy& c : line->copies) {
            lyt::layout_update(c.layout);
            if (line == &title_ ? shown_ : sub_shown_)
                whiten(c);
        }
}

void Caption::draw() {
    for (Line* line : {&title_, &subtitle_})
        if (line == &title_ ? shown_ : sub_shown_)
            for (const Copy& c : line->copies)
                lyt::layout_draw(c.layout);
}

void Caption::place(f32 x, f32 y) {
    for (Line* line : {&title_, &subtitle_})
        for (const Copy& c : line->copies)
            lyt::move(lyt::root_pane(c.layout), x + c.dx, y + c.dy);
}

}  // namespace engine::ui
