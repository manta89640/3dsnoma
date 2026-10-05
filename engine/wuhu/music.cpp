// 「広い世界へ」 in the channel's sound system (see music.h).
//
// The sequence is ours (tools/music/hiroi_sekai.py, compiled by tools/music/rseq.py into
// hiroi_sekai_seq.h); its instruments are the channel's own bank, BANK_BGM_ALL. nw4r::snd plays a
// sequence from the caller's own data through SoundStartable::StartInfo: with
// ENABLE_SEQ_SOUND_INFO set, SoundArchivePlayer's PrepareSeqImpl (fn_8047A9A0) reads the sequence
// at seqDataAddress (an RSEQ file, as the archive's own: SeqFileReader checks 'RSEQ'), from its
// start-location label's offset (none: offset 0), and opens the tracks its leading alloctrack
// command asks for (MmlParser::ParseAllocTrack). Everything else comes from the sound ID it is
// started with: the player, the volume, the priorities, and the bank its notes find their
// instruments in (SeqNoteOnCallback::NoteOn reads the bank of the sound the SeqPlayer was set up
// for). So our piece starts as SEQ_BGM_SAIZIKI_D, one of the seasonal slideshows' pieces (on
// PLAYER_BGM, volume 125, BANK_BGM_ALL), with our sequence in place of its own.
//
// The channel asks for its music by sound ID through sound::BGMManager (its fields: soundplayer/
// music.cpp), which starts a piece with sound::SoundManager's start (fn_802A0BD0: manager, sound
// ID, SoundHandle*; true once started). WIDE_WORLD_ID is no sound of the archive: the start,
// patched here, plays ours for it, so the BGM manager's crossfades, pauses and slots, and the
// Sound Player's playback and beat, work for it as for the channel's own pieces.

#include "engine/wuhu/music.h"

#include <cstdio>
#include <cstdlib>

#include "engine/system/host_scene.h"
#include "engine/wuhu/hiroi_sekai_seq.h"
#include "noma_decomp.h"
#include "runtime/hw/hw.h"

namespace engine::wuhu {

namespace {

const u32 NONE = ~0u;  // the BGM manager's "no sound" (-1)

// The sound our piece is started as (see above): looked up by its label, as the channel's own
// PlayByLabel does, the first time the piece starts (the archive is loaded after the start).
const char HOST_LABEL[] = "SEQ_BGM_SAIZIKI_D";

// nw4r::snd::SoundStartable::StartInfo: +0x00 enableFlag, +0x04 startOffsetType, +0x08 startOffset,
// +0x0C playerId, +0x10 playerPriority, +0x14 actorPlayerId, +0x18 seqDataAddress,
// +0x1C startLocationLabel (each read only when its flag is set)
enum : u32 {
    START_INFO_SIZE = 0x20,
    SI_ENABLE = 0x00,
    SI_SEQ_DATA = 0x18,
    SI_SEQ_LABEL = 0x1C,
    ENABLE_SEQ_SOUND_INFO = 0x10,
};
const u32 LABEL_SPACE = 0x20;  // HOST_LABEL, NUL-terminated

// Fades, in frames (60 a second). Leaving the room: as long as the room's fade to black (16
// frames) and a little into the black, so the theme is gone by the time the island loads.
// Leaving the island when the room's theme isn't waiting paused: ours just fades.
const u32 DEPART_FADE_FRAMES = 30;
const u32 LEAVE_FADE_FRAMES = 60;

u32 s_start_info;      // guest StartInfo (lasting)
u32 s_label;           // HOST_LABEL in guest memory
u32 s_sequence;        // the RSEQ file in guest memory (32-byte aligned, as the archive's files are)
u32 s_host = NONE;     // HOST_LABEL's sound ID, once looked up
u32 s_room = NONE;     // what played when the player left: the piece the BGM manager had current,
s32 s_room_slot = -1;  // and its slot (2: the room's theme, kept paused there)

// ---- the BGM manager (sound_BGMManager_sInstance; soundplayer/music.cpp has its fields)

u32 bgm() { return guest::load32(GA_sound_BGMManager_sInstance); }
s32 slot() { return (s32)guest::load32(bgm() + 0x24); }
u32 current() { return guest::load32(bgm() + 0x28); }
u32 next() { return guest::load32(bgm() + 0x2C); }
// the piece current, or the one waiting for its slot to be free
u32 asked() { return next() != NONE ? next() : current(); }
u32 sound_in(s32 s) { return s >= 0 && s <= 2 ? guest::load32(bgm() + 4 * (u32)s) : 0; }

void play_by_id(u32 id, bool to_slot_2) {
    g::sound_BGMManager_PlayById(bgm(), id, to_slot_2 ? 1 : 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
void stop(u32 frames) { g::sound_BGMManager_Stop(bgm(), frames, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
void stop_now() { g::sound_BGMManager_StopNow(bgm(), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }

// ---- the start

void make_lasting() {
    u32 size = START_INFO_SIZE + LABEL_SPACE + hiroi_sekai::SIZE;
    u32 block = engine::system::lasting_alloc(size, 32);
    s_start_info = block;
    s_label = block + START_INFO_SIZE;
    s_sequence = s_label + LABEL_SPACE;
    for (u32 i = 0; i < START_INFO_SIZE + LABEL_SPACE; i += 4)
        guest::store32(block + i, 0);
    guest::write_g(s_label, HOST_LABEL, sizeof(HOST_LABEL));
    guest::write_g(s_sequence, hiroi_sekai::RSEQ, hiroi_sekai::SIZE);
    guest::store32(s_start_info + SI_ENABLE, ENABLE_SEQ_SOUND_INFO);
    guest::store32(s_start_info + SI_SEQ_DATA, s_sequence);
    guest::store32(s_start_info + SI_SEQ_LABEL, 0);  // no label: the sequence starts at offset 0
}

// HOST_LABEL's sound ID (NONE if the archive has no such sound). The sound manager's +0 is its
// MemoryArchive, whose +4 is the nw4r::snd::SoundArchive (soundplayer/catalog.cpp);
// fn_804777B0 is SoundArchive::ConvertLabelStringToSoundId.
u32 host_sound(u32 manager) {
    if (s_host == NONE) {
        u32 archive = guest::load32(manager) + 4;
        s_host = g::fn_804777B0(archive, s_label);
        if (s_host == NONE)
            guest::log("wuhu: music: no sound %s in the archive: the island's music can't play", HOST_LABEL);
        else
            guest::log("wuhu: music: plays as %s (sound %u)", HOST_LABEL, (unsigned)s_host);
    }
    return s_host;
}

// Our piece into a slot's SoundHandle: SoundStartable::StartSound (fn_8047BF30: handle, sound ID,
// StartInfo; 0 once started, else nw4r::snd's StartResult: 1 a sound of higher priority holds
// the player, 9 no free tracks...). The SoundStartable is the SoundArchivePlayer's (the sound
// manager's +4) +0xC.
bool start(u32 manager, u32 handle) {
    if (!s_start_info) {
        guest::log("wuhu: music: asked for before the start made its data");
        return false;
    }
    u32 host = host_sound(manager);
    if (host == NONE)
        return false;
    u32 startable = guest::load32(manager + 4) + 0xC;
    u32 result = g::fn_8047BF30(startable, handle, host, s_start_info, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (result)
        guest::log("wuhu: music: Hiroi Sekai e didn't start (nw4r::snd StartResult %u)", (unsigned)result);
    return result == 0;
}

// ---- NOMA_WUHU_MUSIC_TEST

// A slot's sequence position: the ticks its SeqSound's player has played (the SeqSound at the
// slot's SeqSoundHandle, +0x110 its SeqPlayer, whose +0x160 counts them), -1 if no sequence.
s64 ticks_in(s32 s) {
    u32 handle = guest::load32(bgm() + 0xC + 8 * (u32)s);
    u32 seq = handle ? guest::load32(handle) : 0;
    if (!seq || guest::load32(seq) != GA_nw4r_snd_SeqSound_vtable)
        return -1;
    return guest::load32(seq + 0x110 + 0x160);
}

void log_state(const char* when) {
    // a BasicSound's +0x88: its pause state
    static const char* const PAUSE[] = {"playing", "pausing", "paused", "resuming"};
    char slots[160];
    int at = 0;
    for (s32 s = 0; s < 3; s++) {
        u32 sound = sound_in(s);
        at += std::snprintf(slots + at, sizeof(slots) - (size_t)at, " [%d: %s, ticks %lld]", (int)s,
                            sound ? PAUSE[guest::load32(sound + 0x88) & 3] : "-", (long long)ticks_in(s));
    }
    guest::log("wuhu: music test: %s at %.2f s: current %d, next %d, slot %d;%s", when,
               guest::vi_game_frames() / 60.0, (int)(s32)current(), (int)(s32)next(), (int)slot(), slots);
}

// NOMA_WUHU_MUSIC_TEST=N,M[,L]: the island's music flow without the island, wherever the game is
// at game time N s (the room, for a test): music_depart, L s later (the island loading; 1 if not
// given) music_arrive, M seconds after that music_leave. The BGM manager's state is logged at each
// step and every second for 10 s after leaving (the room theme's ticks show it paused and resumed
// where it was); with NOMA_DUMP_AUDIO the recording is what a player would have heard.
void test_frame() {
    static int step = -1;
    static double depart_at, play_for, loading = 1, last_log;
    if (step < 0) {
        const char* e = std::getenv("NOMA_WUHU_MUSIC_TEST");
        step = e && std::sscanf(e, "%lf,%lf,%lf", &depart_at, &play_for, &loading) >= 2 ? 0 : 99;
    }
    if (step >= 99)
        return;
    double t = guest::vi_game_frames() / 60.0;
    switch (step) {
    case 0:
        if (t < depart_at)
            return;
        log_state("before departing");
        music_depart();
        log_state("departed");
        step = 1;
        return;
    case 1:
        if (t < depart_at + loading)
            return;
        music_arrive();
        log_state("arrived");
        step = 2, last_log = t;
        return;
    case 2:
        if (t < depart_at + loading + play_for) {
            if (t - last_log >= 10)
                log_state("on the island"), last_log = t;
            return;
        }
        log_state("before leaving");
        music_leave();
        log_state("left");
        step = 3, last_log = t;
        return;
    default:
        if (t - last_log >= 1) {
            log_state("back");
            last_log = t;
            if (t > depart_at + loading + play_for + 10)
                step = 99;
        }
        return;
    }
}

}  // namespace

void music_boot() { engine::system::host_scene_boot(&make_lasting); }

u32 music_bar_beats(u32 sound_id) { return sound_id == WIDE_WORLD_ID ? hiroi_sekai::BAR_BEATS : 4; }

void music_depart() {
    s_room = asked();
    s_room_slot = slot();
    // sound_BGMManager_Stop: the pieces in slots 0 and 1 fade out and stop, the room's theme (slot
    // 2) fades out and pauses; nothing is current after it
    stop(DEPART_FADE_FRAMES);
}

void music_arrive() {
    if (current() == WIDE_WORLD_ID && next() == NONE) {
        u32 sound = sound_in(slot());
        if (sound && guest::load32(sound + 0x88) == 0)
            return;  // playing already
    }
    if (asked() != NONE) {
        // Arriving without having departed, or with ours current but paused or ended: PlayById
        // would queue ours behind a 3 s fade, or do nothing. StopNow cuts slots 0 and 1 at once
        // and pauses slot 2 (what played is remembered, as departing does).
        if (s_room_slot < 0 && asked() != WIDE_WORLD_ID)
            s_room = asked(), s_room_slot = slot();
        stop_now();
    }
    // nothing current: PlayById starts ours at once in slot 0 or 1 (no fade in)
    play_by_id(WIDE_WORLD_ID, false);
    // and pauses slot 2 again over its crossfade (180 frames). A theme still fading out (the island
    // came up within half a second of departing, or StopNow has only just paused it) has its fade
    // made that long again, scaled by the volume it has left, and would sound under ours for up to
    // 3 s: it goes on fading at the departure's pace instead (BasicSound::Pause, fn_8046C9A0: the
    // frames asked for, times the volume left; nothing if it has paused already).
    if (u32 theme = sound_in(2))
        if (guest::load32(theme + 0x88) == 1)  // pausing
            g::fn_8046C9A0(theme, 1, DEPART_FADE_FRAMES);
}

void music_leave() {
    u32 room = s_room;
    s32 room_slot = s_room_slot;
    s_room = NONE, s_room_slot = -1;
    u32 playing = asked();
    if (playing != WIDE_WORLD_ID && playing != NONE)
        return;  // something else has taken over: leave it be
    // (Nothing at all, if ours couldn't start or the island never came up: the room's theme is
    // still waiting in slot 2, and is brought back the same way.)
    if (room_slot == 2 && room != NONE) {
        // The room's theme waits paused in slot 2. The room won't ask for it: its BGM controller
        // (lbl_80882D14) asks again, as the room is made, only if its own mode has changed, and
        // the island went past it, so the theme would stay paused and the room silent. Asking for
        // it in slot 2 here makes the BGM manager fade ours out (Stop over its crossfade, +0x34:
        // 180 frames) and resume the theme with the same fade from where it paused; the room then
        // finds it current.
        play_by_id(room, true);
        return;
    }
    // Something else played before the island (the controller's mode isn't the theme's, so the
    // room asks for its theme when it is made again), or nothing: ours, if it plays, fades out of
    // the way.
    if (playing == WIDE_WORLD_ID)
        stop(LEAVE_FADE_FRAMES);
}

}  // namespace engine::wuhu

namespace g {

u32 fn_802A0BD0_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
                     f64 f4, f64 f5, f64 f6, f64 f7, f64 f8);
void fn_802A23A0_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
                      f64 f4, f64 f5, f64 f6, f64 f7, f64 f8);

// sound::SoundManager's start (to be named sound_SoundManager_StartSound): r3 the manager, r4 the
// sound ID, r5 the SoundHandle to start it in; true once started. WIDE_WORLD_ID is our piece.
u32 fn_802A0BD0(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3, f64 f4,
                f64 f5, f64 f6, f64 f7, f64 f8) {
    if (r4 != engine::wuhu::WIDE_WORLD_ID)
        return fn_802A0BD0_orig(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7, f8);
    return engine::wuhu::start(r3, r5) ? 1 : 0;
}

// sound::BGMManager's step, once a frame from the application's update (to be named
// sound_BGMManager_Update: it starts the piece waiting for its slot); then the music test's.
void fn_802A23A0(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3, f64 f4,
                 f64 f5, f64 f6, f64 f7, f64 f8) {
    fn_802A23A0_orig(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7, f8);
    engine::wuhu::test_frame();
}

}  // namespace g
