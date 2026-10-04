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
// alpha) rather than being cut off, as the TV programme list's do. Holding a tab, or the D-pad's
// up or down, keeps it moving with the TV list's rhythm: a row in 13 frames, the next in 7, then
// one every 4.
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
// user of the layout), and given back before the rows go.
#include "engine/soundplayer/track_list.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

#include "engine/soundplayer/game.h"
#include "engine/soundplayer/lyt.h"
#include "engine/system/host_scene.h"

namespace engine::soundplayer {

using namespace lyt;

namespace {

const char* const WINDOW_LEX = "layout/Friend_Message/Friend_Sel_Window.lex";
const char* const ROW_LEX = "layout/Message_Inbox/Message_List_Text.lex";
const char* const UP_LEX = "layout/Profile/Profile_Up_Btn.lex";
const char* const DOWN_LEX = "layout/Profile/Profile_Down_Btn.lex";

// Geometry, in layout units (608 x 456, the origin at the centre, y up), as it shows on the screen.
// The window is in the screen's left half, clear of the Home tab at the bottom left; the rows'
// places are counted from the window's centre. The window takes the channel's location adjust
// (lyt.h: drawn 3/4 as wide on a 16:9 screen, as the channel's own lists are, their text with
// them), so the widths inside it are made 1/0.75 as wide (`wide`) to show at these sizes.
const f32 WINDOW_X = -154, WINDOW_Y = 42;
const f32 WINDOW_W = 284, WINDOW_H = 292;
const int VISIBLE = 6;                // rows shown at once
const int SLOTS = VISIBLE + 2;        // row buttons: the ones shown, one coming in, a spare
const f32 PITCH = 44;                 // the message list's row pitch
const f32 FIRST_Y = 110;              // the top row's centre
const f32 ROW_W = 252, ROW_H = 41.43f;  // 16 inside the window's sides, as the message list's
const f32 MARK_X = -112;              // ♪ / ∥ in the playing row
const f32 TEXT_X = 13, TEXT_W = 222;  // the name: left-aligned from x -98
const f32 HEADER_X = 0, HEADER_W = 240;
const f32 FONT_W = 22.08f, FONT_H = 26.22f;  // the dialogs' and balloons' text size
const f32 HEADER_FONT_W = 17.6f, HEADER_FONT_H = 20.9f;
const f32 TAB_SCALE = 0.8f, TAB_H = 44 * TAB_SCALE;
const u32 NAME_CHARS = 32;            // room in each text box
// widths inside the window: 1 / the location adjust's scale (set as the list is made)
f32 wide = 1;

// the playing row's bar: cyan, lighter when the pointer or the D-pad's highlight is on it (the
// colour a pushed button flashes); grey when paused
const Rgb PAUSED_BAR = {150, 150, 150}, PAUSED_BAR_LIT = {178, 178, 178};
const Rgb HEADER_GREY = {128, 128, 128};
const char16_t* const HEADER = u"ジングル";
const char16_t NOTE = 0x266A, PAUSE = 0x2225;  // ♪ ∥ (the font has no ▶ ‖)

const u32 SOUND_UNKNOWN = 0xFFFFFFFEu;

// the scroll rhythm (frames a row), as the TV programme list's Up/Down animations: the first
// row, the second, every one after
const int RHYTHM[] = {13, 7, 4};
int rhythm(int step) { return RHYTHM[std::min(step, 2)]; }

// ---- the message board's focus colour in the rows' animations, made the room's cyan

const u32 RLMC = 0x524C4D43;  // a material colour animation
const f32 REST_GREY = 80, CYAN_F[3] = {52, 190, 237};

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
        text = find_pane(layout, "B_TextBox_00");
        text_reserve(text, NAME_CHARS);
        char_space(text, -1);
        name_style();
        date = find_pane(layout, "B_TextBox_Date");
    }
    void name_style() {
        move(text, TEXT_X * wide, 0);
        resize(text, TEXT_W * wide, 35);
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
    // the list: catalog indices, with the jingles' heading (-1) before the first jingle
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
    // (in rows); a glide takes pos from where it was to top
    int top = 0;
    f32 pos = 0;
    f32 glide_from = 0;
    int glide_t = 0, glide_frames = 0;
    bool glide_ease = false;
    int tab_dir = 0, tab_steps = 0;                 // a tab held
    f32 tab_x = 0, tab_y = 0;                       // where the pointer pressed it
    int pad_dir = 0, pad_steps = 0, pad_timer = 0;  // the D-pad held

    int highlight = 0;      // the D-pad's highlight (an entry)
    bool pad_mode = false;  // the D-pad has been used since the pointer last moved
    f32 pointer_x = 0, pointer_y = 0;
    u32 last_sound = SOUND_UNKNOWN;

    int chosen = -1;
    bool opened = false;  // its FadeIn has started (the rows can be used)
    int frames = 0;

    std::vector<std::pair<u32, u32>> recoloured;  // animation words changed, and what they were

    int max_top() const { return std::max(0, (int)entries.size() - VISIBLE); }
    int entry_of_sound(u32 id) const;
    void assign(Row& r, int entry);
    void glide(int frames, bool ease);
    void scroll_to_show(int entry, int frames, bool ease);
    void move_highlight(int dir, int frames);
    void follow_music(const Music& music);
    void input();
    void advance();
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
        if (tracks[i].jingle && (i == 0 || !tracks[i - 1].jingle))
            m.entries.push_back(-1);
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
    resize(m.bar.date, 28 * wide, 30);
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
        recolour(r.focus, m.recoloured);
        recolour(find_anim(button_layout(r.button), "Push"), m.recoloured);
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
        f32 y = (WINDOW_H + TAB_H) / 2 - 1;
        move(button_root(tab), 0, tab == m.up ? y : -y);
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
    // the animations' colours back, while the archive holding them is still in use
    for (auto it = m.recoloured.rbegin(); it != m.recoloured.rend(); ++it)
        guest::store32(it->first, it->second);
    button_delete(m.down);
    button_delete(m.up);
    for (int i = SLOTS - 1; i >= 0; i--)
        button_delete(m.rows[i].button);
    layout_delete(m.bar_layout);
    layout_delete(m.window);
    delete impl_;
}

// a row takes an entry: its name (or the jingles' heading, which can't be chosen)
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
        set_text(r.panes.text, HEADER);
        paint(r.panes.text, HEADER_GREY);
    } else {
        fit_text(r.panes.text, catalog()[entries[entry]].name, TEXT_W * wide, FONT_W, FONT_H, 0.7f);
    }
}

// pos glides to top over n frames (eased in and out, or at an even speed: one of several rows
// in a row)
void TrackList::Impl::glide(int n, bool ease) {
    glide_from = pos;
    glide_t = 0;
    glide_frames = std::max(1, n);
    glide_ease = ease;
}

// the list scrolls as little as it takes to show this entry
void TrackList::Impl::scroll_to_show(int entry, int n, bool ease) {
    int t = top;
    if (entry < t)
        t = entry;
    else if (entry > t + VISIBLE - 1)
        t = entry - VISIBLE + 1;
    t = std::min(std::max(t, 0), max_top());
    if (t == top)
        return;
    top = t;
    glide(n, ease);
}

// the D-pad's highlight one entry up or down (over the heading), the list following it
void TrackList::Impl::move_highlight(int dir, int n) {
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
        highlight = e;
        scroll_to_show(e, std::min(std::max(10 + 2 * std::abs(e - top), 13), 36), true);
    }
}

// the D-pad and the tabs (the pointer and A on the rows are the cursor's)
void TrackList::Impl::input() {
    u32 pressed = game::buttons_triggered(), held = game::buttons_held();
    f32 px, py;
    pointer_at(px, py);
    if (game::pointer_on_screen() && (std::fabs(px - pointer_x) > 0.002f || std::fabs(py - pointer_y) > 0.002f))
        pad_mode = false;
    pointer_x = px;
    pointer_y = py;

    // up and down: the highlight moves at once, again after 13 frames, after 7, then every 4
    if (pressed & (game::PAD_UP | game::PAD_DOWN)) {
        pad_dir = (pressed & game::PAD_UP) ? -1 : 1;
        pad_steps = 1;
        pad_timer = rhythm(0);
        pad_mode = true;
        move_highlight(pad_dir, rhythm(0));
    } else if (pad_dir && (held & (pad_dir < 0 ? game::PAD_UP : game::PAD_DOWN))) {
        if (--pad_timer <= 0) {
            int n = rhythm(pad_steps++);
            pad_timer = n;
            move_highlight(pad_dir, n);
        }
    } else {
        pad_dir = 0;
    }

    // A on a tab scrolls a row; held there, it scrolls on (the tab loses its focus while its push
    // plays: the pointer staying where it was pressed keeps it held)
    if (!tab_dir && (pressed & game::PAD_A)) {
        for (u32 tab : {up, down}) {
            if (focused(tab) && enabled(tab) && active(tab)) {
                tab_dir = tab == up ? -1 : 1;
                tab_steps = 0;
                tab_x = px;
                tab_y = py;
            }
        }
    }
    if (tab_dir) {
        u32 tab = tab_dir < 0 ? up : down;
        bool still = std::fabs(px - tab_x) < 0.03f && std::fabs(py - tab_y) < 0.03f;
        if (!(held & game::PAD_A) || !still || !enabled(tab)) {
            tab_dir = 0;
        } else if (!glide_frames) {
            int t = std::min(std::max(top + tab_dir, 0), max_top());
            if (t != top) {
                top = t;
                glide(rhythm(tab_steps), tab_steps == 0);
                tab_steps++;
            }
        }
    }
}

void TrackList::Impl::advance() {
    if (!glide_frames)
        return;
    glide_t++;
    f32 f = (f32)glide_t / (f32)glide_frames;
    if (glide_ease)
        f = f * f * (3 - 2 * f);
    pos = glide_from + ((f32)top - glide_from) * f;
    if (glide_t >= glide_frames) {
        pos = (f32)top;
        glide_frames = 0;
    }
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
        move(r.panes.root, 0, FIRST_Y - ((f32)e - pos) * PITCH);
        set_alpha(r.panes.root, (u8)(part * 255.0f + 0.5f));
        show(r.panes.root, part > 0);
        set_active(r.button, opened && !r.header && part >= 0.5f);
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
        set_alpha(bar.date, state == PlayState::SWITCHING && (frames / 8) % 2 ? 80 : 255);
        move(root, 0, FIRST_Y - ((f32)e - pos) * PITCH);
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
    if (!m.opened && m.frames > 1 && (!wipe_playing() || m.frames > 90)) {
        m.opened = true;
        play(m.window, m.fade_in);
    }
    m.follow_music(music);
    if (m.opened)
        m.input();
    // the pointer on a row: the highlight is there
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

// A row's push: what it showed when the push started is what was chosen (the list can glide on
// during the push, the D-pad changing the track, and give the row to another entry).
bool TrackList::on_push_start(u32 button) {
    for (Impl::Row& r : impl_->rows) {
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
