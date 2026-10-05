// The household's Mii on the island (island_mii.h).
//
// Made as the Sound Player's stage makes its Miis (engine/soundplayer/stage.cpp, after the TV
// programme screen's caster), without a set: the 3D sound and a ScnRoot as g3d::Stage makes them
// (engine/ui/g3d.h; g3d_Stage_sInstance stays 0: no code a Mii runs reads it, and the room's code that
// does isn't running), the Mii shadow's resource and the 3D sound handles' list (two globals every
// Mii body and BodyController reads), the actor manager over the root with the room's walking, waiting,
// head and colour animation files only, and the household's first member spawned and given a
// SpecialAnim behaviour at once (a Mii without one would get the room's AI). The first member is the
// Mii registered first, the player's own (research port.md §4.2).
//
// The walk. A SpecialAnim plays the clip its names give (the BodyController crossfades when they
// change): Wait from Wait.lex at its own pace, Walk and Run from Move.lex (Female_Walk, Female_Run for
// a woman, as the room's Move state picks them) with their update rate 0 and their frame set every
// frame from the distance walked: a clip goes one frame on for every `speed` guest units walked, the
// speed the room's Move state walks or runs at (room_BodyController_Move_Enter: 1.593 and 4.31 units a
// frame for a man, 1.687 and 5.343 for a woman), so at that speed it plays at its own pace and at any
// other the stride keeps to the ground. (That the clips were made for those speeds is an inference:
// player.h's stride lengths make the same one.) Both walking clips are driven all the time, so that the
// one being crossfaded from keeps step too. Falling, the Mii keeps the clip it had, its frame still.
#include "engine/wuhu/island_mii.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "engine/ui/g3d.h"
#include "noma_decomp.h"

namespace engine::wuhu {

namespace g3d = engine::ui::g3d;
using engine::math::store_f32;

namespace {

const u32 FACE_RESOLUTION = 0x80;
// the animation files the actor manager loads (Wait and Head the BodyController's own; Color_Anim the
// Mii light's)
const char* const ANIM_FILES[] = {
    "g3d/Mii/Animation/Move.lex",
    "g3d/Mii/Animation/Wait.lex",
    "g3d/Mii/Animation/Head.lex",
    "g3d/Mii/Animation/Color_Anim.lex",
};
const u32 N_ANIM_FILES = sizeof(ANIM_FILES) / sizeof(ANIM_FILES[0]);
// a blink every 3.5 s, for a tenth of a second
const u32 BLINK_EVERY = 210, BLINK_FRAMES = 6;

// the guest strings, made into one block
struct Strings {
    std::vector<u8> bytes;
    u32 add(const char* s) {
        u32 at = (u32)bytes.size();
        bytes.insert(bytes.end(), s, s + std::strlen(s) + 1);
        return at;
    }
};

}  // namespace

IslandMii::IslandMii(const u8* npc_record) : npc_record_(npc_record) {}

IslandMii::~IslandMii() {
    // the reverse of make_step's order (nothing was made if the block couldn't be had: step_ is
    // then STEPS, the addresses 0): the NPC's dress first (his actor goes with the manager's)
    outfit_.reset();
    if (block_) {
        if (step_ >= 5)
            g3d::sound_handles_dtor(handles_);
        if (step_ >= 4)
            g3d::actors_dtor(actors_);
        if (step_ >= 3)
            g3d::shadow_res_dtor(shadow_res_);
        if (step_ >= 2) {
            g3d::root_unregister(guest::load32(root_word_));
            g3d::g3d_destroy(root_word_);
        }
        if (step_ >= 1)
            g3d::sound3d_dtor(sound3d_);
        g::operator_delete(block_);
    }
    std::string heap;
    u32 free_after = g3d::heap_free(&heap);
    guest::log("wuhu: Mii: gone; heap %s: %u bytes free (%u before it)", heap.c_str(), (unsigned)free_after,
               (unsigned)heap_before_);
}

f32 IslandMii::progress() const { return (f32)std::min(step_, STEPS) / (f32)STEPS; }

bool IslandMii::make_step() {
    if (step_ >= STEPS)
        return false;
    switch (step_) {
    case 0: {
        std::string heap;
        heap_before_ = g3d::heap_free(&heap);
        // the household's first member's RFLCreateID (model::Family reads the channel's save:
        // model_Family_ctor, model_Family_GetCount, model_Family_GetPerson (0x48 bytes, +0 the ID),
        // model_Family_dtor; it lives on the guest stack only for this)
        u8 id[8] = {};
        {
            GFRAME(0x20);
            u32 family = (u32)&GS(u32, 0x8);
            g::model_Family_ctor(family, 0);
            if (g::model_Family_GetCount(family, 0) > 0) {
                u32 person = g::model_Family_GetPerson(family, 0);
                for (u32 k = 0; k < 8; k++)
                    id[k] = guest::load8(person + k);
                have_id_ = true;
            }
            g::model_Family_dtor(family, (u32)-1);
        }
        // the strings: the files, the clips' names (a woman's or a man's: both, as the sex is known
        // only once the Mii is made)
        Strings s;
        std::vector<u32> paths;
        for (const char* p : ANIM_FILES)
            paths.push_back(s.add(p));
        // the NPC's clip (NOMA_MIDOHA_CLIP=file/clip, e.g. Move/Walk or Sit_Sofa/Sit_Sofa: his outfit's
        // renders; another file than the four is loaded too), Wait otherwise
        u32 npc_file = 0, npc_anim = 0;
        if (const char* c = npc_record_ ? std::getenv("NOMA_MIDOHA_CLIP") : nullptr) {
            const char* slash = std::strchr(c, '/');
            if (slash && slash > c && slash[1]) {
                const std::string file(c, slash), path = "g3d/Mii/Animation/" + file + ".lex";
                npc_file = s.add(file.c_str()), npc_anim = s.add(slash + 1);
                bool have = false;
                for (const char* p : ANIM_FILES)
                    have |= path == p;
                if (!have)
                    paths.push_back(s.add(path.c_str()));
            }
        }
        const u32 n_files = (u32)paths.size();
        u32 wait = s.add("Wait"), move = s.add("Move"), head = s.add("Head_Only");
        u32 walk = s.add("Walk"), run = s.add("Run"), f_walk = s.add("Female_Walk"), f_run = s.add("Female_Run");
        // the block's layout
        u32 size = 0;
        auto take = [&](u32 n) {
            u32 a = size;
            size = (size + n + 7) & ~7u;
            return a;
        };
        u32 o_sound = take(g3d::SOUND3D_SIZE), o_shadow = take(g3d::SHADOW_RES_SIZE);
        u32 o_handles = take(g3d::SOUND_HANDLES_SIZE), o_actors = take(g3d::ACTORS_SIZE), o_root = take(4);
        u32 o_list = take(8 + 4 * n_files), o_expr = take(4), o_id = take(8);
        u32 o_strings = take((u32)s.bytes.size());
        // the NPC's middle database, its buffer (an entry is 0x40 bytes), his record
        const u32 o_db = npc_record_ ? take(0x18) : 0, o_db_buffer = npc_record_ ? take(0x40) : 0;
        const u32 o_npc = npc_record_ ? take(0x4A) : 0;
        block_ = g::operator_new(size);
        if (!block_) {
            guest::log("wuhu: Mii: out of memory (%u bytes): no Mii", (unsigned)size);
            step_ = STEPS;
            return false;
        }
        guest::memset_g(block_, 0, size);
        sound3d_ = block_ + o_sound, shadow_res_ = block_ + o_shadow, handles_ = block_ + o_handles;
        actors_ = block_ + o_actors, root_word_ = block_ + o_root, anim_list_ = block_ + o_list;
        expression_ = block_ + o_expr, create_id_ = block_ + o_id, strings_ = block_ + o_strings;
        if (npc_record_) {
            npc_db_ = block_ + o_db, npc_db_buffer_ = block_ + o_db_buffer, npc_data_ = block_ + o_npc;
            guest::write_g(npc_data_, npc_record_, 0x4A);
        }
        guest::write_g(strings_, s.bytes.data(), (u32)s.bytes.size());
        guest::write_g(create_id_, id, 8);
        guest::store32(anim_list_, anim_list_ + 8);
        guest::store32(anim_list_ + 4, n_files);
        for (u32 i = 0; i < n_files; i++)
            guest::store32(anim_list_ + 8 + 4 * i, strings_ + paths[i]);
        npc_clip_file_ = strings_ + (npc_file ? npc_file : wait), npc_clip_anim_ = strings_ + (npc_anim ? npc_anim : wait);
        guest::store32(expression_, g3d::FACE_NORMAL);
        // the clips' names, a man's for now (the walking ones are a woman's once she is known)
        clip_file_[WAIT] = strings_ + wait, clip_anim_[WAIT] = strings_ + wait;
        clip_file_[WALK] = clip_file_[RUN] = strings_ + move;
        clip_anim_[WALK] = strings_ + walk, clip_anim_[RUN] = strings_ + run;
        longest_file_ = strings_ + move, longest_anim_ = strings_ + f_walk;
        female_walk_ = strings_ + f_walk, female_run_ = strings_ + f_run;
        head_name_ = strings_ + head;
        // the 3D sound first, as g3d::Stage's ctor makes it before its root
        g3d::sound3d_ctor(sound3d_);
        break;
    }
    case 1: {
        u32 root = g3d::root_construct();
        guest::store32(root_word_, root);
        g3d::root_register(root);
        g3d::root_camera_setup(root);
        f32 a = g3d::camera_aspect(g3d::camera(root, 1));
        aspect_ = a > 0.5f && a < 3.0f ? a : 16.0f / 9.0f;
        break;
    }
    case 2: g3d::shadow_res_ctor(shadow_res_); break;
    case 3:
        g3d::actors_ctor(actors_, guest::load32(root_word_), npc_record_ ? 2 : 1, FACE_RESOLUTION, anim_list_);
        break;
    case 4: {
        g3d::sound_handles_ctor(handles_);
        actor_ = have_id_ ? g3d::spawn_mii(create_id_) : 0;
        if (!actor_) {
            guest::log("wuhu: Mii: the household's first Mii isn't in the database%s: no Mii",
                       have_id_ ? "" : " (no household)");
            break;
        }
        female_ = g3d::actor_female(actor_);
        if (female_)
            clip_anim_[WALK] = female_walk_, clip_anim_[RUN] = female_run_;
        behavior_ = g3d::give_special_anim(actor_);
        {
            // the names' std::strings in the actor's own heap, first as long as the longest names (an
            // MSL string keeps its buffer for any shorter text: no allocation when the clip changes)
            g3d::ScopedHeap heap(g3d::actor_heap(actor_));
            g3d::assign_string(g3d::behavior_file(behavior_), longest_file_);
            g3d::assign_string(g3d::behavior_anim(behavior_), longest_anim_);
        }
        g3d::behavior_set_expression(behavior_, expression_);
        // the clips: their animation objects (made with the body), loops and own speeds
        const Gait gt = gait();
        const f32 loops[N_CLIPS] = {120, 60, female_ ? 30.0f : 40.0f};
        const f32 speeds[N_CLIPS] = {0, gt.walk, gt.run};
        for (int c = 0; c < N_CLIPS; c++) {
            anim_[c] = g3d::body_anim(g3d::actor_body(actor_), clip_file_[c], clip_anim_[c]);
            frames_[c] = loops[c], speed_[c] = speeds[c];
        }
        height_scale_ = g3d::body_height_scale(g3d::actor_body(actor_));
        set_clip(WAIT);
        // what it costs: the block, the Mii model and the animation files (the actor manager's
        // resources, +0x14: +0x4 Model.lex's size, +0xC the AnimFile* vector, count at +0x10; an
        // AnimFile's +0x10 its file's size), the shadow's file, the actor's slot
        u32 res = actors_ + 0x14, n_files = guest::load32(res + 0x10), files = 0;
        for (u32 i = 0; i < n_files; i++)
            files += guest::load32(guest::load32(guest::load32(res + 0xC) + 4 * i) + 0x10);
        std::string heap;
        u32 free_now = g3d::heap_free(&heap);
        guest::log("wuhu: Mii: %s, height scale %.2f; memory: Mii model %u, %u animation files %u, Mii shadow "
                   "%u, actor slot %u; heap %s: %u bytes taken, %u free",
                   female_ ? "female" : "male", height_scale_, (unsigned)guest::load32(res + 4), (unsigned)n_files,
                   (unsigned)files, (unsigned)guest::load32(shadow_res_ + 4), (unsigned)g3d::actor_slot_size(actors_),
                   heap.c_str(), (unsigned)(heap_before_ - free_now), (unsigned)free_now);
        for (int c = 0; c < N_CLIPS; c++)
            if (!anim_[c])
                guest::log("wuhu: Mii: no animation %s/%s", guest::read_string(clip_file_[c]).c_str(),
                           guest::read_string(clip_anim_[c]).c_str());
        break;
    }
    case 5: make_npc(); break;
    }
    step_++;
    return step_ < STEPS;
}

// The NPC, a frame after the player's Mii: his record into a middle database of his own
// (RFLInitMiddleDB: type 4, "user set", one entry; RFLiAddMiddleDBCharData adds an RFLCharData,
// RFLAddMiddleDBStoreData's work less the store data's CRC: it checks his create ID and his parts'
// ranges), then the actor manager's spawn from a middle database (room_ActorManager_SpawnFromMiddleDB,
// what room_SpawnMii does for a Mii found in one: entry 0, age 20), his Wait clip, his dress.
void IslandMii::make_npc() {
    if (!npc_record_ || !block_)
        return;
    g::RFLInitMiddleDB(npc_db_, 4, npc_db_buffer_, 1);
    const u32 err = g::RFLiAddMiddleDBCharData(npc_db_, npc_data_);
    if (err) {
        guest::log("wuhu: NPC: RFL refuses his record (error %u): no NPC", (unsigned)err);
        return;
    }
    const u32 manager = guest::load32(GA_room_ActorManager_sInstance);
    npc_actor_ = g::room_ActorManager_SpawnFromMiddleDB(manager, npc_db_, 0, 20);
    if (!npc_actor_) {
        guest::log("wuhu: NPC: no actor: no NPC");
        return;
    }
    npc_behavior_ = g3d::give_special_anim(npc_actor_);
    {
        g3d::ScopedHeap heap(g3d::actor_heap(npc_actor_));
        g3d::assign_string(g3d::behavior_file(npc_behavior_), npc_clip_file_);
        g3d::assign_string(g3d::behavior_anim(npc_behavior_), npc_clip_anim_);
    }
    g3d::behavior_set_expression(npc_behavior_, expression_);
    const u32 body = g3d::actor_body(npc_actor_), scn = guest::load32(body + 0x1C);
    npc_height_scale_ = g3d::body_height_scale(body);
    const u32 node = g3d::model_node(scn, head_name_);
    npc_head_node_ = node ? guest::load32(node + 0x0C) : 0;
    std::string heap;
    const u32 free_now = g3d::heap_free(&heap);
    guest::log("wuhu: NPC: %s, height scale %.2f, head node %u; heap %s: %u bytes free",
               g3d::actor_female(npc_actor_) ? "female" : "male", npc_height_scale_, (unsigned)npc_head_node_,
               heap.c_str(), (unsigned)free_now);
    outfit_ = std::make_unique<MidohaOutfit>();
    if (!outfit_->make(scn))
        outfit_.reset();
}

bool IslandMii::npc_head(engine::math::Mtx34* world) const {
    if (!npc_head_ok_)
        return false;
    engine::math::Mtx34 inv;
    if (!engine::math::inverse(camera_view_, inv))
        return false;
    // RFLSetCoordinate(+X up, +Y front) (the channel's): the node's x is RFL's y, its y RFL's z,
    // its z RFL's x
    const engine::math::Mtx34 axes = {{{0, 1, 0, 0}, {0, 0, 1, 0}, {1, 0, 0, 0}}};
    *world = engine::math::concat(inv, engine::math::concat(npc_head_view_, axes));
    return true;
}

// the clip the behaviour plays (the BodyController crossfades to it)
void IslandMii::set_clip(Clip c) {
    if ((int)c == clip_)
        return;
    clip_ = c;
    g3d::ScopedHeap heap(g3d::actor_heap(actor_));
    g3d::assign_string(g3d::behavior_file(behavior_), clip_file_[c]);
    g3d::assign_string(g3d::behavior_anim(behavior_), clip_anim_[c]);
}

// camera 1, the root's current one: a look-at and a perspective (no off-axis frustum here)
void IslandMii::write_camera(const engine::g3d::Camera& c) {
    GFRAME(0x50);
    GS(u32, 0x8) = g3d::camera(guest::load32(root_word_), 1);
    u32 cam = (u32)&GS(u32, 0x8), pos = (u32)&GS(u32, 0x10), posture = (u32)&GS(u32, 0x20);
    store_f32(pos, c.eye.x), store_f32(pos + 4, c.eye.y), store_f32(pos + 8, c.eye.z);
    // PostureInfo: type 0 (look at), up at +4, the target at +0x10
    guest::memset_g(posture, 0, 0x2C);
    store_f32(posture + 0x4, c.up.x), store_f32(posture + 0x8, c.up.y), store_f32(posture + 0xC, c.up.z);
    store_f32(posture + 0x10, c.target.x), store_f32(posture + 0x14, c.target.y);
    store_f32(posture + 0x18, c.target.z);
    g3d::camera_set_position(cam, pos);
    g3d::camera_set_posture(cam, posture);
    g3d::camera_set_perspective(cam, c.fovy_deg, c.aspect, c.near_, c.far_);
}

void IslandMii::update(const engine::g3d::Camera& camera, engine::math::Vec3 at, f32 yaw, PlayerState state,
                       f32 walked) {
    if (step_ < STEPS || !block_)
        return;
    const u32 root = guest::load32(root_word_);
    if (!actor_ && npc_actor_)
        g3d::actors_update(actors_);
    if (actor_) {
        frame_count_++;
        switch (state) {
        case PlayerState::WAIT: set_clip(WAIT); break;
        case PlayerState::WALK: set_clip(WALK); break;
        case PlayerState::RUN: set_clip(RUN); break;
        case PlayerState::FALL: break;
        }
        for (int c = WALK; c <= RUN; c++)
            if (speed_[c] > 0)
                frame_[c] = std::fmod(frame_[c] + walked / speed_[c], frames_[c]);
        guest::store32(expression_,
                       frame_count_ % BLINK_EVERY < BLINK_FRAMES ? g3d::FACE_BLINK : g3d::FACE_NORMAL);
        // the behaviour and the BodyController first; then the body where the player is and the
        // walking clips at their frames, for the root's calc that follows to pose (no frame late)
        g3d::actors_update(actors_);
        // (out of sight: far below the island, its shadow with it)
        g3d::actor_place(actor_, at.x, hide_player_ ? at.y - 1e6f : at.y, at.z, yaw);
        for (int c = WALK; c <= RUN; c++)
            if (anim_[c]) {
                g3d::anim_set_rate(anim_[c], 0);  // (the BodyController's PlayAnim never sets it back)
                g3d::anim_set_frame(anim_[c], frame_[c]);
            }
    }
    if (npc_actor_)
        g3d::actor_place(npc_actor_, npc_at_.x, npc_at_.y, npc_at_.z, npc_yaw_);
    write_camera(camera);
    camera_fovy_ = camera.fovy_deg;
    g3d::invalidate_gx_state();
    g3d::root_calc(root);
    GFRAME(0x50);
    GS(u32, 0x8) = g3d::camera(root, 1);
    g3d::sound3d_listen(sound3d_, (u32)&GS(u32, 0x8));
    // the NPC's head as RFL will draw it (the body's calc made this frame's view matrices), and the
    // camera's view matrix it is under
    npc_head_ok_ = false;
    if (npc_actor_) {
        const u32 scn = guest::load32(g3d::actor_body(npc_actor_) + 0x1C), m = (u32)&GS(u32, 0x10);
        if (g::nw4r_g3d_ScnMdlSimple_GetScnMtxPos(scn, m, 2, npc_head_node_)) {
            npc_head_view_ = engine::math::load<engine::math::Mtx34>(m);
            g3d::camera_view_matrix((u32)&GS(u32, 0x8), m);
            camera_view_ = engine::math::load<engine::math::Mtx34>(m);
            npc_head_ok_ = true;
        }
    }
}

void IslandMii::view() {
    if (step_ < STEPS || !block_)
        return;
    const u32 root = guest::load32(root_word_);
    GFRAME(0x10);
    GS(u32, 0x8) = g3d::camera(root, 1);
    g3d::root_viewport(root);
    g3d::camera_load_view((u32)&GS(u32, 0x8));
}

void IslandMii::draw_opaque() {
    if (step_ < STEPS || !block_)
        return;
    g3d::root_draw_opaque(guest::load32(root_word_));
    // the NPC's dressed body, under the camera's view again (the root's pass may have loaded another)
    if (outfit_ && npc_actor_) {
        view();
        outfit_->draw(guest::load32(g3d::actor_body(npc_actor_) + 0x1C), camera_fovy_);
    }
}

void IslandMii::draw_translucent() {
    if (step_ >= STEPS && block_)
        g3d::root_draw_translucent(guest::load32(root_word_));
}

}  // namespace engine::wuhu
