// What the island scene shows: the island and the Mii walking on it (the native g3d world, the
// player, the camera; docs/wuhu_design.md §5). The scene (scene.cpp) is the shell around it: it
// starts the load, keeps the screen black (with the loading rings once the load takes more than
// half a second) until the content is ready, fades in, runs it, freezes it while its window asks
// whether to go back, and deletes it as the scene ends.
//
// The content's guest objects (the Mii's ScnRoot, actors, files...) follow the channel's heap rule
// (its heaps are stacks): made in start_loading() or in progress() while loading, never in
// update(), and freed in its destructor in the reverse order. The scene deletes the content before
// its own guest objects, which were made before the content's.
#pragma once

#include "runtime/runtime.h"

namespace engine::wuhu {

// what the player does, read once a frame by the scene
struct Controls {
    u32 held = 0, pressed = 0;       // the remote's buttons held and newly pressed (KPAD bits: engine/ui/game.h)
    f32 move_x = 0, move_y = 0;      // the move stick (the 3DS's circle pad): -1..1, x right, y up
    f32 camera_x = 0, camera_y = 0;  // the camera stick (C-stick, L/R): -1..1, x right, y up
};

class IslandContent {
public:
    virtual ~IslandContent() {}
    // the load starts (called once, by the scene's constructor; the screen is black)
    virtual void start_loading() = 0;
    // Once a frame while loading: the load's next step (a step short enough for the frame: the
    // loading rings go on turning and the sound thread goes on running), and how far it is, 0 to 1.
    virtual f32 progress() = 0;
    // loaded: the first frame can be drawn (the scene then fades in from black)
    virtual bool ready() const = 0;
    // once a frame from ready() on; frozen: the player and the camera stay as they are (the window
    // is open, or the scene fades in or out), animations may go on
    virtual void update(const Controls& controls, bool frozen) = 0;
    // the frame's 3D, from ready() on: the opaque pass, then the translucent one (the scene's 2D
    // goes over them)
    virtual void draw_opaque() = 0;
    virtual void draw_translucent() = 0;
};

// the island's content, made by the scene's constructor (the gameplay's; content_placeholder.cpp
// until it is there)
IslandContent* make_content();

}  // namespace engine::wuhu
