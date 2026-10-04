// The Sound Player's playback (see music.h).
//
// The channel's sound::BGMManager (lbl_80882CB0, kept by the application across scenes) plays all
// its music: two slots that crossfade (0 and 1) and a third (2) where the room keeps its theme,
// paused when something else plays and resumed where it stopped. Its fields:
//   +0x00/+0x04/+0x08  each slot's nw4r::snd BasicSound* (0 once it has ended)
//   +0x0C + 8*slot     the slot's SeqSoundHandle* (its +0 the SeqSound*)
//   +0x24 current slot   +0x28 current sound (-1 none)   +0x2C next sound (-1 none; waits for the
//   slot's last piece to fade out)   +0x34 crossfade length in frames (180)
//   +0x3A "held paused" (resuming does nothing while set)
// Its calls: PlayById fn_802A2564(bgm, id, to_slot_2) (asking for the current or next piece does
// nothing), Stop fn_802A2728(bgm, fade frames), StopNow fn_802A27C8(bgm), Pause fn_802A284C(bgm, on)
// (an 18-frame fade). A BasicSound's +0x88 is its pause state (0 playing, 1 pausing, 2 paused,
// 3 resuming). A SeqSound (vtable 0x8085B3B8) has +0x85 u8 started and its SeqPlayer at +0x110,
// whose fields (engine/snd/seq.cpp) give the position: +0xD4 f32 tempo ratio, +0xD8 f32 the part of
// a tick still to come, +0xEA u8 timebase (ticks a quarter note), +0xEC u16 tempo, +0x160 u32 ticks.
#include "engine/soundplayer/music.h"

#include "noma_decomp.h"

namespace engine::soundplayer {

namespace {

const u32 SEQ_SOUND_VTABLE = GA_lbl_8085B3B8;
const s32 FADE_FRAMES = 30;

u32 bgm() { return guest::load32(GA_lbl_80882CB0); }
s32 slot() { return (s32)guest::load32(bgm() + 0x24); }
u32 sound_in(s32 s) { return s >= 0 && s <= 2 ? guest::load32(bgm() + 4 * s) : 0; }
u32 sequence_in(s32 s) {
    if (s < 0 || s > 2)
        return 0;
    u32 handle = guest::load32(bgm() + 0xC + 8 * s);
    u32 seq = handle ? guest::load32(handle) : 0;
    return seq && guest::load32(seq) == SEQ_SOUND_VTABLE && guest::load8(seq + 0x85) ? seq : 0;
}

void play_by_id(u32 id) { g::fn_802A2564(bgm(), id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
void stop_with_fade(u32 frames) { g::fn_802A2728(bgm(), frames, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
void stop_now() { g::fn_802A27C8(bgm(), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }

f32 f32_at(u32 a) {
    u32 bits = guest::load32(a);
    f32 f;
    __builtin_memcpy(&f, &bits, 4);
    return f;
}

}  // namespace

Music::Music() {
    saved_fade_frames_ = (s32)guest::load32(bgm() + 0x34);
    guest::store32(bgm() + 0x34, (u32)FADE_FRAMES);
}

Music::~Music() {
    if (state() == PlayState::PAUSED)
        resume();
    guest::store32(bgm() + 0x34, (u32)saved_fade_frames_);
}

void Music::play(u32 id) {
    if (id == current()) {
        PlayState s = state();
        if (s == PlayState::PAUSED) {
            resume();
            return;
        }
        if (s == PlayState::PLAYING || s == PlayState::SWITCHING)
            return;
        stop_now();  // it ended (a piece that doesn't loop): asking for it again would do nothing
    }
    play_by_id(id);
}

void Music::pause() { g::fn_802A284C(bgm(), 1); }

// (Stop leaves the room's theme paused in slot 2 with the current slot still 2: resuming then
// would play it again behind the player's back, with nothing current. Only a piece current resumes.)
void Music::resume() {
    if (guest::load32(bgm() + 0x28) != NO_SOUND)
        g::fn_802A284C(bgm(), 0);
}

void Music::stop() { stop_with_fade(FADE_FRAMES); }

u32 Music::current() const {
    u32 next = guest::load32(bgm() + 0x2C);
    return next != NO_SOUND ? next : guest::load32(bgm() + 0x28);
}

PlayState Music::state() const {
    if (guest::load32(bgm() + 0x2C) != NO_SOUND)
        return PlayState::SWITCHING;
    if (guest::load32(bgm() + 0x28) == NO_SOUND)
        return PlayState::STOPPED;
    u32 sound = sound_in(slot());
    if (!sound)
        return PlayState::STOPPED;
    u32 pause = guest::load32(sound + 0x88);
    return pause == 1 || pause == 2 ? PlayState::PAUSED : PlayState::PLAYING;
}

Beat Music::beat() const {
    Beat b;
    u32 seq = sequence_in(slot());
    if (!seq || state() != PlayState::PLAYING)
        return b;
    u32 player = seq + 0x110;
    u32 timebase = guest::load8(player + 0xEA);
    if (!timebase)
        return b;
    b.valid = true;
    b.bpm = (f32)guest::load16(player + 0xEC) * f32_at(player + 0xD4);
    f32 to_come = f32_at(player + 0xD8);
    if (!(to_come >= 0.0f && to_come <= 1.0f))
        to_come = 1.0f;
    b.beats = ((double)guest::load32(player + 0x160) + (1.0 - to_come)) / timebase;
    return b;
}

}  // namespace engine::soundplayer
