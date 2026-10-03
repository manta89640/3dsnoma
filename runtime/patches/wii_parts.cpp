// Parts of the channel that are about the Wii itself, left out on the 3DS (the rest of the program
// is unchanged; each original stays as <name>_orig).
#include <algorithm>

#include "noma_decomp.h"

namespace g {

u32 scene_TitleScene_ctor_orig(u32 r3, u32 r4, u32 r5, u32 r6);
u32 scene_PreTitleScene_ctor_orig(u32 r3, u32 r4);
void wii_MessageData_Load_orig(u32 r3);

// The Wii Remote strap screen ("use the strap"), shown at the start until a button is pressed or
// 20 seconds have passed (wii_Strap_*): there's no strap on a 3DS. The scenes that show it are
// made without it (r5 for the title scene, r4 for the pre-title one), as they are when the
// channel comes back to them: the title scene plays the start's jingle and the opening at once,
// as it would have when the strap screen went, and the pre-title one moves on when it has loaded.
u32 scene_TitleScene_ctor(u32 r3, u32 r4, u32, u32 r6) { return scene_TitleScene_ctor_orig(r3, r4, 0, r6); }
u32 scene_PreTitleScene_ctor(u32 r3, u32) { return scene_PreTitleScene_ctor_orig(r3, 0); }

// The first start's setup: after the family's Miis, the video quality (StateVideoOption: high or
// normal quality for streaming over the internet, with samples to watch). Left out: the setting
// keeps its default (the scene's +0x8), and the setup goes on to its end (StateFinish).
u32 scene_FirstSetupScene_StateConfirmAddMii_OnEnd(u32 r3) {
    u32 scene = M2C_FIELD(r3, gp<u32>, 0x4) + 0x100000;
    M2C_FIELD(scene, gp<gp<void>>, 0x10) = GFN(scene_FirstSetupScene_StateFinish_Create);
    return scene;
}

// The step before (the family registered) ends its message with "続いて映像の設定を行います。"
// ("next, the video settings"), after a blank line. The channel's texts (msg/message.lex, a BMG,
// loaded here once) have that message end before the blank line.
void wii_MessageData_Load(u32 r3) {
    wii_MessageData_Load_orig(r3);
    u32 bmg = M2C_FIELD(r3, gp<u32>, 0x0);
    const u8* p = guest::host_ptr(bmg);
    if (!p)
        return;
    u32 size = guest::load32(bmg + 0x8);
    static const char16_t text[] = u"\n\n続いて映像の設定を行います。";
    u8 be[sizeof(text) - 2];  // (big-endian, without the terminator)
    for (size_t i = 0; i + 1 < sizeof(text) / 2; i++)
        be[2 * i] = (u8)(text[i] >> 8), be[2 * i + 1] = (u8)text[i];
    const u8* hit = std::search(p, p + size, be, be + sizeof(be));
    if (hit != p + size)
        guest::store16(bmg + (u32)(hit - p), 0);
}

}  // namespace g
