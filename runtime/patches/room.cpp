// The room (scene::LivingRoomScene) on the 3DS's two screens: the room on the top one, its buttons
// on the bottom one (engine/room/panel.h). The room's own bar and その他 are hidden and out of
// reach (runtime/patches/menus.cpp, layout_living_MoveBtn_ctor). The
// first start's setup, before the room, is whole on the bottom screen.

#include "engine/room/panel.h"
#include "noma_decomp.h"

namespace g {

u32 scene_LivingRoomScene_ctor_orig(u32 r3);
u32 scene_LivingRoomScene_dtor_orig(u32 r3, u32 r4);
u32 scene_LivingRoomScene_Update_orig(u32 r3);
u32 scene_LivingRoomScene_Draw_orig(u32 r3);
u32 scene_FirstSetupScene_ctor_orig(u32 r3, u32 r4);

u32 scene_LivingRoomScene_ctor(u32 r3) {
    engine::room::back();  // (the room's layouts for the top screen's 5:3 again, before it makes them)
    u32 r = scene_LivingRoomScene_ctor_orig(r3);
    engine::room::open(r3);
    return r;
}

u32 scene_LivingRoomScene_dtor(u32 r3, u32 r4) {
    engine::room::close();
    return scene_LivingRoomScene_dtor_orig(r3, r4);
}

u32 scene_LivingRoomScene_Update(u32 r3) {
    u32 r = scene_LivingRoomScene_Update_orig(r3);
    engine::room::update();
    return r;
}

// The room draws its 3D, its layouts, then the cursor's (its hand hidden: overlays.cpp), then the
// panel on the bottom screen.
u32 scene_LivingRoomScene_Draw(u32 r3) {
    u32 r = scene_LivingRoomScene_Draw_orig(r3);
    engine::room::draw();
    return r;
}

// The first start's setup: whole on the bottom screen, as the room's other screens are
u32 scene_FirstSetupScene_ctor(u32 r3, u32 r4) {
    engine::room::away_for_setup();
    return scene_FirstSetupScene_ctor_orig(r3, r4);
}

}  // namespace g
