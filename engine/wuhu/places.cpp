// The places the exploration menu can warp to (see places.h).
#include "engine/wuhu/places.h"

#include "engine/g3d/package.h"
#include "engine/ui/art.h"
#include "engine/ut/font.h"

namespace engine::wuhu {

namespace {

const u16 NO_GLYPH = 0xFFFF;              // engine::ut::find_glyph_index's "the font has none"
const char16_t REPLACEMENT = 0xFF1F;      // ？
// the full-width forms of ASCII ! .. ~ (U+FF01 .. U+FF5E), and the full-width space
const char16_t FULL_FIRST = 0xFF01, FULL_LAST = 0xFF5E, FULL_OFFSET = 0xFF01 - 0x21, FULL_SPACE = 0x3000;

// UTF-8 to UTF-16 (the Basic Multilingual Plane, which the font's codes are; anything else, or a
// broken sequence, is the replacement)
std::u16string utf16_of(const char* s) {
    std::u16string out;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        u32 c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (c >= 0x80 && !more) {
            out += REPLACEMENT;
            continue;
        }
        c &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0x7F;
        bool ok = true;
        for (int i = 0; i < more; i++) {
            if ((*p & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            c = c << 6 | (*p++ & 0x3F);
        }
        out += ok && c < 0x10000 ? (char16_t)c : REPLACEMENT;
    }
    return out;
}

// the character's full- or half-width twin (0: none)
char16_t twin(char16_t c) {
    if (c >= 0x21 && c <= 0x7E)
        return (char16_t)(c + FULL_OFFSET);
    if (c >= FULL_FIRST && c <= FULL_LAST)
        return (char16_t)(c - FULL_OFFSET);
    if (c == 0x20)
        return FULL_SPACE;
    if (c == FULL_SPACE)
        return 0x20;
    return 0;
}

// the name as the menu lists it (see places.h)
std::u16string display_name(const char* utf8, u32 font) {
    std::u16string name = utf16_of(utf8);
    size_t digits = 0;
    while (digits < name.size() && name[digits] >= u'0' && name[digits] <= u'9')
        digits++;
    if (digits && digits + 1 < name.size() && name[digits] == u'_')
        name.erase(0, digits + 1);
    if (!font)
        return name;
    for (char16_t& c : name) {
        if (engine::ut::find_glyph_index(font, c) != NO_GLYPH)
            continue;
        char16_t t = twin(c);
        char16_t with = t && engine::ut::find_glyph_index(font, t) != NO_GLYPH ? t : REPLACEMENT;
        guest::log("wuhu: place \"%s\": the font has no U+%04X, set as U+%04X", utf8, (unsigned)c, (unsigned)with);
        c = with;
    }
    return name;
}

// nothing to read: empty, or spaces only
bool blank(const std::u16string& s) {
    for (char16_t c : s)
        if (c != 0x20 && c != FULL_SPACE)
            return false;
    return true;
}

}  // namespace

std::vector<Place> warp_places(const engine::g3d::Package& package) {
    const u32 font = engine::ui::art::system_font();
    std::vector<Place> places;
    const engine::g3d::Span<engine::g3d::fmt::Marker> markers = package.markers();
    for (u32 i = 0; i < markers.count; i++) {
        if (markers[i].kind != MARKER_WARP)
            continue;
        std::u16string name = display_name(package.str(markers[i].name), font);
        if (blank(name)) {
            guest::log("wuhu: warp marker %u has no name to list: left out", (unsigned)i);
            continue;
        }
        places.push_back({i, name});
    }
    return places;
}

}  // namespace engine::wuhu
