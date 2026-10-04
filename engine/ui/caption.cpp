// A caption at the top of the screen (see caption.h).
#include "engine/ui/caption.h"

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

}  // namespace

Caption::Caption(const std::u16string& title, const std::u16string& subtitle) {
    for (Line* line : {&title_, &subtitle_}) {
        line->layout = lyt::layout_new(TOP_MESSAGE_LEX);
        line->fade_in = lyt::find_anim(line->layout, "FadeIn_All");
        line->fade_out = lyt::find_anim(line->layout, "FadeOut_All");
        lyt::pose(line->layout, line->fade_in, 0);  // transparent until shown
        u32 box = lyt::find_pane(line->layout, "Text_Title");
        const std::u16string& text = line == &title_ ? title : subtitle;
        lyt::text_reserve(box, (u32)text.size() + 1);
        lyt::set_text(box, text);
        if (line == &subtitle_) {
            lyt::show(lyt::find_pane(line->layout, "Pic_Line"), false);
            lyt::move(box, 0.0f, SUBTITLE_TOP - TITLE_BOX_H / 2);
            lyt::font_size(box, SUBTITLE_FONT_W, SUBTITLE_FONT_H);
        }
    }
}

Caption::~Caption() {
    lyt::layout_delete(subtitle_.layout);
    lyt::layout_delete(title_.layout);
}

void Caption::show(int frames) {
    frame_ = 0;
    hide_at_ = frames;
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
    lyt::layout_update(title_.layout);
    lyt::layout_update(subtitle_.layout);
}

void Caption::draw() {
    if (shown_)
        lyt::layout_draw(title_.layout);
    if (sub_shown_)
        lyt::layout_draw(subtitle_.layout);
}

}  // namespace engine::ui
