// nw4r::lyt's text measuring (lyt_textBox.cpp, CalcLineRectImpl<wchar_t>), native: the extent of
// one line of a text box and how many characters fit in a width. Every text box measures its
// string line by line whenever it lays out, so this is the per-character inner loop of the UI.
// Characters are read and measured natively (UTF-16 and the game's ArchiveFontBase fonts); control
// characters go to the writer's tag processor, a guest virtual, as in the original.
#include "noma_decomp.h"
#include "../snd/guest_obj.h"
#include "../ut/font.h"

namespace engine::lyt {

using engine::snd::GuestObj;

namespace {

// TextWriterBase<wchar_t> (CharWriter's fields first)
enum : u32 {
    W_SCALE_H = 0x24,       // f32
    W_CURSOR_X = 0x2C,      // f32
    W_WIDTH_FIXED = 0x43,   // u8
    W_FIXED_WIDTH = 0x44,   // f32
    W_FONT = 0x48,
    W_CHAR_SPACE = 0x50,    // f32
    W_TAG_PROCESSOR = 0x60,
};
// Font's CharStrmReader read function, a pointer to member: {s32 this offset; s32 vtable offset
// (-1: not virtual); function, or the vtable pointer's offset}
enum : u32 { FONT_READ_FUNC = 0x4 };
const u32 READ_UTF16 = 0x80487420u;  // CharStrmReader::ReadNextCharUTF16

// the guest frame the tag processor sees: as the original's, as it gets pointers into it
enum : u32 {
    FR_RECT = 0x8,      // ut::Rect {left, top, right, bottom}
    FR_READER = 0x28,   // CharStrmReader {pos; read function}
    FR_CONTEXT = 0x38,  // PrintContext<wchar_t> {writer; str; x; y; flags}
    FR_SIZE = 0xD0,
};
// TagProcessorBase::CalcRect's result
enum : u32 { OP_NO_CHAR_SPACE = 1, OP_CHAR_SPACE = 2, OP_NEXT_LINE = 3, OP_END_DRAW = 4 };

struct Rect {
    f32 left, top, right, bottom;
    void load(u32 a) {
        GuestObj r(a);
        left = r.getf(0x0), top = r.getf(0x4), right = r.getf(0x8), bottom = r.getf(0xC);
    }
    void store(u32 a) const {
        GuestObj r(a);
        r.setf(0x0, left), r.setf(0x4, top), r.setf(0x8, right), r.setf(0xC, bottom);
    }
};

// The font's character reader. UTF-16 strings are read here; any other encoding's function is
// called through the pointer to member as __ptmf_scall would, on the reader in the guest frame.
struct Reader {
    u32 addr;  // the reader in the guest frame
    u32 pos;
    s32 this_offset, vtable_offset;
    u32 fn;
    bool utf16;

    Reader(u32 addr, u32 font) : addr(addr), pos(0) {
        GuestObj f(font);
        this_offset = f.gets32(FONT_READ_FUNC);
        vtable_offset = f.gets32(FONT_READ_FUNC + 4);
        fn = f.get32(FONT_READ_FUNC + 8);
        utf16 = this_offset == 0 && vtable_offset < 0 && fn == READ_UTF16;
        guest::store32(addr + 4, (u32)this_offset);
        guest::store32(addr + 8, (u32)vtable_offset);
        guest::store32(addr + 12, fn);
    }
    u32 next() {
        if (utf16) {
            u32 c = guest::load16(pos);
            pos += 2;
            return c;
        }
        guest::store32(addr, pos);
        u32 self = addr + (u32)this_offset, f = fn;
        if (vtable_offset >= 0)
            f = guest::load32(guest::load32(self + fn) + (u32)vtable_offset);
        u32 c = gcall<u32>(f, self);
        pos = guest::load32(addr);
        return c;
    }
};

// ut::Min/Max as the original compares (a NaN keeps the second)
inline f32 min_f(f32 a, f32 b) { return a > b ? b : a; }
inline f32 max_f(f32 a, f32 b) { return a < b ? b : a; }

s32 chars(u32 from, u32 to) { return (s32)(to - from) / 2; }

// the extent of the line at str (at most `length` characters) and the characters measured;
// *over is set if it is wider than max_width (then the characters that fit, and their extent)
u32 calc_line_rect(u32 rect_addr, u32 writer, u32 str, s32 length, u32 over, f64 max_width) {
    GFRAME(FR_SIZE);
    const u32 frame = _gframe.sp;
    GuestObj w(writer);
    GuestObj context(frame + FR_CONTEXT);
    context.set32(0x0, writer);
    context.set32(0x4, str);
    context.set32(0x8, 0);
    context.set32(0xC, 0);
    context.set32(0x10, 0);

    Reader reader(frame + FR_READER, w.get32(W_FONT));
    u32 prev_pos = 0;  // the reader's position before the last character (none yet)
    f32 x = 0.0f;
    bool char_space = false;

    Rect rect;
    rect.left = rect.right = 0.0f;
    f32 line_height = engine::ut::line_height(writer);
    rect.top = 0.0f > line_height ? line_height : 0.0f;
    line_height = engine::ut::line_height(writer);
    rect.bottom = 0.0f < line_height ? line_height : 0.0f;
    guest::store8(over, 0);
    reader.pos = str;
    Rect prev_rect = rect;

    bool overflow = false;
    for (u32 code = reader.next(); chars(str, reader.pos) <= length;) {
        if ((u16)code < 0x20) {
            // a tag: the tag processor measures it, and may consume more of the string
            GuestObj r(frame + FR_RECT);
            r.setf(0x0, x), r.setf(0x4, 0.0f), r.setf(0x8, 0.0f), r.setf(0xC, 0.0f);
            context.set32(0x4, reader.pos);
            context.set32(0x10, !char_space);
            w.setf(W_CURSOR_X, x);
            u32 tp = w.get32(W_TAG_PROCESSOR);
            u32 op = gcall<u32>(guest::load32(guest::load32(tp) + 0x10), tp, frame + FR_RECT, (u32)(u16)code,
                                frame + FR_CONTEXT, reader.pos);
            reader.pos = context.get32(0x4);
            Rect tag;
            tag.load(frame + FR_RECT);
            rect.left = min_f(rect.left, tag.left);
            rect.top = min_f(rect.top, tag.top);
            rect.right = max_f(rect.right, tag.right);
            rect.bottom = max_f(rect.bottom, tag.bottom);
            x = w.getf(W_CURSOR_X);
            if ((f64)(rect.right - rect.left) > max_width) {
                overflow = true;
                break;
            }
            if (op == OP_END_DRAW) {
                rect.store(rect_addr);
                return (u32)length;
            }
            if (op == OP_NO_CHAR_SPACE)
                char_space = false;
            else if (op == OP_CHAR_SPACE)
                char_space = true;
            else if (op == OP_NEXT_LINE)
                break;
        } else {
            if (char_space)
                x += w.getf(W_CHAR_SPACE);
            char_space = true;
            if (w.get8(W_WIDTH_FIXED))
                x += w.getf(W_FIXED_WIDTH);
            else {
                f32 scale = w.getf(W_SCALE_H);
                x += (f32)engine::ut::char_width(w.get32(W_FONT), (u16)code) * scale;
            }
            rect.left = min_f(rect.left, x);
            rect.right = max_f(rect.right, x);
            if ((f64)(rect.right - rect.left) > max_width) {
                overflow = true;
                break;
            }
        }
        prev_pos = reader.pos;
        code = reader.next();
        prev_rect = rect;
    }

    if (overflow) {
        guest::store8(over, 1);
        if (prev_pos) {
            prev_rect.store(rect_addr);
            return (u32)chars(str, prev_pos);
        }
    }
    rect.store(rect_addr);
    return (u32)chars(str, reader.pos);
}

}  // namespace

}  // namespace engine::lyt

// ---- the guest ABI (decl.h)
namespace g {

u32 nw4r_lyt_CalcLineRectImpl(u32 rect, u32 writer, u32 str, u32 length, u32 over, f64 max_width) {
    return engine::lyt::calc_line_rect(rect, writer, str, (s32)length, over, max_width);
}

}  // namespace g
