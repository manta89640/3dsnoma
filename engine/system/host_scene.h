// Scenes written in native code, run by the channel's own scene manager.
//
// The channel's screens are lib::Scene objects (the room, the theater, the options...): the
// application's lib::SceneManager destroys the current one and makes the next with its
// lib::Scene::Factory when a scene change is requested, then calls the scene's virtuals every
// frame (Update, PreDraw, Draw; scenes on the layout base also answer GetCursor, which
// LayoutSceneBase::ChangeSceneWithWipe asks). A HostScene is such a scene whose virtuals are
// native: the guest objects (factory, scene, their vtables and type_infos) are built here, their
// slots point at native functions (guest::host_fn_address), and those call the C++ object.
#pragma once

#include "runtime/runtime.h"

namespace engine::system {

class HostScene {
public:
    virtual ~HostScene() {}
    // once a frame, before the frame is drawn
    virtual void update() = 0;
    // the frame's drawing (3D, then layouts, then the cursor), after the application's 2D setup
    virtual void draw() = 0;
    // before draw (the application's draw pass hands its 2D settings object)
    virtual void pre_draw(u32 /*settings*/) {}
    // the scene's layout::Cursor (0: none): ChangeSceneWithWipe locks it while the wipe plays
    virtual u32 cursor() { return 0; }
    // a button given to hook_button() was pushed (its Push animation starts) / has been pushed
    // (the animation ended: where the game's own callbacks act)
    virtual void on_push_start(u32 /*button*/) {}
    virtual void on_pushed(u32 /*button*/) {}
    // the guest lib::Scene object
    u32 guest() const { return guest_; }

private:
    friend struct HostSceneAccess;
    u32 guest_ = 0;
};

using HostSceneMaker = HostScene* (*)();

// Everything guest-side that has to last (factories, vtables) is made once, at the start: the
// channel's heaps are stacks, so blocks made later would pin the scene running then. make_lasting
// is called right after the channel has made its own scene factories, and is where the calls
// below that make something belong.
void host_scene_boot(void (*make_lasting)());

// guest memory for as long as the program runs (aligned to `align` bytes: 32 for what the GPU
// reads), only from inside make_lasting
u32 lasting_alloc(u32 size, u32 align = 8);

// The guest lib::Scene::Factory for a scene class (made at the start, then found again by `make`;
// class_name is the C++ name its type_info carries, e.g. "scene::SoundPlayerScene"). Hand it to
// lib::SceneManager::ChangeScene. It isn't on the manager's list of factories, so changing scene
// by name won't find it.
u32 host_scene_factory(const char* class_name, HostSceneMaker make);

// the scene the manager runs now, if it is a HostScene (else nullptr)
HostScene* current_host_scene();

// lib::SceneManager::ChangeScene (lib_SceneManager_ChangeScene): the manager keeps the factory; its next scene
// step (once a frame, right before it updates the scene) ends the current scene and makes the
// factory's, which it then updates
void change_scene(u32 factory);

// Let the current HostScene hear about a layout::Button's pushes: the button gets a copy of its
// class's vtable whose push hooks (slots 19 and 20: empty in layout::Button) call the original and
// then the scene's on_push_start / on_pushed. The class (its vtable, of n_slots virtuals;
// layout::Button has 21) is made hookable at the start.
void hookable_button_class(u32 vtable, u32 n_slots);
void hook_button(u32 button);

}  // namespace engine::system
