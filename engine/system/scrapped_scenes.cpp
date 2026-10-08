// The channel's scenes the 3DS port leaves out (the user's call for the 3DS redesign): いろんな間
// (CompanySelect, CompanyLogo, CompanyRoom, CompanyPicture, CompanyContact), the shop
// (ShoppingRoom), the TV programmes (Tv), the votes (VoteRoom, VideoVoteRoom, PictureVoteRoom),
// deliveries and coupons (DeliveryService, DeliveryConfirm, CouponConfirm), the browser and its
// links (Browser, UrlLinkConfirm), and the Wii's online and settings checks (ConfirmUpdateAgree,
// ConfirmParentalControl and its movie-profile variant, Password, InputRegion, RegionFail).
//
// Their factories are still made with the others (app_SceneFactories_ctor) and stay on the scene
// manager's list, but their Create is this: nothing of the scene is built, and the code that only
// these scenes reached isn't compiled (tools/decomp/decompile.py SCRAPPED_SCENES: about 3,500
// functions are stubs). Nothing leads to them any more: the room bar's buttons for the shop,
// いろんな間 and the TV programmes are hidden (runtime/patches/menus.cpp), and the rest were reached
// from server content, which the port, being offline, never has. Reaching one is a bug: it stops
// here with the scene's name rather than as a stub returning no scene.
#include "noma_decomp.h"

namespace g {

namespace {
[[noreturn]] void scrapped(const char* scene) {
    guest::fatal("the %s isn't part of the 3DS port (engine/system/scrapped_scenes.cpp)", scene);
}
}  // namespace

u32 scene_CompanySelectScene_Factory_Create(u32) { scrapped("CompanySelectScene (いろんな間)"); }
u32 scene_CompanyLogoScene_Factory_Create(u32, u32, u32, u32, u32, u32, u32, u32, f64, f64, f64, f64, f64, f64,
                                          f64, f64) {
    scrapped("CompanyLogoScene (いろんな間)");
}
u32 scene_CompanyRoomScene_Factory_Create(u32, u32) { scrapped("CompanyRoomScene (いろんな間)"); }
u32 scene_CompanyPictureScene_Factory_Create(u32) { scrapped("CompanyPictureScene (いろんな間)"); }
u32 scene_CompanyContactScene_Factory_Create() { scrapped("CompanyContactScene (いろんな間)"); }
u32 scene_ShoppingRoomScene_Factory_Create(u32) { scrapped("ShoppingRoomScene (the shop)"); }
u32 scene_TvScene_Factory_Create(u32) { scrapped("TvScene (the TV programmes)"); }
u32 scene_VoteRoomScene_Factory_Create(u32) { scrapped("VoteRoomScene"); }
u32 scene_VideoVoteRoomScene_Factory_Create(u32) { scrapped("VideoVoteRoomScene"); }
u32 scene_PictureVoteRoomScene_Factory_Create(u32) { scrapped("PictureVoteRoomScene"); }
u32 scene_DeliveryServiceScene_Factory_Create(u32) { scrapped("DeliveryServiceScene"); }
u32 scene_DeliveryConfirmScene_Factory_Create(u32) { scrapped("DeliveryConfirmScene"); }
u32 scene_CouponConfirmScene_Factory_Create(u32) { scrapped("CouponConfirmScene"); }
u32 scene_BrowserScene_Factory_Create(u32) { scrapped("BrowserScene"); }
u32 scene_UrlLinkConfirmScene_Factory_Create(u32) { scrapped("UrlLinkConfirmScene"); }
u32 scene_ConfirmUpdateAgreeScene_Factory_Create() { scrapped("ConfirmUpdateAgreeScene"); }
u32 scene_ConfirmParentalControlScene_Factory_Create(u32) { scrapped("ConfirmParentalControlScene"); }
u32 scene_ConfirmParentalControlMovieProfileScene_Factory_Create(u32) {
    scrapped("ConfirmParentalControlMovieProfileScene");
}
u32 scene_PasswordScene_Factory_Create() { scrapped("PasswordScene"); }
u32 scene_InputRegionScene_Factory_Create() { scrapped("InputRegionScene"); }
u32 scene_RegionFailScene_Factory_Create(u32) { scrapped("RegionFailScene"); }

}  // namespace g
