// nw4r::ut::ArchiveFontBase's per-character queries (ut_ArchiveFontBase.cpp), native: every
// character a text box draws or measures is mapped to its glyph through the font's CMAP blocks (a
// linked list of code ranges each mapped directly, through a table, or by a sorted list of pairs),
// then to its widths through the CWDH blocks. (The game's two font classes, ArchiveFont and
// PackedFont, share these; the build has no RTTI, the class is told by the glyph-adjust table at
// 0x1C, which ResFontBase doesn't have.)
#include "font.h"

#include "noma_decomp.h"
#include "../snd/guest_obj.h"

namespace engine::ut {

using engine::snd::GuestObj;

namespace {

enum : u32 {
    F_VTABLE = 0x0,
    F_READ_FUNC = 0x4,    // CharStrmReader's read function: a pointer to member (12 bytes)
    F_INFO = 0x14,        // FontInformation*
    F_LAST_CODE = 0x18,   // u16: a one-entry cache
    F_LAST_INDEX = 0x1A,  // u16
    F_ADJUST = 0x1C,      // u16[sheet]: a sheet's first glyph, 0xFFFF while it isn't loaded
};
enum : u32 {
    INFO_LINE_FEED = 0x1,     // s8
    INFO_ALTER_CHAR = 0x2,    // u16
    INFO_DEFAULT_WIDTH = 0x4, // CharWidths {s8 left; u8 glyphWidth; s8 charWidth}
    INFO_TGLP = 0x8,          // u16 cells per row at 0xC, rows at 0xE
    INFO_CWDH = 0xC,
    INFO_CMAP = 0x10,
};
// CWDH: {u16 begin, end; next*; CharWidths[]}
enum : u32 { CWDH_BEGIN = 0x0, CWDH_END = 0x2, CWDH_NEXT = 0x4, CWDH_WIDTHS = 0x8 };
enum : u32 {
    CMAP_BEGIN = 0x0,     // u16
    CMAP_END = 0x2,       // u16
    CMAP_METHOD = 0x4,    // u16: 0 direct, 1 table, 2 scan
    CMAP_NEXT = 0x8,
    CMAP_INFO = 0xC,
};
const u16 INVALID = 0xFFFF;

// FindGlyphIndex(map, code): the glyph of a code inside a CMAP block's range
u16 glyph_index(u32 cmap, u32 code) {
    GuestObj c(cmap);
    switch (c.get16(CMAP_METHOD)) {
    case 0:  // direct: consecutive glyphs from a first one
        return (u16)(c.get16(CMAP_INFO) + (code - c.get16(CMAP_BEGIN)));
    case 1:  // table (a guest address: codes outside the range read around it, as the original's)
        return guest::load16(cmap + CMAP_INFO + (code - c.get16(CMAP_BEGIN)) * 2);
    case 2: {  // scan: {u16 count; {u16 code, u16 index}[count]}, sorted by code
        u32 lo = cmap + CMAP_INFO + 2, hi = cmap + CMAP_INFO + (c.get16(CMAP_INFO) - 1) * 4 + 2;
        while (lo <= hi) {
            s32 n = (s32)(hi - lo) / 4;
            u32 mid = lo + (u32)(n / 2) * 4;
            u16 mid_code = guest::load16(mid);
            if (mid_code < code)
                lo = mid + 4;
            else if (code < mid_code)
                hi = mid - 4;
            else
                return guest::load16(mid + 2);
        }
        return INVALID;
    }
    default:
        return INVALID;
    }
}

}  // namespace

// FindGlyphIndex(code): the glyph of a code (INVALID if the font has none)
u16 find_glyph_index(u32 font, u32 code) {
    GuestObj f(font);
    if (code == f.get16(F_LAST_CODE))
        return f.get16(F_LAST_INDEX);
    f.set16(F_LAST_CODE, (u16)code);
    for (u32 cmap = guest::load32(f.get32(F_INFO) + INFO_CMAP); cmap; cmap = guest::load32(cmap + CMAP_NEXT)) {
        GuestObj c(cmap);
        if (c.get16(CMAP_BEGIN) <= code && code <= c.get16(CMAP_END)) {
            u16 index = glyph_index(cmap, code);
            f.set16(F_LAST_INDEX, index);
            return index;
        }
    }
    f.set16(F_LAST_INDEX, INVALID);
    return INVALID;
}

// GetGlyphIndex: the glyph, or the font's alternative character's
u16 get_glyph_index(u32 font, u32 code) {
    u16 index = find_glyph_index(font, code);
    if (index != INVALID)
        return index;
    return guest::load16(guest::load32(font + F_INFO) + INFO_ALTER_CHAR);
}

// GetCharWidthsFromIndex: the address of a glyph's CharWidths (the font's default if no CWDH
// block covers it)
u32 char_widths_from_index(u32 font, u32 index) {
    u32 info = guest::load32(font + F_INFO);
    for (u32 cwdh = guest::load32(info + INFO_CWDH); cwdh; cwdh = guest::load32(cwdh + CWDH_NEXT)) {
        GuestObj c(cwdh);
        if (c.get16(CWDH_BEGIN) <= index && index <= c.get16(CWDH_END))
            return cwdh + CWDH_WIDTHS + (index - c.get16(CWDH_BEGIN)) * 3;
    }
    return info + INFO_DEFAULT_WIDTH;
}

// GetCharWidths: a character's widths, as the guest returns the struct (in r3: left << 24 |
// glyphWidth << 16 | charWidth << 8). A glyph on a sheet that isn't loaded gets the alternative
// character's widths.
u32 char_widths(u32 font, u32 code) {
    u16 index = get_glyph_index(font, code);
    u32 info = guest::load32(font + F_INFO);
    u32 tglp = guest::load32(info + INFO_TGLP);
    s32 sheet = NOMA_DIVW((s32)index, (s32)(guest::load16(tglp + 0xC) * guest::load16(tglp + 0xE)));
    u16 first = guest::load16(guest::load32(font + F_ADJUST) + (u32)sheet * 2);
    if (first == INVALID || (u16)(index - first) == INVALID)
        index = guest::load16(info + INFO_ALTER_CHAR);
    u32 w = char_widths_from_index(font, index);
    return (u32)guest::load16(w) << 16 | (u32)guest::load8(w + 2) << 8;
}

s32 line_feed(u32 font) {
    return (s8)guest::load8(guest::load32(font + F_INFO) + INFO_LINE_FEED);
}

// Font::GetCharWidth through the vtable: native while it is ArchiveFontBase's
s32 char_width(u32 font, u32 code) {
    u32 vt = guest::load32(font + F_VTABLE);
    u32 fn = guest::load32(vt + 0x48);
    if (fn != 0x80488A50u)
        return (s32)gcall<u32>(fn, font, code);
    u32 widths_fn = guest::load32(vt + 0x4C);
    u32 w = widths_fn == 0x804894B0u ? char_widths(font, code) : gcall<u32>(widths_fn, font, code);
    return (s8)(w >> 8);
}

// Font::GetLineFeed through the vtable
s32 font_line_feed(u32 font) {
    u32 fn = guest::load32(guest::load32(font + F_VTABLE) + 0x34);
    return fn == 0x80488930u ? line_feed(font) : (s32)gcall<u32>(fn, font);
}

// TextWriterBase::GetLineHeight: the line spacing plus the font's line feed scaled
f32 line_height(u32 writer) {
    GuestObj w(writer);
    u32 font = w.get32(0x48);
    s32 feed = font ? font_line_feed(font) : 0;
    return w.getf(0x54) + (f32)feed * w.getf(0x28);
}

}  // namespace engine::ut

// ---- the guest ABI (decl.h)
namespace g {

using namespace engine::ut;

u32 nw4r_ut_ArchiveFontBase_GetGlyphIndex(u32 font, u32 code) {
    return get_glyph_index(font, code);
}

// FindGlyphIndex(map, code)
u32 nw4r_ut_ArchiveFontBase_FindGlyphIndex(u32 /*font*/, u32 cmap, u32 code) {
    return glyph_index(cmap, code);
}

// FindGlyphIndex(code)
u32 nw4r_ut_ArchiveFontBase_FindGlyphIndexByCode(u32 font, u32 code) {
    return find_glyph_index(font, code);
}

u32 nw4r_ut_ArchiveFontBase_GetCharWidthsFromIndex(u32 font, u32 index) {
    return char_widths_from_index(font, index);
}

u32 nw4r_ut_ArchiveFontBase_GetCharWidths(u32 font, u32 code) {
    return char_widths(font, code);
}

u32 nw4r_ut_ArchiveFontBase_GetCharWidth(u32 font, u32 code, u32, u32, u32, u32, u32, u32, f64, f64,
                                         f64, f64, f64, f64, f64, f64) {
    u32 fn = guest::load32(guest::load32(font) + 0x4C);
    u32 w = fn == 0x804894B0u ? char_widths(font, code) : gcall<u32>(fn, font, code);
    return (u32)(s32)(s8)(w >> 8);
}

u32 nw4r_ut_ArchiveFontBase_GetLineFeed(u32 font) {
    return (u32)line_feed(font);
}

f64 nw4r_ut_TextWriterBase_GetLineHeight(u32 writer, u32, u32, u32, u32, u32, u32, u32, f64, f64, f64,
                                         f64, f64, f64, f64, f64) {
    return line_height(writer);
}

// CharStrmReader::ReadNextCharUTF16
u32 nw4r_ut_CharStrmReader_ReadNextCharUTF16(u32 reader) {
    u32 pos = guest::load32(reader);
    guest::store32(reader, pos + 2);
    return guest::load16(pos);
}

}  // namespace g
