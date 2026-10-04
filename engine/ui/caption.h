// A caption at the top of the screen in the channel's own heading (Common/Top_Message.lex, the
// screens' titles: a line of grey text over a thin rule): a title, and under the rule a second,
// smaller line (a second Top_Message, its rule hidden, its text moved down). Shown for a while: it
// fades in (Top_Message's FadeIn_All, 11 frames; the second line a few frames later), stays, and
// fades out (FadeOut_All). Its guest objects are made with it and deleted with it.
#pragma once

#include <string>

#include "runtime/runtime.h"

namespace engine::ui {

class Caption {
public:
    Caption(const std::u16string& title, const std::u16string& subtitle);
    ~Caption();
    Caption(const Caption&) = delete;
    Caption& operator=(const Caption&) = delete;

    // fade in now, out after `frames` (60 a second) from now
    void show(int frames);
    // fade out now (if shown)
    void hide();
    // once a frame
    void update();
    void draw();

private:
    struct Line {
        u32 layout = 0, fade_in = 0, fade_out = 0;
    };
    Line title_, subtitle_;
    int frame_ = -1;  // frames since show() (-1: hidden)
    int hide_at_ = 0;
    bool shown_ = false, sub_shown_ = false, hiding_ = false;
};

}  // namespace engine::ui
