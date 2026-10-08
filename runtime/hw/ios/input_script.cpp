// NOMA_INPUT: scripted Wii Remote input, "TIME:BUTTONS[:SECONDS],..." -- e.g. "3:A,6.5:AB:1"
// presses A at 3 s and A and B together for one second at 6.5 s (a press lasts 0.15 s by default).
// Buttons, written together: A B 1 2 + - H(ome) U D L R (U D L R: the D-pad; "+" is the PLUS
// button, not a separator: "A+B" holds A, PLUS and B), and the 3DS's own [L] [R] [ZL] [ZR]
// (shoulder buttons, which only native scenes read: "5:[L]:2" holds L for 2 s); the 3DS's names
// for the others, as the platform maps them: [X] (= 1), [Y] (= 2), [START] (= +), [SELECT] (= -).
// "T(x;y)" touches the bottom screen at pixel x, y (0..319, 0..239) for the event's seconds (a tap:
// 0.15 s), and the pointer goes where the touch points, as the 3DS's own touch moves it;
// "T(x0;y0;x1;y1)" drags from x0, y0 to x1, y1 over the event's seconds;
// "P(x;y)" moves the pointer to x, y (-1..1) once, as a touch
// would: the platform's own input (the touch screen, the circle pad) moves it on; "P(off)" takes it
// off the screen (a remote pointed away, or the 3DS's pointer left alone) until the next P(x;y);
// "OFF" presses the Wii's power button once (what closing from the 3DS's HOME Menu does).
// "S(x;y)" tilts the stick to x, y (-1..1, y up: what the circle pad gives a native scene in stick
// mode) and "C(x;y)" the camera stick (the C-stick): held for the event's seconds when given
// ("4:S(0;1):2.5" walks forward for 2.5 s), else until the next S (or C) event; "S(0;0)" lets go.
// Times are on the game's
// clock: the frames it has finished, 60 to a second (so a script reaches the same screens
// however fast the machine runs the game); both platforms merge the script into their input.
// A time can also count from a text: "M0x10255+1.5:A" presses A 1.5 s (game time) after the
// game first asked for message 0x10255 (group << 16 | index, as NOMA_DEBUG_MSG=1 logs them).
// Loading makes screens come up at other game times on other machines; their texts are the same.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/input/remote.h"
#include "../hw.h"
#include "ios.h"

namespace guest {

namespace {

enum : u8 { STICK_NONE, STICK_MOVE, STICK_CAMERA };

struct ScriptEvent {
    double t, len;
    u16 buttons, pad;
    bool point, power, off;
    u8 stick;          // STICK_: an S or C event
    bool until_next;   // (a stick event with no seconds)
    float x, y;
    bool after_message;  // t counts from the first time the game asked for message `message`
    u32 message;
    bool touch;          // a T event: from x, y to x1, y1 (bottom-screen pixels) over len
    float x1, y1;
};

std::unordered_set<u32> s_awaited;            // the messages the script counts from
std::unordered_map<u32, double> s_first_asked;  // message -> game time it was first asked for

const std::vector<ScriptEvent>& script() {
    static std::vector<ScriptEvent> s;
    static bool parsed;
    if (parsed)
        return s;
    parsed = true;
    const char* e = getenv("NOMA_INPUT");
    if (!e)
        return s;
    std::string sc = e;
    size_t p = 0;
    while (p < sc.size()) {
        size_t q = sc.find(',', p);
        if (q == std::string::npos)
            q = sc.size();
        std::string item = sc.substr(p, q - p);
        p = q + 1;
        ScriptEvent ev{0, 0.15, 0, 0, false, false, false, STICK_NONE, false, 0, 0, false, 0, false, 0, 0};
        size_t c1 = item.find(':');
        if (c1 == std::string::npos)
            continue;
        std::string when = item.substr(0, c1);
        if (!when.empty() && when[0] == 'M') {
            char* end;
            ev.message = (u32)strtoul(when.c_str() + 1, &end, 0);
            ev.after_message = true;
            ev.t = *end == '+' ? atof(end + 1) : 0;
            s_awaited.insert(ev.message);
        } else {
            ev.t = atof(when.c_str());
        }
        std::string what = item.substr(c1 + 1);
        size_t c2 = what.find(':');
        bool timed = c2 != std::string::npos;
        if (timed) {
            ev.len = atof(what.substr(c2 + 1).c_str());
            what = what.substr(0, c2);
        }
        if (!what.empty() && (what[0] == 'S' || what[0] == 'C') && what.size() > 1 && what[1] == '(') {
            ev.stick = what[0] == 'S' ? STICK_MOVE : STICK_CAMERA;
            ev.until_next = !timed;
            sscanf(what.c_str() + 1, "(%f;%f)", &ev.x, &ev.y);
        } else if (what.size() > 1 && what[0] == 'T' && what[1] == '(') {
            ev.touch = true;
            if (sscanf(what.c_str(), "T(%f;%f;%f;%f)", &ev.x, &ev.y, &ev.x1, &ev.y1) != 4)
                ev.x1 = ev.x, ev.y1 = ev.y;
        } else if (what == "OFF") {
            ev.power = true;
        } else if (what == "P(off)") {
            ev.point = ev.off = true;
        } else if (!what.empty() && what[0] == 'P') {
            ev.point = true;
            sscanf(what.c_str(), "P(%f;%f)", &ev.x, &ev.y);
        } else {
            for (size_t i = 0; i < what.size(); i++) {
                char ch = what[i];
                if (ch == '[') {
                    size_t close = what.find(']', i);
                    if (close == std::string::npos)
                        break;
                    std::string name = what.substr(i + 1, close - i - 1);
                    if (name == "L") ev.pad |= PAD_L;
                    if (name == "R") ev.pad |= PAD_R;
                    if (name == "ZL") ev.pad |= PAD_ZL;
                    if (name == "ZR") ev.pad |= PAD_ZR;
                    if (name == "X") ev.buttons |= WM_ONE;
                    if (name == "Y") ev.buttons |= WM_TWO;
                    if (name == "START") ev.buttons |= WM_PLUS;
                    if (name == "SELECT") ev.buttons |= WM_MINUS;
                    i = close;
                    continue;
                }
                switch (ch) {
                case 'A': ev.buttons |= WM_A; break;
                case 'B': ev.buttons |= WM_B; break;
                case '1': ev.buttons |= WM_ONE; break;
                case '2': ev.buttons |= WM_TWO; break;
                case '+': ev.buttons |= WM_PLUS; break;
                case '-': ev.buttons |= WM_MINUS; break;
                case 'H': ev.buttons |= WM_HOME; break;
                case 'U': ev.buttons |= WM_UP; break;
                case 'D': ev.buttons |= WM_DOWN; break;
                case 'L': ev.buttons |= WM_LEFT; break;
                case 'R': ev.buttons |= WM_RIGHT; break;
                }
            }
        }
        s.push_back(ev);
    }
    return s;
}

// a message's UTF-16 text as UTF-8, on one line; escape sequences (0x1A, then their length in
// bytes: colours, sizes) as <>, the font's own characters (button icons: U+E042 is A) as [E042]
std::string message_text(u32 a) {
    std::string out;
    for (u32 n = 0; n < 200; n++) {
        u16 c = load16(a);
        if (c == 0)
            break;
        if (c == 0x1A) {
            u8 len = load8(a + 2);
            out += "<>";
            a += len ? len : 2;
            continue;
        }
        a += 2;
        char pua[8];
        if (c >= 0xE000 && c < 0xF900)
            snprintf(pua, sizeof(pua), "[%04X]", c), out += pua;
        else if (c == '\n')
            out += " / ";
        else if (c < 0x80)
            out += (char)c;
        else if (c < 0x800)
            out += (char)(0xC0 | c >> 6), out += (char)(0x80 | (c & 0x3F));
        else
            out += (char)(0xE0 | c >> 12), out += (char)(0x80 | (c >> 6 & 0x3F)), out += (char)(0x80 | (c & 0x3F));
    }
    return out;
}

}  // namespace

bool input_script_active() { return !script().empty(); }

void input_script_message(u32 id, u32 text) {
    static const bool s_log = getenv("NOMA_DEBUG_MSG") != nullptr;
    script();  // (s_awaited)
    if (!s_log && !s_awaited.count(id))
        return;
    if (s_first_asked.count(id))
        return;
    double t = vi_game_frames() / 60.0;
    s_first_asked[id] = t;
    log("msg: 0x%X at %.2f s: %s", id, t, text ? message_text(text).c_str() : "(none)");
}

void input_script_apply(WiimoteInput& in) {
    double t = vi_game_frames() / 60.0;
    const std::vector<ScriptEvent>& s = script();
    static std::vector<bool> s_moved;  // the P and OFF events that have happened
    static bool s_off;                 // the pointer is off the screen (P(off), until P(x;y))
    s_moved.resize(s.size());
    // each stick follows the latest of its events to have started (the later one in the script if
    // two start together); once a timed one is over, the stick is the platform's own again (centred
    // on the host; on the 3DS the circle pad and the C-stick, which writing 0 here would have kept
    // dead for the rest of the run, as P(x;y) leaves the pointer to the touch screen)
    const ScriptEvent* stick[3] = {nullptr, nullptr, nullptr};
    double stick_at[3] = {0, 0, 0};
    for (size_t i = 0; i < s.size(); i++) {
        const ScriptEvent& ev = s[i];
        double at = ev.t;
        if (ev.after_message) {
            auto m = s_first_asked.find(ev.message);
            if (m == s_first_asked.end())
                continue;
            at += m->second;
        }
        if (ev.touch) {
            if (t >= at && t < at + ev.len) {
                const float f = ev.len > 0 ? (float)((t - at) / ev.len) : 0.0f;
                float px = ev.x + (ev.x1 - ev.x) * f, py = ev.y + (ev.y1 - ev.y) * f;
                px = px < 0 ? 0 : px > 319 ? 319 : px;
                py = py < 0 ? 0 : py > 239 ? 239 : py;
                in.touching = true;
                in.touch_x = (u16)px, in.touch_y = (u16)py;
                in.x = px / 160.0f - 1.0f, in.y = py / 120.0f - 1.0f;  // (as platform/3ds/main.cpp's touch)
                s_off = false;
            }
        } else if (ev.stick) {
            if (t >= at && (!stick[ev.stick] || at >= stick_at[ev.stick]))
                stick[ev.stick] = &ev, stick_at[ev.stick] = at;
        } else if (ev.power) {
            if (t >= at && !s_moved[i])
                stm_power_event(), s_moved[i] = true;
        } else if (ev.point) {
            if (t >= at && !s_moved[i]) {
                if (!ev.off)
                    in.x = ev.x, in.y = ev.y;
                s_off = ev.off, s_moved[i] = true;
            }
        } else if (t >= at && t < at + ev.len) {
            in.buttons |= ev.buttons;
            in.pad |= ev.pad;
        }
    }
    for (u8 k : {STICK_MOVE, STICK_CAMERA}) {
        const ScriptEvent* ev = stick[k];
        if (!ev || !(ev->until_next || t < stick_at[k] + ev->len))
            continue;
        if (k == STICK_MOVE)
            in.stick_x = ev->x, in.stick_y = ev->y;
        else
            in.cam_x = ev->x, in.cam_y = ev->y;
    }
    if (s_off)
        in.pointing = false;
}

}  // namespace guest
