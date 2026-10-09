// nw4r::lyt's TextBox::DrawSelf (0x8045B880), native: a text box's glyphs as quads of the layout's
// packet (draw.h). The decompiled draw made a TextWriter, measured the string line by line, set
// nw4r::ut CharWriter's GX state and sent every glyph as a GX quad of its own (GXBegin, four
// vertices, a texture load whenever the glyph's sheet changed). Here the same layout arithmetic runs
// on a writer of our own: each glyph becomes four vertices transformed as GX's transform unit would
// have transformed them, drawn with the state CharWriter::SetupGX leaves, as register values.
//
// The string is read as UTF-16 from guest memory. The font is the channel's ArchiveFont (wbf1, every
// text box's): its glyphs and widths come from engine/ut/font.h and are kept in a cache, as the font
// never changes once it is built; each glyph sheet's texture words are made once, by the GX
// library's own functions. Control characters follow the default tag processor's rules (a new line,
// a tab), done here. A text box with another font class or its own tag processor goes to the guest
// (false).
//
// Every floating-point step is the decompiled code's, in its order and in the m2c output's expression
// shape (one expression where m2c wrote one, separate statements where it wrote several, f64 where it
// computed in f64): the host compiler fuses a*b+c within an expression, so written alike, the host's
// floats come out the same as the decompiled draw's.
#include <vector>

#include "draw.h"
#include "noma_decomp.h"
#include "../snd/guest_obj.h"
#include "../ut/font.h"

namespace engine::lyt {

namespace {

using engine::snd::GuestObj;

// ---- the text box and its font in guest memory

// nw4r::lyt::TextBox (a Pane)
enum : u32 {
    TB_MATERIAL = 0x28,        // Material*: only its TEV colours 0 and 1 are read (the colour mapping)
    TB_SIZE_X = 0x4C,          // f32: the box's size: its width breaks the lines, both place the text
    TB_SIZE_Y = 0x50,
    TB_ALPHA = 0xB9,           // u8: the pane's global alpha
    TB_BASE_POSITION = 0xBA,   // u8: the pane's origin, horizontal + 3 * vertical
    TB_TEXT = 0xD8,            // the string (UTF-16)
    TB_COLOR_TOP = 0xDC,       // ut::Color (RGBA): the text's colour at the top of a glyph
    TB_COLOR_BOTTOM = 0xE0,    // and at its bottom
    TB_FONT = 0xE4,
    TB_FONT_WIDTH = 0xE8,      // f32: the font size
    TB_FONT_HEIGHT = 0xEC,
    TB_LINE_SPACE = 0xF0,      // f32
    TB_CHAR_SPACE = 0xF4,      // f32
    TB_TAG_PROCESSOR = 0xF8,   // 0: the writer keeps its default one
    TB_TEXT_LEN = 0xFE,        // u16: the characters drawn
    TB_TEXT_POSITION = 0x100,  // u8: the text block's place in the box, horizontal + 3 * vertical
    TB_BITS = 0x101,           // u8: bits 5-6, the lines' alignment (0 as the text position, 1 left,
                               // 2 centre, 3 right)
};

// nw4r::ut::ArchiveFont: its FontInformation (FINF) and FontTextureGlyph (TGLP) blocks hold the
// metrics and the glyph sheets
enum : u32 {
    FONT_VTABLE = 0x0,
    FONT_READ_FUNC = 0x4,  // CharStrmReader's read function, a pointer to member: {s32 this offset;
                           // s32 vtable offset (-1: not virtual); function}
    FONT_INFO = 0x14,
    FONT_ADJUST = 0x1C,    // u16 a sheet: its first glyph's index, 0xFFFF while it isn't loaded
};
enum : u32 {
    INFO_LINE_FEED = 0x1,   // s8
    INFO_ALTER_CHAR = 0x2,  // u16: the glyph drawn for a character the font lacks
    INFO_TGLP = 0x8,
    INFO_HEIGHT = 0x14,     // u8
    INFO_WIDTH = 0x15,      // u8
    INFO_ASCENT = 0x16,     // u8
};
enum : u32 {
    TGLP_CELL_WIDTH = 0x0,     // u8
    TGLP_CELL_HEIGHT = 0x1,    // u8: also the height of every glyph's quad
    TGLP_BASELINE = 0x2,       // s8
    TGLP_SHEET_SIZE = 0x4,     // u32: bytes a sheet
    TGLP_SHEET_FORMAT = 0xA,   // u16: GXTexFmt
    TGLP_SHEET_ROW = 0xC,      // u16: cells across a sheet
    TGLP_SHEET_LINE = 0xE,     // u16: cells down
    TGLP_SHEET_WIDTH = 0x10,   // u16: texels
    TGLP_SHEET_HEIGHT = 0x12,  // u16
    TGLP_SHEET_IMAGE = 0x14,   // the first sheet's image; the others follow it
};
// ArchiveFont's vtable (nw4r_ut_ArchiveFont_vtable: nw4r_ut_ArchiveFont_vtable); TextWriterBase<wchar_t>'s default
// tag processor (nw4r_ut_TextWriterBaseW_sDefaultTagProcessor: nw4r_ut_TextWriterBaseW_sDefaultTagProcessor) and its class's vtable,
// TagProcessorBase<wchar_t>'s (nw4r_ut_TagProcessorBaseW_vtable: nw4r_ut_TagProcessorBaseW_vtable)
const u32 ARCHIVE_FONT_VTABLE = GA_nw4r_ut_ArchiveFont_vtable;
const u32 DEFAULT_TAG_PROCESSOR = GA_nw4r_ut_TextWriterBaseW_sDefaultTagProcessor;
const u32 TAG_PROCESSOR_VTABLE = GA_nw4r_ut_TagProcessorBaseW_vtable;

// L"\n" (nw4r_lyt_TextBox_sNewLine: nw4r_lyt_TextBox_sNewLine), printed after a line that was broken for its width
const u8 NEW_LINE[4] = {0x00, 0x0A, 0x00, 0x00};

// a UTF-16 code unit of a big-endian string in host memory
inline u32 text_code(const u8* s, s32 i) { return (u32)s[2 * i] << 8 | s[2 * i + 1]; }

// The font as a text box's draw reads it. Plain words, no padding: compared whole to tell whether the
// glyph cache below still holds this font's glyphs.
struct Font {
    u32 addr, info, tglp;
    s32 width, height, ascent, line_feed, baseline;
    u32 cell_width, cell_height;
    u32 sheet_size, sheet_format, sheet_row, sheet_line, sheet_width, sheet_height, sheet_image;
    u32 adjust, alter;
};

// false: not an ArchiveFont reading UTF-16 (the font's metrics and glyphs would come from other
// functions than the ones done here)
bool read_font(u32 addr, Font& f) {
    const GuestObj font(addr);
    if (font.get32(FONT_VTABLE) != ARCHIVE_FONT_VTABLE || font.get32(FONT_READ_FUNC) != 0 ||
        font.gets32(FONT_READ_FUNC + 4) >= 0 || font.get32(FONT_READ_FUNC + 8) != GA_nw4r_ut_CharStrmReader_ReadNextCharUTF16)
        return false;
    f.addr = addr;
    f.info = font.get32(FONT_INFO);
    const GuestObj info(f.info);
    f.tglp = info.get32(INFO_TGLP);
    const GuestObj tglp(f.tglp);
    // ResFontBase's GetWidth, GetHeight, GetAscent, GetLineFeed and GetBaselinePos
    f.width = info.get8(INFO_WIDTH);
    f.height = info.get8(INFO_HEIGHT);
    f.ascent = info.get8(INFO_ASCENT);
    f.line_feed = (s8)info.get8(INFO_LINE_FEED);
    f.baseline = (s8)tglp.get8(TGLP_BASELINE);
    f.cell_width = tglp.get8(TGLP_CELL_WIDTH);
    f.cell_height = tglp.get8(TGLP_CELL_HEIGHT);
    f.sheet_size = tglp.get32(TGLP_SHEET_SIZE);
    f.sheet_format = tglp.get16(TGLP_SHEET_FORMAT);  // also ResFontBase::GetTextureFormat
    f.sheet_row = tglp.get16(TGLP_SHEET_ROW);
    f.sheet_line = tglp.get16(TGLP_SHEET_LINE);
    f.sheet_width = tglp.get16(TGLP_SHEET_WIDTH);
    f.sheet_height = tglp.get16(TGLP_SHEET_HEIGHT);
    f.sheet_image = tglp.get32(TGLP_SHEET_IMAGE);
    f.adjust = font.get32(FONT_ADJUST);
    f.alter = info.get16(INFO_ALTER_CHAR);
    return true;
}

// ---- glyphs

// One character's glyph as ArchiveFont::GetGlyph (0x8048B0F0) gives it and CharWriter::PrintGlyph
// uses it: its sheet's image, its widths, and the texture coordinates of its cell's corners as
// PrintGlyph sends them (u16 with 15 fraction bits), dequantised as GX's vertex decode does
// (runtime/hw/gx/vertex.cpp)
struct Glyph {
    u32 sheet;
    f32 s0, t0, s1, t1;
    s32 left, char_width;  // s8 in the font
    u32 glyph_width;       // u8
};
struct GlyphSlot {
    u32 code;  // 0: empty (glyphs are only looked up for characters from 0x20)
    Glyph glyph;
};
constexpr u32 GLYPH_SLOTS = 512;  // direct-mapped by the character's low bits
GlyphSlot s_glyphs[GLYPH_SLOTS];
Font s_glyph_font;  // the font the slots hold glyphs of

inline f32 texcoord(u32 v) { return (f32)(u16)v * (1.0f / (f32)(1u << 15)); }

// ArchiveFontBase::AdjustIndex (nw4r_ut_ArchiveFontBase_AdjustIndex): a glyph's index among the loaded sheets' glyphs, 0xFFFF
// if its sheet isn't loaded
u32 adjust_index(const Font& f, u32 index) {
    const u16 first = guest::load16(f.adjust + (u32)NOMA_DIVW((s32)index, (s32)(f.sheet_row * f.sheet_line)) * 2);
    if (first != 0xFFFF)
        return (u16)(index - first);
    return 0xFFFF;
}

// The glyph of character `code` (GetGlyph: GetGlyphIndex, then GetGlyphFromIndex, the alternate
// character's glyph where the character's sheet isn't loaded). Its char width is also what
// Font::GetCharWidth gives for it (engine/ut/font.cpp char_widths: the same choice of glyph).
const Glyph& glyph(const Font& f, u32 code) {
    GlyphSlot& slot = s_glyphs[code & (GLYPH_SLOTS - 1)];
    if (slot.code == code)
        return slot.glyph;
    u32 index = engine::ut::get_glyph_index(f.addr, code);
    u32 c = adjust_index(f, index);
    if ((u16)c == 0xFFFF) {
        index = f.alter;
        c = adjust_index(f, index);
    }
    const u32 cells = f.sheet_row * f.sheet_line;
    const u32 cell = NOMA_MODWU(c, cells);
    const u32 cell_x = (u16)(NOMA_MODWU(cell, f.sheet_row) * (f.cell_width + 1) + 1);  // past a 1-texel border
    const u32 cell_y = (u16)(NOMA_DIVWU(cell, f.sheet_row) * (f.cell_height + 1) + 1);
    const u32 widths = engine::ut::char_widths_from_index(f.addr, index);
    Glyph& g = slot.glyph;
    g.sheet = f.sheet_image + NOMA_DIVWU((u16)c, cells) * f.sheet_size;
    g.left = (s8)guest::load8(widths);
    g.glyph_width = guest::load8(widths + 1);
    g.char_width = (s8)guest::load8(widths + 2);
    g.s0 = texcoord(NOMA_DIVWU(cell_x << 15, f.sheet_width));
    g.t0 = texcoord(NOMA_DIVWU(cell_y << 15, f.sheet_height));
    g.s1 = texcoord(NOMA_DIVWU((cell_x + g.glyph_width) << 15, f.sheet_width));
    g.t1 = texcoord(NOMA_DIVWU((cell_y + f.cell_height) << 15, f.sheet_height));
    slot.code = code;
    return g;
}

// A glyph sheet as CharWriter::PrintGlyph loads it into texture map 0 (GXInitTexObj: clamped both
// ways, no mipmaps; GXInitTexObjLOD: the writer's filters, linear both ways, LOD 0 and no bias), made
// by the GX library's own functions on a guest frame. Kept by the sheet's address: a font's sheets
// stay where they are, and the words depend on nothing else that changes.
struct SheetTex {
    bool valid;
    u32 image, format, width, height;
    LytTex tex;
};
SheetTex s_sheet_tex[128];  // by the image's address / 64 KB (wbf1's sheets are 64 KB each)

const LytTex& sheet_tex(const Font& f, u32 image) {
    SheetTex& e = s_sheet_tex[(image >> 16) & 127];
    if (!e.valid || e.image != image || e.format != f.sheet_format || e.width != f.sheet_width ||
        e.height != f.sheet_height) {
        const u32 GX_CLAMP = 0, GX_LINEAR = 1;
        GuestFrame frame(0x40);
        const u32 obj = frame.sp + 0x18;
        g::GXInitTexObj(obj, image, f.sheet_width, f.sheet_height, f.sheet_format, GX_CLAMP, GX_CLAMP, 0);
        g::GXInitTexObjLOD(obj, GX_LINEAR, GX_LINEAR, 0, 0, 0, 0.0f, 0.0f, 0.0f);
        e.tex = tex_from_obj(obj);
        e.valid = true, e.image = image, e.format = f.sheet_format, e.width = f.sheet_width, e.height = f.sheet_height;
    }
    return e.tex;
}

// ---- the writer

// TextWriterBase<wchar_t> as DrawSelf sets it up, the part a text box's drawing reads: the font's
// scale, the cursor, the spacing. Its other fields keep the constructor's values for text boxes: no
// fixed width, no width limit, tab width 4, no draw flags, cursor z 0, alpha 255.
constexpr s32 TAB_WIDTH = 4;

struct Writer {
    const Font* font;
    f32 scale_x, scale_y;
    f32 cursor_x, cursor_y;
    f32 char_space, line_space;

    // GetLineHeight: engine/ut/font.cpp's line_height (the native the decompiled code calls), the
    // same expression
    f32 line_height() const { return line_space + (f32)font->line_feed * scale_y; }
    // CharWriter's GetFontWidth, GetFontHeight and GetFontAscent (nw4r_ut_CharWriter_GetFontWidth, nw4r_ut_CharWriter_GetFontHeight,
    // nw4r_ut_CharWriter_GetFontAscent), in f64 as they return it
    f64 font_width() const { return (f64)(scale_x * (f32)font->width); }
    f64 font_height() const { return (f64)(scale_y * (f32)font->height); }
    f64 font_ascent() const { return (f64)(scale_y * (f32)font->ascent); }

    // the default tag processor's new line (Process and CalcRect at 0x0A): back to the line's start
    // x0, down one line
    void new_line(f32 x0) {
        const f32 y = cursor_y;
        const f32 height = line_height();
        cursor_x = x0;
        cursor_y = y + height;
    }
    // the default tag processor's tab (Process and CalcRect at 0x09): on to the next multiple of four
    // font widths from the line's start x0
    void tab(f32 x0) {
        const f32 tab = (f32)TAB_WIDTH * (f32)font_width();
        cursor_x = x0 + (tab * (f32)(s32)((u32)NOMA_FCTIWZ((cursor_x - x0) / tab) + 1u));
    }
};

// ---- measuring (as GetTextDrawRect measures)

struct Rect {
    f32 left, top, right, bottom;
};
// ut::Min and ut::Max as nw4r compares (a NaN takes the second)
inline f32 min_f(f32 a, f32 b) { return a > b ? b : a; }
inline f32 max_f(f32 a, f32 b) { return a < b ? b : a; }
void merge(Rect& r, const Rect& o) {
    r.left = min_f(r.left, o.left);
    r.top = min_f(r.top, o.top);
    r.right = max_f(r.right, o.right);
    r.bottom = max_f(r.bottom, o.bottom);
}
// a tag's rectangle made right side up as the tag processor's CalcRect does it, by fsel (a NaN
// difference takes the second): the bottom as computed, the rest as stored
void normalize(Rect& r, f32 bottom) {
    const f32 dy = bottom - r.top, dx = r.right - r.left;
    const f32 left = r.left, top = r.top, right = r.right;
    r.left = (f64)dx >= 0.0 ? left : right;
    r.right = (f64)dx >= 0.0 ? right : left;
    r.top = (f64)dy >= 0.0 ? top : bottom;
    r.bottom = (f64)dy >= 0.0 ? bottom : top;
}

// lyt's CalcLineRectImpl (engine/lyt/textbox.cpp calc_line_rect, the native the decompiled code
// calls; the same steps) on our writer, with the default tag processor's CalcRect (nw4r_ut_TagProcessorBaseW_CalcRect)
// done here: the extent of the line at str (at most `length` characters) and the characters it takes;
// `over` if it is wider than max_width (then the characters that fit, and their extent). As the
// original, it reads one character past the ones it takes; when the line runs to the end of the
// string, that one is in the count too (DrawSelf then prints it: normally the string's terminator,
// which draws nothing).
s32 line_rect(Writer& w, const u8* str, s32 length, f64 max_width, Rect& out, bool& over) {
    s32 prev_pos = 0;  // the characters read before the last one (none yet)
    f32 x = 0.0f;
    bool char_space = false;
    Rect rect;
    rect.left = rect.right = 0.0f;
    const f32 line_height = w.line_height();
    rect.top = 0.0f > line_height ? line_height : 0.0f;
    rect.bottom = 0.0f < line_height ? line_height : 0.0f;
    over = false;
    Rect prev_rect = rect;

    bool overflow = false;
    s32 pos = 0;
    for (u32 code = text_code(str, pos++); pos <= length;) {
        if (code < 0x20) {
            // a tag: its rectangle, with the line's start (the context's x) at 0; any other control
            // character than these two leaves the rectangle as given
            Rect tag{x, 0.0f, 0.0f, 0.0f};
            w.cursor_x = x;
            if (code == 0x0A) {
                tag.right = w.cursor_x;
                tag.top = w.cursor_y;
                w.new_line(0.0f);
                tag.left = w.cursor_x;
                normalize(tag, (f32)((f64)w.cursor_y + w.font_height()));
            } else if (code == 0x09) {
                tag.left = w.cursor_x;
                w.tab(0.0f);
                tag.right = w.cursor_x;
                tag.top = w.cursor_y;
                normalize(tag, tag.top + (f32)w.font_height());
            }
            merge(rect, tag);
            x = w.cursor_x;
            if ((f64)(rect.right - rect.left) > max_width) {
                overflow = true;
                break;
            }
            if (code == 0x0A)  // the line ends with its new line
                break;
            if (code == 0x09)  // no character space after a tab
                char_space = false;
        } else {
            if (char_space)
                x += w.char_space;
            char_space = true;
            x += (f32)glyph(*w.font, code).char_width * w.scale_x;
            rect.left = min_f(rect.left, x);
            rect.right = max_f(rect.right, x);
            if ((f64)(rect.right - rect.left) > max_width) {
                overflow = true;
                break;
            }
        }
        prev_pos = pos;
        code = text_code(str, pos++);
        prev_rect = rect;
    }

    if (overflow) {
        over = true;
        if (prev_pos) {
            out = prev_rect;
            return prev_pos;
        }
    }
    out = rect;
    return pos;
}

// a line as the draw prints it: its characters, whether it was broken for its width (then a new line
// follows it), its width
struct Line {
    s32 chars;
    bool over;
    f32 width;
};
std::vector<Line> s_lines;  // the text box's, reused from draw to draw

// lyt's CalcStringRectImpl (0x8045C9C0): the whole text's extent, line by line. Each line's
// characters, break and width are kept for the draw: DrawSelf measures every line again with a fresh
// writer, which gives the same, as only the extent's top and bottom depend on the writer's cursor.
void string_rect(Writer& w, const u8* str, s32 length, f64 max_width, Rect& rect) {
    rect.left = rect.right = rect.top = rect.bottom = 0.0f;
    w.cursor_x = w.cursor_y = 0.0f;
    do {
        Rect line;
        bool over;
        const s32 n = line_rect(w, str, length, max_width, line, over);
        s_lines.push_back({n, over, line.right - line.left});
        str += n * 2;
        length -= n;
        merge(rect, line);
        if (over) {
            line_rect(w, NEW_LINE, 1, max_width, line, over);
            merge(rect, line);
        }
    } while (length > 0);
}

// ---- drawing

// GXSetTevColorIn / GXSetTevAlphaIn's inputs
enum : u32 { CC_CPREV = 0x0, CC_C0 = 0x2, CC_C1 = 0x4, CC_TEXC = 0x8, CC_RASC = 0xA, CC_ZERO = 0xF };
enum : u32 { CA_APREV = 0x0, CA_A0 = 0x1, CA_A1 = 0x2, CA_TEXA = 0x4, CA_RASA = 0x5, CA_ZERO = 0x7 };
// A stage's colour and alpha combiners as BP 0xC0 + 2i and 0xC1 + 2i hold them after GXSetTevColorIn
// (GXSetTevAlphaIn) and CharWriter's GXSetTevColorOp (GXSetTevAlphaOp): add, no bias, scale 1,
// clamped, into PREV; the alpha's swap selects 0 (GXSetTevSwapMode(s, SWAP0, SWAP0)).
constexpr u32 TEV_CLAMP = 1u << 19;
constexpr u32 color_env(u32 a, u32 b, u32 c, u32 d) { return a << 12 | b << 8 | c << 4 | d | TEV_CLAMP; }
constexpr u32 alpha_env(u32 a, u32 b, u32 c, u32 d) { return a << 13 | b << 10 | c << 7 | d << 4 | TEV_CLAMP; }
// A stage's half of TREF after GXSetTevOrder with texture coordinate and map 0 or none (both encode
// as 0): whether the map is enabled, and the rasterized colour (0: COLOR0A0, 7: none).
constexpr u32 tev_order(bool map, u32 ras) { return (map ? 1u : 0u) << 6 | ras << 7; }
// KSEL's low bits: the four swap tables as the layouts' materials leave them (0 RGBA, 1 RRRA, 2 GGGA,
// 3 BBBA; CharWriter sets table 0 to RGBA again): table t's red and green selects in KSEL 2t, its
// blue and alpha in KSEL 2t + 1.
constexpr u32 SWAP_TABLES[8] = {0 | 1 << 2, 2 | 3 << 2, 0 | 0 << 2, 0 | 3 << 2, 1 | 1 << 2, 1 | 3 << 2, 2 | 2 << 2, 2 | 3 << 2};
// GXSetAlphaCompare(ALWAYS, 0, AND, ALWAYS, 0) (BP 0xF3)
constexpr u32 ALPHA_COMPARE_ALWAYS = 7u << 16 | 7u << 19;
// GXSetBlendMode(BLEND, SRCALPHA, INVSRCALPHA, LO_SET): CMODE0's blend part
constexpr u32 BLEND_SRC_ALPHA = 1u | 5u << 5 | 4u << 8 | 0xFu << 12;

// detail::MultipleAlpha (nw4r_lyt_detail_MultipleAlpha): a colour's alpha times the pane's alpha / 255, rounded down
u32 multiple_alpha(u32 color, u32 alpha) {
    if (alpha != 0xFF)
        color = (color & ~0xFFu) | ((color & 0xFF) * alpha) / 255;
    return color;
}

// a GXColorS10 TEV colour of the material (s16 RGBA) clamped to 0..255 a component, as DrawSelf makes
// the colour mapping from the material's TEV colours 0 and 1
u32 clamp_color(const GuestObj& m, u32 o) {
    u32 c = 0;
    for (u32 k = 0; k < 4; k++) {
        const s32 v = m.gets16(o + k * 2);
        c = c << 8 | (u32)(v < 0 ? 0 : v > 0xFF ? 0xFF : v);
    }
    return c;
}

// the alignment factors of a position's part (0 left or top, 1 centre, 2 right or bottom)
inline f32 align_of(u32 part) { return part == 1 ? 0.5f : part == 2 ? 1.0f : 0.0f; }

// What CharWriter draws a text box's glyphs with: LoadMtx's matrix, the vertex colours, the GX state
// SetupGX leaves (map 0 set from each glyph's sheet).
struct Printer {
    Builder& b;
    const Font& font;
    Writer& w;
    f32 m[3][4];
    u32 colors[4];  // LT, RT, RB, LB: in the order PrintGlyph sends them
    LytState state;
    bool have_state = false;
    u32 sheet = 0, state_index = 0;

    Printer(Builder& b, const Font& font, Writer& w) : b(b), font(font), w(w) {}

    void vertex(LytVertex& v, f32 x, f32 y, u32 color, f32 s, f32 t) const {
        const f32 p[4] = {x, y, 0.0f, 1.0f};  // the writer's cursor z is 0
        mtx_apply(m, p, v.pos);
        v.color[0] = (u8)(color >> 24), v.color[1] = (u8)(color >> 16), v.color[2] = (u8)(color >> 8);
        v.color[3] = (u8)color;
        v.tc[0][0] = s, v.tc[0][1] = t;  // texture coordinate 0 through the identity
    }

    // CharWriter::Print (0x8048C1C0) and PrintGlyph (0x8048C2F0): the glyph's quad at the cursor,
    // moved in by the glyph's left margin, and the cursor on by its width. A glyph on another sheet
    // than the last one's is another state.
    void put_glyph(u32 code) {
        const Glyph& g = glyph(font, code);
        const f32 width = (f32)g.char_width * w.scale_x;
        const f32 margin = (f32)g.left * w.scale_x;
        const f32 x = w.cursor_x + margin, y = w.cursor_y;
        const f32 x1 = x + ((f32)g.glyph_width * w.scale_x);
        const f32 y1 = y + ((f32)font.cell_height * w.scale_y);
        if (!have_state || g.sheet != sheet) {
            state.tex[0] = sheet_tex(font, g.sheet);
            state_index = b.add_state(state);
            sheet = g.sheet, have_state = true;
        }
        LytVertex* v = b.add_quads(state_index, 1);
        vertex(v[0], x, y, colors[0], g.s0, g.t0);
        vertex(v[1], x1, y, colors[1], g.s1, g.t0);
        vertex(v[2], x1, y1, colors[2], g.s1, g.t1);
        vertex(v[3], x, y1, colors[3], g.s0, g.t1);
        b.glyphs++;
        w.cursor_x = w.cursor_x + width;
    }

    // TextWriterBase<wchar_t>::Print (PrintImpl, 0x80490F70) as a text box's writer runs it: no
    // width limit and no draw flags (AdjustCursor only lifts the cursor by the ascent, and the end
    // takes it back), the default tag processor, the line's start x0 where the cursor is. Reads one
    // character past the string, as the original.
    void print(const u8* str, s32 length) {
        const f32 x0 = w.cursor_x;
        w.cursor_y = w.cursor_y + (f32)w.font_ascent();
        bool char_space = false;
        s32 pos = 0;
        for (u32 code = text_code(str, pos++); pos <= length; code = text_code(str, pos++)) {
            if (code < 0x20) {
                // a tag: a new line (back to the line's start, where PrintImpl puts the cursor too)
                // or a tab ends the character space; the default processor does nothing for any
                // other control character
                if (code == 0x0A) {
                    w.new_line(x0);
                    char_space = false;
                } else if (code == 0x09) {
                    w.tab(x0);
                    char_space = false;
                }
            } else {
                const f32 base_y = w.cursor_y;
                if (char_space)
                    w.cursor_x = w.cursor_x + w.char_space;
                char_space = true;
                const f32 baseline = (f32)(-font.baseline) * w.scale_y;
                w.cursor_y = w.cursor_y + baseline;
                put_glyph(code);
                w.cursor_y = base_y;
            }
        }
        w.cursor_y = (f32)((f64)w.cursor_y - w.font_ascent());
    }
};

}  // namespace

bool draw_textbox(Builder& b, u32 pane, const DrawCtx& ctx) {
    NOMA_PROF_SCOPE("(lyt text)");
    const GuestObj tb(pane);
    const u32 text = tb.get32(TB_TEXT), font_addr = tb.get32(TB_FONT), material = tb.get32(TB_MATERIAL);
    const s32 text_len = tb.get16(TB_TEXT_LEN);
    if (!text || !font_addr || !material || text_len == 0)
        return true;  // DrawSelf draws nothing
    u32 tag_processor = tb.get32(TB_TAG_PROCESSOR);
    if (!tag_processor)
        tag_processor = DEFAULT_TAG_PROCESSOR;
    if (guest::load32(tag_processor) != TAG_PROCESSOR_VTABLE)
        return false;
    Font font;
    if (!read_font(font_addr, font))
        return false;
    const u8* str = guest::host_ptr(text);
    if (!str)
        return false;
    if (::memcmp(&s_glyph_font, &font, sizeof font)) {
        for (GlyphSlot& s : s_glyphs)
            s.code = 0;
        s_glyph_font = font;
    }

    // GetTextDrawRect (0x8045C710): the writer's font size (SetFontSize) and spacing; the text
    // block's extent placed in the box by the text position, from the pane's top left corner
    // (Pane::GetVtxPos)
    Writer w;
    w.font = &font;
    w.cursor_x = w.cursor_y = 0.0f;
    w.scale_y = tb.getf(TB_FONT_HEIGHT) / (f32)font.height;
    w.scale_x = tb.getf(TB_FONT_WIDTH) / (f32)font.width;
    w.line_space = tb.getf(TB_LINE_SPACE);
    w.char_space = tb.getf(TB_CHAR_SPACE);
    Writer measure = w;
    Rect r;
    s_lines.clear();
    string_rect(measure, str, text_len, (f64)tb.getf(TB_SIZE_X), r);
    const f32 text_h = r.bottom - r.top;
    const f32 text_w = r.right - r.left;
    const u32 base = tb.get8(TB_BASE_POSITION);
    const f32 vx = base % 3 == 1 ? -tb.getf(TB_SIZE_X) * 0.5f : base % 3 == 2 ? -tb.getf(TB_SIZE_X) : 0.0f;
    const f32 vy = base / 3 == 1 ? tb.getf(TB_SIZE_Y) * 0.5f : base / 3 == 2 ? tb.getf(TB_SIZE_Y) : 0.0f;
    const u32 position = tb.get8(TB_TEXT_POSITION);
    const f32 left = vx + ((tb.getf(TB_SIZE_X) - text_w) * align_of(position % 3));
    const f32 right = left + text_w;
    const f32 slack_y = tb.getf(TB_SIZE_Y) - text_h;
    const f32 top = vy - (slack_y * align_of(position / 3));

    Printer p(b, font, w);
    // LoadMtx (0x8045C680): the pane's matrix with its y column negated (the text runs downwards)
    pane_matrix(pane, p.m);
    for (u32 row = 0; row < 3; row++)
        p.m[row][1] = -p.m[row][1];
    // the text colours with the pane's alpha; UpdateVertexColor makes a vertical gradation of them
    // when they differ (the writer's own alpha is 255): top at the glyphs' top corners
    const u32 alpha = tb.get8(TB_ALPHA);
    const u32 top_color = multiple_alpha(tb.get32(TB_COLOR_TOP), alpha);
    const u32 bottom_color = multiple_alpha(tb.get32(TB_COLOR_BOTTOM), alpha);
    p.colors[0] = p.colors[1] = top_color;
    p.colors[2] = p.colors[3] = bottom_color;

    // CharWriter::SetupGX (0x8048B620) and DrawSelf's GXSetAlphaCompare: one colour channel from the
    // vertex colours, texture coordinate 0 through the identity, map 0 the glyph's sheet, blending by
    // the source alpha. Mapped colours (the material's TEV colours other than black and white) take
    // SetupGXWithColorMapping's two stages: lerp(min, max, texel) times the vertex colour; otherwise an
    // intensity font's stage takes its colour from the vertex and its alpha from the texel times the
    // vertex's, any other format's (GXSetTevOp's MODULATE) both from the texel times the vertex's.
    LytState& s = p.state;
    s = LytState{};  // every word set below or zero (plain words: no padding for memcmp to see)
    const GuestObj mat(material);
    const u32 min = clamp_color(mat, 0x10), max = clamp_color(mat, 0x18);
    if (min != 0 || max != 0xFFFFFFFF) {
        s.tev.n_stages = 2;
        s.tev.color_env[0] = color_env(CC_C0, CC_C1, CC_TEXC, CC_ZERO);
        s.tev.alpha_env[0] = alpha_env(CA_A0, CA_A1, CA_TEXA, CA_ZERO);
        s.tev.color_env[1] = color_env(CC_ZERO, CC_CPREV, CC_RASC, CC_ZERO);
        s.tev.alpha_env[1] = alpha_env(CA_ZERO, CA_APREV, CA_RASA, CA_ZERO);
        s.tev.tref[0] = tev_order(true, 7) | tev_order(false, 0) << 12;
        for (u32 k = 0; k < 4; k++) {
            s.tev.reg[1][k] = (s32)((min >> (24 - 8 * k)) & 0xFF);  // GXSetTevColor(GX_TEVREG0, min)
            s.tev.reg[2][k] = (s32)((max >> (24 - 8 * k)) & 0xFF);  // GXSetTevColor(GX_TEVREG1, max)
        }
    } else {
        const u32 GX_TF_I8 = 1;
        s.tev.n_stages = 1;
        if (font.sheet_format <= GX_TF_I8)
            s.tev.color_env[0] = color_env(CC_ZERO, CC_ZERO, CC_ZERO, CC_RASC);
        else
            s.tev.color_env[0] = color_env(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO);
        s.tev.alpha_env[0] = alpha_env(CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO);
        s.tev.tref[0] = tev_order(true, 0);
    }
    for (u32 k = 0; k < 8; k++)
        s.tev.ksel[k] = SWAP_TABLES[k];
    s.n_texgens = 1;
    s.n_chans = 1;
    s.n_maps = 1;
    s.alpha_compare = ALPHA_COMPARE_ALWAYS;
    s.zmode = ctx.zmode;
    s.cmode0 = BLEND_SRC_ALPHA | ctx.cmode0_keep;

    // DrawSelf's lines: each placed by the line alignment within the text block, printed, and
    // followed by a new line where it was broken for its width
    f32 align;
    switch ((tb.get8(TB_BITS) >> 5) & 3) {
    case 1: align = 0.0f; break;
    case 2: align = 0.5f; break;
    case 3: align = 1.0f; break;
    default: align = align_of(position % 3); break;
    }
    w.cursor_y = -top;
    const f32 draw_w = right - left;
    s32 remain = text_len;
    for (u32 i = 0; remain > 0; i++) {
        const Line& line = s_lines[i];
        const f32 t = align * (draw_w - line.width);
        w.cursor_x = left + t;
        p.print(str, line.chars);
        if (line.over)
            p.print(NEW_LINE, 1);
        remain -= line.chars;
        str += line.chars * 2;
    }
    return true;
}

}  // namespace engine::lyt
