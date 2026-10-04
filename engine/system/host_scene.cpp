// Scenes written in native code (see host_scene.h).
//
// The guest side of one scene class, in the layout CodeWarrior gives a C++ class:
//   type_info {+0 const char* name, +4 bases: {type_info*, offset} pairs ended by a null type_info}
//   vtable    {+0 type_info*, +4 0, then the virtual functions}
//   lib::Scene::Factory object: +0/+4 the links of the manager's list of factories (here pointing
//             at itself: not on the list), +8 its vtable {Create(factory, ...) -> lib::Scene*}
//   lib::Scene object: +0 its vtable {dtor(scene, flags), Update(scene), PreDraw(scene, settings),
//             Draw(scene), GetCursor(scene)}, +4 unused
// (lib::SceneManager: Create = factory vtable slot 0 at factory+8; the scene's slots 0..3 as the
// manager calls them; slot 4 as LayoutSceneBase::ChangeSceneWithWipe calls it.)
#include "engine/system/host_scene.h"

#include <cstring>
#include <unordered_map>
#include <vector>

#include "noma_decomp.h"

namespace engine::system {

struct HostSceneAccess {
    static void set_guest(HostScene* s, u32 g) { s->guest_ = g; }
};

namespace {

// type_infos of the bases (in the program's .sdata)
const u32 TYPEINFO_LIB_SCENE = GA_lbl_80880958;          // lib::Scene
const u32 TYPEINFO_LIB_SCENE_FACTORY = GA_lbl_8087B040;  // lib::Scene::Factory
const u32 TYPEINFO_AUTOLIST_FACTORY = GA_lbl_8087B048;   // lib::Autolist<lib::Scene::Factory>
// the scene manager (lib::SceneManager*): +0xC the current scene
const u32 SCENE_MANAGER = GA_lbl_80882CA8;

struct SceneClass {
    u32 factory;
    u32 scene_vtable;
    HostSceneMaker make;
};
std::vector<SceneClass> s_classes;
std::unordered_map<u32, HostScene*> s_scenes;  // guest lib::Scene -> its native object

// Memory for as long as the program runs. The channel's heaps are stacks (lib::allocator::HeapStd:
// blocks are pushed at the head or the tail and freed in reverse order), and a scene's blocks are
// freed when it ends, so a lasting block made while a scene runs would keep that scene's memory
// from coming back. Lasting blocks are therefore made at the start, with the channel's own
// (host_scene_boot()), from the heap current then.
bool s_booting;
u32 alloc_lasting(u32 size, u32 align = 8) {
    if (!s_booting)
        guest::fatal("host scene: lasting guest memory asked for after the start");
    GFRAME(0x10);
    u32 scope = (u32)&GS(u32, 0x8);
    if (align > 8)
        g::lib_allocator_ScopedAlignment_ctor(scope, align);
    u32 p = g::operator_new(size);
    if (align > 8)
        g::lib_allocator_ScopedAlignment_dtor(scope, (u32)-1);
    if (!p)
        guest::fatal("host scene: out of guest memory (%u bytes)", (unsigned)size);
    return p;
}

u32 lasting_string(const char* s) {
    u32 n = (u32)std::strlen(s) + 1;
    u32 p = alloc_lasting(n);
    guest::write_g(p, s, n);
    return p;
}

// {name, bases...} with the bases' type_infos at offset 0
u32 type_info(const char* name, std::initializer_list<u32> bases) {
    u32 b = alloc_lasting(8 * ((u32)bases.size() + 1));
    u32 i = 0;
    for (u32 t : bases) {
        guest::store32(b + 8 * i, t);
        guest::store32(b + 8 * i + 4, 0);
        i++;
    }
    guest::store32(b + 8 * i, 0);
    guest::store32(b + 8 * i + 4, 0);
    u32 ti = alloc_lasting(8);
    guest::store32(ti, lasting_string(name));
    guest::store32(ti + 4, b);
    return ti;
}

u32 vtable(u32 ti, std::initializer_list<u32> slots) {
    u32 vt = alloc_lasting(8 + 4 * (u32)slots.size());
    guest::store32(vt, ti);
    guest::store32(vt + 4, 0);
    u32 i = 0;
    for (u32 f : slots)
        guest::store32(vt + 8 + 4 * i++, f);
    return vt;
}

HostScene* scene_of(u32 scene) {
    auto it = s_scenes.find(scene);
    if (it == s_scenes.end() || !it->second)
        guest::fatal("host scene: no native scene for %08X", scene);
    return it->second;
}

// ---- the guest-callable functions (the register ABI: u32 per integer argument)

u32 factory_create(u32 factory) {
    for (const SceneClass& c : s_classes) {
        if (c.factory != factory)
            continue;
        u32 scene = g::operator_new(8);
        if (!scene)
            guest::fatal("host scene: out of guest memory for a scene");
        guest::store32(scene, c.scene_vtable);
        guest::store32(scene + 4, 0);
        s_scenes[scene] = nullptr;
        HostScene* s = c.make();
        HostSceneAccess::set_guest(s, scene);
        s_scenes[scene] = s;
        return scene;
    }
    guest::fatal("host scene: unknown factory %08X", factory);
}

u32 scene_dtor(u32 scene, u32 flags) {
    HostScene* s = scene_of(scene);
    s_scenes.erase(scene);
    delete s;
    if ((s16)flags > 0)
        g::operator_delete(scene);
    return scene;
}

u32 scene_update(u32 scene) {
    scene_of(scene)->update();
    return 0;
}

u32 scene_pre_draw(u32 scene, u32 settings) {
    scene_of(scene)->pre_draw(settings);
    return 0;
}

u32 scene_draw(u32 scene) {
    scene_of(scene)->draw();
    return 0;
}

u32 scene_cursor(u32 scene) { return scene_of(scene)->cursor(); }

// layout::Button's push hooks (vtable slots 19 and 20: ButtonPushTask calls them when the push
// starts and when its animation has ended)
const u32 SLOT_PUSH_START = 19, SLOT_PUSHED = 20;
std::unordered_map<u32, u32> s_hooked_vtables;  // copy -> the class's own vtable

void call_original(u32 button, u32 slot) {
    auto it = s_hooked_vtables.find(guest::load32(button));
    if (it != s_hooked_vtables.end())
        gcall<u32>(guest::load32(it->second + 8 + 4 * slot), button);
}

u32 button_push_start(u32 button) {
    call_original(button, SLOT_PUSH_START);
    if (HostScene* s = current_host_scene())
        s->on_push_start(button);
    return 0;
}

u32 button_pushed(u32 button) {
    call_original(button, SLOT_PUSHED);
    if (HostScene* s = current_host_scene())
        s->on_pushed(button);
    return 0;
}

}  // namespace

u32 host_scene_factory(const char* class_name, HostSceneMaker make) {
    for (const SceneClass& c : s_classes)
        if (c.make == make)
            return c.factory;
    if (!s_booting)
        guest::fatal("host scene: %s's factory wasn't made at the start", class_name);
    std::string factory_name = std::string(class_name) + "::Factory";
    u32 factory_ti = type_info(factory_name.c_str(), {TYPEINFO_AUTOLIST_FACTORY, TYPEINFO_LIB_SCENE_FACTORY});
    u32 scene_ti = type_info(class_name, {TYPEINFO_LIB_SCENE});
    u32 factory_vt = vtable(factory_ti, {guest::host_fn(&factory_create)});
    u32 scene_vt = vtable(scene_ti, {guest::host_fn(&scene_dtor), guest::host_fn(&scene_update),
                                     guest::host_fn(&scene_pre_draw), guest::host_fn(&scene_draw),
                                     guest::host_fn(&scene_cursor)});
    u32 factory = alloc_lasting(0xC);
    guest::store32(factory, factory);
    guest::store32(factory + 4, factory);
    guest::store32(factory + 8, factory_vt);
    s_classes.push_back({factory, scene_vt, make});
    return factory;
}

void hookable_button_class(u32 vtable, u32 n_slots) {
    for (const auto& kv : s_hooked_vtables)
        if (kv.second == vtable)
            return;
    u32 size = 8 + 4 * n_slots;
    u32 copy = alloc_lasting(size);
    for (u32 i = 0; i < size; i += 4)
        guest::store32(copy + i, guest::load32(vtable + i));
    guest::store32(copy + 8 + 4 * SLOT_PUSH_START, guest::host_fn(&button_push_start));
    guest::store32(copy + 8 + 4 * SLOT_PUSHED, guest::host_fn(&button_pushed));
    s_hooked_vtables[copy] = vtable;
}

void hook_button(u32 button) {
    u32 vt = guest::load32(button);
    if (s_hooked_vtables.count(vt))
        return;
    for (const auto& kv : s_hooked_vtables) {
        if (kv.second == vt) {
            guest::store32(button, kv.first);
            return;
        }
    }
    guest::fatal("host scene: button %08X's class (vtable %08X) wasn't made hookable at the start", button, vt);
}

u32 lasting_alloc(u32 size, u32 align) { return alloc_lasting(size, align); }

void host_scene_boot(void (*make_lasting)()) {
    s_booting = true;
    make_lasting();
    s_booting = false;
}

HostScene* current_host_scene() {
    u32 mgr = guest::load32(SCENE_MANAGER);
    if (!mgr)
        return nullptr;
    auto it = s_scenes.find(guest::load32(mgr + 0xC));
    return it == s_scenes.end() ? nullptr : it->second;
}

}  // namespace engine::system
