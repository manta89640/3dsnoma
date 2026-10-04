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

// G3DState::Invalidate: g3d sends all its GX state again before its next draw
inline void invalidate_gx_state() { g::nw4r_g3d_G3DState_Invalidate(); }
// The stage's draw (g3d::Stage::Draw g3d_Stage_Draw), split around the opaque and the translucent
// passes so that native drawing can go between them: dither on, the current camera's viewport set
// to the whole screen by app::Renderer (app_Renderer_SetCameraViewport, app_Renderer_sInstance), ScnRoot::DrawOpa (nw4r_g3d_ScnRoot_DrawOpa)
// ... ScnRoot::DrawXlu (nw4r_g3d_ScnRoot_DrawXlu).
inline void stage_draw_opaque(u32 s) {
    GFRAME(0x10);
    g::GXSetDither(1);
    GS(u32, 0x8) = current_camera(stage_root(s));
    g::app_Renderer_SetCameraViewport(guest::load32(GA_app_Renderer_sInstance), (u32)&GS(u32, 0x8));
    invalidate_gx_state();
    g::nw4r_g3d_ScnRoot_DrawOpa(stage_root(s));
}
inline void stage_draw_translucent(u32 s) {
    invalidate_gx_state();
    g::nw4r_g3d_ScnRoot_DrawXlu(stage_root(s));
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
inline void actor_place(u32 a, f32 x, f32 z, f32 yaw) {
    u32 body = actor_body(a);
    store_f32(body + 0x4, x);
    store_f32(body + 0x8, 0.0f);
    store_f32(body + 0xC, z);
    store_f32(body + 0x10, yaw);
}
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
