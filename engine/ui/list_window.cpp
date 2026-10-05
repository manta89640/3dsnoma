// A scrolling list of names in the channel's own list window (see list_window.h).
//
// The rows' layout is the message board's, whose focus colour isn't the room's (in this version of
// the channel its Focus takes the text and line from grey 80 to black): its Focus and Push
// animations are given the room's cyan while the list lives (the colour keys of the animations in
// the archive the channel caches for every user of the layout, as the Sound Player's list does),
// and given back before the rows go.
#include "engine/ui/list_window.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/system/host_scene.h"
#include "engine/ui/game.h"
#include "engine/ui/lyt.h"

namespace engine::ui {

using namespace lyt;

namespace {

const char* const WINDOW_LEX = "layout/Friend_Message/Friend_Sel_Window.lex";
const char* const ROW_LEX = "layout/Message_Inbox/Message_List_Text.lex";
const char* const UP_LEX = "layout/Profile/Profile_Up_Btn.lex";
const char* const DOWN_LEX = "layout/Profile/Profile_Down_Btn.lex";

// Geometry, in layout units as it shows on the screen (the Sound Player's list's: five big rows,
// legible on the 3DS's screen). The window takes the channel's location adjust (lyt.h: drawn 3/4
// as wide on a 16:9 screen, as the channel's own lists are, their text with them), so the widths
// inside it are made 1/0.75 as wide (`wide`) to show at these sizes.
const int VISIBLE = 5;           // rows shown at once
const int SLOTS = VISIBLE + 2;   // row buttons: the ones shown, one coming in, a spare
const f32 PITCH = 54;            // the row pitch
const f32 WINDOW_H = 292;        // five rows and a margin
const f32 FIRST_Y = 2 * PITCH;   // the top row's centre (the middle row's at the window's)
const f32 ROW_MARGIN = 16;       // the rows' ends inside the window's sides, as the message list's
// the row's height: the message list's proportion of it to its pitch (Message_List_Text's
// Bound_Btn, 41.43 high at a pitch of 44)
const f32 ROW_H = PITCH * 41.43f / 44;
const f32 TEXT_H = 42;           // the name's box (35 in the layout, for the larger text)
const f32 TEXT_MARGIN = 12;      // the name, left-aligned this far inside the row
const f32 FONT_W = 29, FONT_H = 34.58f;  // the message list's own row text (Message_List_Text)
const f32 MIN_FONT_SCALE = 0.7f;  // a long name is set smaller, down to this, then cut short with …
const f32 TAB_LAYOUT_H = 44;     // the tabs' height in their layout (Profile_Up_Btn's Pic_Btn)
const f32 TAB_SCALE = 0.8f, TAB_H = TAB_LAYOUT_H * TAB_SCALE;
const u32 NAME_CHARS = 32;       // room in each row's text box

// the scroll rhythm (frames a row), as the TV programme list's Up/Down animations: the first
// row, the second, every one after
const int RHYTHM[] = {13, 7, 4};
int rhythm(int step) { return RHYTHM[std::min(step, 2)]; }

// ---- the message board's focus colour in the rows' animations, made the room's cyan

const u32 RLMC = 0x524C4D43;  // a material colour animation
const f32 REST_GREY = 80, CYAN_F[3] = {(f32)CYAN.r, (f32)CYAN.g, (f32)CYAN.b};

// An RLAN's pai1 block (the AnimTransform's +0xC): +0xE the number of contents, +0x10 the offset
// of their offsets; a content: +0x14 its number of infos, +0x18 their offsets; an info: +0 its
// kind, +4 its number of targets, +8 their offsets; a target: +1 what it animates (a material
// colour: 4 + 4 * colour + component), +4 its number of keys, +8 their offset; a key: frame,
// value, slope (f32). Each colour curve of the rows' Focus and Push goes between the rest grey
// (80) and the focus colour: its keys at the focus colour take the cyan's component instead, and
// all its slopes are scaled with the change. The words changed are kept, to be put back. (A
// curve already cyan is left alone.)
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

// how much of an entry shows, at this scroll position: 1 inside the window, fading to 0 half a row
// beyond its first and last rows (before it reaches the window's edge)
f32 shown_part(f32 pos, int entry) {
    f32 d = std::max(pos - (f32)entry, (f32)entry - (pos + (VISIBLE - 1)));
    return 1.0f - std::min(std::max(2 * d, 0.0f), 1.0f);
}

}  // namespace

struct ListWindow::Impl {
    std::vector<std::u16string> names;
    u32 cursor = 0;
    f32 wide = 1;            // 1 / the location adjust's scale
    f32 row_w = 0, text_w = 0;

    u32 window = 0;          // Friend_Sel_Window, a layout of its own: the rows and tabs hang from it
    u32 loc_text = 0;        // its Loc_Text, whose alpha the FadeIn and FadeOut animate
    u32 fade_in = 0, fade_out = 0;
    struct Row {
        u32 button = 0;
        u32 root = 0, text = 0;
        u32 focus = 0;           // its Focus animation
        int entry = -1;          // the entry it shows (-1: none)
        int pushed_entry = -1;   // the entry it showed when its push started (the list may move on)
        bool posed = false;      // posed in its focused look (the D-pad's highlight)
    } rows[SLOTS];
    u32 up = 0, down = 0;    // the scroll tabs

    enum class State { HIDDEN, OPEN, CLOSING } state = State::HIDDEN;

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

    int highlight = 0;       // the D-pad's highlight (an entry)
    bool pad_mode = false;   // the D-pad has been used since the pointer last moved
    f32 pointer_x = 0, pointer_y = 0;
    int chosen = -1;

    std::vector<std::pair<u32, u32>> recoloured;  // animation words changed, and what they were

    int count() const { return (int)names.size(); }
    // the window's fade for the rows and tabs, 0 to 1 (Loc_Text's alpha as its animation left it)
    f32 fade() const { return (f32)guest::load8(loc_text + 0xB8) / 255.0f; }
    int max_top() const { return std::max(0, count() - VISIBLE); }
    void assign(Row& r, int entry);
    void glide(int frames, bool ease);
    void scroll_to_show(int entry, int frames, bool ease);
    void move_highlight(int dir, int frames);
    Row* row_of(int entry);
    bool any_focused() const;
    void input();
    void advance();
    void place_rows();
    void pose_highlight();
};

ListWindow::ListWindow(u32 cursor, u32 group, f32 x, f32 y, f32 width) : impl_(new Impl) {
    Impl& m = *impl_;
    m.cursor = cursor;
    m.wide = 1.0f / location_adjust_x();
    m.row_w = width - 2 * ROW_MARGIN;
    m.text_w = m.row_w - 2 * TEXT_MARGIN;

    // the window: moved and resized (its animations scale and fade Wind_01 and Loc_Text and slide
    // Loc_Window; Loc_Top, above them, is free to move)
    m.window = layout_new(WINDOW_LEX);
    u32 top = find_pane(m.window, "Loc_Top");
    location_adjust(top, true);
    move(top, x, y);
    resize(find_pane(m.window, "Wind_01"), width * m.wide, WINDOW_H);
    for (const char* page : {"Text_Page_Num", "Text_Total_Num", "Text_Slash"})
        show(find_pane(m.window, page), false);
    // The rows and tabs hang under Loc_Text, which the FadeIn fades in last and the FadeOut fades
    // out first. Its alpha is given to them as their own (fade()), not passed down the pane tree
    // (its flag off): a button under the pointer works its layout out again by itself, from its
    // own root down, and passed down the window's alpha would be lost there (that row showed
    // whole in the fade's first frames).
    m.loc_text = find_pane(m.window, "Loc_Text");
    alpha_to_children(m.loc_text, false);

    // the rows (Message_List_Text: Loc_Top > Bound_Btn (the hit area), Loc_Scale > Pic_Bar, the
    // name B_TextBox_00, the line B_Line under it, B_TextBox_Date at the left)
    for (Impl::Row& r : m.rows) {
        r.button = button_new(ROW_LEX, m.window, "Name_Top", cursor);
        u32 layout = button_layout(r.button);
        r.root = root_pane(layout);
        alpha_to_children(r.root, true);
        if (u32 bound = find_pane(layout, "Bound_Btn"))
            resize(bound, m.row_w * m.wide, PITCH - 2);
        resize(find_pane(layout, "Pic_Bar"), m.row_w * m.wide, ROW_H);
        u32 line = find_pane(layout, "B_Line");
        resize(line, m.row_w * m.wide, load_f32(line + 0x50));
        move(line, 0, -PITCH / 2);  // at the row's foot (-22 in the layout, for its pitch of 44)
        r.text = find_pane(layout, "B_TextBox_00");
        text_reserve(r.text, NAME_CHARS);
        char_space(r.text, -1);
        move(r.text, 0, 0);
        resize(r.text, m.text_w * m.wide, TEXT_H);
        text_position(r.text, TEXT_LEFT);
        font_size(r.text, FONT_W, FONT_H);
        show(find_pane(layout, "B_TextBox_Date"), false);
        r.focus = find_anim(layout, "Focus");
        recolour(r.focus, m.recoloured);
        recolour(find_anim(layout, "Push"), m.recoloured);
        pose(layout, r.focus, 0);
        engine::system::hook_button(r.button);
        game::group_add(r.button, group);
        set_active(r.button, false);
        show(r.root, false);
    }

    // the tabs, on the window's top and bottom edges (their root panes moved and scaled: their
    // animations work on the panes under them)
    m.up = button_new(UP_LEX, m.window, "Loc_Text", cursor);
    m.down = button_new(DOWN_LEX, m.window, "Loc_Text", cursor);
    for (u32 tab : {m.up, m.down}) {
        f32 ty = (WINDOW_H + TAB_H) / 2 - 1;
        move(button_root(tab), 0, tab == m.up ? ty : -ty);
        scale(button_root(tab), TAB_SCALE, TAB_SCALE);
        alpha_to_children(button_root(tab), true);
        set_sounds(tab, SE_OVER_M, SE_CLICK_S);
        engine::system::hook_button(tab);
        game::group_add(tab, group);
        set_active(tab, false);
    }

    m.fade_in = find_anim(m.window, "FadeIn");
    m.fade_out = find_anim(m.window, "FadeOut");
    pose(m.window, m.fade_in, 0);
    layout_update(m.window);
}

ListWindow::~ListWindow() {
    Impl& m = *impl_;
    // the animations' colours back, while the archive holding them is still in use
    for (auto it = m.recoloured.rbegin(); it != m.recoloured.rend(); ++it)
        guest::store32(it->first, it->second);
    button_delete(m.down);
    button_delete(m.up);
    for (int i = SLOTS - 1; i >= 0; i--)
        button_delete(m.rows[i].button);
    layout_delete(m.window);
    delete impl_;
}

void ListWindow::set_entries(const std::vector<std::u16string>& names) {
    Impl& m = *impl_;
    if (m.state != Impl::State::HIDDEN)
        return;
    m.names = names;
    m.highlight = std::min(m.highlight, std::max(0, m.count() - 1));
    m.top = std::min(m.top, m.max_top());
    m.pos = (f32)m.top;
    for (Impl::Row& r : m.rows)
        r.entry = -1;
}

int ListWindow::size() const { return impl_->count(); }

void ListWindow::open(int highlight) {
    Impl& m = *impl_;
    if (m.state == Impl::State::OPEN || m.names.empty())
        return;
    m.state = Impl::State::OPEN;
    if (highlight >= 0)
        m.highlight = highlight;
    m.highlight = std::min(std::max(m.highlight, 0), m.count() - 1);
    // the highlight in the middle row where the list allows
    m.top = std::min(std::max(0, m.highlight - VISIBLE / 2), m.max_top());
    m.pos = (f32)m.top;
    m.glide_frames = 0;
    m.tab_dir = m.pad_dir = 0;
    m.chosen = -1;
    for (Impl::Row& r : m.rows) {
        r.pushed_entry = -1;
        if (r.posed) {
            pose(button_layout(r.button), r.focus, 0);
            r.posed = false;
        }
    }
    play(m.window, m.fade_in);
}

void ListWindow::close() {
    Impl& m = *impl_;
    if (m.state != Impl::State::OPEN)
        return;
    m.state = Impl::State::CLOSING;
    m.tab_dir = m.pad_dir = 0;
    play(m.window, m.fade_out);
}

bool ListWindow::shown() const { return impl_->state != Impl::State::HIDDEN; }
bool ListWindow::usable() const { return impl_->state == Impl::State::OPEN; }

// a row takes an entry: its name, set smaller if it is long
void ListWindow::Impl::assign(Row& r, int entry) {
    r.entry = entry;
    if (entry >= 0)
        fit_text(r.text, names[entry], text_w * wide, FONT_W, FONT_H, MIN_FONT_SCALE);
}

// pos glides to top over n frames (eased in and out, or at an even speed: one of several rows
// in a row)
void ListWindow::Impl::glide(int n, bool ease) {
    glide_from = pos;
    glide_t = 0;
    glide_frames = std::max(1, n);
    glide_ease = ease;
}

// the list scrolls as little as it takes to show this entry
void ListWindow::Impl::scroll_to_show(int entry, int n, bool ease) {
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

// the D-pad's highlight one entry up or down, the list following it
void ListWindow::Impl::move_highlight(int dir, int n) {
    int h = highlight;
    if (h < top || h > top + VISIBLE - 1)
        h = dir > 0 ? top : top + VISIBLE - 1;  // out of sight (scrolled away): back at the edge
    else
        h += dir;
    if (h < 0 || h >= count())
        return;
    if (h != highlight)
        play_sound(rows[0].button, sound_label(SE_OVER_M));
    highlight = h;
    scroll_to_show(h, n, n >= RHYTHM[0]);
}

ListWindow::Impl::Row* ListWindow::Impl::row_of(int entry) {
    for (Row& r : rows)
        if (r.entry == entry && entry >= 0)
            return &r;
    return nullptr;
}

bool ListWindow::Impl::any_focused() const {
    for (const Row& r : rows)
        if (focused(r.button))
            return true;
    return focused(up) || focused(down);
}

// the D-pad, A on the highlight and the tabs (the pointer and A on the rows are the cursor's)
void ListWindow::Impl::input() {
    u32 pressed = game::buttons_triggered(), held = game::buttons_held();
    f32 px, py;
    game::pointer_at(px, py);
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

    // A pushes the highlighted row when the pointer is away (or the D-pad has been used since it
    // last moved, and it is on none of the list's buttons): as the cursor would push it
    if ((pressed & game::PAD_A) && (!game::pointer_on_screen() || (pad_mode && !any_focused()))) {
        Row* r = row_of(highlight);
        if (r && active(r->button) && !group_busy(r->button))
            game::cursor_press(cursor, r->button);
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

void ListWindow::Impl::advance() {
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
void ListWindow::Impl::place_rows() {
    int first = (int)std::floor(pos) - 1;
    for (int k = 0; k < SLOTS; k++) {
        Row& r = rows[k];
        int e = first + (((k - first) % SLOTS) + SLOTS) % SLOTS;
        if (e < 0 || e >= count()) {
            r.entry = -1;
            show(r.root, false);
            set_active(r.button, false);
            continue;
        }
        if (r.entry != e)
            assign(r, e);
        f32 part = shown_part(pos, e);
        move(r.root, 0, FIRST_Y - ((f32)e - pos) * PITCH);
        set_alpha(r.root, (u8)(part * fade() * 255.0f + 0.5f));
        show(r.root, part > 0);
        set_active(r.button, state == State::OPEN && part >= 0.5f);
    }
}

// The D-pad's highlight shows (as the row's focused look) while the pointer is away from the
// screen, or since the D-pad was last used; a row the pointer is on shows its own focus.
void ListWindow::Impl::pose_highlight() {
    bool on = state == State::OPEN && (!game::pointer_on_screen() || pad_mode);
    for (Row& r : rows) {
        if (focused(r.button)) {
            r.posed = false;  // the button's own focus (its animation plays back when it ends)
            continue;
        }
        bool want = on && r.entry >= 0 && r.entry == highlight;
        if (want != r.posed) {
            pose(button_layout(r.button), r.focus, want ? last_frame(r.focus) : 0);
            r.posed = want;
        }
    }
}

void ListWindow::update() {
    Impl& m = *impl_;
    if (m.state == Impl::State::HIDDEN)
        return;
    if (m.state == Impl::State::CLOSING && !animating(m.window)) {
        m.state = Impl::State::HIDDEN;
        for (Impl::Row& r : m.rows)
            set_active(r.button, false);
        set_active(m.up, false);
        set_active(m.down, false);
        return;
    }
    if (m.state == Impl::State::OPEN) {
        m.input();
        // the pointer on a row: the highlight is there
        for (const Impl::Row& r : m.rows)
            if (focused(r.button) && r.entry >= 0)
                m.highlight = r.entry;
    }
    m.advance();
    m.place_rows();
    m.pose_highlight();
    set_enabled(m.up, m.top > 0);
    set_enabled(m.down, m.top < m.max_top());
    for (u32 tab : {m.up, m.down})
        set_alpha(button_root(tab), (u8)(m.fade() * 255.0f + 0.5f));
    set_active(m.up, m.state == Impl::State::OPEN);
    set_active(m.down, m.state == Impl::State::OPEN);
    layout_update(m.window);
}

void ListWindow::draw() {
    if (impl_->state != Impl::State::HIDDEN)
        layout_draw(impl_->window);
}

// A row's push: what it showed when the push started is what was chosen (the list can glide on
// during the push and give the row to another entry).
bool ListWindow::on_push_start(u32 button) {
    for (Impl::Row& r : impl_->rows) {
        if (r.button == button) {
            r.pushed_entry = r.entry;
            return true;
        }
    }
    return false;
}

bool ListWindow::on_pushed(u32 button) {
    Impl& m = *impl_;
    if (button == m.up || button == m.down)
        return true;
    for (Impl::Row& r : m.rows) {
        if (r.button != button)
            continue;
        int e = r.pushed_entry;
        r.pushed_entry = -1;
        if (e >= 0 && m.state == Impl::State::OPEN) {
            m.chosen = e;
            m.highlight = e;
        }
        return true;
    }
    return false;
}

int ListWindow::take_chosen() {
    int c = impl_->chosen;
    impl_->chosen = -1;
    return c;
}

int ListWindow::highlighted() const { return impl_->highlight; }

}  // namespace engine::ui
