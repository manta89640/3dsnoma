// The Sound Player's stage (see stage.h): the household's Miis at a table with an amp, dancing to
// the music, in the channel's white TV studio.
//
// Made as the TV programme screen (scene::TvScene) makes its studio and its caster (g3d.h): the
// studio set g3d/Wii_Tv.lex (its white cyclorama, its pedestal table and the table's soft shadow:
// unlit materials, no lights needed) as a g3d::Stage; the Mii shadow's resource and the list of
// 3D sound handles, two globals every Mii body and BodyController reads; then the Miis' actor
// manager over the stage's ScnRoot with only the animation files the dances use, and the faces at
// 128 pixels (a head is some 40 pixels high here). Every member of the household (model::Family)
// is spawned (room_SpawnMii works outside the room) and given a SpecialAnim behaviour at once (a Mii
// without one would get the room's AI). There's no camera manager: camera 1, the stage's, is
// written before every calc. Torn down in the TvScene's order; each global goes back to 0 with its
// object.
//
// The tableau. The table is the studio's pedestal table (its board and its shadow scaled in the
// loaded model for a bigger household), at the set's table spot (0, 0, -50); the Miis stand behind
// it facing the camera, turned a little toward the middle, the tallest in the middle: up to four on
// an arc around the table (82 apart), five or more in two staggered rows (the taller ones behind,
// 66 apart), so that no two heads overlap; the amp stands on the floor at the right. The camera is
// a long lens (13 degrees; 9 degrees above, 12 over two rows), as the channel's are, pulled back
// until the whole tableau (with the room the jumps need) fits the right half of the screen, and its
// picture is shifted there with an off-axis frustum: the studio fills the screen (its cyclorama
// moved across to the picture's middle), the tableau sits right of the list.
//
// The dances. Each Mii plays one of the channel's own dances and cheers (the TV caster's dances,
// the DSi relay's clapping and jumping, the messages' celebration and birthday jumps), not the one
// of its neighbours, and they change every 8 bars and with every new piece. The clips don't
// play at their own pace: each one's frame is set every frame (its update rate is 0) from the
// music's position, Music::beat(), through the clip's beat map (the frames where its bounces land,
// read from its CHR0 keys), so its bounces land on the beats whatever the frame rate; a clip takes
// 1 of its beats to a beat of the music, or 2 for a slow piece, or 1 to 2 beats for a fast one.
// Faces smile, mouths open on accents. When the music pauses or stops the Miis ease into the
// caster's calm waits (the BodyController's crossfade) with a plain face, blinking now and then;
// the gap between two pieces goes on at the last tempo, a piece without a sequence (a wave jingle)
// at 120. The amp's speaker and pilot light pulse on the beat.
#include "engine/soundplayer/stage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/math/vecmath.h"
#include "engine/soundplayer/amp.h"
#include "engine/soundplayer/angles.h"
#include "engine/ui/g3d.h"
#include "noma_decomp.h"

namespace engine::soundplayer {

namespace g3d = engine::ui::g3d;

namespace {

using namespace engine::math;

// ---- the clips
// A dance: (file, animation), its loop's length, its beat map (the frames where its bounces,
// claps or landings are: these land on the music's beats), how far above the Mii's height it
// reaches (jumps, arms up: the camera's framing allows for it), whether it jumps.
struct Clip {
    const char* file;
    const char* anim;
    u16 frames;
    u8 n_beats;
    u16 beat[8];
    f32 lift;
    bool jumps;
};
enum ClipId { DANCE_A, DANCE_B, CLAP, JUMP, CELEBRATION, BIRTHDAY, N_DANCES };
const Clip DANCES[N_DANCES] = {
    // the TV caster's dances (made for the TV's theme, 200 BPM): a sway from side to side every 36
    // frames; a figure of swings in 8 beats of 18 frames
    {"Tv_Mii_Caster", "Tv_Mii_Caster_Dance_A", 144, 4, {0, 36, 72, 108}, 8, false},
    {"Tv_Mii_Caster", "Tv_Mii_Caster_Dance_B", 144, 8, {0, 18, 36, 54, 72, 90, 108, 126}, 20, false},
    // the DSi relay's clapping (hands together at 11, 26, 41, 57), and its two jumps then a rest
    // (crouches at 9 and 43, back down at 81)
    {"Send_DSi", "Send_DSi_Clap", 62, 4, {11, 26, 41, 57}, 6, false},
    {"Send_DSi", "Send_DSi_Jump", 88, 3, {9, 43, 81}, 60, true},
    // the messages' cheers: four jumps a loop (their crouches)
    {"Message_Genre", "Celebration", 145, 4, {7, 41, 73, 111}, 52, true},
    {"Message_Genre", "Birthday", 149, 4, {7, 40, 74, 110}, 42, true},
};
// the calm waits, played at their own pace (two, so that neighbours don't sway together)
const Clip CALM[2] = {
    {"Tv_Mii_Caster", "Tv_Mii_Caster_Wait_A", 144, 0, {}, 0, false},
    {"Tv_Mii_Caster", "Tv_Mii_Caster_Wait_B", 144, 0, {}, 0, false},
};
// who dances what: children jump, grown-ups dance and clap, the old clap and sway
const ClipId POOL_CHILD[] = {JUMP, CELEBRATION, DANCE_B, BIRTHDAY};
const ClipId POOL_ADULT[] = {DANCE_A, CLAP, DANCE_B, CELEBRATION};
const ClipId POOL_SENIOR[] = {CLAP, DANCE_A, DANCE_B};
// the animation files the actor manager loads (Wait and Head for the BodyController's own use)
const char* const ANIM_FILES[] = {
    "g3d/Mii/Animation/Wait.lex",          "g3d/Mii/Animation/Head.lex",
    "g3d/Mii/Animation/Message_Genre.lex", "g3d/Mii/Animation/Send_DSi.lex",
    "g3d/Mii/Animation/Tv_Mii_Caster.lex", "g3d/Mii/Animation/Color_Anim.lex",
};
const u32 N_ANIM_FILES = sizeof(ANIM_FILES) / sizeof(ANIM_FILES[0]);
// the dances change every so many bars (32 beats in 4/4, 24 in 3/4: a phrase either way)
const int PHRASE_BARS = 8;
// the tempo of a piece without a sequence (a wave jingle), and the frames a piece plays without
// one before it is taken for such a piece (it then gets that tempo, and its dances)
const f32 DEFAULT_BPM = 120;
const u32 NO_SEQUENCE_WAIT_FRAMES = 20;
// a clip switched from is still driven while the BodyController crossfades from it
const u32 CROSSFADE_FRAMES = 45;

// ---- the composition
const u32 MAX_MIIS = 8;
const u32 FACE_RESOLUTION = 0x80;
const f32 TABLE_Z = -50;      // the set's table spot (its Loc_Table)
const f32 TABLE_RADIUS = 33;  // the board's, unscaled
const f32 MII_HALF_WIDTH = 34;
const f32 MII_HEIGHT = 158;  // at height scale 1, with the hair
const f32 AMP_HALF_WIDTH = 30, AMP_HEIGHT = 55, AMP_HALF_DEPTH = 16;
// The places (compose()), set by eye so that heads clear the table and each other: up to four
// Miis on an arc, ARC_SPACING apart across and ARC_GAP behind the table's edge in the middle, a Mii
// x across from the middle ARC_CURVE * x * x further forward; five or more in two rows, ROW_SPACING
// apart across, the front row ROW_GAP behind the table's edge and the back row ROW_DEPTH behind
// it, bowed forward by ROWS_CURVE * x * x. A Mii turns toward the middle by TURN_PER_UNIT degrees
// for every unit across it stands off it.
const f32 ARC_SPACING = 82, ARC_GAP = 42, ARC_CURVE = 0.0012f;
const f32 ROW_SPACING = 66, ROW_GAP = 38, ROW_DEPTH = 84, ROWS_CURVE = 0.0006f;
const f32 TURN_PER_UNIT = 0.075f;
// the camera: a long lens, a little above, as the channel's (a little higher over two rows, so that
// the back row shows behind the front one)
const f32 FOVY = 13, ELEVATION = 9, ELEVATION_ROWS = 12;
// where the tableau goes (layout units: 608 x 456 around the middle, y up): the whole top screen,
// a margin round it (the controls are on the bottom screen: panel.h)
const f32 FRAME_X0 = -272, FRAME_X1 = 272, FRAME_Y0 = -196, FRAME_Y1 = 196;

struct Mii {
    u32 actor = 0, behavior = 0;
    u32 expression = 0;  // the guest u32 its behaviour shows
    u32 age = 0;
    f32 height = MII_HEIGHT;
    f32 x = 0, z = 0, yaw = 0;
    const ClipId* pool = POOL_ADULT;
    int pool_size = 4;
    int beat_offset = 0;  // in the clip's beats: neighbours are at different poses
    int dance = DANCE_A;  // its dance for this phrase
    int clip = -2;        // what plays: a dance, the calm wait (-1), nothing yet (-2)
    int previous = -1;    // the dance it switched from, still driven until previous_until
    u32 previous_until = 0;
    u32 anim[N_DANCES] = {};  // its body's animation object of each dance (made with the body)
};

// the guest strings, made into one block
struct Strings {
    std::vector<u8> bytes;
    u32 add(const char* s) {
        u32 at = (u32)bytes.size();
        bytes.insert(bytes.end(), s, s + std::strlen(s) + 1);
        return at;
    }
};

// Clip beats to a beat of the music: 1; or 2, a slow piece felt in double time (the music under
// half the clip's own tempo); or 1/2, a fast one in half time (over 5/4 of it). From a quarter to
// 5/2 of its own tempo the clip so plays at a half to 5/4 of its own pace; further out it plays
// slower or faster still (CLAP, its own tempo 232 BPM, plays at 0.43 of its pace to 50 BPM).
f32 pace(const Clip& c, f32 bpm) {
    f32 own_bpm = 3600.0f * c.n_beats / c.frames;
    f32 k = 1, rate = bpm / own_bpm;
    if (rate > 1.25f)
        k = 0.5f;
    else if (rate < 0.5f)
        k = 2;
    return k;
}

// the clip's frame at u of its beats: between the beat map's frames, the motion goes evenly
f32 frame_at(const Clip& c, double u) {
    double whole = std::floor(u);
    f32 frac = (f32)(u - whole);
    int i = (int)(((long long)whole % c.n_beats + c.n_beats) % c.n_beats);
    f32 f0 = c.beat[i], f1 = i + 1 < c.n_beats ? c.beat[i + 1] : c.beat[0] + c.frames;
    f32 f = f0 + (f1 - f0) * frac;
    return f >= c.frames ? f - c.frames : f;
}

}  // namespace

struct Stage::Impl {
    // one guest block for the fixed objects: the stage, the Mii shadow's resource, the sound
    // handles' list, the actor manager, the animation file list, the faces' expressions, the
    // household's RFLCreateIDs, a matrix, the strings
    u32 block = 0;
    u32 stage = 0, shadow_res = 0, handles = 0, actors = 0;
    u32 anim_list = 0, words = 0, create_ids = 0, matrix = 0, strings = 0;
    // the clips' names (offsets in `strings`), and the longest of each
    u32 dance_file[N_DANCES] = {}, dance_anim[N_DANCES] = {}, calm_file[2] = {}, calm_anim[2] = {};
    u32 longest_file = 0, longest_anim = 0;
    bool ok = false;

    Amp* amp = nullptr;
    std::vector<Mii> miis;
    f32 table_scale = 1;
    Vec3 amp_at = {0, 0, 0};
    f32 amp_yaw = 0;

    // the camera
    Vec3 cam_pos = {0, 0, 0}, cam_target = {0, 0, 0};
    f32 picture_middle_x = 0;  // what the picture's middle looks at (x at the target's distance)
    f32 fr_top = 0, fr_bottom = 0, fr_left = 0, fr_right = 0, fr_near = 0, fr_far = 0;

    // the groove
    bool dancing = false;
    double beats = 0;       // the music's position in beats (or the free clock's)
    u64 last_tb = 0;        // the guest's time at the last update
    u32 without_beat = 0;   // frames a piece has played without a sequence
    f32 bpm = DEFAULT_BPM;        // the tempo last heard
    f32 paced_bpm = DEFAULT_BPM;  // the tempo the clips' paces go by
    u32 bar = 4;            // quarter notes a bar, last heard
    u32 sound = ~0u;        // the piece current
    bool new_piece = false; // its dances aren't chosen yet
    u32 new_piece_wait = 0;
    int phrase = 0;         // dances chosen so far
    int phrase_index = 0;   // the music's phrase (bars / PHRASE_BARS) they were chosen in
    f32 pulse = 0;
    u32 frame = 0;

    Impl();
    ~Impl();
    u32 str(u32 offset) const { return strings + offset; }
    void make_household(const std::vector<u8>& ids);
    void compose();
    void fit_camera();
    void write_camera();
    void set_clip(Mii& m, int clip);
    void choose_dances();
    void groove(const Music& music);
    void drive(Mii& m);
};

Stage::Impl::Impl() {
    std::string heap;
    u32 free_before = g3d::heap_free(&heap);
    // the household's RFLCreateIDs (model::Family reads the channel's save; it lives on the guest
    // stack only for this)
    std::vector<u8> ids;
    miis.reserve(MAX_MIIS);
    {
        // model::Family (0x10 bytes): the household from the channel's save; model_Family_ctor ctor,
        // model_Family_GetCount count, model_Family_GetPerson person i (0x48 bytes: +0 the Mii's RFLCreateID, 8
        // bytes), model_Family_dtor dtor
        GFRAME(0x20);
        u32 family = (u32)&GS(u32, 0x8);
        g::model_Family_ctor(family, 0);
        u32 n = std::min<u32>(g::model_Family_GetCount(family, 0), MAX_MIIS);
        for (u32 i = 0; i < n; i++) {
            u32 person = g::model_Family_GetPerson(family, i);
            for (u32 k = 0; k < 8; k++)
                ids.push_back(guest::load8(person + k));
        }
        g::model_Family_dtor(family, (u32)-1);
    }
    u32 n = (u32)ids.size() / 8;

    // the strings: the animation files' paths, the clips' names, two of the set's nodes
    Strings s;
    std::vector<u32> paths;
    for (const char* p : ANIM_FILES)
        paths.push_back(s.add(p));
    size_t lf = 0, la = 0;
    auto note = [&](const Clip& c, u32* file, u32* anim) {
        *file = s.add(c.file), *anim = s.add(c.anim);
        if (std::strlen(c.file) > lf)
            lf = std::strlen(c.file), longest_file = *file;
        if (std::strlen(c.anim) > la)
            la = std::strlen(c.anim), longest_anim = *anim;
    };
    for (int c = 0; c < N_DANCES; c++)
        note(DANCES[c], &dance_file[c], &dance_anim[c]);
    for (int c = 0; c < 2; c++)
        note(CALM[c], &calm_file[c], &calm_anim[c]);
    u32 node_board = s.add("pTableBoard"), node_shadow = s.add("pTableShadow");
    u32 node_set = s.add("Mii_Sel_BG");

    // the block's layout
    u32 size = 0;
    auto take = [&](u32 n_bytes) {
        u32 a = size;
        size = (size + n_bytes + 7) & ~7u;
        return a;
    };
    u32 o_stage = take(g3d::STAGE_SIZE), o_shadow = take(g3d::SHADOW_RES_SIZE);
    u32 o_handles = take(g3d::SOUND_HANDLES_SIZE), o_actors = take(g3d::ACTORS_SIZE);
    u32 o_list = take(8 + 4 * N_ANIM_FILES), o_words = take(4 * MAX_MIIS), o_ids = take(8 * MAX_MIIS);
    u32 o_matrix = take(0x30), o_strings = take((u32)s.bytes.size());
    block = g::operator_new(size);
    if (!block) {
        guest::log("soundplayer: stage: out of memory (%u bytes)", (unsigned)size);
        return;
    }
    guest::memset_g(block, 0, size);
    stage = block + o_stage, shadow_res = block + o_shadow, handles = block + o_handles;
    actors = block + o_actors, anim_list = block + o_list, words = block + o_words;
    create_ids = block + o_ids, matrix = block + o_matrix, strings = block + o_strings;
    guest::write_g(strings, s.bytes.data(), (u32)s.bytes.size());
    guest::store32(anim_list, anim_list + 8);
    guest::store32(anim_list + 4, N_ANIM_FILES);
    for (u32 i = 0; i < N_ANIM_FILES; i++)
        guest::store32(anim_list + 8 + 4 * i, str(paths[i]));
    if (!ids.empty())
        guest::write_g(create_ids, ids.data(), (u32)ids.size());

    // the amp's guest data (under everything that follows: freed after it)
    amp = new Amp();

    // the studio, the Miis' globals and their manager, in the TvScene's order
    g3d::stage_ctor(stage, g3d::PATH_WII_TV, g3d::MODEL_WII_TV);
    g3d::shadow_res_ctor(shadow_res);
    g3d::actors_ctor(actors, g3d::stage_root(stage), std::max<u32>(n, 1), FACE_RESOLUTION, anim_list);
    g3d::sound_handles_ctor(handles);
    ok = true;

    make_household(ids);
    compose();

    // the table for this household: its board and its shadow scaled across in the loaded model (the
    // set is this stage's own copy). A node's scale is at +0x20 (x, y, z); its flags lose "scale
    // one" and "scale uniform" (and "SRT identity"), as it is neither now.
    for (u32 name : {node_board, node_shadow}) {
        u32 node = g3d::model_node(g3d::stage_model(stage), str(name));
        if (!node)
            continue;
        g3d::store_f32(node + 0x20, g3d::load_f32(node + 0x20) * table_scale);
        g3d::store_f32(node + 0x28, g3d::load_f32(node + 0x28) * table_scale);
        guest::store32(node + 0x14, guest::load32(node + 0x14) & ~0x19u);
    }

    fit_camera();
    // The set's cyclorama (its node Mii_Sel_BG, 2,520 wide) moved across to the picture's middle: the
    // camera looks at the tableau off to the side, and from far off (a big household) it would see
    // past the set's left end. A node's translation is at +0x38.
    if (u32 node = g3d::model_node(g3d::stage_model(stage), str(node_set)))
        g3d::store_f32(node + 0x38, picture_middle_x);
    choose_dances();
    write_camera();
    g3d::stage_calc(stage);

    // what this costs: the block, the set's file, the Mii model and animation files, the Mii
    // shadow's file, the actors' slots (the faces in them); the resource manager is the actor
    // manager's +0x14 (+0x4 Model.lex's size, +0xC the AnimFile* vector, count at +0x10; an AnimFile's
    // +0x10 is its file's size)
    u32 res = actors + 0x14, n_files = guest::load32(res + 0x10), files = 0;
    for (u32 i = 0; i < n_files; i++)
        files += guest::load32(guest::load32(guest::load32(res + 0xC) + 4 * i) + 0x10);
    f32 dx = cam_pos.x - cam_target.x, dy = cam_pos.y - cam_target.y, dz = cam_pos.z - cam_target.z;
    u32 free_after = g3d::heap_free(&heap);
    guest::log("soundplayer: stage: %u Miis, table x%.2f, camera %.0f away; memory: block %u, set %u, "
               "Mii model %u, %u animation files %u, Mii shadow %u, actor slots %u x %u; heap %s: %u bytes "
               "taken, %u free",
               (unsigned)miis.size(), table_scale, std::sqrt(dx * dx + dy * dy + dz * dz), (unsigned)size,
               (unsigned)guest::load32(stage + 4), (unsigned)guest::load32(res + 4), (unsigned)n_files,
               (unsigned)files, (unsigned)guest::load32(shadow_res + 4), (unsigned)std::max<u32>(n, 1),
               (unsigned)g3d::actor_slot_size(actors), heap.c_str(), (unsigned)(free_before - free_after),
               (unsigned)free_after);
}

Stage::Impl::~Impl() {
    if (ok) {
        // the TvScene's order: the handles' list, the actor manager (its actors with it), the Mii
        // shadow's resource, the stage
        g3d::sound_handles_dtor(handles);
        g3d::actors_dtor(actors);
        g3d::shadow_res_dtor(shadow_res);
        g3d::stage_dtor(stage);
    }
    delete amp;
    if (block)
        g::operator_delete(block);
    std::string heap;
    u32 free_after = g3d::heap_free(&heap);
    guest::log("soundplayer: stage: gone; heap %s: %u bytes free", heap.c_str(), (unsigned)free_after);
}

// every member of the household, each given its behaviour at once
void Stage::Impl::make_household(const std::vector<u8>& ids) {
    u32 n = (u32)ids.size() / 8;
    for (u32 i = 0; i < n; i++) {
        u32 actor = g3d::spawn_mii(create_ids + 8 * i);
        if (!actor) {
            guest::log("soundplayer: stage: member %u's Mii isn't in the database", (unsigned)i);
            continue;
        }
        Mii m;
        m.actor = actor;
        m.expression = words + 4 * (u32)miis.size();
        guest::store32(m.expression, g3d::FACE_NORMAL);
        m.behavior = g3d::give_special_anim(actor);
        {
            // the names' std::strings in the actor's own heap, first made as long as the longest names
            // (an MSL string keeps its buffer for any shorter text: no allocation when a clip changes)
            g3d::ScopedHeap heap(g3d::actor_heap(actor));
            g3d::assign_string(g3d::behavior_file(m.behavior), str(longest_file));
            g3d::assign_string(g3d::behavior_anim(m.behavior), str(longest_anim));
        }
        g3d::behavior_set_expression(m.behavior, m.expression);
        // the body's animation object of each dance, found once (by name: a search of every clip)
        for (int c = 0; c < N_DANCES; c++)
            m.anim[c] = g3d::body_anim(g3d::actor_body(actor), str(dance_file[c]), str(dance_anim[c]));
        m.age = g3d::actor_age(actor);
        m.height = MII_HEIGHT * g3d::body_height_scale(g3d::actor_body(actor));
        if (m.age < 13)
            m.pool = POOL_CHILD, m.pool_size = 4;
        else if (m.age >= 60)
            m.pool = POOL_SENIOR, m.pool_size = 3;
        miis.push_back(m);
        set_clip(miis.back(), -1);
    }
}

// where everyone stands (see the top of the file), and the amp
void Stage::Impl::compose() {
    size_t n = miis.size();
    // the table's scale for a household of 0 to 8, by eye
    static const f32 SCALE[] = {1.0f, 1.0f, 1.35f, 1.6f, 1.8f, 1.9f, 1.9f, 2.0f, 2.1f};
    table_scale = SCALE[std::min<size_t>(n, 8)];
    f32 table_r = TABLE_RADIUS * table_scale;
    // Places (x, z) for a group, the tallest in the middle and the shorter ones to the sides, as in a
    // photograph (and no child crouches out of sight behind the table).
    struct Place {
        f32 x, z;
    };
    auto stand = [&](std::vector<size_t> who, std::vector<Place> places) {
        std::stable_sort(who.begin(), who.end(),
                         [&](size_t a, size_t b) { return miis[a].height > miis[b].height; });
        std::stable_sort(places.begin(), places.end(),
                         [](const Place& a, const Place& b) { return std::fabs(a.x) < std::fabs(b.x) - 1; });
        for (size_t i = 0; i < who.size() && i < places.size(); i++)
            miis[who[i]].x = places[i].x, miis[who[i]].z = places[i].z;
    };
    std::vector<size_t> all(n);
    for (size_t i = 0; i < n; i++)
        all[i] = i;
    if (n == 1) {
        stand(all, {{0, -110}});  // the TV caster's spot (the set's Loc_Mii)
    } else if (n <= 4) {
        // an arc around the far side of the table
        std::vector<Place> arc;
        for (size_t k = 0; k < n; k++) {
            f32 x = ((f32)k - (n - 1) / 2.0f) * ARC_SPACING;
            arc.push_back({x, TABLE_Z - (table_r + ARC_GAP) + ARC_CURVE * x * x});
        }
        stand(all, arc);
    } else {
        // two staggered rows, every other place from the left in front: the shorter half of the
        // household there, the taller half behind
        std::stable_sort(all.begin(), all.end(),
                         [&](size_t a, size_t b) { return miis[a].height < miis[b].height; });
        size_t n_front = (n + 1) / 2;
        std::vector<Place> front, back;
        for (size_t k = 0; k < n; k++) {
            f32 x = ((f32)k - (n - 1) / 2.0f) * ROW_SPACING;
            bool in_front = k % 2 == 0;
            f32 z = TABLE_Z - (table_r + ROW_GAP) - (in_front ? 0 : ROW_DEPTH) + ROWS_CURVE * x * x;
            (in_front ? front : back).push_back({x, z});
        }
        stand(std::vector<size_t>(all.begin(), all.begin() + n_front), front);
        stand(std::vector<size_t>(all.begin() + n_front, all.end()), back);
    }
    f32 max_x = 0;
    for (Mii& m : miis) {
        m.yaw = -TURN_PER_UNIT * m.x;  // facing the camera, turned a little toward the middle
        max_x = std::max(max_x, m.x);
    }
    // the amp: on the floor at the right, a little forward, turned toward the middle
    amp_at = {std::max(max_x + 62.0f, table_r + 46.0f), 0, TABLE_Z + 14};
    amp_yaw = -22;
}

// The camera: a long lens looking at the tableau's middle from a little above, pulled back until
// the tableau fits the frame's size; the frustum's shift then puts the tableau's picture on the
// frame (layout units to the picture's -1..1: x / 304, y / 228).
void Stage::Impl::fit_camera() {
    std::vector<Vec3> pts;
    auto add_box = [&](Vec3 lo, Vec3 hi) {
        for (int i = 0; i < 8; i++)
            pts.push_back({i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z});
    };
    f32 table_r = TABLE_RADIUS * table_scale;
    add_box({-table_r, 0, TABLE_Z - table_r}, {table_r, 68, TABLE_Z + table_r});
    for (const Mii& m : miis) {
        f32 lift = 0;
        for (int k = 0; k < m.pool_size; k++)
            lift = std::max(lift, DANCES[m.pool[k]].lift);
        add_box({m.x - MII_HALF_WIDTH, 0, m.z - 15}, {m.x + MII_HALF_WIDTH, m.height + lift, m.z + 15});
    }
    add_box({amp_at.x - AMP_HALF_WIDTH - 8, 0, amp_at.z - AMP_HALF_DEPTH},
            {amp_at.x + AMP_HALF_WIDTH + 8, AMP_HEIGHT, amp_at.z + AMP_HALF_DEPTH});

    Vec3 lo = pts[0], hi = pts[0];
    for (const Vec3& p : pts) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    Vec3 target = {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
    f32 aspect = g3d::camera_aspect(g3d::camera(g3d::stage_root(stage), 1));
    if (!(aspect > 0.5f && aspect < 3.0f))
        aspect = 5.0f / 3;  // (the top screen's: runtime/patches/graphics.cpp)
    f32 elevation = miis.size() >= 5 ? ELEVATION_ROWS : ELEVATION;
    f32 tan_h = std::tan(deg_to_rad(FOVY) / 2), se = std::sin(deg_to_rad(elevation));
    f32 ce = std::cos(deg_to_rad(elevation));
    // the points' extent in the picture from distance d: x0, x1, y0, y1 in -1..1 units around the
    // lens's axis
    auto extent = [&](f32 d, f32* e) {
        Vec3 c = {target.x, target.y + d * se, target.z + d * ce};
        e[0] = e[2] = 1e9f, e[1] = e[3] = -1e9f;
        for (const Vec3& p : pts) {
            Vec3 v = {p.x - c.x, p.y - c.y, p.z - c.z};
            f32 depth = -v.y * se - v.z * ce;
            f32 x = v.x / (depth * tan_h * aspect), y = (v.y * ce - v.z * se) / (depth * tan_h);
            e[0] = std::min(e[0], x), e[1] = std::max(e[1], x);
            e[2] = std::min(e[2], y), e[3] = std::max(e[3], y);
        }
    };
    f32 want_w = (FRAME_X1 - FRAME_X0) / 304, want_h = (FRAME_Y1 - FRAME_Y0) / 228;
    f32 d_in = 300, d_out = 20000, e[4];
    for (int i = 0; i < 40; i++) {
        f32 d = (d_in + d_out) / 2;
        extent(d, e);
        if (e[1] - e[0] <= want_w && e[3] - e[2] <= want_h)
            d_out = d;
        else
            d_in = d;
    }
    f32 d = d_out;
    extent(d, e);
    // where the axis goes in the picture: the extent's middle on the frame's
    f32 sx = (FRAME_X0 + FRAME_X1) / 2 / 304 - (e[0] + e[1]) / 2;
    f32 sy = (FRAME_Y0 + FRAME_Y1) / 2 / 228 - (e[2] + e[3]) / 2;
    cam_target = target;
    cam_pos = {target.x, target.y + d * se, target.z + d * ce};
    picture_middle_x = target.x - sx * d * tan_h * aspect;
    fr_near = std::max(20.0f, d * 0.3f), fr_far = d + 4000;
    fr_top = fr_near * tan_h * (1 - sy), fr_bottom = -fr_near * tan_h * (1 + sy);
    fr_left = -fr_near * tan_h * aspect * (1 + sx), fr_right = fr_near * tan_h * aspect * (1 - sx);
}

// camera 1 (the stage's current one), before every calc
void Stage::Impl::write_camera() {
    GFRAME(0x50);
    GS(u32, 0x8) = g3d::camera(g3d::stage_root(stage), 1);
    u32 cam = (u32)&GS(u32, 0x8), pos = (u32)&GS(u32, 0x10), posture = (u32)&GS(u32, 0x20);
    g3d::store_f32(pos, cam_pos.x), g3d::store_f32(pos + 4, cam_pos.y), g3d::store_f32(pos + 8, cam_pos.z);
    // PostureInfo: type 0 (look at), up (0, 1, 0) at +4, the target at +0x10
    guest::memset_g(posture, 0, 0x2C);
    g3d::store_f32(posture + 0x8, 1.0f);
    g3d::store_f32(posture + 0x10, cam_target.x);
    g3d::store_f32(posture + 0x14, cam_target.y);
    g3d::store_f32(posture + 0x18, cam_target.z);
    g3d::camera_set_position(cam, pos);
    g3d::camera_set_posture(cam, posture);
    g3d::camera_set_frustum(cam, fr_top, fr_bottom, fr_left, fr_right, fr_near, fr_far);
}

// a Mii's animation: a dance, or the calm wait (-1); the BodyController crossfades to it
void Stage::Impl::set_clip(Mii& m, int clip) {
    if (clip == m.clip)
        return;
    if (m.clip >= 0)
        m.previous = m.clip, m.previous_until = frame + CROSSFADE_FRAMES;
    m.clip = clip;
    int calm = (int)(&m - miis.data()) % 2;
    g3d::ScopedHeap heap(g3d::actor_heap(m.actor));
    g3d::assign_string(g3d::behavior_file(m.behavior), str(clip >= 0 ? dance_file[clip] : calm_file[calm]));
    g3d::assign_string(g3d::behavior_anim(m.behavior), str(clip >= 0 ? dance_anim[clip] : calm_anim[calm]));
}

// Each Mii's dance for this phrase: the next of its pool, not its left-hand neighbour's, nor (when
// the pool allows) the one's two places to the left (in rows: the same row's neighbour).
void Stage::Impl::choose_dances() {
    std::vector<size_t> across(miis.size());
    for (size_t i = 0; i < across.size(); i++)
        across[i] = i;
    std::sort(across.begin(), across.end(), [&](size_t a, size_t b) { return miis[a].x < miis[b].x; });
    int left = -1, left2 = -1;
    for (size_t k = 0; k < across.size(); k++) {
        Mii& m = miis[across[k]];
        int start = phrase + (int)k;
        int pick = -1;
        for (int t = 0; t < m.pool_size && pick < 0; t++) {
            int c = m.pool[(start + t) % m.pool_size];
            if (c != left && c != left2)
                pick = c;
        }
        for (int t = 0; t < m.pool_size && pick < 0; t++) {
            int c = m.pool[(start + t) % m.pool_size];
            if (c != left)
                pick = c;
        }
        m.dance = pick >= 0 ? pick : m.pool[start % m.pool_size];
        left2 = left, left = m.dance;
        // the Miis with the same dance a beat apart from each other (all on the beat, none in step)
        m.beat_offset = 0;
        for (size_t j = 0; j < k; j++)
            if (miis[across[j]].dance == m.dance)
                m.beat_offset++;
        if (dancing)
            set_clip(m, m.dance);
    }
    phrase++;
    static const bool trace = std::getenv("NOMA_SP_BEATS") != nullptr;
    if (trace) {
        std::string heap;
        u32 free = g3d::heap_free(&heap);
        guest::log("soundplayer: stage: dances for phrase %d (heap %s: %u bytes free)", phrase, heap.c_str(),
                   (unsigned)free);
    }
}

// where the music is, and what the Miis do about it
void Stage::Impl::groove(const Music& music) {
    PlayState state = music.state();
    Beat b = music.beat();
    // the guest's clock (the one the music keeps to), for the beats without a sequence
    u64 now = guest::timebase();
    double seconds = last_tb ? (double)(now - last_tb) / guest::TB_HZ : 0;
    last_tb = now;
    bool playing = state == PlayState::PLAYING || state == PlayState::SWITCHING;
    if (playing && b.valid) {
        beats = b.beats, bar = b.bar, without_beat = 0;
        if (b.bpm > 30 && b.bpm < 400)
            bpm = b.bpm;
    } else if (playing) {
        // No sequence to follow: between two pieces the last tempo and bar go on; a piece that plays
        // on without one (a wave jingle) gets the default tempo, and its own bar, not the last's.
        if (state == PlayState::PLAYING && ++without_beat > NO_SEQUENCE_WAIT_FRAMES)
            bpm = DEFAULT_BPM, bar = b.bar;
        beats += seconds * bpm / 60;
    }
    // a tempo that changed by more than a little: the clips' paces again
    if (std::fabs(bpm - paced_bpm) > 0.04f * paced_bpm)
        paced_bpm = bpm;

    // new dances with a new piece (once it plays: the crossfade hides the jump to its first beat)
    // and every PHRASE_BARS bars
    u32 now_sound = music.current();
    if (now_sound != sound)
        sound = now_sound, new_piece = true, new_piece_wait = 0;
    int now_phrase = (int)std::floor(beats / (PHRASE_BARS * bar));
    if (new_piece) {
        if (state == PlayState::PLAYING && (b.valid || ++new_piece_wait > NO_SEQUENCE_WAIT_FRAMES)) {
            new_piece = false, phrase_index = now_phrase;
            choose_dances();
        }
    } else if (playing && now_phrase != phrase_index) {
        phrase_index = now_phrase;
        choose_dances();
    }

    if (playing != dancing) {
        dancing = playing;
        for (Mii& m : miis)
            set_clip(m, dancing ? m.dance : -1);
    }
    double frac = beats - std::floor(beats);
    pulse = dancing ? std::exp(-5.0f * (f32)frac) : pulse * 0.85f;

    // NOMA_SP_BEATS=1: a line a frame (where the music is, what each Mii plays and its frame)
    static const bool trace = std::getenv("NOMA_SP_BEATS") != nullptr;
    if (trace) {
        char line[256];
        int at = std::snprintf(line, sizeof(line), "beat %.3f (%u a bar) bpm %.1f %s", beats, (unsigned)bar, bpm,
                               dancing ? (b.valid ? "sync" : "free") : "calm");
        for (const Mii& m : miis) {
            if (m.clip < 0 || at >= (int)sizeof(line) - 40)
                continue;
            const Clip& c = DANCES[m.clip];
            const char* name = c.anim + (c.anim[0] == 'T' ? 14 : 0);  // less "Tv_Mii_Caster_"
            at += std::snprintf(line + at, sizeof(line) - at, " | %s %.1f", name,
                                frame_at(c, beats * pace(c, paced_bpm) + m.beat_offset));
        }
        guest::log("soundplayer: %s", line);
    }
}

// a Mii's clip's frame from the music, and its face
void Stage::Impl::drive(Mii& m) {
    auto set = [&](int clip) {
        const Clip& c = DANCES[clip];
        if (u32 obj = m.anim[clip]) {
            g3d::anim_set_rate(obj, 0);  // (the BodyController's PlayAnim never sets it back)
            g3d::anim_set_frame(obj, frame_at(c, beats * pace(c, paced_bpm) + m.beat_offset));
        }
    };
    double frac = beats - std::floor(beats);
    u32 face = g3d::FACE_NORMAL;
    if (dancing && m.clip >= 0) {
        set(m.clip);
        if (m.previous >= 0 && frame < m.previous_until)
            set(m.previous);
        // a smile; the mouth opens on the Mii's accent (every beat for a jumper, else once a bar,
        // each Mii on its own beat of it)
        long long beat = (long long)std::floor(beats) + (&m - miis.data());
        bool accent = DANCES[m.clip].jumps ? frac < 0.4 : beat % bar == 0 && frac < 0.5;
        face = accent ? g3d::FACE_OPEN_MOUTH : g3d::FACE_SMILE;
    } else {
        // waiting: a plain face, blinking every 3.5 seconds or so, each Mii at its own moment
        u32 i = (u32)(&m - miis.data());
        face = (frame + 37 * i) % 210 < 6 ? g3d::FACE_BLINK : g3d::FACE_NORMAL;
    }
    guest::store32(m.expression, face);
}

Stage::Stage() : impl_(new Impl) {}

Stage::~Stage() { delete impl_; }

// The frame's 3D, as the TvScene's update does it, with the clips' frames set before the calc that
// evaluates them (zero frames late) and the places and faces written before the actors' update.
void Stage::update(const Music& music) {
    Impl& s = *impl_;
    if (!s.ok)
        return;
    s.frame++;
    s.groove(music);
    for (Mii& m : s.miis) {
        g3d::actor_place(m.actor, m.x, m.z, m.yaw);
        s.drive(m);
    }
    s.write_camera();
    g3d::invalidate_gx_state();
    g3d::stage_calc(s.stage);
    g3d::actors_update(s.actors);
}

// the 3D, first in the scene's draw: the set and the Miis, the amp between the opaque and the
// translucent passes
void Stage::draw() {
    Impl& s = *impl_;
    if (!s.ok)
        return;
    GFRAME(0x10);
    GS(u32, 0x8) = g3d::camera(g3d::stage_root(s.stage), 1);
    u32 cam = (u32)&GS(u32, 0x8);
    g3d::camera_load_projection(cam);
    g3d::stage_draw_opaque(s.stage);
    g3d::camera_view_matrix(cam, s.matrix);
    const f32 at[3] = {s.amp_at.x, s.amp_at.y, s.amp_at.z};
    s.amp->draw(s.matrix, at, s.amp_yaw, s.pulse);
    g3d::stage_draw_translucent(s.stage);
}

}  // namespace engine::soundplayer
