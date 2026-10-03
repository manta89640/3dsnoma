// NOMA_INPUT: scripted Wii Remote input, "TIME:BUTTONS[:SECONDS],..." -- e.g. "3:A,6.5:A+B:1"
// presses A at 3 s and A+B for one second at 6.5 s (a press lasts 0.15 s by default).
// Buttons: A B 1 2 + - H(ome) U D L R; "P(x;y)" moves the pointer to x, y (-1..1) once, as a touch
// would: the platform's own input (the touch screen, the circle pad) moves it on; "OFF" presses the
// Wii's power button once (what closing from the 3DS's HOME Menu does). Times are on the game's
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

struct ScriptEvent {
    double t, len;
    u16 buttons;
    bool point, power;
    float x, y;
    bool after_message;  // t counts from the first time the game asked for message `message`
    u32 message;
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
        ScriptEvent ev{0, 0.15, 0, false, false, 0, 0, false, 0};
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
        if (c2 != std::string::npos) {
            ev.len = atof(what.substr(c2 + 1).c_str());
            what = what.substr(0, c2);
        }
        if (what == "OFF") {
            ev.power = true;
        } else if (!what.empty() && what[0] == 'P') {
            ev.point = true;
            sscanf(what.c_str(), "P(%f;%f)", &ev.x, &ev.y);
        } else {
            for (char ch : what) {
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
    s_moved.resize(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        const ScriptEvent& ev = s[i];
        double at = ev.t;
        if (ev.after_message) {
            auto m = s_first_asked.find(ev.message);
            if (m == s_first_asked.end())
                continue;
            at += m->second;
        }
        if (ev.power) {
            if (t >= at && !s_moved[i])
                stm_power_event(), s_moved[i] = true;
        } else if (ev.point) {
            if (t >= at && !s_moved[i])
                in.x = ev.x, in.y = ev.y, s_moved[i] = true;
        } else if (t >= at && t < at + ev.len) {
            in.buttons |= ev.buttons;
        }
    }
}

}  // namespace guest
