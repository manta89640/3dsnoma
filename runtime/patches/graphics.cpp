// The game's frame renderer (the object at App + 0x30): how a frame reaches the 3DS's GPU.
//
// On the Wii it draws in anti-aliasing mode (+4 = 1): the EFB then holds only half the picture,
// so app_Renderer_DrawPasses draws the whole scene twice, as two horizontal bands with their own
// scissor and EFB copy. The 3DS's GPU draws the frame in one pass, so the second band was all
// waste (every draw, transform and vertex twice) and its overlap rows blended twice. The renderer
// always uses its one-pass mode (+4 = 0), which seven scenes already chose for themselves.
//
// The TV's shape. The channel draws for a 16:9 TV (render mode 7: tools/stage_data.py stages
// SYSCONF's IPL.AR as 16:9) into its 640x456 frame, which gx_pica.cpp shows on the 3DS's 400x240
// top screen: a 5:3 screen, so the 16:9 picture came out 6.25% too narrow (the opening's circles
// measured 69x74 px). Here the channel's "16:9" is the top screen's 5:3: its 3D cameras' 16:9
// becomes 5:3, the layouts' 16:9 squeeze of panes made for 4:3 (0.75) becomes 5:3's (0.8), and the
// width it gives the 16:9 picture in layout units, for placing 3D things under layout panes (832),
// becomes 5:3's (760, 608 / 0.8). The frame and its mapping to the screen don't change.
#include "engine/math/vecmath.h"
#include "noma_decomp.h"

namespace g {

void app_Renderer_ctor_orig(u32 r3);
void app_VideoMode_Set_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7);
void nw4r_g3d_Camera_GXSetProjection_orig(u32 r3);
void nw4r_g3d_Camera_ProjectToScreen_orig(u32 r3, u32 r4, u32 r5);
void nw4r_g3d_Camera_GetProjectionTexMtx_orig(u32 r3, u32 r4);

void app_Renderer_ctor(u32 r3) {
    app_Renderer_ctor_orig(r3);
    M2C_FIELD(r3, gp<s32>, 0x4) = 0;
}

void app_Renderer_SetAntiAlias(u32 r3, u32 r4) {
    (void)r4;
    M2C_FIELD(r3, gp<s32>, 0x4) = 0;
    M2C_FIELD(r3, gp<s8>, 0xC) = 1;  // (the mode is applied again, as the original does)
}

// app::VideoMode (app_VideoMode_sInstance) takes the render mode with its layout width (+4: 608 for
// 4:3, 832 for 16:9), height (+6) and whether it is wide (+8): wide, the width is 5:3's.
void app_VideoMode_Set(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7) {
    app_VideoMode_Set_orig(r3, r4, (r7 & 0xFF) ? 760 : r5, r6, r7);
}

// layout::DrawInfo's location adjust: on a wide TV, panes flagged for it (flag 0x20 at +0x50) drawn
// narrower by +0x44 (and +0x48 tall), as panes laid out for 4:3 keep their shape on a wider screen:
// 0.75 for 16:9 on the Wii, 0.8 (4:3 over 5:3) here.
void layout_DrawInfo_SetLocationAdjust(u32 r3) {
    if (guest::load8(guest::load32(GA_app_VideoMode_sInstance) + 0x8)) {
        guest::store8(r3 + 0x50, guest::load8(r3 + 0x50) | 0x20);
        engine::math::store_f32(r3 + 0x44, 0.8f);
        engine::math::store_f32(r3 + 0x48, 1.0f);
    } else {
        guest::store8(r3 + 0x50, guest::load8(r3 + 0x50) & ~0x20);
    }
}

namespace {
// nw4r::g3d::Camera (r3: its CameraData at +0): the perspective's aspect (+0xB0) of 16:9, as the
// channel sets its cameras for a 16:9 TV, made 5:3 before the camera's projection is used, and the
// projection then made again (+0x70 bit 0x80: the projection matrix is up to date).
void top_screen_aspect(u32 camera) {
    const u32 data = guest::load32(camera);
    if (!data)
        return;
    if (guest::load32(data + 0xB0) != 0x3FE38E39u)  // (16/9 as an f32, exactly)
        return;
    engine::math::store_f32(data + 0xB0, 5.0f / 3.0f);
    guest::store32(data + 0x70, guest::load32(data + 0x70) & ~0x80u);
}
}  // namespace

void nw4r_g3d_Camera_GXSetProjection(u32 r3) {
    top_screen_aspect(r3);
    nw4r_g3d_Camera_GXSetProjection_orig(r3);
}

void nw4r_g3d_Camera_ProjectToScreen(u32 r3, u32 r4, u32 r5) {
    top_screen_aspect(r3);
    nw4r_g3d_Camera_ProjectToScreen_orig(r3, r4, r5);
}

void nw4r_g3d_Camera_GetProjectionTexMtx(u32 r3, u32 r4) {
    top_screen_aspect(r3);
    nw4r_g3d_Camera_GetProjectionTexMtx_orig(r3, r4);
}

}  // namespace g
