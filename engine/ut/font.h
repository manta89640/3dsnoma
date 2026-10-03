// nw4r::ut font queries, native (font.cpp): for the text layout and drawing natives, which call
// them per character without a trip through the guest ABI.
#pragma once

#include "noma_decomp.h"

namespace engine::ut {

u16 find_glyph_index(u32 font, u32 code);
u16 get_glyph_index(u32 font, u32 code);
u32 char_widths_from_index(u32 font, u32 index);
u32 char_widths(u32 font, u32 code);
s32 line_feed(u32 font);

// through the font's vtable (native while it is ArchiveFontBase's)
s32 char_width(u32 font, u32 code);
s32 font_line_feed(u32 font);

// TextWriterBase<wchar_t>::GetLineHeight
f32 line_height(u32 writer);

}  // namespace engine::ut
