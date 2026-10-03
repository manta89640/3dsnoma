// The channel's software keyboard, native: the 3DS's own keyboard takes its place.
//
// On the Wii, every scene that takes text (the TV programme and paylist searches, messages, PINs,
// parental control, the browser's forms, Flash content) holds a keyboard object (0x468 bytes, in the
// scene) that brings the SDK's software keyboard with it: a 4.49 MB heap, the ATOK dictionaries
// (contents 6, 7, 8), a sound archive and its layout, all loaded when the scene is made, whether
// or not anything is typed. Scenes reach it only through the functions below, so here the object
// keeps just what they set (mode, text, title, lengths) and the two callbacks they hand it. Open
// asks the platform for the text (the 3DS: the system keyboard applet; see platform_text_input).
//
// The result goes to the scene on the next Update, as the Wii's keyboard gives it after its close
// animation, never inside Open: scenes do things right after Open that the callbacks undo (the
// message editor locks its cursor, the Flash glue marks its request as open).
//
// Layout of the object (guest memory, big-endian), in the space the SDK's keyboard used to take:
//   +0x000 u8  result waiting for Update     +0x001 u8 OK (else cancelled)
//   +0x004 text buffer (UTF-16, MAX_LENGTH + 1 units, from the game's heap)
//   +0x008 max length (UTF-16 units)         +0x00C max lines
//   +0x010 title (the scene's UTF-16 string)
//   +0x02C callback "decided" (the OK/cancel press), +0x098 callback "closed": the game's
//          std::tr1::function<void(bool)> wrappers (0x6C bytes each), as in the SDK keyboard
//   +0x460 mode (KeyboardMode)               +0x464 the scene's cursor object, while open
#include "noma_decomp.h"
#include "runtime/hw/gx/gx.h"
#include "runtime/runtime.h"

#include <string>

namespace {

const u32 PENDING = 0x000, OK = 0x001, TEXT = 0x004, MAX_LEN = 0x008, MAX_LINES = 0x00C, TITLE = 0x010;
const u32 ON_DECIDE = 0x02C, ON_CLOSED = 0x098, MODE = 0x460, CURSOR = 0x464;
const u32 MAX_LENGTH = 0x3FF, MAX_LINE_COUNT = 9999;

u32 field(u32 k, u32 off) { return guest::load32(k + off); }
// (the game's own functions these call keep the register ABI's full argument list)
void invoke(u32 fn, u32 ok) { g::keyboard_Callback_Invoke(fn, ok, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
void set_field(u32 k, u32 off, u32 v) { guest::store32(k + off, v); }

// guest UTF-16BE (NUL-terminated, at most max units) <-> UTF-8
std::string utf8_of(u32 at, u32 max) {
    std::string s;
    for (u32 i = 0; at && i < max; i++) {
        u32 c = guest::load16(at + 2 * i);
        if (!c)
            break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < max) {
            u32 lo = guest::load16(at + 2 * i + 2);
            if (lo >= 0xDC00 && lo < 0xE000)
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00), i++;
        }
        if (c < 0x80) {
            s += (char)c;
        } else if (c < 0x800) {
            s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += (char)(0xE0 | c >> 12), s += (char)(0x80 | ((c >> 6) & 0x3F)), s += (char)(0x80 | (c & 0x3F));
        } else {
            s += (char)(0xF0 | c >> 18), s += (char)(0x80 | ((c >> 12) & 0x3F));
            s += (char)(0x80 | ((c >> 6) & 0x3F)), s += (char)(0x80 | (c & 0x3F));
        }
    }
    return s;
}

// writes at most max units and the NUL; a surrogate pair is never cut in half
void put_utf16(u32 at, const std::string& s, u32 max) {
    u32 n = 0;
    for (size_t i = 0; i < s.size();) {
        u8 b = (u8)s[i];
        u32 c, len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        if (i + len > s.size())
            break;
        c = len == 1 ? b : b & (0xFF >> (len + 1));
        for (u32 j = 1; j < len; j++)
            c = c << 6 | ((u8)s[i + j] & 0x3F);
        i += len;
        if (c >= 0x10000) {
            if (n + 2 > max)
                break;
            guest::store16(at + 2 * n++, (u16)(0xD800 + ((c - 0x10000) >> 10)));
            guest::store16(at + 2 * n++, (u16)(0xDC00 + ((c - 0x10000) & 0x3FF)));
        } else {
            if (n + 1 > max)
                break;
            guest::store16(at + 2 * n++, (u16)c);
        }
    }
    guest::store16(at + 2 * n, 0);
}

}  // namespace

namespace g {

void keyboard_Keyboard_ctor(u32 k, u32 mode) {
    guest::memset_g(k, 0, 0x468);
    set_field(k, TEXT, operator_new_array((MAX_LENGTH + 1) * 2));
    guest::store16(field(k, TEXT), 0);
    set_field(k, MAX_LEN, MAX_LENGTH);
    set_field(k, MAX_LINES, MAX_LINE_COUNT);
    keyboard_Callback_ctor(k + ON_DECIDE);
    keyboard_Callback_ctor(k + ON_CLOSED);
    set_field(k, MODE, mode);
    // the newest keyboard, which a Flash player made after it uses (flash::keyboard::KeyboardController)
    GV(u32, GA_keyboard_Keyboard_sCurrent) = k;
}

void keyboard_Keyboard_dtor(u32 k, u32 del) {
    if (!k)
        return;
    GV(u32, GA_keyboard_Keyboard_sCurrent) = 0;
    keyboard_Callback_dtor(k + ON_CLOSED, (u32)-1);
    keyboard_Callback_dtor(k + ON_DECIDE, (u32)-1);
    operator_delete_array(field(k, TEXT));
    if ((s16)del > 0)
        operator_delete(k);
}

// every frame, open or not: true on the frame the "closed" callback runs
u32 keyboard_Keyboard_Update(u32 k) {
    if (!guest::load8(k + PENDING))
        return 0;
    guest::store8(k + PENDING, 0);
    u32 ok = guest::load8(k + OK);
    invoke(k + ON_DECIDE, ok);
    invoke(k + ON_CLOSED, ok);
    set_field(k, CURSOR, 0);
    return 1;
}

u32 keyboard_Keyboard_Draw(u32) { return 0; }

u32 keyboard_Keyboard_Open(u32 k, u32 cursor) {
    set_field(k, CURSOR, cursor);
    guest::TextInput in;
    u32 mode = field(k, MODE);
    in.kind = mode == 1 || mode == 2 || mode == 3 ? guest::TextInput::NUMBERS
              : mode == 4 || mode == 6 || mode == 7 || mode == 8 ? guest::TextInput::LATIN
                                                                  : guest::TextInput::ANY;
    in.secret = mode == 1 || mode == 6 || mode == 8;
    in.max_length = field(k, MAX_LEN);
    in.multiline = mode != 5 && field(k, MAX_LINES) > 1;
    in.text = utf8_of(field(k, TEXT), MAX_LENGTH);
    in.title = utf8_of(field(k, TITLE), 256);
    guest::gx_end_frame_for_applet();  // (the GPU idle, no frame open, while the keyboard has the screens)
    std::string out;
    bool ok = guest::platform_text_input(in, out);
    guest::log("keyboard: mode %u, max %u: %s", mode, in.max_length, ok ? "entered" : "cancelled");
    if (ok)
        put_utf16(field(k, TEXT), out, field(k, MAX_LEN));
    guest::store8(k + OK, ok);
    guest::store8(k + PENDING, 1);
    return 0;
}

// (into k + ON_DECIDE / ON_CLOSED: the SDK keyboard's own setters, which take the object they fill)
void keyboard_Keyboard_SetOnDecide(u32 k, u32 fn) { keyboard_Impl_SetOnDecide(k, fn, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
void keyboard_Keyboard_SetOnClosed(u32 k, u32 fn) { keyboard_Impl_SetOnClosed(k, fn, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
void keyboard_Keyboard_SetMode(u32 k, u32 mode) { set_field(k, MODE, mode); }

void keyboard_Keyboard_SetText(u32 k, u32 text) {
    u32 buf = field(k, TEXT), max = field(k, MAX_LEN), n = 0;
    for (u16 c; text && n < max && (c = guest::load16(text + 2 * n)); n++)
        guest::store16(buf + 2 * n, c);
    guest::store16(buf + 2 * n, 0);
}

u32 keyboard_Keyboard_GetText(u32 k) { return field(k, TEXT); }
void keyboard_Keyboard_SetTitle(u32 k, u32 title) { set_field(k, TITLE, title); }
void keyboard_Keyboard_SetMaxLines(u32 k, u32 n) { set_field(k, MAX_LINES, n > MAX_LINE_COUNT ? MAX_LINE_COUNT : n); }
void keyboard_Keyboard_SetMaxLength(u32 k, u32 n) { set_field(k, MAX_LEN, n > MAX_LENGTH ? MAX_LENGTH : n); }
u32 keyboard_Keyboard_GetDefaultMaxLength() { return MAX_LENGTH; }

}  // namespace g
