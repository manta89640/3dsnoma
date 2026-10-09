// The Sound Player's list of tracks (see track_list.h).
//
// Made of the channel's own list parts: the white window of the friend and message lists
// (Friend_Message/Friend_Sel_Window.lex, its Wind_01 resized; the page counter hidden), rows of the
// message board's list (Message_Inbox/Message_List_Text.lex, each a layout::Button: grey text over
// a 1-pixel line, the line and text taking the focus colour and the line 2 pixels when the pointer
// is on it, a dark flash when pushed) and the profile screen's scroll tabs on the window's top and
// bottom edges (Profile/Profile_Up_Btn.lex, Profile_Down_Btn.lex: white, cyan when focused,
// greyed when there is nothing more that way).
//
// The list scrolls a row at a time, smoothly: the rows are a few buttons (the rows shown, one
// coming in and a spare) that move as the list moves and take the next track's name as they leave
// at one end and come back at the other, out of sight. Rows near the window's edges fade (their
// alpha) rather than being cut off, as the TV programme list's do. A tab scrolls a row as its push
// starts; held, it keeps the list moving with the TV list's rhythm, as the D-pad's up or down held
// does: a row in 13 60ths of a second, the next in 7, then one every 4 (the TV list's frames,
// counted in time: engine/ui/drag_scroll.h). A touch on the rows drags them, and a flick sends them
// coasting. The list has headings (the channel's jingles, each game's music and jingles: rows in
// the dialogs' grey that can't be chosen); L and R jump from one to the next, and so does a tap on
// a heading.
//
// The track playing has its row marked: a cyan bar with the name in white and ♪ before it (∥ and
// a grey bar when paused, ♪ blinking while the player switches to it). The bar is a row layout of
// its own under the rows (a Message_List_Text that is not a button, its Pic_Bar made opaque); the
// row over it shows only its line, and its focus. The D-pad's highlight, which A plays when the
// pointer is away from the screen, is a row's focused look, posed; the pointer, when it is on a
// row, moves the highlight there.
//
// The rows' layout is the message board's, whose focus colour isn't the room's (in this version of
// the channel its Focus takes the text and line from grey 80 to black; the designers' files had
// orange, 209,133,35): its Focus and Push animations are given the room's cyan instead while the
// screen is open (the colour keys of the animations in the archive the channel caches for every
// user of the layout), and their line's heights in the bottom screen's pixels (1 at rest, 2
// focused: engine/ui/list_window.h row_lines_to_pixels), and given back before the rows go. The
// rows (and the playing bar) sit on the bottom screen's pixels as they scroll (row_pixel_snap),
// so that every row's line is drawn, as thick as every other's.
#include "engine/soundplayer/track_list.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

#include "engine/input/remote.h"
#include "engine/system/host_scene.h"
#include "engine/ui/drag_scroll.h"
#include "engine/ui/game.h"
#include "engine/ui/list_window.h"
#include "engine/ui/lyt.h"

namespace engine::soundplayer {

namespace game = engine::ui::game;
namespace lyt = engine::ui::lyt;

using namespace lyt;

namespace {

const char* const WINDOW_LEX = "layout/Friend_Message/Friend_Sel_Window.lex";
const char* const ROW_LEX = "layout/Message_Inbox/Message_List_Text.lex";
const char* const UP_LEX = "layout/Profile/Profile_Up_Btn.lex";
const char* const DOWN_LEX = "layout/Profile/Profile_Down_Btn.lex";

// Geometry, in layout units (608 x 456, the origin at the centre, y up), as it shows on the screen.
// The window is on the bottom screen (scene.cpp's region: x -304..229, y -228..172), above the video
// player's buttons and clear of the Home tab at the bottom left, its tabs inside the region; the
// rows' places are counted from the window's centre. The window takes the channel's location
// adjust (lyt.h; on a 16:9 screen drawn narrower, as the channel's own lists are, their text with
// them), so the widths inside it are made wider by its inverse (`wide`) to show at these sizes; on
// the bottom screen, whose pixels are square, the adjust is off (engine/ui/screen.h) and `wide` 1.
const f32 WINDOW_X = -37, WINDOW_Y = 12;
const int VISIBLE = 4;                // rows shown at once (few and big: legible on the 3DS's screen)
const int SLOTS = VISIBLE + 2;        // row buttons: the ones shown, one coming in, a spare
const f32 PITCH = 54;                 // the row pitch (the message list's rows at its 5 a window)
const f32 FIRST_Y = (VISIBLE - 1) * PITCH / 2;  // the top row's centre (the rows centred in the window)
const f32 WINDOW_W = 284, WINDOW_H = VISIBLE * PITCH + 22;  // (5 rows: 292, as the message list's)
const f32 ROW_W = 252;                // 16 inside the window's sides, as the message list's
// the row's height: the message list's proportion of it to its pitch (Message_List_Text's
// Bound_Btn, 41.43 high at a pitch of 44)
const f32 ROW_H = PITCH * 41.43f / 44;
const f32 TEXT_H = 42;                // the name's box (35 in the layout, for the larger text)
const f32 MARK_X = -112;              // ♪ / ∥ in the playing row
const f32 TEXT_X = 13, TEXT_W = 222;  // the name: left-aligned from x -98
const f32 HEADER_X = 0, HEADER_W = 240;
const f32 FONT_W = 29, FONT_H = 34.58f;      // the message list's own row text (Message_List_Text)
const f32 HEADER_FONT_W = 22.08f, HEADER_FONT_H = 26.22f;  // the dialogs' text size
const f32 TAB_LAYOUT_H = 44;          // the tabs' height in their layout (Profile_Up_Btn's Pic_Btn)
const f32 TAB_SCALE = 0.8f, TAB_H = TAB_LAYOUT_H * TAB_SCALE;
const f32 TAB_Y = (WINDOW_H + TAB_H) / 2 - 1;  // the tabs' middles, above and below the window's
// the tabs' hit area (their Bound_Btn, 230 x 60 in their layout), around their middles
const f32 TAB_HIT_W = 230 * TAB_SCALE, TAB_HIT_H = 60 * TAB_SCALE;
const u32 NAME_CHARS = 32;            // room in each text box
// widths inside the window: 1 / the location adjust's scale (set as the list is made)
f32 wide = 1;

// the playing row's bar: cyan, lighter when the pointer or the D-pad's highlight is on it (the
// colour a pushed button flashes); grey when paused
const Rgb PAUSED_BAR = {150, 150, 150}, PAUSED_BAR_LIT = {178, 178, 178};
const Rgb HEADER_GREY = {128, 128, 128};
const char16_t NOTE = 0x266A, PAUSE = 0x2225;  // ♪ ∥ (the font has no ▶ ‖)

const u32 SOUND_UNKNOWN = 0xFFFFFFFEu;

// the scroll rhythm (60ths of a second a row), as the TV programme list's Up/Down animations (their
// frames at the Wii's 60 a second): the first row, the second, every one after
const f32 RHYTHM[] = {13, 7, 4};
f32 rhythm(int step) { return RHYTHM[std::min(step, 2)]; }
const f32 SETTLE = 8;  // the glide onto the nearest row when a drag is let go (60ths of a second)
const f32 BLINK = 8;   // ♪'s blink while the player switches: on, off (60ths of a second)

// ---- the message board's focus colour in the rows' animations, made the room's cyan

const u32 RLMC = 0x524C4D43;  // a material colour animation
const f32 REST_GREY = 80, CYAN_F[3] = {(f32)CYAN.r, (f32)CYAN.g, (f32)CYAN.b};

// An RLAN's pai1 block (the AnimTransform's +0xC): +0xE the number of contents, +0x10 the offset
// of their offsets; a content: +0x14 its number of infos, +0x18 their offsets; an info: +0 its
// kind, +4 its number of targets, +8 their offsets; a target: +1 what it animates (a material
// colour: 4 + 4 * colour + component), +4 its number of keys, +8 their offset; a key: frame,
// value, slope (f32). Each colour curve of the rows' Focus and Push goes between the rest grey
// (80) and the focus colour: its keys at the focus colour take the cyan's component instead, and
// all its slopes are scaled with the change. The words changed are kept, to be put back.
void recolour(u32 anim, std::vector<std::pair<u32, u32>>& saved) {
    if (!anim)
        return;
    u32 res = guest::load32(anim + 0xC);
    u32 contents = guest::load16(res + 0xE), table = res + guest::load32(res + 0x10);
    for (u32 c = 0; c < contents; c++) {
        u32 content = res + guest::load32(table + 4 * c);
        for (u32 i = 0; i < guest::load8(content + 0x14); i++) {
            u32 info = content + guest::load32(content + 0x18 + 4 * i);
            if (guest::load32(info) != RLMC)
                continue;
            for (u32 t = 0; t < guest::load8(info + 4); t++) {
                u32 target = info + guest::load32(info + 8 + 4 * t);
                u32 element = guest::load8(target + 1);
                if (element < 4 || element > 11 || (element & 3) == 3)
                    continue;
                const f32 cyan = CYAN_F[element & 3];
                u32 keys = target + guest::load32(target + 8), n = guest::load16(target + 4);
                f32 focus = REST_GREY;
                for (u32 k = 0; k < n; k++)
                    if (load_f32(keys + 12 * k + 4) != REST_GREY)
                        focus = load_f32(keys + 12 * k + 4);
                if (focus == REST_GREY || focus == cyan)
                    continue;
                const f32 ratio = (cyan - REST_GREY) / (focus - REST_GREY);
                for (u32 k = 0; k < n; k++) {
                    u32 key = keys + 12 * k;
                    saved.push_back({key + 4, guest::load32(key + 4)});
                    saved.push_back({key + 8, guest::load32(key + 8)});
                    if (load_f32(key + 4) == focus)
                        store_f32(key + 4, cyan);
                    store_f32(key + 8, load_f32(key + 8) * ratio);
                }
            }
        }
    }
}

// a row's panes (Message_List_Text: Loc_Top > Bound_Btn (the hit area), Loc_Scale > Pic_Bar, the
// name B_TextBox_00, the line B_Line under it, B_TextBox_Date at the left)
struct RowPanes {
    u32 root = 0, bar = 0, text = 0, line = 0, date = 0;

    void make(u32 layout) {
        root = root_pane(layout);
        alpha_to_children(root, true);
        if (u32 bound = find_pane(layout, "Bound_Btn"))
            resize(bound, ROW_W * wide, PITCH - 2);
        bar = find_pane(layout, "Pic_Bar");
        resize(bar, ROW_W * wide, ROW_H);
        line = find_pane(layout, "B_Line");
        resize(line, ROW_W * wide, load_f32(line + 0x50));
        move(line, 0, -PITCH / 2);  // at the row's foot (-22 in the layout, for its pitch of 44)
        text = find_pane(layout, "B_TextBox_00");
        text_reserve(text, NAME_CHARS);
        char_space(text, -1);
        name_style();
        date = find_pane(layout, "B_TextBox_Date");
    }
    void name_style() {
        move(text, TEXT_X * wide, 0);
        resize(text, TEXT_W * wide, TEXT_H);
        text_position(text, TEXT_LEFT);
        font_size(text, FONT_W, FONT_H);
    }
    void header_style() {
        move(text, HEADER_X * wide, -4);
        resize(text, HEADER_W * wide, 30);
        text_position(text, TEXT_LEFT);
        font_size(text, HEADER_FONT_W, HEADER_FONT_H);
    }
};

// how much of an entry shows, at this scroll position: 1 inside the window, fading to 0 half a row
// beyond its first and last rows (before it reaches the window's edge)
f32 shown_part(f32 pos, int entry) {
    f32 d = std::max(pos - (f32)entry, (f32)entry - (pos + (VISIBLE - 1)));
    return 1.0f - std::min(std::max(2 * d, 0.0f), 1.0f);
}

}  // namespace

struct TrackList::Impl {
    // the list: catalog indices, with a heading (-1 - the index of the track it heads) before the
    // first track of each heading (the jingles', each game's)
    std::vector<int> entries;

    u32 window = 0;   // Friend_Sel_Window, a layout of its own: the rows, tabs and bar hang from it
    u32 fade_in = 0;  // its FadeIn
    struct Row {
        u32 button = 0;
        RowPanes panes;
        u32 focus = 0;           // its Focus animation
        int entry = -1;          // the entry it shows (-1: none)
        int pushed_entry = -1;   // the entry it showed when its push started (it may move on before the push ends)
        bool header = false;
        bool posed = false;      // posed in its focused look (the D-pad's highlight)
        bool name_shown = true;  // its name is shown (not under the playing bar)
    } rows[SLOTS];
    u32 up = 0, down = 0;  // the scroll tabs

    // the playing track's bar
    u32 bar_layout = 0;
    RowPanes bar;
    int bar_entry = -1;
    char16_t bar_mark = 0;

    // scrolling: `top` is the entry the list is going to have at the top, `pos` where it is now
    // (in rows); a glide takes pos from where it was to top over glide_len 60ths of a second (0:
    // none)
    int top = 0;
    f32 pos = 0;
    f32 glide_from = 0, glide_t = 0, glide_len = 0;
    bool glide_ease = false;
    int tab_dir = 0, tab_steps = 0;  // a tab held: its way, the rows it has scrolled
    f32 tab_timer = 0;               // 60ths of a second to its next row
    f32 tab_x = 0, tab_y = 0;        // where the pointer pressed it
    u32 touch_tab = 0;               // the tab the touch going on came down on (tab_touch)
    u32 taken_tab = 0;               // a tab's press taken as the touch came down, its push awaited
    bool taken_read = false;         // a game read has carried the touch's A since
    u64 taken_at = 0;                // when it was taken (guest::timebase)
    bool touched = false;            // the touch screen touched at the last update
    int pad_dir = 0, pad_steps = 0;  // the D-pad held, likewise
    f32 pad_timer = 0;
    u16 shoulders = 0;               // L and R held at the last frame
    engine::ui::Ticker ticker;       // the 60ths of a second since the last update (dt)
    f32 dt = 0, clock = 0;           // (clock: their sum, for the blink)

    int highlight = 0;      // the D-pad's highlight (an entry)
    bool pad_mode = false;  // the D-pad has been used since the pointer last moved
    f32 pointer_x = 0, pointer_y = 0;
    u32 last_sound = SOUND_UNKNOWN;

    int chosen = -1;
    bool opened = false;  // its FadeIn has started (the rows can be used)
    bool gone = false;    // the scene is going (leaving())
    engine::ui::DragScroll drag;  // the touch dragging the rows
    int frames = 0;

    std::vector<std::pair<u32, u32>> edited;  // animation words changed, and what they were

    int max_top() const { return std::max(0, (int)entries.size() - VISIBLE); }
    int entry_of_sound(u32 id) const;
    void assign(Row& r, int entry);
    void glide(f32 len, bool ease);
    void scroll_by(int dir, f32 len, bool ease);
    void scroll_to_show(int entry, f32 len, bool ease);
    void move_highlight(int dir, f32 len);
    f32 jump_glide(int t) const;
    void jump_heading(int dir);
    void tap_heading(f32 y);
    void take_over();
    void tab_press(u32 tab);
    bool on_tab(u32 tab, f32 x, f32 y) const;
    void tab_touch();
    void follow_music(const Music& music);
    void input();
    void advance();
    f32 row_y(int entry) const;
    void place_rows();
    void place_bar(const Music& music);
    void pose_highlight();
};

int TrackList::Impl::entry_of_sound(u32 id) const {
    if (id == Music::NO_SOUND)
        return -1;
    const std::vector<Track>& tracks = catalog();
    for (size_t e = 0; e < entries.size(); e++)
        if (entries[e] >= 0 && tracks[entries[e]].sound_id == id)
            return (int)e;
    return -1;
}

TrackList::TrackList(u32 cursor, u32 group) : impl_(new Impl) {
    Impl& m = *impl_;
    const std::vector<Track>& tracks = catalog();
    for (size_t i = 0; i < tracks.size(); i++) {
        if (!tracks[i].heading.empty() && (i == 0 || tracks[i].heading != tracks[i - 1].heading))
            m.entries.push_back(-1 - (int)i);
        m.entries.push_back((int)i);
    }

    // the window: moved and resized (its animations scale and fade Wind_01 and Loc_Text and slide
    // Loc_Window; Loc_Top, above them, is free to move)
    wide = 1.0f / location_adjust_x();
    m.window = layout_new(WINDOW_LEX);
    location_adjust(find_pane(m.window, "Loc_Top"), true);
    move(find_pane(m.window, "Loc_Top"), WINDOW_X, WINDOW_Y);
    resize(find_pane(m.window, "Wind_01"), WINDOW_W * wide, WINDOW_H);
    for (const char* page : {"Text_Page_Num", "Text_Total_Num", "Text_Slash"})
        show(find_pane(m.window, page), false);
    // the rows and tabs hang under Loc_Text, which the FadeIn fades in last: its alpha reaches them
    alpha_to_children(find_pane(m.window, "Loc_Text"), true);

    // the playing track's bar, first under Name_Top (the rows' locator): drawn under the rows
    m.bar_layout = layout_new_in(ROW_LEX, m.window, "Name_Top");
    m.bar.make(m.bar_layout);
    paint(m.bar.bar, CYAN);
    set_alpha(m.bar.bar, 255);
    paint(m.bar.text, WHITE);
    show(m.bar.line, false);
    move(m.bar.date, MARK_X * wide, 0);
    resize(m.bar.date, 34 * wide, TEXT_H);
    text_position(m.bar.date, TEXT_CENTRE);
    font_size(m.bar.date, FONT_W, FONT_H);
    text_reserve(m.bar.date, 2);
    paint(m.bar.date, WHITE);
    show(root_pane(m.bar_layout), false);

    for (Impl::Row& r : m.rows) {
        r.button = button_new(ROW_LEX, m.window, "Name_Top", cursor);
        r.panes.make(button_layout(r.button));
        show(r.panes.date, false);
        r.focus = find_anim(button_layout(r.button), "Focus");
        const u32 push = find_anim(button_layout(r.button), "Push");
        recolour(r.focus, m.edited);
        recolour(push, m.edited);
        engine::ui::row_lines_to_pixels(r.focus, m.edited);
        engine::ui::row_lines_to_pixels(push, m.edited);
        pose(button_layout(r.button), r.focus, 0);
        engine::system::hook_button(r.button);
        game::group_add(r.button, group);
        set_active(r.button, false);
        show(r.panes.root, false);
    }

    // the tabs, on the window's top and bottom edges (their root panes moved and scaled: their
    // animations work on the panes under them)
    m.up = button_new(UP_LEX, m.window, "Loc_Text", cursor);
    m.down = button_new(DOWN_LEX, m.window, "Loc_Text", cursor);
    for (u32 tab : {m.up, m.down}) {
        move(button_root(tab), 0, tab == m.up ? TAB_Y : -TAB_Y);
        scale(button_root(tab), TAB_SCALE, TAB_SCALE);
        set_sounds(tab, SE_OVER_M, SE_CLICK_S);
        engine::system::hook_button(tab);
        game::group_add(tab, group);
        set_active(tab, false);
    }

    // hidden until the white fade into the screen has ended (update() plays the FadeIn)
    m.fade_in = find_anim(m.window, "FadeIn");
    pose(m.window, m.fade_in, 0);
    m.place_rows();
    layout_update(m.window);
}

TrackList::~TrackList() {
    Impl& m = *impl_;
    // the animations' colours and line heights back, while the archive holding them is still in use
    for (auto it = m.edited.rbegin(); it != m.edited.rend(); ++it)
        guest::store32(it->first, it->second);
    button_delete(m.down);
    button_delete(m.up);
    for (int i = SLOTS - 1; i >= 0; i--)
        button_delete(m.rows[i].button);
    layout_delete(m.bar_layout);
    layout_delete(m.window);
    delete impl_;
}

// a row takes an entry: its name (or a heading, which can't be chosen)
void TrackList::Impl::assign(Row& r, int entry) {
    r.entry = entry;
    bool header = entry >= 0 && entries[entry] < 0;
    if (header != r.header) {
        r.header = header;
        // the Focus pose at its first frame gives the rest colours back
        pose(button_layout(r.button), r.focus, 0);
        r.posed = false;
        if (header)
            r.panes.header_style();
        else
            r.panes.name_style();
    }
    if (entry < 0)
        return;
    if (header) {
        fit_text(r.panes.text, catalog()[-1 - entries[entry]].heading, HEADER_W * wide, HEADER_FONT_W,
                 HEADER_FONT_H, 0.7f);
        paint(r.panes.text, HEADER_GREY);
    } else {
        fit_text(r.panes.text, catalog()[entries[entry]].name, TEXT_W * wide, FONT_W, FONT_H, 0.7f);
    }
}

// pos glides to top over len 60ths of a second (eased in and out, or at an even speed: one of
// several rows in a row), from where it is
void TrackList::Impl::glide(f32 len, bool ease) {
    glide_from = pos;
    glide_t = 0;
    glide_len = std::max(len, 1.0f);
    glide_ease = ease;
}

// a row up or down, if there is one
void TrackList::Impl::scroll_by(int dir, f32 len, bool ease) {
    int t = std::min(std::max(top + dir, 0), max_top());
    if (t == top)
        return;
    top = t;
    glide(len, ease);
}

// the list scrolls as little as it takes to show this entry
void TrackList::Impl::scroll_to_show(int entry, f32 len, bool ease) {
    int t = top;
    if (entry < t)
        t = entry;
    else if (entry > t + VISIBLE - 1)
        t = entry - VISIBLE + 1;
    t = std::min(std::max(t, 0), max_top());
    if (t == top)
        return;
    top = t;
    glide(len, ease);
}

// the D-pad's highlight one entry up or down (over the heading), the list following it
void TrackList::Impl::move_highlight(int dir, f32 n) {
    int h = highlight;
    if (h < top || h > top + VISIBLE - 1)
        h = dir > 0 ? top : top + VISIBLE - 1;  // out of sight (scrolled away): back at the edge
    else
        h += dir;
    if (h >= 0 && h < (int)entries.size() && entries[h] < 0)
        h += dir;
    if (h < 0 || h >= (int)entries.size())
        return;
    highlight = h;
    scroll_to_show(h, n, n >= RHYTHM[0]);
}

// a jump's glide to top t (L, R, a heading tapped): longer the further it goes (60ths of a second)
f32 TrackList::Impl::jump_glide(int t) const {
    return (f32)std::min(std::max(10 + std::abs(t - (int)pos) / 2, 13), 30);
}

// L and R: the highlight to the first track under the next heading, or the one before (the list's
// start counts as one), the list scrolled to show that heading at its top
void TrackList::Impl::jump_heading(int dir) {
    std::vector<int> starts;  // the first track under each heading, and the list's first
    for (size_t e = 0; e < entries.size(); e++)
        if (entries[e] >= 0 && (e == 0 || entries[e - 1] < 0))
            starts.push_back((int)e);
    if (starts.empty())
        return;
    int current = 0;  // the start of the highlight's part
    for (size_t k = 0; k < starts.size(); k++)
        if (starts[k] <= highlight)
            current = (int)k;
    int k = std::min(std::max(current + dir, 0), (int)starts.size() - 1);
    if (dir < 0 && highlight > starts[current])
        k = current;  // back to the start of its own part first
    highlight = starts[k];
    int t = std::min(std::max(highlight - (highlight > 0 && entries[highlight - 1] < 0 ? 1 : 0), 0), max_top());
    if (t != top) {
        top = t;
        glide(jump_glide(t), true);
    }
}

// A tap on a heading (a grey row, no button) jumps as R does from there: to the next heading, its
// first track highlighted and the heading at the list's top (from the last heading, to that one),
// the pointer sent away as by the D-pad, so that A plays the highlight. The tap's A, still to come
// for the game's cursor, presses nothing: the rows are held a moment.
void TrackList::Impl::tap_heading(f32 y) {
    const int e = (int)std::lround(pos + (WINDOW_Y + FIRST_Y - y) / PITCH);
    if (e < 0 || e >= (int)entries.size() || entries[e] >= 0)
        return;
    int next = e;
    for (int k = e + 1; k < (int)entries.size(); k++) {
        if (entries[k] < 0) {
            next = k;
            break;
        }
    }
    if (next + 1 >= (int)entries.size())
        return;
    drag.hold();
    pad_mode = true;
    guest::input_pointer_away();
    highlight = next + 1;
    const int t = std::min(next, max_top());
    if (t != top) {
        top = t;
        glide(jump_glide(t), true);
    }
}

// another input (a tab, the D-pad, L or R) takes the list from a coast: it settles on the nearest
// row, unless that input moves it on
void TrackList::Impl::take_over() {
    if (drag.halt()) {
        top = std::min(std::max((int)std::lround(pos), 0), max_top());
        glide(SETTLE, true);
    }
}

// A tab's push starts: a row that way at once (gliding on from where the list is, at the rhythm's
// second pace, if a row is still gliding in: taps in quick succession each scroll a row), and on
// with the rhythm while it is held (input()).
void TrackList::Impl::tab_press(u32 tab) {
    take_over();
    tab_dir = tab == up ? -1 : 1;
    tab_steps = 1;
    tab_timer = rhythm(0);
    game::pointer_at(tab_x, tab_y);
    const bool gliding = glide_len > 0;
    scroll_by(tab_dir, gliding ? rhythm(1) : rhythm(0), !gliding);
}

// the point (layout units) is on the tab's hit area
bool TrackList::Impl::on_tab(u32 tab, f32 x, f32 y) const {
    const f32 ty = WINDOW_Y + (tab == up ? TAB_Y : -TAB_Y);
    return std::fabs(x - WINDOW_X) <= TAB_HIT_W / 2 && std::fabs(y - ty) <= TAB_HIT_H / 2;
}

// A touch coming down on a tab scrolls at once (tab_press), whether or not the game's cursor takes
// it as a press: the cursor takes none on a button until its focus is back, some 13 frames after its
// last push began (the tab's push, 7, then 6 more), and would drop every other tap of a quick
// succession. Its push, when it comes (in the cursor's update after the game's read that carries
// the touch's A: its second read), gives the press its look and its click and is no press of its own
// (on_push_start). Whether it came is known at the update after that read's: the input changes
// anywhere in the guest's code, so a read may come one update later than the touch was seen here.
// When it doesn't come (or no read carries the touch's A within half a second), the click is played
// here.
void TrackList::Impl::tab_touch() {
    if (taken_tab) {
        if (taken_read || guest::timebase() - taken_at > guest::TB_HZ / 2) {
            play_sound(taken_tab, sound_label(SE_CLICK_S));
            taken_tab = 0;
        } else if ((game::buttons_triggered() & game::PAD_A) && guest::wiimote_a_from_touch()) {
            taken_read = true;
        }
    }
    f32 x, y;
    const bool touching = engine::ui::touch_point(x, y);
    if (!touching) {
        touch_tab = 0;
    } else if (!touched && opened && !gone) {
        for (u32 tab : {up, down}) {
            if (on_tab(tab, x, y) && active(tab) && enabled(tab)) {
                touch_tab = taken_tab = tab;
                taken_read = false, taken_at = guest::timebase();
                tab_press(tab);
            }
        }
    }
    touched = touching;
}

// The track changed (from the list, the controls, left and right): its row comes into sight,
// and the highlight goes with it. When the screen opens, the list starts there.
void TrackList::Impl::follow_music(const Music& music) {
    u32 sound = music.current();
    if (sound == last_sound)
        return;
    bool first = last_sound == SOUND_UNKNOWN;
    last_sound = sound;
    int e = entry_of_sound(sound);
    if (first) {
        highlight = e >= 0 ? e : entries[0] < 0 ? 1 : 0;
        top = std::min(std::max(0, highlight - VISIBLE / 2 + 1), max_top());
        pos = (f32)top;
    } else if (e >= 0) {
        take_over();  // (from a coast or a drag's settling: the playing row comes into sight all the same)
        highlight = e;
        scroll_to_show(e, (f32)std::min(std::max(10 + 2 * std::abs(e - top), 13), 36), true);
    }
}

// the D-pad, L and R, and a tab held (the pointer and A on the rows are the cursor's; a tab's press
// is its push's start, tab_press)
void TrackList::Impl::input() {
    u32 pressed = game::buttons_triggered(), held = game::buttons_held();
    f32 px, py;
    game::pointer_at(px, py);
    if (game::pointer_on_screen() && (std::fabs(px - pointer_x) > 0.002f || std::fabs(py - pointer_y) > 0.002f))
        pad_mode = false;
    pointer_x = px;
    pointer_y = py;

    // L and R: from heading to heading. They, and the D-pad, send the pointer away from the
    // screen, as a remote pointed away, so that the cursor drops the focus it had and A plays the
    // highlight.
    u16 shoulders_held = guest::wiimote_input().pad & (guest::PAD_L | guest::PAD_R);
    u16 shoulders_pressed = shoulders_held & ~shoulders;
    shoulders = shoulders_held;
    if (shoulders_pressed) {
        pad_mode = true;
        guest::input_pointer_away();
        take_over();
        jump_heading(shoulders_pressed & guest::PAD_R ? 1 : -1);
    }

    // up and down: the highlight moves at once, again after 13 60ths of a second, after 7, then
    // every 4
    if (pressed & (game::PAD_UP | game::PAD_DOWN)) {
        pad_dir = (pressed & game::PAD_UP) ? -1 : 1;
        pad_steps = 1;
        pad_timer = rhythm(0);
        pad_mode = true;
        guest::input_pointer_away();
        take_over();
        move_highlight(pad_dir, rhythm(0));
    } else if (pad_dir && (held & (pad_dir < 0 ? game::PAD_UP : game::PAD_DOWN))) {
        pad_timer -= dt;
        if (pad_timer <= 0) {
            const f32 n = rhythm(pad_steps++);
            pad_timer += n;
            move_highlight(pad_dir, n);
        }
    } else {
        pad_dir = 0;
    }

    // A tab held scrolls on with the rhythm, at an even speed: by the touch that came down on it,
    // while it stays on it; by A, while the pointer stays where it pressed it (the tab loses its
    // focus while its push plays).
    if (tab_dir) {
        u32 tab = tab_dir < 0 ? up : down;
        f32 tx, ty;
        const bool still = std::fabs(px - tab_x) < 0.03f && std::fabs(py - tab_y) < 0.03f;
        const bool on = touch_tab ? touch_tab == tab && engine::ui::touch_point(tx, ty) && on_tab(tab, tx, ty)
                                  : (held & game::PAD_A) && still;
        if (!on || !enabled(tab)) {
            tab_dir = 0;
        } else {
            tab_timer -= dt;
            if (tab_timer <= 0) {
                const f32 n = rhythm(tab_steps++);
                tab_timer += n;
                scroll_by(tab_dir, n, false);
            }
        }
    }
}

void TrackList::Impl::advance() {
    if (glide_len <= 0)
        return;
    glide_t += dt;
    f32 f = std::min(glide_t / glide_len, 1.0f);
    if (glide_ease)
        f = f * f * (3 - 2 * f);
    pos = glide_from + ((f32)top - glide_from) * f;
    if (glide_t >= glide_len) {
        pos = (f32)top;
        glide_len = 0;
    }
}

// an entry's row's place (its centre's y under the window's centre) at the list's scroll position,
// on the bottom screen's pixels
f32 TrackList::Impl::row_y(int entry) const {
    const f32 y = FIRST_Y - ((f32)entry - pos) * PITCH;
    return y + engine::ui::row_pixel_snap(WINDOW_Y + y - PITCH / 2);
}

// every row button at its entry's place: slot k shows the entry e = k (mod SLOTS) among the
// SLOTS entries from the one above the top
void TrackList::Impl::place_rows() {
    int first = (int)std::floor(pos) - 1;
    for (int k = 0; k < SLOTS; k++) {
        Row& r = rows[k];
        int e = first + (((k - first) % SLOTS) + SLOTS) % SLOTS;
        if (e < 0 || e >= (int)entries.size()) {
            if (r.entry != -1)
                assign(r, -1);
            show(r.panes.root, false);
            set_active(r.button, false);
            continue;
        }
        if (r.entry != e)
            assign(r, e);
        f32 part = shown_part(pos, e);
        move(r.panes.root, 0, row_y(e));
        set_alpha(r.panes.root, (u8)(part * 255.0f + 0.5f));
        show(r.panes.root, part > 0);
        set_active(r.button, opened && !r.header && part >= 0.5f && !drag.holding());
    }
}

// the playing track's bar, under its row: ♪ playing, ∥ paused, ♪ blinking while switching
void TrackList::Impl::place_bar(const Music& music) {
    PlayState state = music.state();
    int e = state == PlayState::STOPPED ? -1 : entry_of_sound(music.current());
    f32 part = e >= 0 ? shown_part(pos, e) : 0;
    u32 root = root_pane(bar_layout);
    if (part <= 0) {
        show(root, false);
    } else {
        if (e != bar_entry) {
            fit_text(bar.text, catalog()[entries[e]].name, TEXT_W * wide, FONT_W, FONT_H, 0.7f);
            bar_entry = e;
        }
        char16_t mark = state == PlayState::PAUSED ? PAUSE : NOTE;
        if (mark != bar_mark) {
            set_text(bar.date, std::u16string(1, mark));
            bar_mark = mark;
        }
        bool lit = false;
        for (const Row& r : rows)
            lit |= r.entry == e && (r.posed || focused(r.button));
        if (state == PlayState::PAUSED)
            paint(bar.bar, lit ? PAUSED_BAR_LIT : PAUSED_BAR);
        else
            paint(bar.bar, lit ? CYAN_PUSH : CYAN);
        set_alpha(bar.date, state == PlayState::SWITCHING && (int)(clock / BLINK) % 2 ? 80 : 255);
        move(root, 0, row_y(e));
        set_alpha(root, (u8)(part * 255.0f + 0.5f));
        show(root, true);
    }
    // the row over the bar shows only its line (and its focus)
    for (Row& r : rows) {
        bool name = r.header || r.entry != e || part <= 0;
        if (name != r.name_shown) {
            show(r.panes.text, name);
            r.name_shown = name;
        }
    }
}

// The D-pad's highlight shows (as the row's focused look) while the pointer is away from the
// screen, or since the D-pad was last used; a row the pointer is on shows its own focus.
void TrackList::Impl::pose_highlight() {
    bool on = opened && (!game::pointer_on_screen() || pad_mode);
    for (Row& r : rows) {
        if (focused(r.button)) {
            r.posed = false;  // the button's own focus (its animation plays back when it ends)
            continue;
        }
        bool want = on && !r.header && r.entry >= 0 && r.entry == highlight;
        if (want != r.posed) {
            pose(button_layout(r.button), r.focus, want ? last_frame(r.focus) : 0);
            r.posed = want;
        }
    }
}

void TrackList::update(const Music& music) {
    Impl& m = *impl_;
    m.frames++;
    m.dt = m.ticker.tick();
    m.clock += m.dt;
    if (!m.opened && screen_may_open(m.frames)) {
        m.opened = true;
        play(m.window, m.fade_in);
    }
    m.follow_music(music);
    m.tab_touch();
    if (m.opened) {
        const engine::ui::screen::Region rows{WINDOW_X - WINDOW_W / 2, WINDOW_Y + VISIBLE * PITCH / 2, WINDOW_W,
                                              VISIBLE * PITCH};
        switch (m.drag.update(rows, m.pos, PITCH, m.max_top(), m.dt)) {
        case engine::ui::DragScroll::Step::DRAG:
        case engine::ui::DragScroll::Step::COAST:
            m.glide_len = 0, m.tab_dir = 0;
            m.top = (int)std::lround(m.pos);
            break;
        case engine::ui::DragScroll::Step::LET_GO:
            m.top = std::min(std::max((int)std::lround(m.pos), 0), m.max_top());
            m.glide(SETTLE, true);
            break;
        case engine::ui::DragScroll::Step::TAP: m.tap_heading(m.drag.tap_y()); break;
        case engine::ui::DragScroll::Step::NONE: break;
        }
        m.input();
    }
    // the pointer on a row: the highlight is there (not when the D-pad, L or R have moved it since
    // the pointer last moved: the cursor keeps the focus it had until the pointer, sent away, is
    // read as gone)
    if (!m.pad_mode)
        for (const Impl::Row& r : m.rows)
            if (focused(r.button) && r.entry >= 0 && !r.header)
                m.highlight = r.entry;

    m.advance();
    m.place_rows();
    m.place_bar(music);
    m.pose_highlight();
    set_enabled(m.up, m.top > 0);
    set_enabled(m.down, m.top < m.max_top());
    set_active(m.up, m.opened);
    set_active(m.down, m.opened);
    layout_update(m.window);
}

void TrackList::draw() { layout_draw(impl_->window); }

void TrackList::leaving() {
    impl_->gone = true;
    impl_->tab_dir = 0;
}

// A tab's push scrolls at once (tab_press), unless it is that of a touch already taken (tab_touch).
// A row's push: what it showed when the push started is what was chosen (the list can glide on
// during the push, the D-pad changing the track, and give the row to another entry).
bool TrackList::on_push_start(u32 button) {
    Impl& m = *impl_;
    if (button == m.up || button == m.down) {
        if (button == m.taken_tab)
            m.taken_tab = 0;  // (pressed as the touch came down: tab_touch)
        else if (m.opened)
            m.tab_press(button);
        return true;
    }
    for (Impl::Row& r : m.rows) {
        if (r.button == button) {
            r.pushed_entry = r.entry >= 0 && !r.header ? r.entry : -1;
            return true;
        }
    }
    return false;
}

bool TrackList::on_pushed(u32 button) {
    Impl& m = *impl_;
    if (button == m.up || button == m.down)
        return true;
    for (Impl::Row& r : m.rows) {
        if (r.button != button)
            continue;
        int e = r.pushed_entry;
        r.pushed_entry = -1;
        if (e >= 0 && m.entries[e] >= 0) {
            m.chosen = m.entries[e];
            m.highlight = e;
        }
        return true;
    }
    return false;
}

int TrackList::take_chosen() {
    int c = impl_->chosen;
    impl_->chosen = -1;
    return c;
}

int TrackList::highlighted() const {
    const Impl& m = *impl_;
    if (m.highlight >= 0 && m.highlight < (int)m.entries.size() && m.entries[m.highlight] >= 0)
        return m.entries[m.highlight];
    return 0;
}

}  // namespace engine::soundplayer
