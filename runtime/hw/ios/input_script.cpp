// NOMA_INPUT: scripted Wii Remote input, "TIME:BUTTONS[:SECONDS],..." -- e.g. "3:A,6.5:A+B:1"
// presses A at 3 s and A+B for one second at 6.5 s (a press lasts 0.15 s by default).
// Buttons: A B 1 2 + - H(ome) U D L R; "P(x;y)" points at x, y (-1..1). Times are on the game's
// clock: the frames it has finished, 60 to a second (so a script reaches the same screens
// however fast the machine runs the game); both platforms merge the script into their input.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bt.h"
#include "../hw.h"

namespace guest {

namespace {

struct ScriptEvent {
    double t, len;
    u16 buttons;
    bool point;
    float x, y;
};

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
        ScriptEvent ev{0, 0.15, 0, false, 0, 0};
        size_t c1 = item.find(':');
        if (c1 == std::string::npos)
            continue;
        ev.t = atof(item.substr(0, c1).c_str());
        std::string what = item.substr(c1 + 1);
        size_t c2 = what.find(':');
        if (c2 != std::string::npos) {
            ev.len = atof(what.substr(c2 + 1).c_str());
            what = what.substr(0, c2);
        }
        if (!what.empty() && what[0] == 'P') {
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

}  // namespace

bool input_script_active() { return !script().empty(); }

void input_script_apply(WiimoteInput& in) {
    double t = vi_game_frames() / 60.0;
    for (const ScriptEvent& ev : script()) {
        if (ev.point && t >= ev.t)
            in.x = ev.x, in.y = ev.y;
        else if (!ev.point && t >= ev.t && t < ev.t + ev.len)
            in.buttons |= ev.buttons;
    }
}

}  // namespace guest
