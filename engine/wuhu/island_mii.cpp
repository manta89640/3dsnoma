// The household's Mii on the island (island_mii.h).
//
// Made as the Sound Player's stage makes its Miis (engine/soundplayer/stage.cpp, after the TV
// programme screen's caster), without a set: the 3D sound and a ScnRoot as g3d::Stage makes them
// (engine/ui/g3d.h; g3d_Stage_sInstance stays 0: no code a Mii runs reads it, and the room's code that
// does isn't running), the Mii shadow's resource and the 3D sound handles' list (two globals every
// Mii body and BodyController reads), the actor manager over the root with the animation files the
// island plays (and the BodyController's Wait and Head, the Mii light's Color_Anim) only, the island's
// clip archive added to them if it is staged, and the household's first member spawned and given a
// SpecialAnim behaviour at once (a Mii without one would get the room's AI). The first member is the
// Mii registered first, the player's own (research port.md §4.2).
//
// The clips. A SpecialAnim plays the clip its names give; the BodyController crossfades when they
// change, for as long as island.cpp's motion asks (set_clip: a fade's length in sixtieths of a
// second, written after the actor manager's update as so many of this frame rate's frames: g3d.h
// body_set_blend_frames). Every clip's update rate is 0 and its frame set natively after that update,
// for the root's calc that follows to pose (no frame late):
//   - the gaits (Walk, Male_Walk_Fast, Run, Run_A; a woman's Female_Walk, Female_Run, Female_Run_A)
//     by the distance gone: a frame for every `foot` units, the planted foot's speed over the ground
//     at the clip's own pace (measured on the clips: research anim report §1) times the body's
//     height scale, so the planted foot keeps to the ground whatever the speed. Which gait by the
//     speed (bands with some hysteresis), and all of them share one phase -- 0 at the left foot's
//     strike, 0.5 at the right's -- so that a change of gait keeps the step. A run faster than its
//     clip's pace takes too many steps a second (the room's runs were made for 4.3 and 5.3 units a
//     frame; the island's are 2.5 times that): over `cadence` steps a second the feet still go by
//     the distance while they are down, and the time in the air between is stretched to make up the
//     step (longer bounds, not quicker ones).
//   - a jump (Send_DSi's Jump, frames 12..47: take-off, rise, apex, fall, landing squat) by its
//     rise: the take-off speed over the jump's speed picks the frame from the feet leaving the ground
//     to their reaching it; on landing it plays on from its touchdown by time. A fall off a ledge
//     (Vote's Male_Land or Female_Land: falling, arms up, then the landing) the same way by time. The
//     room's clips have their own rise in them (the feet 30 units up at a jump's apex); where they are
//     played the body is lowered by it (LIFT tables, the lowest ankle's height over its rest by FK of
//     the clips), the archive's have it taken out.
//   - Wait, Turn_Simple, the parachute's hang, a long fall's windmill, treading water by time;
//     after 8 to 15 s of standing a pass of one of Wait_A..D (fidgets).
//   - in the water: the stroke by the distance swum (its hands' entries the strike frames: strokes()
//     says when one went in, and where the wrist was, for the island's splash); treading water under
//     a swimmer's stopping speed, floating after 5 s of it; going in by a fall, a jump or over a
//     boat's side, a plunge played once (feet first, the arms up, into treading water), cut short by
//     a stroke; out onto ground over the water's surface (a quay, a rock), the room's Seiza_Out from
//     its hands on the floor (getting up from kneeling) played once, cut short by a gait. The body is
//     placed by the clip's own water line, bobbed under after going in (as deep as its speed carries it
//     slowing over DIP_TIME), and eased over EASE from where it was drawn when it goes into the water
//     or comes out of it (the swimmer's body is drawn on the waves, the walker's feet on the ground).
//   - the driving poses (the archive's: a pilot's seat, a jet ski's hands on the bars) by the stick:
//     frame 10 + 10 x its x, eased.
// Where a clip's archive source isn't there (no archive staged, or an older one), its content source
// stands in; with neither, Wait does.
//
// NOMA_WUHU_SWIM=paddle: the archive's Paddle (WSR's jet-ski fall as a head-up paddle) for the
// stroke instead of its crawl, to compare them.
//
// NOMA_WUHU_FEET=1: the foot's slide measured on the posed body -- the ankles' world matrices after
// the root's calc: of the updates where the lower ankle is down (within 0.8 units of its rest height
// over the feet, times the height scale) in this update and the last, how far it moved over the
// ground against how far the body went -- logged a second at a time with the gait and its speed.
#include "engine/wuhu/island_mii.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/ui/g3d.h"
#include "noma_decomp.h"

namespace engine::wuhu {

namespace g3d = engine::ui::g3d;
using engine::math::load_f32;
using engine::math::store_f32;
using engine::math::Vec3;
using Clip = IslandMii::Clip;

namespace {

const u32 FACE_RESOLUTION = 0x80;
// The animation files the actor manager loads: always Move (the gaits, turning), Wait (standing; the
// BodyController's own, with Head) and Color_Anim (the Mii light's); the content's Vote, Send_DSi and
// Sit_Sofa only when the island's archive isn't there to stand in for them.
const char* const ANIM_FILES[] = {
    "g3d/Mii/Animation/Move.lex",       "g3d/Mii/Animation/Wait.lex",     "g3d/Mii/Animation/Head.lex",
    "g3d/Mii/Animation/Color_Anim.lex", "g3d/Mii/Animation/Vote.lex",     "g3d/Mii/Animation/Send_DSi.lex",
    "g3d/Mii/Animation/Sit_Sofa.lex",
};
const u32 N_ANIM_FILES = sizeof(ANIM_FILES) / sizeof(ANIM_FILES[0]), N_ANIM_FILES_ALWAYS = 4;
// the island's clip archive (tools/wuhu/mii_anims.py), and its short name among the animation files
const char* const ARCHIVE = "/wuhu/mii_anim.brres";
const char* const ARCHIVE_NAME = "Island";
// a blink every 3.5 s, for a tenth of a second
const u32 BLINK_EVERY = 210, BLINK_FRAMES = 6;
// the ankle's rest height over the sole (Wii no Ma units), and what over it still counts as down
const f32 ANKLE_REST = 10.0f, ANKLE_DOWN = 0.8f;

// the guest strings, made into one block
struct Strings {
    std::vector<u8> bytes;
    u32 add(const char* s) {
        u32 at = (u32)bytes.size();
        bytes.insert(bytes.end(), s, s + std::strlen(s) + 1);
        return at;
    }
};

// How a clip's frame goes.
enum Drive : u8 {
    BY_TIME,      // a frame a sixtieth of a second, looped
    ONCE,         // a frame a sixtieth, one pass (then back to IDLE)
    BY_TURN,      // by time, quicker or slower with the turn
    BY_DISTANCE,  // a gait: by the distance gone
    BY_RISE,      // a jump: by its rise, then (landed) by time from its touchdown
    BY_FALL,      // a fall: by time to just before its touchdown, then (landed) on from it
    BY_STEER,     // a steering axis: 10 + 10 x the stick
    HELD,         // one frame
};
enum Who : u8 { ANY, MAN, WOMAN };

// The feet's height over the ground in the room's clips that rise (units at height scale 1, a frame
// from the clip's frame 0; none after the last): the lowest ankle over its rest height, by FK of the
// clip on the body (it is the same for a man's and a woman's). Send_DSi_Jump's feet leave the ground
// at 15 and are down again at 39; the landings' are down at 14 (Male_Land) and 10 (Female_Land).
const f32 JUMP_LIFT[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5.7f, 11.3f, 16.1f, 19.9f, 23.0f,
                         25.4f, 27.1f, 28.4f, 29.2f, 29.6f, 29.9f, 30.0f, 30.1f, 30.1f, 29.7f, 29.0f, 27.8f,
                         26.0f, 23.4f, 19.9f, 15.4f, 10.0f, 3.4f, 0.8f, 0};
const f32 MALE_LAND_LIFT[] = {26.5f, 24.5f, 22.5f, 20.5f, 18.6f, 16.7f, 14.8f, 12.9f, 11.1f, 9.2f, 7.1f, 4.9f, 2.9f,
                              1.0f, 0};
const f32 FEMALE_LAND_LIFT[] = {13.5f, 11.9f, 10.4f, 8.9f, 7.5f, 6.0f, 4.5f, 3.3f, 2.1f, 1.1f, 0};
template <size_t N>
constexpr u8 count(const f32 (&)[N]) {
    return (u8)N;
}

}  // namespace

// A clip's source: one of the archive's or the content's clips, and how it plays.
struct IslandMii::Source {
    Clip clip;
    Who who;
    bool archive;          // the island's archive's (else the content's)
    const char* file;      // the animation file's short name
    const char* anim;
    u16 first, frames;     // the frames played: the clip's first .. first + frames - 1
    Drive drive;
    f32 foot;              // BY_DISTANCE: the planted foot's speed (units a frame at height scale 1)
    u8 strike, right;      // BY_DISTANCE: the left and right feet's strikes (frames from first)
    u8 down;               //   and the frames a foot is down for (the stretched run's)
    f32 cadence;           //   the most steps a second at which the flight isn't stretched (0: never)
    u8 touchdown;          // BY_RISE, BY_FALL: the frame the feet are down again (from first)
    const f32* lift;       // the feet's height in the clip's own rise, by the clip's own frame (0: none)
    u8 lift_n;
    f32 surface;           // SWIM, TREAD: the water's surface over the feet in the clip (0: none)
};

namespace {

using S = IslandMii::Source;
constexpr S timed(Clip c, Who w, bool ar, const char* f, const char* a, u16 n, Drive d = BY_TIME, f32 surface = 0) {
    return S{c, w, ar, f, a, 0, n, d, 0, 0, 0, 0, 0, 0, nullptr, 0, surface};
}
constexpr S held(Clip c, Who w, bool ar, const char* f, const char* a, u16 first, const f32* lift = nullptr,
                 u8 lift_n = 0) {
    return S{c, w, ar, f, a, first, 1, HELD, 0, 0, 0, 0, 0, 0, lift, lift_n, 0};
}
constexpr S gait(Clip c, Who w, bool ar, const char* f, const char* a, u16 n, f32 foot, u8 strike, u8 right, u8 down,
                 f32 cadence, f32 surface = 0) {
    return S{c, w, ar, f, a, 0, n, BY_DISTANCE, foot, strike, right, down, cadence, 0, nullptr, 0, surface};
}
constexpr S landing(Clip c, Who w, bool ar, const char* f, const char* a, u16 first, u16 n, Drive d, u8 touchdown,
                    const f32* lift = nullptr, u8 lift_n = 0, f32 surface = 0) {
    return S{c, w, ar, f, a, first, n, d, 0, 0, 0, 0, 0, touchdown, lift, lift_n, surface};
}

// The clips' sources, the archive's before the content's (the first one the body has is played). The
// gaits' numbers are measured on the clips (research anim report §1: the planted foot's median speed,
// the feet's strikes and how long they are down); the cadences are a sprinter's.
const S SOURCES[] = {
    timed(IslandMii::IDLE, ANY, false, "Wait", "Wait", 120),
    timed(IslandMii::FIDGET_A, ANY, false, "Wait", "Wait_A", 200, ONCE),
    timed(IslandMii::FIDGET_B, ANY, false, "Wait", "Wait_B", 340, ONCE),
    timed(IslandMii::FIDGET_C, ANY, false, "Wait", "Wait_C", 400, ONCE),
    timed(IslandMii::FIDGET_D, ANY, false, "Wait", "Wait_D", 400, ONCE),
    timed(IslandMii::TURN, ANY, false, "Move", "Turn_Simple", 36, BY_TURN),
    gait(IslandMii::WALK, MAN, false, "Move", "Walk", 60, 1.475f, 12, 42, 21, 0),
    gait(IslandMii::WALK, WOMAN, false, "Move", "Female_Walk", 60, 1.581f, 12, 43, 20, 0),
    gait(IslandMii::WALK_FAST, MAN, false, "Move", "Male_Walk_Fast", 40, 2.66f, 16, 36, 6, 0),
    // (a woman's Female_Walk_Fast is a mincing shuffle of half the stride: she walks on into her run)
    gait(IslandMii::WALK_FAST, WOMAN, false, "Move", "Female_Walk", 60, 1.581f, 12, 43, 20, 0),
    gait(IslandMii::RUN, MAN, false, "Move", "Run", 40, 4.29f, 17, 37, 5, 4.6f),
    gait(IslandMii::RUN, WOMAN, false, "Move", "Female_Run", 30, 5.38f, 14, 29, 4, 4.6f),
    gait(IslandMii::SPRINT, MAN, false, "Move", "Run_A", 40, 4.2f, 17, 37, 5, 5.2f),
    gait(IslandMii::SPRINT, WOMAN, false, "Move", "Female_Run_A", 30, 5.38f, 14, 29, 4, 5.2f),
    // the archive's: the jump and the landings without their rise, WSR's poses
    landing(IslandMii::JUMP, ANY, true, "Island", "Jump", 0, 36, BY_RISE, 26),
    landing(IslandMii::DROP, MAN, true, "Island", "Land_M", 0, 31, BY_FALL, 14),
    landing(IslandMii::DROP, WOMAN, true, "Island", "Land_F", 0, 31, BY_FALL, 10),
    timed(IslandMii::FALL, ANY, true, "Island", "Fall", 20),
    held(IslandMii::SLIDE, ANY, true, "Island", "Slide", 0),
    timed(IslandMii::GLIDE, ANY, true, "Island", "Glide", 99),
    // (the crawl's hands go in at 0 and 24, the paddle's at 17 and 4: their strikes; the hands' pull
    // through the water their foot)
    gait(IslandMii::SWIM, ANY, true, "Island", "Swim", 48, 3.1f, 0, 24, 0, 0, 98.0f),
    gait(IslandMii::SWIM, ANY, true, "Island", "Paddle", 20, 6.0f, 17, 4, 0, 0, 98.0f),
    timed(IslandMii::TREAD, ANY, true, "Island", "Tread", 60, BY_TIME, 98.0f),
    landing(IslandMii::PLUNGE, ANY, true, "Island", "Plunge", 0, 30, ONCE, 0, nullptr, 0, 98.0f),
    timed(IslandMii::FLOAT, ANY, true, "Island", "Float", 60, BY_TIME, 22.0f),
    timed(IslandMii::DRIVE, ANY, true, "Island", "Drive", 21, BY_STEER),
    timed(IslandMii::HELM, ANY, true, "Island", "Helm", 21, BY_STEER),
    // the content's, where there is no archive: the room's jump and landings lowered by their rise;
    // a fall, the parachute's hang and a slide held on a frame of a landing; swimming as walking,
    // treading water and the helm as standing, a sofa for the driving seat
    landing(IslandMii::JUMP, ANY, false, "Send_DSi", "Send_DSi_Jump", 12, 36, BY_RISE, 26, JUMP_LIFT,
            count(JUMP_LIFT)),
    landing(IslandMii::DROP, MAN, false, "Vote", "Male_Land", 0, 31, BY_FALL, 14, MALE_LAND_LIFT,
            count(MALE_LAND_LIFT)),
    landing(IslandMii::DROP, WOMAN, false, "Vote", "Female_Land", 0, 31, BY_FALL, 10, FEMALE_LAND_LIFT,
            count(FEMALE_LAND_LIFT)),
    held(IslandMii::FALL, MAN, false, "Vote", "Male_Land", 4, MALE_LAND_LIFT, count(MALE_LAND_LIFT)),
    held(IslandMii::FALL, WOMAN, false, "Vote", "Female_Land", 4, FEMALE_LAND_LIFT, count(FEMALE_LAND_LIFT)),
    held(IslandMii::SLIDE, ANY, false, "Vote", "Male_Land", 17),
    held(IslandMii::GLIDE, MAN, false, "Vote", "Male_Land", 0, MALE_LAND_LIFT, count(MALE_LAND_LIFT)),
    held(IslandMii::GLIDE, WOMAN, false, "Vote", "Female_Land", 0, FEMALE_LAND_LIFT, count(FEMALE_LAND_LIFT)),
    gait(IslandMii::SWIM, MAN, false, "Move", "Walk", 60, 1.475f, 12, 42, 21, 0),
    gait(IslandMii::SWIM, WOMAN, false, "Move", "Female_Walk", 60, 1.581f, 12, 43, 20, 0),
    timed(IslandMii::TREAD, ANY, false, "Wait", "Wait", 120),
    // climbing out: Seiza_Out from the hands on the floor to standing (Wait is always loaded)
    landing(IslandMii::CLIMB, ANY, false, "Wait", "Seiza_Out", 36, 54, ONCE, 0),
    timed(IslandMii::DRIVE, ANY, false, "Sit_Sofa", "Sit_Sofa", 120),
    timed(IslandMii::HELM, ANY, false, "Wait", "Wait", 120),
};
const u32 N_SOURCES = sizeof(SOURCES) / sizeof(SOURCES[0]);
// the guest strings after the sources' file and clip names: the archive's short name, the ankles', the
// wrists'
const u32 NAME_ARCHIVE = 2 * N_SOURCES, NAME_ANKLE = NAME_ARCHIVE + 1, NAME_WRIST = NAME_ANKLE + 2;

const char* const CLIP_NAMES[] = {"idle", "fidget A", "fidget B", "fidget C", "fidget D", "turn",  "walk",
                                  "walk fast", "run", "sprint", "jump", "drop", "fall", "slide", "glide", "swim",
                                  "tread", "plunge", "float", "climb", "drive", "helm"};
static_assert(sizeof(CLIP_NAMES) / sizeof(CLIP_NAMES[0]) == IslandMii::N_CLIPS, "a name a clip");

// The gaits' speed bands (guest units a sixtieth over the height scale): walk, walk fast, run, then
// sprint (from its speed or with the sprint button held, running); a change up needs the speed HYST
// over a band's top, down HYST under it. A woman has no fast walk (the band is her walk's).
const f32 WALK_TOP = 1.4f, WALK_FAST_TOP = 3.2f, RUN_TOP = 8.0f, WOMAN_WALK_TOP = 1.6f, WOMAN_RUN_TOP = 10.0f;
const f32 HYST = 0.08f;
// moving at all: from START, stopped under STOP (the same units); turning on the spot from TURN_RATE
// degrees a sixtieth, held on for TURN_HOLD sixtieths after the turn stops
const f32 START = 0.12f, STOP = 0.05f, TURN_RATE = 0.8f, TURN_HOLD = 10.0f;
// a fall (not a jump) takes the falling clip after DROP_AFTER s falling faster than DROP_RISE (of a
// jump's take-off speed), the long fall's after FALL_AFTER s (a leap's after LEAP_AFTER)
const f32 DROP_AFTER = 0.1f, DROP_RISE = -0.25f, FALL_AFTER = 0.6f, LEAP_AFTER = 0.25f;
// a swimmer strokes from SWIM_START (guest units a sixtieth over the height scale), treads water under
// SWIM_STOP
const f32 SWIM_START = 0.8f, SWIM_STOP = 0.4f;
// the plunge gives way to a stroke PLUNGE_HOLD sixtieths in (by then the way a running jump carried
// into the water has died away: a stroke is the player's); treading water turns to floating after
// FLOAT_AFTER sixtieths
const f32 PLUNGE_HOLD = 20.0f, FLOAT_AFTER = 300.0f;
// climbing out is played onto ground CLIMB_OVER of the Mii's height over the water and more (wading
// out up a shore is a plain gait); it gives way to a gait once the Mii moves, CLIMB_HOLD sixtieths in
const f32 CLIMB_OVER = 0.05f, CLIMB_HOLD = 20.0f;
// The body eased over EASE sixtieths from where it was drawn, going into the water or out of it (a
// change of place no further than EASE_REACH of the Mii's height on x/z: not a warp). The bob
// after going in: the body carries on down at the speed it went in, slowing to its deepest DIP_TIME
// sixtieths later (that speed x DIP_TIME / e: dip(t) = peak (t / DIP_TIME) e^(1 - t / DIP_TIME)), at
// most DIP_MOST of the Mii's height (the top of the head stays out of the water: a jump off the sea
// front took it under whole for a third of a second, the plunge unseen), and is back up by DIP_END
// (under a unit off).
// Out of the water the ease begins no more than OUT_FROM of the Mii's height under where the body
// goes: the swimmer's feet hang 0.62 of it under the surface, and from there the climb's first pose
// (kneeling, the hands on the quay) would be under water whole.
const f32 EASE = 12.0f, EASE_REACH = 1.5f, OUT_FROM = 0.45f, DIP_TIME = 8.0f, DIP_MOST = 0.35f, DIP_END = 60.0f;
const f32 EULER_E = 2.7182818f;
// a landing gives way to a gait once the Mii moves, LAND_HOLD sixtieths after its touchdown
const f32 LAND_HOLD = 5.0f;
// the steering poses follow the stick with this time constant (s)
const f32 STEER_EASE = 0.15f;

// Crossfades (sixtieths of a second) from one clip to another.
f32 fade_between(int from, Clip to) {
    auto gait = [](int c) { return c >= IslandMii::WALK && c <= IslandMii::SPRINT; };
    auto idle = [](int c) { return c >= IslandMii::IDLE && c <= IslandMii::FIDGET_D; };
    if (from < 0 || to == IslandMii::DRIVE || to == IslandMii::HELM || from == IslandMii::DRIVE ||
        from == IslandMii::HELM)
        return 1;  // (in or out of a vehicle: the Mii is put there)
    if (to == IslandMii::JUMP)
        return 2;
    // the water's: into the plunge at once (it is the fall's end), out of it slowly into the stroke or
    // treading water; into the climb out quickly, out of it as a landing; drifting into floating
    if (to == IslandMii::PLUNGE)
        return 3;
    if (from == IslandMii::PLUNGE)
        return 12;
    if (to == IslandMii::CLIMB)
        return 4;
    if (from == IslandMii::CLIMB)
        return 8;
    if (to == IslandMii::FLOAT)
        return 20;
    if (gait(from) && gait(to))
        return 6;
    if (idle(from) && idle(to))
        return 15;
    if (to == IslandMii::TURN || from == IslandMii::TURN)
        return 6;
    if (from == IslandMii::JUMP || from == IslandMii::DROP)
        return idle(to) ? 10 : 6;
    if (from == IslandMii::FALL && to == IslandMii::DROP)
        return 4;
    if (to == IslandMii::DROP)
        return 8;
    if (to == IslandMii::FALL)
        return from == IslandMii::DROP ? 12 : 10;
    if (to == IslandMii::GLIDE)
        return 18;
    if (to == IslandMii::SLIDE)
        return 6;
    if (to == IslandMii::SWIM || to == IslandMii::TREAD)
        return from == IslandMii::SWIM || from == IslandMii::TREAD || from == IslandMii::FLOAT ? 15 : 12;
    return 8;
}

f32 lift_at(const S& s, f32 frame) {
    if (!s.lift)
        return 0;
    const f32 f = (f32)s.first + frame;
    const int i = (int)f;
    if (i < 0 || i + 1 >= (int)s.lift_n)
        return i >= 0 && i < (int)s.lift_n ? s.lift[i] : 0;
    const f32 t = f - (f32)i;
    return s.lift[i] * (1 - t) + s.lift[i + 1] * t;
}

f32 wrap(f32 f, f32 n) {
    f = std::fmod(f, n);
    return f < 0 ? f + n : f;
}

// A gait's frame `f` on by `ticks` sixtieths at `rate_down` frames a sixtieth while a foot is down
// (the distance's) and `rate_up` while neither is
f32 stretched(const S& s, f32 f, f32 ticks, f32 rate_down, f32 rate_up) {
    const f32 n = (f32)s.frames, down = (f32)s.down;
    for (int guard = 0; ticks > 1e-6f && guard < 16; guard++) {
        const f32 from_l = wrap(f - s.strike, n), from_r = wrap(f - s.right, n);
        f32 rate, edge;
        if (from_l < down)
            rate = rate_down, edge = down - from_l;
        else if (from_r < down)
            rate = rate_down, edge = down - from_r;
        else
            rate = rate_up, edge = std::min(n - from_l, n - from_r);
        if (!(rate > 0))
            break;
        const f32 t = edge / rate;
        if (t >= ticks) {
            f += rate * ticks;
            break;
        }
        f += edge + 1e-4f;
        ticks -= t;
    }
    return wrap(f, n);
}

}  // namespace

IslandMii::IslandMii() {}

IslandMii::~IslandMii() {
    // the reverse of make_step's order (nothing was made if the block couldn't be had: step_ is
    // then STEPS, the addresses 0); the archive's AnimFile goes with the actor manager's resources
    if (block_) {
        if (step_ >= 6)
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
        feet_log_ = std::getenv("NOMA_WUHU_FEET") && std::atoi(std::getenv("NOMA_WUHU_FEET"));
        // the island's archive, if it is staged (an uncompressed BRRES: "bres", big-endian, its size)
        const std::string path = guest::data_dir() + ARCHIVE;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0x40 && n < (1 << 22)) {
                archive_.resize((size_t)n);
                if (std::fread(archive_.data(), 1, (size_t)n, f) != (size_t)n)
                    archive_.clear();
            }
            std::fclose(f);
            const u8* b = archive_.data();
            if (!archive_.empty() &&
                (std::memcmp(b, "bres", 4) != 0 || b[4] != 0xFE || b[5] != 0xFF ||
                 (u32)(b[8] << 24 | b[9] << 16 | b[10] << 8 | b[11]) != (u32)n)) {
                guest::log("wuhu: Mii: %s isn't a BRRES as the stager writes them: the content's clips", path.c_str());
                archive_.clear();
            }
        }
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
        // the strings: the files, the clips' names (a woman's and a man's: the sex is known only
        // once the Mii is made), the archive's short name, the ankles' (NOMA_WUHU_FEET), the wrists'
        // (the strokes' splashes)
        const u32 n_files = archive_.empty() ? N_ANIM_FILES : N_ANIM_FILES_ALWAYS;
        Strings s;
        std::vector<u32> paths;
        for (u32 i = 0; i < n_files; i++)
            paths.push_back(s.add(ANIM_FILES[i]));
        std::vector<u32> names;
        size_t longest_file = 0, longest_anim = 0;
        u32 o_longest_file = 0, o_longest_anim = 0;
        for (const S& src : SOURCES) {
            names.push_back(s.add(src.file));
            names.push_back(s.add(src.anim));
            if (std::strlen(src.file) > longest_file)
                longest_file = std::strlen(src.file), o_longest_file = names[names.size() - 2];
            if (std::strlen(src.anim) > longest_anim)
                longest_anim = std::strlen(src.anim), o_longest_anim = names.back();
        }
        const u32 o_archive = s.add(ARCHIVE_NAME), o_ankle_l = s.add("Ankle_L"), o_ankle_r = s.add("Ankle_R");
        const u32 o_wrist_l = s.add("Wrist_L"), o_wrist_r = s.add("Wrist_R");
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
        guest::write_g(strings_, s.bytes.data(), (u32)s.bytes.size());
        guest::write_g(create_id_, id, 8);
        guest::store32(anim_list_, anim_list_ + 8);
        guest::store32(anim_list_ + 4, n_files);
        for (u32 i = 0; i < n_files; i++)
            guest::store32(anim_list_ + 8 + 4 * i, strings_ + paths[i]);
        guest::store32(expression_, g3d::FACE_NORMAL);
        for (u32 o : names)
            name_at_.push_back(strings_ + o);
        name_at_.push_back(strings_ + o_archive);
        name_at_.push_back(strings_ + o_ankle_l);
        name_at_.push_back(strings_ + o_ankle_r);
        name_at_.push_back(strings_ + o_wrist_l);
        name_at_.push_back(strings_ + o_wrist_r);
        longest_file_ = strings_ + o_longest_file, longest_anim_ = strings_ + o_longest_anim;
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
        g3d::actors_ctor(actors_, guest::load32(root_word_), 1, FACE_RESOLUTION, anim_list_);
        break;
    case 4: add_archive(); break;
    case 5: {
        g3d::sound_handles_ctor(handles_);
        actor_ = have_id_ ? g3d::spawn_mii(create_id_) : 0;
        if (!actor_) {
            guest::log("wuhu: Mii: the household's first Mii isn't in the database%s: no Mii",
                       have_id_ ? "" : " (no household)");
            break;
        }
        female_ = g3d::actor_female(actor_);
        behavior_ = g3d::give_special_anim(actor_);
        {
            // the names' std::strings in the actor's own heap, first as long as the longest names (an
            // MSL string keeps its buffer for any shorter text: no allocation when the clip changes)
            g3d::ScopedHeap heap(g3d::actor_heap(actor_));
            g3d::assign_string(g3d::behavior_file(behavior_), longest_file_);
            g3d::assign_string(g3d::behavior_anim(behavior_), longest_anim_);
        }
        g3d::behavior_set_expression(behavior_, expression_);
        height_scale_ = g3d::body_height_scale(g3d::actor_body(actor_));
        resolve_clips();
        set_clip(IDLE, 1);
        // what it costs: the block, the Mii model and the animation files (the actor manager's
        // resources, +0x14: +0x4 Model.lex's size, +0xC the AnimFile* vector, count at +0x10; an
        // AnimFile's +0x10 its file's size), the shadow's file, the actor's slot
        u32 res = actors_ + 0x14, n_files = guest::load32(res + 0x10), files = 0;
        for (u32 i = 0; i < n_files; i++)
            files += guest::load32(guest::load32(guest::load32(res + 0xC) + 4 * i) + 0x10);
        std::string heap;
        u32 free_now = g3d::heap_free(&heap);
        guest::log("wuhu: Mii: %s, height scale %.2f; memory: Mii model %u, %u animation files %u (the island's "
                   "archive %u), Mii shadow %u, actor slot %u; heap %s: %u bytes taken, %u free",
                   female_ ? "female" : "male", height_scale_, (unsigned)guest::load32(res + 4), (unsigned)n_files,
                   (unsigned)files, archive_file_ ? (unsigned)archive_.size() : 0u,
                   (unsigned)guest::load32(shadow_res_ + 4), (unsigned)g3d::actor_slot_size(actors_), heap.c_str(),
                   (unsigned)(heap_before_ - free_now), (unsigned)free_now);
        {
            const u32 model = g3d::body_model(g3d::actor_body(actor_));
            for (int k = 0; k < 2; k++) {
                const u32 wrist = g3d::model_node(model, name_at_[NAME_WRIST + k]);
                wrist_node_[k] = wrist ? guest::load32(wrist + 0xC) : 0;
                if (feet_log_) {
                    const u32 ankle = g3d::model_node(model, name_at_[NAME_ANKLE + k]);
                    ankle_[k] = ankle ? guest::load32(ankle + 0xC) : 0;
                }
            }
            feet_log_ = feet_log_ && ankle_[0] && ankle_[1];
        }
        break;
    }
    }
    step_++;
    return step_ < STEPS;
}

// The island's archive into the actor manager's animation files (before the Mii is spawned: its body
// makes the animation objects of the files there are then), as the one named "Island".
void IslandMii::add_archive() {
    if (archive_.empty())
        return;
    // (the files the body reads are mii_ModelRes_sInstance's: the manager's own)
    if (guest::load32(GA_mii_ModelRes_sInstance) != actors_ + 0x14) {
        guest::log("wuhu: Mii: the Mii resources aren't the island's actor manager's: no archive");
        return;
    }
    archive_file_ = g3d::anim_file_add(actors_, name_at_[NAME_ARCHIVE], archive_.data(), (u32)archive_.size());
    if (!archive_file_)
        guest::log("wuhu: Mii: no room for the island's archive (%u bytes): the content's clips",
                   (unsigned)archive_.size());
}

// Each clip's source: the first of its sources (the archive's first) the body has, for the Mii's
// sex; else IDLE's. NOMA_WUHU_SWIM=paddle: the stroke's Paddle first.
void IslandMii::resolve_clips() {
    const u32 body = g3d::actor_body(actor_);
    const char* swim = std::getenv("NOMA_WUHU_SWIM");
    const bool paddle = swim && std::strcmp(swim, "paddle") == 0;
    u32 from_archive = 0, from_content = 0;
    std::string missing;
    for (int c = 0; c < N_CLIPS; c++) {
        for (int pass = c == SWIM && paddle ? 0 : 1; pass < 2; pass++)
            for (u32 i = 0; i < N_SOURCES && !anim_[c]; i++) {
                const S& s = SOURCES[i];
                if (s.clip != c || (s.who == MAN && female_) || (s.who == WOMAN && !female_) ||
                    (s.archive && !archive_file_) || (pass == 0 && std::strcmp(s.anim, "Paddle") != 0))
                    continue;
                const u32 file = name_at_[2 * i], anim = name_at_[2 * i + 1];
                if (const u32 obj = g3d::body_anim(body, file, anim)) {
                    anim_[c] = obj, source_[c] = &s, clip_file_[c] = file, clip_anim_[c] = anim;
                    (s.archive ? from_archive : from_content)++;
                }
            }
        if (!anim_[c] && c != IDLE) {
            missing += missing.empty() ? "" : ", ";
            missing += CLIP_NAMES[c];
        }
    }
    for (int c = 0; c < N_CLIPS; c++)
        if (!anim_[c] && anim_[IDLE])
            anim_[c] = anim_[IDLE], source_[c] = source_[IDLE], clip_file_[c] = clip_file_[IDLE],
            clip_anim_[c] = clip_anim_[IDLE];
    guest::log("wuhu: Mii: clips: %u from the island's archive, %u from the content%s%s", (unsigned)from_archive,
               (unsigned)from_content, missing.empty() ? "" : "; standing for ", missing.c_str());
}

// The clip the behaviour plays, crossfaded to over `fade` sixtieths of a second (written after the
// actor manager's update: update()). From its first frame; the same names as the clip playing (a
// content clip standing for two) change nothing in the body.
void IslandMii::set_clip(Clip c, f32 fade) {
    if ((int)c == clip_)
        return;
    const bool same = clip_ >= 0 && clip_file_[c] == clip_file_[clip_] && clip_anim_[c] == clip_anim_[clip_];
    clip_ = c;
    frame_ = 0, played_ = 0;
    landed_ = false;
    if (same || !clip_file_[c])
        return;
    fade_ = fade;
    g3d::ScopedHeap heap(g3d::actor_heap(actor_));
    g3d::assign_string(g3d::behavior_file(behavior_), clip_file_[c]);
    g3d::assign_string(g3d::behavior_anim(behavior_), clip_anim_[c]);
}

// the gait for a speed (guest units a sixtieth), with the band the playing gait is in kept a little
// past its edges
IslandMii::Clip IslandMii::gait_for(f32 speed, bool sprint) const {
    const f32 v = speed / std::max(height_scale_, 0.1f);
    const int now = clip_ >= WALK && clip_ <= SPRINT ? clip_ : WALK;
    const f32 tops[3] = {female_ ? WOMAN_WALK_TOP : WALK_TOP, female_ ? WOMAN_WALK_TOP : WALK_FAST_TOP,
                         female_ ? WOMAN_RUN_TOP : RUN_TOP};
    int g = WALK;
    for (int i = 0; i < 3; i++) {
        // (the edge between band i and i + 1: crossed upward HYST over it, downward HYST under it)
        const f32 edge = tops[i] * (now > WALK + i ? 1.0f - HYST : 1.0f + HYST);
        if (v > edge)
            g = WALK + i + 1;
    }
    if (female_ && g == WALK_FAST)
        g = WALK;
    if (sprint && g == RUN)
        g = SPRINT;
    return (Clip)g;
}

// A gait's frame on by `walked` guest units in `frames` sixtieths, from the common phase; the phase
// from the new frame.
void IslandMii::move_gait(Clip c, f32 walked, u32 frames) {
    const S& s = *source_[c];
    if (s.drive != BY_DISTANCE || !(s.foot > 0))
        return;
    const f32 n = (f32)s.frames, hs = std::max(height_scale_, 0.1f);
    f32 f = wrap(phase_ * n + s.strike, n);
    if (walked > 0) {
        const f32 rate_down = walked / (f32)frames / (s.foot * hs);  // frames a sixtieth, foot down
        // the cycle's time at that rate, and the shortest the cadence allows (two steps a cycle)
        const f32 cycle = n / rate_down, shortest = s.cadence > 0 ? 120.0f / s.cadence : 0;
        if (cycle >= shortest || s.down == 0) {
            f = wrap(f + rate_down * (f32)frames, n);
        } else {
            // the feet down at the distance's rate; the time in the air what is left of the shortest
            // cycle (it is more than nothing: the feet are down less than the whole cycle)
            const f32 down = 2.0f * s.down, air = n - down;
            const f32 rate_up = air / std::max(shortest - down / rate_down, 1.0f);
            f = stretched(s, f, (f32)frames, rate_down, rate_up);
        }
    }
    frame_ = f;
    phase_ = wrap(f - s.strike, n) / n;
}

// The clip for what the Mii is doing, and its frame on by this update.
void IslandMii::choose(const MiiMotion& m) {
    const f32 ticks = (f32)m.frames;
    const f32 speed = m.walked / ticks, hs = std::max(height_scale_, 0.1f);
    const bool moving = speed > (clip_ >= WALK && clip_ <= SPRINT ? STOP : START) * hs;
    const bool air_clip = clip_ == JUMP || clip_ == DROP || clip_ == FALL || clip_ == GLIDE || clip_ == SLIDE;
    strokes_ = footfalls_ = 0;
    if (m.kind != MiiMotion::SWIM)
        treading_ = 0;
    // the clip
    Clip want = (Clip)std::max(clip_, 0);
    switch (m.kind) {
    case MiiMotion::CAR: want = DRIVE; break;
    case MiiMotion::BOAT: want = HELM; break;
    case MiiMotion::SWIM:
        if ((m.entry_speed > 0 || air_clip) && clip_ != PLUNGE && own(PLUNGE)) {
            // in from the air or over a boat's side
            want = PLUNGE;
            treading_ = 0;
        } else if (clip_ == PLUNGE && frame_ + ticks < (f32)source_[PLUNGE]->frames - 1 &&
                   !(played_ >= PLUNGE_HOLD && speed > SWIM_START * hs)) {
            want = PLUNGE;
        } else if (speed > (clip_ == SWIM ? SWIM_STOP : SWIM_START) * hs) {
            want = SWIM;
            treading_ = 0;
        } else {
            treading_ += ticks;
            want = treading_ >= FLOAT_AFTER && own(FLOAT) ? FLOAT : TREAD;
        }
        break;
    case MiiMotion::GLIDE: want = GLIDE; break;
    case MiiMotion::SLIDE:
        if (m.air_time > DROP_AFTER || air_clip)
            want = SLIDE;
        break;
    case MiiMotion::AIR:
        if (m.jump) {
            // (a jump from a landing begins again: the same clip, from its take-off)
            if (clip_ == JUMP && landed_)
                frame_ = 0, played_ = 0, landed_ = false;
            want = JUMP;
        } else if (m.leap) {
            if (m.air_time > LEAP_AFTER)
                want = FALL;
        } else if (clip_ == DROP && m.air_time > FALL_AFTER) {
            want = FALL;
        } else if (clip_ != FALL && m.air_time > DROP_AFTER && m.rise < DROP_RISE) {
            want = DROP;
        }
        break;
    case MiiMotion::GROUND: {
        if (m.climbed_out && m.over_surface > CLIMB_OVER * MII_HEIGHT * hs && own(CLIMB)) {
            // out onto a quay or a rock: the climb out, until its end or (moving) a moment into it
            want = CLIMB;
            break;
        }
        if (clip_ == CLIMB) {
            const bool done = frame_ + ticks >= (f32)source_[CLIMB]->frames - 1;
            want = (moving && played_ >= CLIMB_HOLD) || done ? (moving ? gait_for(speed, m.sprint) : IDLE) : CLIMB;
            break;
        }
        if (clip_ == FALL || clip_ == GLIDE || clip_ == SLIDE) {
            // down from a long fall, the parachute or a slide: the drop's landing
            set_clip(DROP, fade_between(clip_, DROP));
            landed_ = true;
            frame_ = source_[DROP]->touchdown;
            want = DROP;
        } else if ((clip_ == JUMP || clip_ == DROP) && !landed_) {
            landed_ = true, played_ = 0;
            frame_ = std::max(frame_, (f32)source_[clip_]->touchdown);
            want = (Clip)clip_;
        }
        if ((clip_ == JUMP || clip_ == DROP) && landed_) {
            // the landing plays on, until its end or (moving) a moment after its touchdown
            const bool done = frame_ + ticks >= (f32)source_[clip_]->frames - 1;
            if ((moving && played_ >= LAND_HOLD) || done)
                want = moving ? gait_for(speed, m.sprint) : IDLE;
            else
                want = (Clip)clip_;
        } else if (moving) {
            want = gait_for(speed, m.sprint);
            turning_ = TURN_HOLD + 1;
        } else {
            turning_ = std::fabs(m.turned) > TURN_RATE * ticks ? 0 : turning_ + ticks;
            if (turning_ <= TURN_HOLD)
                want = TURN;
            else if (clip_ < FIDGET_A || clip_ > FIDGET_D)
                want = IDLE;
        }
        break;
    }
    }
    if (clip_ < 0)
        want = IDLE;
    // (a gait's frame goes on from the common phase)
    const bool gait_change = want != clip_ && want >= WALK && want <= SPRINT;
    set_clip(want, fade_between(clip_, want));
    if (gait_change)
        move_gait(want, 0, m.frames);

    // its frame
    const S& s = *source_[clip_];
    const f32 n = (f32)s.frames;
    switch (s.drive) {
    case BY_TIME: frame_ = wrap(frame_ + ticks, n); break;
    case ONCE:
        frame_ += ticks;
        if (frame_ >= n - 1 && (clip_ == PLUNGE || clip_ == CLIMB)) {
            frame_ = n - 1;  // (held there: the next choose() hands it on, to the water's clips or a gait)
        } else if (frame_ >= n - 1) {
            set_clip(IDLE, fade_between(clip_, IDLE));
            idle_ = 0;
        }
        break;
    case BY_TURN: {
        const f32 rate = std::min(std::max(std::fabs(m.turned) / ticks / 3.5f, 0.7f), 1.3f);
        frame_ = wrap(frame_ + rate * ticks, n);
        break;
    }
    case BY_DISTANCE: {
        const f32 was = frame_;
        move_gait((Clip)clip_, m.walked, m.frames);
        // the strike frames passed (the left's, the right's): the archive's stroke, a hand in the
        // water; a gait's, a foot down on the ground (the island's footsteps)
        if (m.walked > 0) {
            const f32 gone = wrap(frame_ - was, n);
            auto passed = [&](u8 at) {
                const f32 d = wrap((f32)at - was, n);
                return d > 0 && d <= gone;
            };
            const u8 bits = (passed(s.strike) ? 1 : 0) | (passed(s.right) ? 2 : 0);
            if (clip_ == SWIM && s.archive)
                strokes_ = bits;
            else if (clip_ >= WALK && clip_ <= SPRINT && m.kind == MiiMotion::GROUND && bits)
                footfalls_ = bits | (clip_ >= RUN ? FOOTFALL_RUN : 0);
        }
        break;
    }
    case BY_RISE:
        if (!landed_) {
            // the feet leave the ground at 3 and are down again at the touchdown: between, the rise
            // (1 at take-off, 0 at the apex, -1 as fast down as it went up); its first frames (the
            // push off the ground) a frame a sixtieth while the body goes up
            const f32 t = std::min(std::max((1.0f - m.rise) * 0.5f, 0.0f), 1.0f);
            const f32 air = 3.0f + ((f32)s.touchdown - 5.0f) * t;
            frame_ = std::max(frame_, std::min(air, played_));
        } else {
            frame_ = std::min(frame_ + ticks, n - 1);
        }
        break;
    case BY_FALL:
        if (!landed_)
            frame_ = std::min((f32)s.touchdown - 2.0f, played_ * 0.6f);
        else
            frame_ = std::min(frame_ + ticks, n - 1);
        break;
    case BY_STEER: {
        const f32 k = 1.0f - std::exp(-ticks / 60.0f / STEER_EASE);
        steer_ += (std::min(std::max(m.steer, -1.0f), 1.0f) - steer_) * k;
        frame_ = std::min(std::max(10.0f + 10.0f * steer_, 0.0f), n - 1);
        break;
    }
    case HELD: frame_ = 0; break;
    }
    played_ += ticks;
    // standing: now and then a fidget
    if (clip_ == IDLE) {
        idle_ += ticks;
        const f32 wait = 480.0f + (f32)((fidgets_ * 271u) % 420u);
        const Clip fidget = (Clip)(FIDGET_A + fidgets_ % 4);
        if (idle_ > wait && source_[fidget] != source_[IDLE]) {
            fidgets_++;
            set_clip(fidget, fade_between(IDLE, fidget));
        }
    } else if (clip_ < FIDGET_A || clip_ > FIDGET_D) {
        idle_ = 0;
    }
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

void IslandMii::update(const engine::g3d::Camera& camera, Vec3 at, f32 yaw, const MiiMotion& m) {
    if (step_ < STEPS || !block_)
        return;
    const u32 root = guest::load32(root_word_);
    if (actor_ && source_[IDLE]) {
        ticks_ += m.frames;
        choose(m);
        guest::store32(expression_, ticks_ % BLINK_EVERY < BLINK_FRAMES ? g3d::FACE_BLINK : g3d::FACE_NORMAL);
        // the behaviour and the BodyController first (a changed clip attached, its fade begun); then
        // the fade's length, the body where the feet are (less the clip's own rise; a swimmer's by
        // the water's surface) and the clip at its frame, for the root's calc that follows to pose
        g3d::actors_update(actors_);
        const u32 body = g3d::actor_body(actor_);
        if (fade_ >= 0) {
            g3d::body_set_blend_frames(body, (u32)std::max(1.0f, std::round(fade_ / (f32)m.frames)));
            fade_ = -1;
        }
        const S& s = *source_[clip_];
        const Vec3 p = place(m, at);
        g3d::actor_place(actor_, p.x, p.y, p.z, yaw);
        g3d::anim_set_rate(anim_[clip_], 0);  // (the BodyController's PlayAnim never sets it back)
        g3d::anim_set_frame(anim_[clip_], (f32)s.first + frame_);
    }
    write_camera(camera);
    g3d::invalidate_gx_state();
    g3d::root_calc(root);
    if (actor_ && source_[IDLE] && strokes_)
        read_wrists();
    if (actor_ && source_[IDLE] && feet_log_)
        measure_feet(Vec3{at.x, at.y, at.z}, m.kind == MiiMotion::GROUND ? m.walked : 0);
    GFRAME(0x10);
    GS(u32, 0x8) = g3d::camera(root, 1);
    g3d::sound3d_listen(sound3d_, (u32)&GS(u32, 0x8));
}

// Where the body goes for the clip playing: at the feet, less the clip's own rise; a swimmer by the
// water's surface (the clip's water line on it), bobbed under after going in; eased from where it was
// drawn when the Mii goes into the water or comes out of it (up out of the water, over a quay's edge;
// not into or out of a vehicle: it is put there; not a warp's: far off)
Vec3 IslandMii::place(const MiiMotion& m, Vec3 at) {
    const S& s = *source_[clip_];
    const f32 hs = height_scale_, ticks = (f32)m.frames, height = MII_HEIGHT * hs;
    const bool swimming = m.kind == MiiMotion::SWIM;
    f32 y = at.y - lift_at(s, frame_) * hs;
    if (s.surface > 0 && swimming)
        y = at.y + m.depth - s.surface * hs;
    if (swimming && m.entry_speed > 0)
        dip_t_ = 0, dip_peak_ = std::min(m.entry_speed * DIP_TIME / EULER_E, DIP_MOST * height);
    else
        dip_t_ = swimming ? dip_t_ + ticks : DIP_END;
    if (dip_t_ < DIP_END) {
        const f32 u = dip_t_ / DIP_TIME;
        y -= dip_peak_ * u * std::exp(1.0f - u);
    }
    Vec3 p{at.x, y, at.z};
    const Vec3 off = placed_at_ - p;
    const f32 reach = EASE_REACH * height;
    if (placed_ && swimming != was_swimming_ && m.kind != MiiMotion::CAR && m.kind != MiiMotion::BOAT &&
        off.x * off.x + off.z * off.z < reach * reach) {
        ease_t_ = 0, ease_from_ = off;
        if (!swimming)
            ease_from_.y = std::max(ease_from_.y, -OUT_FROM * height);
    } else {
        ease_t_ += ticks;
    }
    if (ease_t_ < EASE) {
        const f32 e = ease_t_ / EASE;
        p = p + ease_from_ * (1.0f - e * e * (3.0f - 2.0f * e));
    }
    placed_ = true, was_swimming_ = swimming;
    placed_at_ = p;
    return p;
}

// the wrists of the hands that went in this update, on the posed body (a hand whose wrist can't be
// had is dropped)
void IslandMii::read_wrists() {
    const u32 body = g3d::actor_body(actor_);
    GFRAME(0x40);
    const u32 mtx = (u32)&GS(u32, 0x8);
    for (int k = 0; k < 2; k++) {
        if (!(strokes_ >> k & 1))
            continue;
        if (wrist_node_[k] && g3d::body_node_world(body, wrist_node_[k], mtx))
            wrist_[k] = Vec3{load_f32(mtx + 0xC), load_f32(mtx + 0x1C), load_f32(mtx + 0x2C)};
        else
            strokes_ &= (u8)~(1u << k);
    }
}

// NOMA_WUHU_FEET: the lower ankle's move over the ground while it is down (in this update and the
// last), against the body's
void IslandMii::measure_feet(Vec3 at, f32 walked) {
    const u32 body = g3d::actor_body(actor_);
    Vec3 p[2];
    {
        GFRAME(0x40);
        const u32 mtx = (u32)&GS(u32, 0x8);
        for (int k = 0; k < 2; k++) {
            if (!g3d::body_node_world(body, ankle_[k], mtx))
                return;
            p[k] = Vec3{load_f32(mtx + 0xC), load_f32(mtx + 0x1C), load_f32(mtx + 0x2C)};
        }
    }
    if (std::getenv("NOMA_WUHU_FEET_TRACE")) {
        GFRAME(0x40);
        const u32 mtx = (u32)&GS(u32, 0x8);
        std::string nodes;
        for (u32 k = 0; k < 21; k++) {
            g3d::body_node_world(body, k, mtx);
            char b[48];
            std::snprintf(b, sizeof(b), " %u:(%.1f %.1f %.1f)", (unsigned)k, load_f32(mtx + 0xC) - at.x,
                          load_f32(mtx + 0x1C) - at.y, load_f32(mtx + 0x2C) - at.z);
            nodes += b;
        }
        guest::log("wuhu: feet trace: %s frame %.1f:%s", CLIP_NAMES[clip_], frame_, nodes.c_str());
    }
    const int low = p[0].y <= p[1].y ? 0 : 1;
    const bool down = p[low].y - at.y < (ANKLE_REST + ANKLE_DOWN) * height_scale_;
    if (feet_had_ && down && walked > 0) {
        const f32 dx = p[low].x - ankle_was_[low].x, dz = p[low].z - ankle_was_[low].z;
        feet_slid_ += std::sqrt(dx * dx + dz * dz);
        feet_walked_ += walked;
        feet_samples_++;
        // (in the body: the ankle's move against the feet's, over the clip's frames gone)
        const f32 bx = dx - (at.x - at_was_.x), bz = dz - (at.z - at_was_.z);
        const f32 gone = wrap(frame_ - frame_was_, (f32)source_[clip_]->frames);
        if (gone > 0 && gone < 0.5f * source_[clip_]->frames)
            feet_body_ += std::sqrt(bx * bx + bz * bz), feet_frames_ += gone;
    }
    feet_had_ = down;
    ankle_was_[0] = p[0], ankle_was_[1] = p[1];
    at_was_ = at, frame_was_ = frame_;
    feet_top_ = std::max(feet_top_, walked);
    if (++feet_updates_ >= 60) {
        guest::log("wuhu: feet: %s, up to %.2f units an update: the planted foot slid %.1f while the body went "
                   "%.1f (%.0f%%; %u updates with a foot down); in the body it went %.2f a clip frame (%.2f "
                   "expected)",
                   CLIP_NAMES[clip_], feet_top_, feet_slid_, feet_walked_,
                   feet_walked_ > 0 ? 100.0f * feet_slid_ / feet_walked_ : 0.0f, (unsigned)feet_samples_,
                   feet_frames_ > 0 ? feet_body_ / feet_frames_ : 0.0f, source_[clip_]->foot * height_scale_);
        feet_slid_ = feet_walked_ = feet_top_ = feet_body_ = feet_frames_ = 0;
        feet_samples_ = feet_updates_ = 0;
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
    if (step_ >= STEPS && block_)
        g3d::root_draw_opaque(guest::load32(root_word_));
}

void IslandMii::draw_translucent() {
    if (step_ >= STEPS && block_)
        g3d::root_draw_translucent(guest::load32(root_word_));
}

}  // namespace engine::wuhu
