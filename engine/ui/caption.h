// A caption at the top of the screen in the channel's own heading (Common/Top_Message.lex, the
// screens' titles: a line of grey text over a thin rule): a title, and under the rule a second,
// smaller line (a second Top_Message, its rule hidden, its text moved down). Each line is outlined
// in white (the line drawn white, a little to each of eight sides, under the grey one): over a 3D
// scene (the island's place names) the grey keeps standing out. The title is at the font's own
// size (30 x 36, its glyph cells: sharpest). Shown for a while: it fades in (Top_Message's
// FadeIn_All, 11 frames; the second line a few frames later), stays, and fades out (FadeOut_All).
// Its texts can change between showings (in text boxes made big enough at the start: nothing
// guest-side is made then). Its guest objects are made with it and deleted with it.
#pragma once

#include <string>
#include <vector>

#include "runtime/runtime.h"

namespace engine::ui {

class Caption {
public:
    // chars: room for texts this long (at least the first ones'), for set_text
    Caption(const std::u16string& title, const std::u16string& subtitle, u32 chars = 0);
    ~Caption();
    Caption(const Caption&) = delete;
    Caption& operator=(const Caption&) = delete;

    // new texts (cut to the room made for them), shown from now on; an empty subtitle: none
    void set_text(const std::u16string& title, const std::u16string& subtitle);
    // fade in now, out after `frames` (60 a second) from now, or when hidden (UNTIL_HIDDEN)
    static constexpr int UNTIL_HIDDEN = -1;
    void show(int frames);
    // fade out now (if shown)
    void hide();
    // once a frame
    void update();
    void draw();
    // the whole caption moved by x, y layout units from the screens' heading (a screen region whose
    // top or middle isn't the layouts': engine/ui/screen.h)
    void place(f32 x, f32 y);

private:
    // a Top_Message drawn for a line: the grey one, or one of its white outline's, moved by dx, dy
    struct Copy {
        u32 layout = 0, fade_in = 0, fade_out = 0;
        f32 dx = 0, dy = 0;
        u32 white_box = 0, white_rule = 0;  // an outline copy's text box and rule, kept white
    };
    struct Line {
        std::vector<Copy> copies;  // the outline's, then the grey line
    };
    Copy make_copy(const std::u16string& text, bool subtitle, f32 dx, f32 dy, bool white) const;
    static void whiten(const Copy& c);
    static void play(const Line& line, bool in);
    static bool animating(const Line& line);
    Line title_, subtitle_;
    u32 chars_ = 0;  // the text boxes' room, the NUL included
    int frame_ = -1;  // frames since show() (-1: hidden)
    int hide_at_ = 0;
    bool shown_ = false, sub_shown_ = false, hiding_ = false;
};

}  // namespace engine::ui
