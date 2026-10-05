// The channel's 3D pieces the native scenes' stages are made of (first the Sound Player's), with
// their arguments spelt out: the studio set (g3d::Stage), the Miis (the room's actor manager,
// behaviours, bodies, faces), the camera, and the guest memory and GX helpers the native amp needs.
//
// Built as the channel's TV programme screen (scene::TvScene, ctor scene_TvScene_ctor, Update scene_TvScene_Update,
// Draw scene_TvScene_Draw, dtor scene_TvScene_dtor) builds its studio and its caster, without its camera manager
// (the stage's camera is written directly every frame).
#pragma once

#include <string>

#include "engine/math/vecmath.h"
#include "noma_decomp.h"
#include "runtime/runtime.h"

namespace engine::ui::g3d {

// ---- guest memory

using engine::math::load_f32;
using engine::math::store_f32;

// The current heap (lib::allocator::HeapBase::sRoot +0x1C) set to `heap` while it lives: what the
// game's ScopedHeap does (lib_allocator_ScopedHeap_ctor sets it and locks the root's mutex, lib_allocator_ScopedHeap_dtor puts the last
// one back). The word that keeps the last heap is on the guest stack.
class ScopedHeap {
public:
    explicit ScopedHeap(u32 heap) : frame_(8) { g::lib_allocator_ScopedHeap_ctor(frame_.sp, heap); }
    ~ScopedHeap() { g::lib_allocator_ScopedHeap_dtor(frame_.sp, (u32)-1); }
    ScopedHeap(const ScopedHeap&) = delete;
    ScopedHeap& operator=(const ScopedHeap&) = delete;

private:
    GuestFrame frame_;
};

// A 32-byte aligned block of the current heap, its size rounded up to 32 bytes, for what the GPU
// reads (textures, display lists): lib::allocator's ScopedAlignment over the heap while the block
// is allocated. 0 if the heap has no room; freed with operator_delete.
inline u32 new_aligned(u32 size) {
    GFRAME(0x10);
    u32 scope = (u32)&GS(u32, 0x8);
    g::lib_allocator_ScopedAlignment_ctor(scope, 0x20);
    u32 p = g::operator_new((size + 31) & ~31u);
    g::lib_allocator_ScopedAlignment_dtor(scope, (u32)-1);
    return p;
}

// The current heap's name and free bytes, when it is a lib::allocator::HeapStd (vtable
// lib_allocator_HeapStd_HeapStdImpl_vtable at +0x8: blocks taken from both ends, +0x24 the low
// end's top, +0x28 the high end's; +0x14 its name): for the logs of what a scene's parts cost and
// give back (the heaps are stacks: a scene gone, the free bytes are what they were before it).
inline u32 heap_free(std::string* name) {
    u32 heap = guest::load32(guest::load32(GA_lib_allocator_HeapBase_sRoot) + 0x1C);
    *name = guest::read_string(guest::load32(heap + 0x14), 16);
    if (guest::load32(heap + 0x8) != GA_lib_allocator_HeapStd_HeapStdImpl_vtable)
        return 0;
    return guest::load32(heap + 0x28) - guest::load32(heap + 0x24);
}

// ---- g3d::Stage (0x118 bytes): a set from a .lex (BRRES) file in its own nw4r::g3d::ScnRoot.
// +0x00 the file (data, size), +0x08 the ScnRoot (current camera 1, 0x80 children), +0x0C the set's
// model (ScnMdlSimple: +0xE8 its ResMdl), +0x10 its scene animations (SCN0, none attached here),
// +0x3C the 3D sound listener (follows the current camera). The ctor sets g3d_Stage_sInstance (the stage
// the rest of the game reads), the dtor clears it.
const u32 STAGE_SIZE = 0x118;
// "g3d/Wii_Tv.lex" (the TV programme's white studio) and its model's name "Wii_Tv"
const u32 PATH_WII_TV = GA_scene_TvScene_sStrings + 0x30, MODEL_WII_TV = GA_scene_TvScene_sModelName;
// g3d_Stage_ctorByModel: g3d::Stage::Stage(path, model name, heap name or 0 for the current one)
inline void stage_ctor(u32 s, u32 path, u32 model) { g::g3d_Stage_ctorByModel(s, path, model, 0); }
inline void stage_dtor(u32 s) { g::g3d_Stage_dtor(s, (u32)-1); }
inline u32 stage_root(u32 s) { return guest::load32(s + 0x8); }
inline u32 stage_model(u32 s) { return guest::load32(s + 0xC); }
// the parts' calc (g3d_Stage_CalcParts), then the stage's (g3d_Stage_Calc: UpdateFrame, CalcWorld, CalcView,
// GatherDrawScnObj, ZSort, the sound listener)
inline void stage_calc(u32 s) {
    g::g3d_Stage_CalcParts(s);
    g::g3d_Stage_Calc(s);
}

// ---- nw4r::g3d::ScnRoot and its cameras (nw4r::g3d::Camera: a word, the CameraData's address;
// the setters take the word's address)
inline u32 camera(u32 root, u32 i) { return g::nw4r_g3d_ScnRoot_GetCamera(root, i); }      // ScnRoot::GetCamera
inline u32 current_camera(u32 root) { return g::nw4r_g3d_ScnRoot_GetCurrentCamera(root); }         // ScnRoot::GetCurrentCamera
// CameraData: +0x00 the view matrix (3x4), +0xB0 f32 aspect (the stage sets the TV's: 16:9 or 4:3)
inline f32 camera_aspect(u32 cam) { return load_f32(cam + 0xB0); }
inline void camera_set_position(u32 cam_word, u32 vec3) { g::nw4r_g3d_Camera_SetPosition(cam_word, vec3); }
// Camera::SetPosture(PostureInfo {u32 type (0: look at), vec3 up @4, vec3 target @0x10, ...})
inline void camera_set_posture(u32 cam_word, u32 posture) { g::nw4r_g3d_Camera_SetPosture(cam_word, posture); }
// Camera::SetFrustum(top, bottom, left, right, near, far): a perspective whose axis needn't be at
// the picture's centre
inline void camera_set_frustum(u32 cam_word, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    g::nw4r_g3d_Camera_SetFrustum(cam_word, 0, t, b, l, r, n, f);
}
inline void camera_load_projection(u32 cam_word) { g::nw4r_g3d_Camera_GXSetProjection(cam_word); }  // GXSetProjection
inline void camera_view_matrix(u32 cam_word, u32 mtx) { g::nw4r_g3d_Camera_GetCameraMtx(cam_word, mtx); }
// Camera::SetPerspective (fn_8044B060): a perspective about the picture's centre (vertical field of
// view in degrees, width over height, near, far); also what ScnRoot's gather culls by
inline void camera_set_perspective(u32 cam_word, f32 fovy, f32 aspect, f32 n, f32 f) {
    g::fn_8044B060(cam_word, fovy, aspect, n, f);
}

// G3DState::Invalidate: g3d sends all its GX state again before its next draw
inline void invalidate_gx_state() { g::nw4r_g3d_G3DState_Invalidate(); }
// A root's draw as g3d::Stage::Draw (g3d_Stage_Draw) does it, split around the opaque and the
// translucent passes so that native drawing can go between them: dither on, the current camera's
// viewport set to the whole screen by app::Renderer (app_Renderer_SetCameraViewport,
// app_Renderer_sInstance), ScnRoot::DrawOpa (nw4r_g3d_ScnRoot_DrawOpa) ... ScnRoot::DrawXlu
// (nw4r_g3d_ScnRoot_DrawXlu). The viewport alone (root_viewport) is what a native draw before the
// opaque pass needs.
inline void root_viewport(u32 root) {
    GFRAME(0x10);
    g::GXSetDither(1);
    GS(u32, 0x8) = current_camera(root);
    g::app_Renderer_SetCameraViewport(guest::load32(GA_app_Renderer_sInstance), (u32)&GS(u32, 0x8));
}
inline void root_draw_opaque(u32 root) {
    root_viewport(root);
    invalidate_gx_state();
    g::nw4r_g3d_ScnRoot_DrawOpa(root);
}
inline void root_draw_translucent(u32 root) {
    invalidate_gx_state();
    g::nw4r_g3d_ScnRoot_DrawXlu(root);
}
inline void stage_draw_opaque(u32 s) { root_draw_opaque(stage_root(s)); }
inline void stage_draw_translucent(u32 s) { root_draw_translucent(stage_root(s)); }

// ---- a ScnRoot of one's own, with no set: what g3d::Stage makes for its set (fn_800F9018, the
// stage's build) less the set's model and animations, for a scene whose 3D the channel's g3d draws
// only some of (the island: its Mii; the island itself is drawn natively).
// ScnRoot::Construct (fn_8044FD30) with a stage's sizes (0x80 children, 0x100 scene objects, 0x80
// lights and light sets), through the MEMAllocator of the current heap (fn_80189394), as the stage.
inline u32 root_construct() { return g::fn_8044FD30(g::fn_80189394(), 0, 0x80, 0x100, 0x80, 0x80); }
// The application's list of roots (lbl_80882E78, up to 8: it gives each a pass before every frame,
// fn_800FA41C): fn_800FA474 adds one, fn_800FA490 takes it out.
inline void root_register(u32 root) { g::fn_800FA474(guest::load32(GA_lbl_80882E78), root); }
inline void root_unregister(u32 root) { g::fn_800FA490(guest::load32(GA_lbl_80882E78), root); }
// The g3d object a guest word points at destroyed once the GPU has drawn the frame that may still
// use it (fn_800E3468: the application's draw waited for, then G3dObj::Destroy), the word cleared.
inline void g3d_destroy(u32 word) { g::fn_800E3468(word); }
// Its current camera 1, set up as the stage's: ScnRoot::SetCurrentCamera (fn_8044FFC0), the viewport
// the whole frame buffer (Camera::SetViewport fn_8044B1B0: the render mode's width and height,
// lbl_808829A0's +0 +4 and +8; SetViewportZRange fn_8044B250 0..1), the TV's aspect (lbl_808829A0
// +8: wide), near 1 and far 3588 until the scene sets its own.
inline void root_camera_setup(u32 root) {
    GFRAME(0x10);
    g::fn_8044FFC0(root, 1);
    u32 video = guest::load32(GA_lbl_808829A0), mode = guest::load32(video);
    GS(u32, 0x8) = current_camera(root);
    u32 cam = (u32)&GS(u32, 0x8);
    g::fn_8044B1B0(cam, 0.0, 0.0, (f64)guest::load16(mode + 0x4), (f64)guest::load16(mode + 0x8));
    g::fn_8044B250(cam, 0.0, 1.0);
    store_f32(GS(u32, 0x8) + 0xB0, guest::load8(video + 0x8) ? 16.0f / 9.0f : 4.0f / 3.0f);
    store_f32(GS(u32, 0x8) + 0xB4, 1.0f);
    store_f32(GS(u32, 0x8) + 0xB8, 3588.0f);
}
// The root's frame, as g3d::Stage's calc (g3d_Stage_Calc) less the stage's parts: ScnRoot::UpdateFrame
// (fn_80450070: animations a frame on), CalcWorld (fn_804504B0), CalcView (fn_80450520),
// GatherDrawScnObj (fn_804505A0: what the camera sees, culled by its perspective), ZSort (fn_80450690).
inline void root_calc(u32 root) {
    g::fn_80450070(root, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    g::fn_804504B0(root, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    g::fn_80450520(root);
    g::fn_804505A0(root, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    g::fn_80450690(root, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

// ---- the 3D sound (0xDC bytes, what g3d::Stage keeps at +0x3C): nw4r's Sound3DManager (+0x0) and
// Sound3DListener (+0x2C, its matrix kept at +0x98). The ctor (fn_802A1060) makes it the one every
// Sound3DActor is made with (lbl_80883088: a Mii's BodyController makes one for its sounds), the
// dtor (fn_802A12F0) clears that again. The ctor takes the stage's path for its distances (none: the
// room's and the TV studio's: interior 8, full volume within 400, unit 200).
const u32 SOUND3D_SIZE = 0xDC;
inline void sound3d_ctor(u32 s) { g::fn_802A1060(s, 0); }
inline void sound3d_dtor(u32 s) { g::fn_802A12F0(s, (u32)-1); }
// the listener where the camera is (the stage's calc does it, fn_802A1374: Sound3DListener::SetMatrix
// fn_80476F90 with the camera's view matrix)
inline void sound3d_listen(u32 s, u32 cam_word) {
    camera_view_matrix(cam_word, s + 0x98);
    g::fn_80476F90(s + 0x2C, s + 0x98);
}

// ResMdl::GetResNode(name) (nw4r_g3d_ResMdl_GetResNode): the MDL0 node's data, 0 if none. ResNodeData: +0x0C
// index, +0x14 flags (1 SRT identity, 8 scale one, 0x10 scale uniform), +0x20 scale, +0x2C rotation
// (degrees), +0x38 translation.
inline u32 model_node(u32 scn_mdl, u32 name) {
    GFRAME(0x10);
    GS(u32, 0x8) = guest::load32(scn_mdl + 0xE8);
    return g::nw4r_g3d_ResMdl_GetResNode((u32)&GS(u32, 0x8), name);
}

// ---- the Mii shadow's resource (0xC bytes): g3d/Mii_Shadow.lex, read by every Mii body's shadow
// through mii_ShadowRes_sInstance (set by the ctor mii_ShadowRes_ctor, cleared by the dtor mii_ShadowRes_dtor)
const u32 SHADOW_RES_SIZE = 0xC;
inline void shadow_res_ctor(u32 r) { g::mii_ShadowRes_ctor(r, guest::load32(GA_mii_ShadowRes_sPath)); }
inline void shadow_res_dtor(u32 r) { g::mii_ShadowRes_dtor(r, (u32)-1); }

// ---- the list of 3D sound handles (0xC bytes: next, prev, u8): every Mii's BodyController links
// its Sound3DHandle into the list sound_Sound3DHandleList_sInstance points at; the dtor sound_Sound3DHandleList_dtor clears it
const u32 SOUND_HANDLES_SIZE = 0xC;
inline void sound_handles_ctor(u32 h) {
    guest::store32(h, h);
    guest::store32(h + 4, h);
    guest::store8(h + 8, 0);
    guest::store32(GA_sound_Sound3DHandleList_sInstance, h);
}
inline void sound_handles_dtor(u32 h) { g::sound_Sound3DHandleList_dtor(h, (u32)-1); }

// ---- the Miis' actor manager (0x218 bytes, room_ActorManager_sInstance): the Mii resources (Mii model,
// animation files) and a slot heap of n actors, each RFLGetModelBufferSize(face resolution, 0x7F) (the faces
// of 7 expressions) + 0x11000 bytes (the body, its animation objects, the behaviour).
// room_ActorManager_ctorWithFiles(mgr, scene parent, n, face resolution, list): the animation files in the list only
// (a list is {u32 array of guest path strings, u32 count}). Its dtor room_ActorManager_dtor destroys the actors.
const u32 ACTORS_SIZE = 0x218;
inline void actors_ctor(u32 m, u32 parent, u32 n, u32 face_res, u32 files) {
    g::room_ActorManager_ctorWithFiles(m, parent, n, face_res, files);
}
inline void actors_dtor(u32 m) { g::room_ActorManager_dtor(m, (u32)-1, 0, 0, 0); }
// room_ActorManager_Update: the Mii light, then every actor (behaviour, BodyController, body)
inline void actors_update(u32 m) { g::room_ActorManager_Update(m, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
inline u32 actor_slot_size(u32 m) { return guest::load32(m + 0xC); }

// room_SpawnMii: make the actor of the Mii of this RFLCreateID (8 guest bytes) in the current actor
// manager (its age from the household's birth years); 0 if the Mii isn't in the database
inline u32 spawn_mii(u32 create_id) { return g::room_SpawnMii(0, create_id); }

// ---- an actor (room::Actor, 0x3E4 bytes, in its slot): +0x08 its own heap (a word: the heap
// object), +0x44 its face (an RFL character model: +0x60 the expression shown, +0xC8 bits: 31 sex
// (1 female), 10-16 height 0..127), +0x118 its body (MiiBody: +0x4 position, +0x10 yaw in degrees,
// 0 facing +z), +0x210 its BodyController (+0x10 who it is: +0x8 age), +0x348 its behaviour.
inline u32 actor_heap(u32 a) { return guest::load32(a + 0x8); }
inline u32 actor_face(u32 a) { return a + 0x44; }
inline u32 actor_body(u32 a) { return a + 0x118; }
inline u32 actor_age(u32 a) { return guest::load32(a + 0x210 + 0x10 + 0x8); }
inline void actor_place(u32 a, f32 x, f32 y, f32 z, f32 yaw) {
    u32 body = actor_body(a);
    store_f32(body + 0x4, x);
    store_f32(body + 0x8, y);
    store_f32(body + 0xC, z);
    store_f32(body + 0x10, yaw);
}
inline void actor_place(u32 a, f32 x, f32 z, f32 yaw) { actor_place(a, x, 0.0f, z, yaw); }
// the actor's sex: its face's bit 31 (what room_BodyController's Move state picks its speeds by,
// fn_801F40A8, and the room its walks by)
inline bool actor_female(u32 a) { return guest::load32(actor_face(a) + 0xC8) >> 31; }
// mii_BodyCallback_GetHeightScale: the body's height scale (0.70 + 0.40 * height / 127), asked of its mii::BodyCallback
// (body +0x4C, whose +0x4 is the body)
inline f32 body_height_scale(u32 body) { return (f32)g::mii_BodyCallback_GetHeightScale(guest::load32(body + 0x4C)); }

// RFL expressions (the faces are made with all seven)
enum : u32 { FACE_NORMAL = 0, FACE_SMILE = 1, FACE_BLINK = 5, FACE_OPEN_MOUTH = 6 };

// ---- g3d::mii::behavior::SpecialAnim (0x90 bytes, vtable 0x8077D5B0): a behaviour that plays one
// animation (+0x08 the file's short name, +0x14 the animation's, both std::string; +0x20 a guest
// pointer to the u32 expression to show, or 0). g3d_mii_behavior_SpecialAnim_Install makes one in the actor's heap and puts
// it in place of the actor's behaviour; the BodyController crossfades to the animation the names
// give whenever they change. std_string_assign assigns a std::string from a guest C string (the MSL
// string keeps its buffer when the new text fits).
inline u32 give_special_anim(u32 actor) { return g::g3d_mii_behavior_SpecialAnim_Install(actor); }
inline void assign_string(u32 str, u32 text) { g::std_string_assign(str, text); }
inline u32 behavior_file(u32 b) { return b + 0x8; }
inline u32 behavior_anim(u32 b) { return b + 0x14; }
inline void behavior_set_expression(u32 b, u32 expr_word) { guest::store32(b + 0x20, expr_word); }

// mii_MiiBody_GetAnmObj: the body's animation object (nw4r::g3d::AnmObjChrRes) of (file, animation), 0 if
// none. Its virtuals: +0x1C SetFrame(f32), +0x28 SetUpdateRate(f32); +0x18 the frame.
inline u32 body_anim(u32 body, u32 file, u32 anim) { return g::mii_MiiBody_GetAnmObj(body, file, anim); }
inline void anim_set_frame(u32 obj, f32 frame) {
    gcall<u32>(guest::load32(guest::load32(obj) + 0x1C), obj, (f64)frame);
}
inline void anim_set_rate(u32 obj, f32 rate) {
    gcall<u32>(guest::load32(guest::load32(obj) + 0x28), obj, (f64)rate);
}

}  // namespace engine::ui::g3d
