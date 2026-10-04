// The channel's layout objects as the native scenes' screens use them: its layout::LayoutWrapper
// (a .lex built into an nw4r::lyt::Layout, with its animations), layout::Button, and the nw4r::lyt
// panes, text boxes and materials inside them. The functions here are the game's own; the
// objects they make are guest objects, made while the screen is built and deleted with it.
//
// Objects and fields relied on (offsets in bytes):
//   layout::LayoutWrapper (0x128): +0x8 the locator pane it hangs from (0: a root layout),
//     +0xC its nw4r::lyt::Layout (+0x10 the root pane), +0x10 the AnimationTask playing (0: none;
//     the task's +0x10 is set while it runs, +0xF0 is its animation)
//   nw4r::lyt::AnimTransform: +0xC its RLAN data (the pai1 block), +0x10 the frame (f32)
//   nw4r::lyt::Pane: +0x0 vtable (+0x30 SetColorElement, +0x3C FindPaneByName), +0x28 material,
//     +0x2C translation (3 f32), +0x44 scale (2 f32), +0x4C size (2 f32), +0xB8 alpha,
//     +0xBB flags (1 visible, 2 the alpha applies to the children too)
//   nw4r::lyt::TextBox (a Pane): +0xD8 the UTF-16 buffer, +0xE8 font size (2 f32), +0xF4 the
//     space between characters (f32), +0xFC buffer size in bytes, +0x100 text position (H + 3 V:
//     0 left / 1 centre / 2 right, 0 top / 1 middle / 2 bottom); vtable +0x74 AllocStringBuffer
//     (min length), +0x7C SetString(string, at)
//   nw4r::lyt::Material: tev colour 0 ("black") and 1 ("white"), s16 RGBA each, which the text
//     or the texture's dark and bright parts take; set through Material::SetColorElement (index
//     4 + 4 * colour + component)
//   layout::Button (0x69C): +0x4 its exclusive group, +0xC the layout::Cursor it is registered
//     with, +0x10 u8 focused (the pointer is on it), +0x14 its ButtonLayout (a LayoutWrapper
//     first: the button's own layout), +0x4A0 its balloon (a tooltip) helper, +0x4BC / +0x4C0 the
//     labels of its focus and push sounds (pointers that must last), +0x5B8 active, +0x5B9 enabled
//   exclusive group (0x10): a vector of its buttons, +0xC set while one of them is being pushed
#pragma once

#include <string>

#include "engine/math/vecmath.h"
#include "engine/ui/game.h"
#include "noma_decomp.h"
#include "runtime/runtime.h"

namespace engine::ui::lyt {

using engine::math::load_f32;
using engine::math::store_f32;

// ---- strings for the game's functions, on the guest stack for the length of one call:
// f(Name("Focus")) (the temporary lasts until the call returns)

class Name {
public:
    explicit Name(const char* s) : frame_(len(s) + 1) {
        for (u32 i = 0;; i++) {
            guest::store8(frame_.sp + i, (u8)s[i]);
            if (!s[i])
                break;
        }
    }
    operator u32() const { return frame_.sp; }

private:
    static u32 len(const char* s) {
        u32 n = 0;
        while (s[n])
            n++;
        return n;
    }
    GuestFrame frame_;
};

// UTF-16 text (big-endian in guest memory), NUL-terminated
class Text {
public:
    explicit Text(const std::u16string& s) : frame_(2 * (u32)s.size() + 2) {
        for (u32 i = 0; i < s.size(); i++)
            guest::store16(frame_.sp + 2 * i, (u16)s[i]);
        guest::store16(frame_.sp + 2 * (u32)s.size(), 0);
    }
    operator u32() const { return frame_.sp; }

private:
    GuestFrame frame_;
};

// ---- layouts (layout::LayoutWrapper)

const u32 LAYOUT_SIZE = 0x128;

// a layout of its own (drawn by draw())
inline u32 layout_new(const char* path) {
    u32 w = g::operator_new(LAYOUT_SIZE);
    if (!w)
        guest::fatal("ui: out of guest memory for %s", path);
    g::layout_LayoutWrapper_ctor(w, Name(path), 0);  // LayoutWrapper::LayoutWrapper(path, flags)
    return w;
}

// a layout hanging from a pane (a locator) of another one, drawn as part of it
inline u32 layout_new_in(const char* path, u32 parent, const char* locator) {
    u32 w = g::operator_new(LAYOUT_SIZE);
    if (!w)
        guest::fatal("ui: out of guest memory for %s", path);
    // LayoutWrapper::LayoutWrapper(path, parent nw4r::lyt::Layout*, locator name, flags)
    g::layout_LayoutWrapper_ctorAttached(w, Name(path), guest::load32(parent + 0xC), Name(locator), 0);
    return w;
}

// LayoutWrapper::~LayoutWrapper (it leaves its locator), then the memory
inline void layout_delete(u32 w) {
    if (w)
        g::layout_LayoutWrapper_dtor(w, 1, 0, 0, 0);
}

inline u32 root_pane(u32 w) { return guest::load32(guest::load32(w + 0xC) + 0x10); }

// LayoutWrapper::FindPane: a pane of the layout (or of one hanging from it) by name, or 0
inline u32 find_pane(u32 w, const char* name) {
    return g::layout_LayoutWrapper_FindPane(w, Name(name), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

// LayoutWrapper::FindAnim: the animation "<layout>_<name>.brlan" of the layout, or 0
inline u32 find_anim(u32 w, const char* name) { return g::layout_LayoutWrapper_FindAnim(w, Name(name)); }

// the animation's last frame (nw4r::lyt::AnimTransform::GetFrameSize, nw4r_lyt_AnimTransform_GetFrameSize, less one)
inline f32 last_frame(u32 anim) { return anim ? (f32)((u16)g::nw4r_lyt_AnimTransform_GetFrameSize(anim, 0) - 1) : 0.0f; }

// LayoutWrapper::SetAnimFrame: the layout posed at this frame of the animation (the animation
// playing stops; the pose stays)
inline void pose(u32 w, u32 anim, f32 frame) {
    if (anim)
        g::layout_LayoutWrapper_SetAnimFrame(w, anim, 1, frame);
}

// LayoutWrapper::PlayAnim (layout_LayoutWrapper_PlayAnim): the animation plays once, from frame 0 a frame at a time
// (a task of the game's runs it), then stops on its last frame. The std::tr1::function it calls
// at the end is an empty one, as the button layouts pass for their Focus. (layout_LayoutWrapper_PlayAnimLoop, which
// the button layouts use for their "Wait", plays an animation over and over.)
inline void play(u32 w, u32 anim) {
    if (!anim)
        return;
    GFRAME(0x20);
    for (u32 i = 0; i < 0x20; i += 4)
        guest::store32(_gframe.sp + i, 0);
    g::layout_LayoutWrapper_PlayAnim(w, anim, _gframe.sp, 0, 0, 0.0, 1.0);
    g::std_tr1_function_dtor(_gframe.sp, (u32)-1, 0, 0, 0);  // the function's destructor
}

// LayoutWrapper::GetPlayingAnim: an animation is playing
inline bool animating(u32 w) { return g::layout_LayoutWrapper_GetPlayingAnim(w) != 0; }

// LayoutWrapper::Update: the animations applied, the panes' matrices worked out (once a frame
// for a layout of its own, after its panes have been moved)
inline void layout_update(u32 w) { g::layout_LayoutWrapper_Update(w, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }

// LayoutWrapper::Draw (with the layouts hanging from it)
inline void layout_draw(u32 w) { g::layout_LayoutWrapper_Draw(w); }

// ---- panes

inline void move(u32 pane, f32 x, f32 y) {
    store_f32(pane + 0x2C, x);
    store_f32(pane + 0x30, y);
}
inline void scale(u32 pane, f32 sx, f32 sy) {
    store_f32(pane + 0x44, sx);
    store_f32(pane + 0x48, sy);
}
inline void resize(u32 pane, f32 w, f32 h) {
    store_f32(pane + 0x4C, w);
    store_f32(pane + 0x50, h);
}
inline void set_alpha(u32 pane, u8 a) { guest::store8(pane + 0xB8, a); }
inline void show(u32 pane, bool on) { g::nw4r_lyt_Pane_SetVisible(pane, on ? 1 : 0); }
// the pane's alpha applies to its children too
inline void alpha_to_children(u32 pane, bool on) {
    u8 f = guest::load8(pane + 0xBB);
    guest::store8(pane + 0xBB, on ? (u8)(f | 2) : (u8)(f & ~2));
}

// nw4r::lyt's location adjust. On a 16:9 screen (the channel's setting here) the channel's 2D
// DrawInfo (layout_sDrawInfo; set up by layout_DrawInfo_SetLocationAdjust) has it on (+0x50 bit 0x20) with the scale (0.75,
// 1) at +0x44: a pane flagged for it (+0xBB bit 4) is drawn at that scale, and with it everything
// under it, the text too. The channel's screens hang their parts under such panes (their scene
// layouts' locators), so the TV's 16:9 widening gives them back their shapes. A layout drawn on
// its own takes the same by flagging its top pane, and then counts its widths in units 1/0.75 as
// wide; location_adjust_x() is the scale (1 on a 4:3 screen).
inline f32 location_adjust_x() {
    u32 draw_info = guest::load32(GA_layout_sDrawInfo);
    return draw_info && (guest::load8(draw_info + 0x50) & 0x20) ? load_f32(draw_info + 0x44) : 1.0f;
}
inline void location_adjust(u32 pane, bool on) {
    u8 f = guest::load8(pane + 0xBB);
    guest::store8(pane + 0xBB, on ? (u8)(f | 4) : (u8)(f & ~4));
}

// ---- colours

struct Rgb {
    u8 r, g, b;
};
constexpr Rgb CYAN = {52, 190, 237};      // the room's focus colour
constexpr Rgb CYAN_PUSH = {70, 208, 255};  // a pushed button's flash
constexpr Rgb WHITE = {255, 255, 255};

// a material's tev colour 0 or 1: rgb, alpha
inline void tev_colour(u32 material, u32 which, Rgb c, u8 a) {
    const u32 base = 4 + 4 * which;
    g::nw4r_lyt_Material_SetColorElement(material, base + 0, c.r);
    g::nw4r_lyt_Material_SetColorElement(material, base + 1, c.g);
    g::nw4r_lyt_Material_SetColorElement(material, base + 2, c.b);
    g::nw4r_lyt_Material_SetColorElement(material, base + 3, a);
}
inline u32 material_of(u32 pane) { return guest::load32(pane + 0x28); }

// the colour a text box (or a white-textured picture) shows: tev colour 0 transparent, 1 opaque,
// both of the colour
inline void paint(u32 pane, Rgb c) {
    tev_colour(material_of(pane), 0, c, 0);
    tev_colour(material_of(pane), 1, c, 255);
}

// ---- text boxes

enum : u8 { TEXT_LEFT = 3, TEXT_CENTRE = 4 };  // vertically centred

// room for a string of this many characters (the buffer from the layout is often small)
inline void text_reserve(u32 box, u32 chars) {
    gcall<u32>(guest::load32(guest::load32(box) + 0x74), box, chars);
}
inline void set_text(u32 box, const std::u16string& s) {
    gcall<u32>(guest::load32(guest::load32(box) + 0x7C), box, (u32)Text(s), 0u);
}
inline void font_size(u32 box, f32 w, f32 h) {
    store_f32(box + 0xE8, w);
    store_f32(box + 0xEC, h);
}
inline void char_space(u32 box, f32 s) { store_f32(box + 0xF4, s); }
inline void text_position(u32 box, u8 pos) { guest::store8(box + 0x100, pos); }

// the width the string takes on one line (TextBox::GetTextDrawRect: left, top, right, bottom; the
// text box breaks a line wider than itself, so it is measured made wide for the time being)
inline f32 text_width(u32 box) {
    GFRAME(0x10);
    const f32 width = load_f32(box + 0x4C);
    store_f32(box + 0x4C, 4096.0f);
    g::nw4r_lyt_TextBox_GetTextDrawRect(_gframe.sp, box);  // TextBox::GetTextDrawRect
    store_f32(box + 0x4C, width);
    return load_f32(_gframe.sp + 8) - load_f32(_gframe.sp);
}

// The string in the box at its font size (w x h), smaller if it is wider than `width` (down to
// min_scale of it), and cut short with "…" if it still is.
inline void fit_text(u32 box, const std::u16string& s, f32 width, f32 w, f32 h, f32 min_scale) {
    font_size(box, w, h);
    set_text(box, s);
    f32 have = text_width(box);
    if (have <= width)
        return;
    f32 k = width / have;
    if (k < min_scale)
        k = min_scale;
    font_size(box, w * k, h * k);
    std::u16string cut = s;
    while (text_width(box) > width && cut.size() > 1) {
        cut.pop_back();
        set_text(box, cut + u"…");
    }
}

// ---- buttons (layout::Button)

const u32 BUTTON_SIZE = 0x69C, PLAY_BUTTON_SIZE = 0x788;
// the classes' vtables, for engine::system::hookable_button_class: layout::Button has 21
// virtuals, and layout::movie::PlayButton as many
const u32 VTABLE_LAYOUT_BUTTON = GA_layout_Button_vtable, VTABLE_PLAY_BUTTON = GA_layout_movie_PlayButton_vtable;
const u32 LAYOUT_BUTTON_SLOTS = 21;

// layout::Button::Button(path, parent nw4r::lyt::Layout*, locator name, cursor): hangs from the
// locator, and the cursor hit-tests it from now on
inline u32 button_new(const char* path, u32 parent, const char* locator, u32 cursor) {
    u32 b = g::operator_new(BUTTON_SIZE);
    if (!b)
        guest::fatal("ui: out of guest memory for %s", path);
    g::layout_Button_ctor(b, Name(path), guest::load32(parent + 0xC), Name(locator), cursor);
    return b;
}

// layout::movie::PlayButton::PlayButton (layout_movie_PlayButton_ctor, the same arguments): the video player's
// play/pause button, a layout::Button whose looks switch between "plays" and "paused" on every
// push (see controls.cpp)
inline u32 play_button_new(const char* path, u32 parent, const char* locator, u32 cursor) {
    u32 b = g::operator_new(PLAY_BUTTON_SIZE);
    if (!b)
        guest::fatal("ui: out of guest memory for %s", path);
    g::layout_movie_PlayButton_ctor(b, Name(path), guest::load32(parent + 0xC), Name(locator), cursor);
    return b;
}

// the button's virtual destructor (slot 0: it leaves the cursor and its locator), then the memory
inline void button_delete(u32 b) {
    if (b)
        gcall<u32>(guest::load32(guest::load32(b) + 8), b, 1u);
}

inline u32 button_layout(u32 b) { return b + 0x14; }
inline u32 button_root(u32 b) { return root_pane(button_layout(b)); }
inline bool focused(u32 b) { return guest::load8(b + 0x10) != 0; }
inline bool active(u32 b) { return guest::load8(b + 0x5B8) != 0; }
inline bool enabled(u32 b) { return guest::load8(b + 0x5B9) != 0; }
// inactive: not hit-tested, can't be focused or pushed
inline void set_active(u32 b, bool on) {
    if (active(b) != on)
        g::layout_Button_SetActive(b, on ? 1 : 0);
}
// disabled: shown greyed (its "Disable" animation), can't be pushed
inline void set_enabled(u32 b, bool on) {
    if (enabled(b) != on)
        g::layout_Button_SetEnabled(b, on ? 1 : 0);
}
// one of the group's buttons is being pushed (its push animation runs)
inline bool group_busy(u32 b) {
    u32 group = guest::load32(b + 0x4);
    return group && guest::load32(group + 0xC) != 0;
}

// the sounds a button plays: the labels' pointers in the program's data (they must last)
inline u32 sound_label(u32 slot) { return guest::load32(slot); }
const u32 SE_OVER_M = GA_layout_sSeIconOverM;        // WAV_SE_010_ICON_OVER_M: focus
const u32 SE_CLICK_S = GA_layout_sSeIconClickS;       // WAV_SE_012_ICON_CLICK_S: a list's arrows
const u32 SE_CLICK_GENERAL = GA_layout_sSeClickGeneral;  // WAV_SE_016_CLICK_GENERAL: push
inline void set_sounds(u32 b, u32 focus_slot, u32 push_slot) {
    guest::store32(b + 0x4BC, focus_slot ? sound_label(focus_slot) : 0);
    guest::store32(b + 0x4C0, push_slot ? sound_label(push_slot) : 0);
}

// ---- the screen's wipe (layout::parts::Wipe, game::wipe(): +0x10C the kind playing; its layouts
// per kind at +0, +4, ...: layout_parts_Wipe_GetCurrentLayout gives the one playing)
inline bool wipe_playing() {
    u32 wipe = game::wipe();
    return wipe && animating(g::layout_parts_Wipe_GetCurrentLayout(wipe));  // Wipe: the layout of the kind playing
}

// The screen's parts open (their FadeIns start, their buttons become usable) once the white fade
// into the screen has ended; `frames` counts the parts' updates, this one included. Never on the
// first update: the application makes the scene and updates it in the same frame, right after the
// constructor has started the fade (game::wipe_in), and the parts stay shut on that update whatever
// the wipe reports. (LayoutWrapper::PlayAnim marks the fade's animation task as running at once, so
// wipe_playing() is already true there: the first-update check only decides anything when no fade
// was started.) The fade (Fade_White_In) takes 11 frames; WIPE_TIMEOUT_FRAMES, a second and a half,
// opens the parts anyway should the wipe never report that it has ended, so that a wipe left
// playing can't keep the screen shut.
const int WIPE_TIMEOUT_FRAMES = 90;
inline bool screen_may_open(int frames) {
    return frames > 1 && (!wipe_playing() || frames > WIPE_TIMEOUT_FRAMES);
}

}  // namespace engine::ui::lyt
