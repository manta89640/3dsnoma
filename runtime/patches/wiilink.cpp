// Code that WiiLink patched into Wii no Ma, written by hand because it doesn't follow the
// calling convention.
//
// wii_Strap_LoadImage loads the "strap" (health and safety) image for the console's language. WiiLink
// replaced the instructions that chose the language with a call to their own WiiLink_GetStrapLanguageCode, which
// hands the buffer back in r3 and the language code ("jp", "En", ...) in r6, then falls into
// the snprintf-like lib_snprintf(buf, size, "strapImage_%s_LZ.bin", code). The size argument
// (r4) is whatever SCGetLanguage happened to leave in r4; the buffer is 0x20 bytes.
#include "noma_decomp.h"

namespace g {

// language code strings in .data: "jp", "En", "Ge", "Fr", "Sp", "It", "Du"
static u32 strap_language_code(u32 lang) {
    switch (lang) {
    case 0: return 0x806ECA00;  // Japanese
    case 2: return 0x806ECA06;  // German
    case 3: return 0x806ECA09;  // French
    case 4: return 0x806ECA0C;  // Spanish
    case 5: return 0x806ECA0F;  // Italian
    case 6: return 0x806ECA12;  // Dutch
    default: return 0x806ECA03;  // English
    }
}

u32 WiiLink_GetStrapLanguageCode(u32 r3) { return r3; }  // only ever called from wii_Strap_LoadImage below

void wii_Strap_LoadImage(u32 r3) {
    GFRAME(0x90);
    u32 file = (u32)&GS(M2C_UNK, 0x30);
    u32 name = (u32)&GS(M2C_UNK, 0x10);
    wii_Cnt_ctor(file);
    lib_snprintf(name, 0x20, (u32)gp<void>(GA_str_strapImage_s_LZ_bin), strap_language_code(SCGetLanguage()), 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    wii_Cnt_Open(file, name, 0xD, 1);
    lib_allocator_ScopedAlignment_ctor((u32)&GS(M2C_UNK, 0x8), 0x20);
    M2C_FIELD(r3, gp<u32>, 0x4) = operator_new_array(wii_Cnt_GetSize(file));
    lib_allocator_ScopedAlignment_dtor((u32)&GS(M2C_UNK, 0x8), -1U);
    wii_Cnt_Read(file, M2C_FIELD(r3, gp<u32>, 0x4), wii_Cnt_GetSize(file));
    wii_Cnt_Close(file);
    TPLBind(M2C_FIELD(r3, gp<u32>, 0x4));
    wii_Cnt_dtor(file, -1U);
}

}  // namespace g
