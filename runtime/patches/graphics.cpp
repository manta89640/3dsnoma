// The game's frame renderer (the object at App + 0x30): how a frame reaches the 3DS's GPU.
//
// On the Wii it draws in anti-aliasing mode (+4 = 1): the EFB then holds only half the picture,
// so app_Renderer_DrawPasses draws the whole scene twice, as two horizontal bands with their own
// scissor and EFB copy. The 3DS's GPU draws the frame in one pass, so the second band was all
// waste (every draw, transform and vertex twice) and its overlap rows blended twice. The renderer
// always uses its one-pass mode (+4 = 0), which seven scenes already chose for themselves.
#include "noma_decomp.h"

namespace g {

void app_Renderer_ctor_orig(u32 r3);

void app_Renderer_ctor(u32 r3) {
    app_Renderer_ctor_orig(r3);
    M2C_FIELD(r3, gp<s32>, 0x4) = 0;
}

void app_Renderer_SetAntiAlias(u32 r3, u32 r4) {
    (void)r4;
    M2C_FIELD(r3, gp<s32>, 0x4) = 0;
    M2C_FIELD(r3, gp<s8>, 0xC) = 1;  // (the mode is applied again, as the original does)
}

}  // namespace g
