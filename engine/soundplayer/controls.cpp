// The Sound Player's controls and title (see controls.h).
//
// The controls are the video player's own (layout/Video): its control row
// (Video_Play_Scene_Test.lex, whose Loc_Wide_Btn holds the places of the rewind button, the
// play/pause disc and the fast-forward button, at the bottom bar's height) with the rewind and
// fast-forward buttons as previous and next (Video_Play_L_Btn, Video_Play_R_Btn), the play/pause
// disc (Video_Play_Play_Btn, a layout::movie::PlayButton) and the square stop button
// (Video_Play_Stop_Btn, its text "■") at the row's right, in the place of the player's guide
// button. The row's panels and title box are hidden (on the white studio the buttons stand by
// themselves), and the row is made smaller to fit under the stage, centred on the right half.
//
// The title over the stage is the screens' heading (Common/Top_Message.lex: a line of text over a
// 1-pixel rule), as wide as the stage: "♪ name" while a track plays, "∥ name" while it is paused,
// a prompt when nothing plays.
//
// layout::movie::PlayButton (0x788 bytes) switches its look on every push itself: +0x6AC is 1 while
// it shows the pause bars (playing) and 0 while it shows ▶; +0x69C/+0x6A0/+0x6A4/+0x6A8 are its
// animations Push_Play, Push_Stop, Focus_Play, Focus_Stop, of which its button layout's "Push" and
// "Focus" are made to play the pair for its look (fn_801456B4, its OnPress). As the music can also
// pause, play or stop from elsewhere (the D-pad, the list, a jingle ending), the look is set here
// to the music's state whenever no push is in progress: the same switch, then Play_to_Stop or
// Stop_to_Play (or, under the pointer, the new Focus at its end), and its balloon's text (再生 /
// 一時停止: fn_80145A80).
#include "engine/soundplayer/controls.h"

#include "engine/soundplayer/game.h"
#include "engine/soundplayer/lyt.h"
#include "engine/system/host_scene.h"

namespace engine::soundplayer {

using namespace lyt;

namespace {

const char* const ROW_LEX = "layout/Video/Video_Play_Scene_Test.lex";
const char* const PREVIOUS_LEX = "layout/Video/Video_Play_L_Btn.lex";
const char* const PLAY_LEX = "layout/Video/Video_Play_Play_Btn.lex";
const char* const NEXT_LEX = "layout/Video/Video_Play_R_Btn.lex";
const char* const STOP_LEX = "layout/Video/Video_Play_Stop_Btn.lex";
const char* const TITLE_LEX = "layout/Common/Top_Message.lex";

// Geometry (layout units: 608 x 456, the origin at the centre, y up). The row at the room bar's
// height; the play disc at PLAY_X, the stop button at STOP_X (the row's buttons: previous at -80,
// next at +83 from the disc, the half-capsules 101 wide, the stop square 84; all scaled by
// CONTROLS_SCALE), so that the four span the right half's x 8..292, centred on the stage.
const f32 CONTROLS_Y = -162;
const f32 CONTROLS_SCALE = 0.8f;
const f32 PLAY_X = 113;
const f32 STOP_X = PLAY_X + (83 + 50.5f + 6 + 42) * CONTROLS_SCALE;
// the title: over the stage (x 0..300), at the screens' heading height (its text box is at y 169,
// the rule at 160)
const f32 TITLE_X = 150, TITLE_W = 290;
const f32 TITLE_FONT_W = 22.4f, TITLE_FONT_H = 26.6f;  // Top_Message's own
const u32 TITLE_CHARS = 40;

const char16_t* const PROMPT = u"曲をえらんでください";
const char16_t* const PLAYING_MARK = u"♪ ";
const char16_t* const PAUSED_MARK = u"∥ ";

// the PlayButton's fields (see above)
enum : u32 {
    PB_PUSH_PLAY = 0x69C, PB_PUSH_STOP = 0x6A0, PB_FOCUS_PLAY = 0x6A4, PB_FOCUS_STOP = 0x6A8,
    PB_SHOWS_PAUSE = 0x6AC,
};
const u32 ANIM_PUSH = GA_lbl_8087D7E8, ANIM_FOCUS = GA_lbl_8087D7F0;  // "Push", "Focus"

}  // namespace

struct Controls::Impl {
    Action action = NONE;
    u32 row = 0;      // Video_Play_Scene_Test, a layout of its own: the buttons and title hang from it
    u32 title = 0;    // Top_Message
    u32 title_text = 0;
    u32 previous = 0, play = 0, next = 0, stop = 0;
    u32 row_fade_in = 0, title_fade_in = 0, title_text_fade_in = 0;
    bool opened = false;
    int frames = 0;
    std::u16string title_shown;
    const Track* title_track = nullptr;
    bool title_stopped = true;

    void show_playing(bool playing);
    void set_title(const Music& music, const Track* playing);
};

Controls::Controls(u32 cursor, u32 group) : impl_(new Impl) {
    Impl& m = *impl_;
    m.row = layout_new(ROW_LEX);
    // the player's panels, title box and time: hidden (the row's FadeIn animates them, not
    // whether they show)
    for (const char* name : {"Pic_Bg_Down", "Pic_Bg_Up", "Loc_Return_Btn", "Loc_Test_Text", "Loc_Icon"})
        show(find_pane(m.row, name), false);
    // Loc_Bg_Down holds the buttons: its alpha (200, a see-through OSD over the video) stays its
    // own; each button's locator fades it in (the FadeIn's frames 5..10)
    alpha_to_children(find_pane(m.row, "Loc_Bg_Down"), false);
    for (const char* name : {"Loc_Btn_L", "Loc_Btn_R", "Loc_Play_Btn", "Loc_Guide_Btn"})
        alpha_to_children(find_pane(m.row, name), true);
    u32 wide = find_pane(m.row, "Loc_Wide_Btn");
    move(wide, PLAY_X, CONTROLS_Y);
    scale(wide, CONTROLS_SCALE, CONTROLS_SCALE);
    u32 guide = find_pane(m.row, "Loc_Guide_Btn");
    move(guide, STOP_X, CONTROLS_Y);
    scale(guide, CONTROLS_SCALE, CONTROLS_SCALE);

    m.previous = button_new(PREVIOUS_LEX, m.row, "Loc_Btn_L", cursor);
    m.next = button_new(NEXT_LEX, m.row, "Loc_Btn_R", cursor);
    m.play = play_button_new(PLAY_LEX, m.row, "Loc_Play_Btn", cursor);
    m.stop = button_new(STOP_LEX, m.row, "Loc_Guide_Btn", cursor);
    // the PlayButton has no sounds of its own (the video player's): the other buttons'
    set_sounds(m.play, SE_OVER_M, SE_CLICK_GENERAL);
    u32 stop_text = find_pane(button_layout(m.stop), "Text_End");
    text_reserve(stop_text, 2);
    set_text(stop_text, u"■");
    for (u32 b : {m.previous, m.play, m.next, m.stop}) {
        engine::system::hook_button(b);
        game::group_add(b, group);
        set_active(b, false);
    }

    // the title, hanging from the row's Loc_Top
    m.title = layout_new_in(TITLE_LEX, m.row, "Loc_Top");
    move(find_pane(m.title, "Loc_Top"), TITLE_X, 0);
    m.title_text = find_pane(m.title, "Text_Title");
    resize(m.title_text, TITLE_W, 38);
    text_reserve(m.title_text, TITLE_CHARS);
    resize(find_pane(m.title, "Pic_Line"), TITLE_W, 1);

    // hidden until the white fade into the screen has ended (update() plays the FadeIns)
    m.row_fade_in = find_anim(m.row, "FadeIn");
    m.title_fade_in = find_anim(m.title, "FadeIn_All");
    m.title_text_fade_in = find_anim(m.title, "FadeIn");
    pose(m.row, m.row_fade_in, 0);
    pose(m.title, m.title_fade_in, 0);
    layout_update(m.row);
}

Controls::~Controls() {
    Impl& m = *impl_;
    layout_delete(m.title);
    for (u32 b : {m.stop, m.play, m.next, m.previous})
        button_delete(b);
    layout_delete(m.row);
    delete impl_;
}

// the play/pause button's look: the pause bars while playing, ▶ otherwise
void Controls::Impl::show_playing(bool playing) {
    u32 layout = button_layout(play);
    guest::store8(play + PB_SHOWS_PAUSE, playing ? 1 : 0);
    u32 focus = guest::load32(play + (playing ? PB_FOCUS_STOP : PB_FOCUS_PLAY));
    // ButtonLayout's map of its standard animations (fn_801244EC: the entry for a name)
    g::fn_801244EC(layout, ANIM_PUSH, guest::load32(play + (playing ? PB_PUSH_PLAY : PB_PUSH_STOP)));
    g::fn_801244EC(layout, ANIM_FOCUS, focus);
    if (focused(play))
        pose(layout, focus, last_frame(focus));
    else
        lyt::play(layout, find_anim(layout, playing ? "Play_to_Stop" : "Stop_to_Play"));
    g::fn_80145A80(play);  // its balloon: 一時停止 or 再生
}

// "♪ name", "∥ name", or the prompt; a new track's name fades in
void Controls::Impl::set_title(const Music& music, const Track* playing) {
    PlayState state = music.state();
    bool stopped = !playing || state == PlayState::STOPPED;
    std::u16string text = stopped ? PROMPT : (state == PlayState::PAUSED ? PAUSED_MARK : PLAYING_MARK) + playing->name;
    if (text == title_shown)
        return;
    title_shown = text;
    fit_text(title_text, text, TITLE_W, TITLE_FONT_W, TITLE_FONT_H, 0.6f);
    if (opened && (stopped != title_stopped || (!stopped && playing != title_track)))
        lyt::play(title, title_text_fade_in);
    title_stopped = stopped;
    title_track = stopped ? nullptr : playing;
}

void Controls::update(const Music& music, const Track* playing) {
    Impl& m = *impl_;
    m.frames++;
    if (!m.opened && m.frames > 1 && (!wipe_playing() || m.frames > 90)) {
        m.opened = true;
        play(m.row, m.row_fade_in);
        play(m.title, m.title_fade_in);
        for (u32 b : {m.previous, m.play, m.next, m.stop})
            set_active(b, true);
    }
    // the play/pause button follows the music, except while a push is on its way (the button
    // has switched its look; the music follows when the push ends)
    PlayState state = music.state();
    bool playing_now = state == PlayState::PLAYING || state == PlayState::SWITCHING;
    if (m.action == NONE && !group_busy(m.play) && (guest::load8(m.play + PB_SHOWS_PAUSE) != 0) != playing_now)
        m.show_playing(playing_now);
    m.set_title(music, playing);
    layout_update(m.row);
}

void Controls::draw() { layout_draw(impl_->row); }

bool Controls::on_pushed(u32 button) {
    Impl& m = *impl_;
    if (button == m.previous)
        m.action = PREVIOUS;
    else if (button == m.play)
        m.action = PLAY_PAUSE;
    else if (button == m.next)
        m.action = NEXT;
    else if (button == m.stop)
        m.action = STOP;
    else
        return false;
    return true;
}

Controls::Action Controls::take_action() {
    Action a = impl_->action;
    impl_->action = NONE;
    return a;
}

}  // namespace engine::soundplayer
