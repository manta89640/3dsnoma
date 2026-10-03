// nw4r::snd's sequencer (snd_SeqPlayer.cpp, snd_SeqTrack.cpp, snd_MmlSeqTrack.cpp,
// snd_MmlParser.cpp), native: every 3 ms AX frame each playing sequence advances by its tempo,
// its tracks read their MML byte code (notes, waits, jumps, calls, loops, variables and the
// parameter commands) and push their parameters to their channels (engine/snd/channel.cpp).
// Ported from the PowerPC code; the rarely run parts (opening and closing tracks, NoteOn, muting,
// the user procedure callback) stay the decompiled functions, called through g::.
#include "noma_decomp.h"
#include "voice.h"

namespace engine::snd {

namespace {

// SeqTrack
enum : u32 {
    T_INDEX = 0x04,         // u8: the track's number in its player
    T_OPENED = 0x05,        // u8
    T_EXT_VOLUME = 0x08,    // f32
    T_EXT_PITCH = 0x0C,
    T_EXT_PAN = 0x10,
    T_EXT_SPAN = 0x14,
    T_PAN_RANGE = 0x18,
    T_BASE = 0x1C,          // u32: the sequence data
    T_CUR = 0x20,           // u32: the MML read position (0: none)
    T_CMP_FLAG = 0x24,      // u8: the last comparison's result
    T_NOTE_WAIT = 0x25,     // u8: a note waits its length
    T_TIE = 0x26,           // u8
    T_STACK = 0x28,         // 3 x {u8 loop, u8 loop count, u16, u32 address}: calls and loops
    T_STACK_DEPTH = 0x40,   // u8
    T_WAIT = 0x44,          // s32: ticks to wait
    T_MUTE = 0x48,          // u8
    T_WAIT_NOTE_END = 0x4A, // u8: waiting for the channels to end
    T_PORTA_FLAG = 0x4B,    // u8
    T_DAMPER = 0x4C,        // u8
    T_PROGRAM = 0x54,       // s32
    T_LFO = 0x58,           // LfoParam: f32 depth, f32 speed, u32 delay, u8 range (+3 bytes), copied to channels
    T_LFO_TARGET = 0x68,    // u8
    T_SWEEP_PITCH = 0x6C,   // f32
    T_VOLUME = 0x70,        // MoveParam: u8 current, u8 target, s16 frames, s16 frame
    T_PAN = 0x76,           // MoveParam, s8 values
    T_SPAN = 0x7C,          // MoveParam, s8 values
    T_BEND = 0x82,          // MoveParam, s8 values
    T_VOLUME2 = 0x88,       // u8
    T_BEND_RANGE = 0x8A,    // u8
    T_INIT_PAN = 0x8B,      // s8
    T_TRANSPOSE = 0x8C,     // s8
    T_PRIORITY = 0x8D,
    T_PORTA_KEY = 0x8E,
    T_PORTA_TIME = 0x8F,
    T_ATTACK = 0x90,        // u8 x4: attack, decay, sustain, release (0xFF: the instrument's)
    T_ENV_HOLD = 0x94,      // s16
    T_MAIN_SEND = 0x96,     // u8
    T_FX_SEND = 0x97,       // u8 x3
    T_BIQUAD_TYPE = 0x9A,
    T_LPF_FREQ = 0x9C,      // f32
    T_BIQUAD_VALUE = 0xA0,  // f32
    T_VARIABLES = 0xA4,     // s16 x16
    T_PLAYER = 0xC4,
    T_CHANNELS = 0xC8,      // Channel list (next at 0xD4)
    T_PARSER = 0xCC,        // MmlSeqTrack: its MmlParser
};

// SeqPlayer (a BasicPlayer)
enum : u32 {
    P_VOLUME = 0x04,
    P_PITCH = 0x08,
    P_PAN = 0x0C,
    P_SPAN = 0x10,
    P_LPF_FREQ = 0x14,
    P_BIQUAD_VALUE = 0x18,
    P_BIQUAD_TYPE = 0x1C,    // u8
    P_REMOTE_FILTER = 0x1D,  // u8
    P_OUTPUT_LINE = 0x20,
    P_MAIN_OUT_VOLUME = 0x24,
    P_MAIN_SEND = 0x28,
    P_FX_SEND = 0x34,        // f32 x3
    P_REMOTE_VOLUME = 0x40,  // f32 x4
    P_VOICE_OUT = 0x50,      // VoiceOutParam x4, 0x18 each
    P_PAN_RANGE = 0xD0,
    P_VOICE_OUTS = 0xE4,     // s32
    P_DISPOSE_LINK = 0xB4,
    P_PLAYER_LINK = 0xC0,
    P_ACTIVE = 0xCC,         // u8
    P_STARTED = 0xCD,        // u8
    P_PAUSED = 0xCE,         // u8
    P_TEMPO_RATIO = 0xD4,
    P_TICK_COUNTER = 0xD8,   // f32: ms into the current tick
    P_SKIP_TICKS = 0xDC,     // s32
    P_SKIP_TIME = 0xE0,      // f32: ms
    P_MAIN_VOLUME = 0xE8,    // u8
    P_TIMEBASE = 0xEA,       // u8
    P_TEMPO = 0xEC,          // u16
    P_ALLOCATOR = 0xF4,      // SeqTrackAllocator*
    P_USERPROC = 0xF8,
    P_TRACKS = 0x100,        // SeqTrack* x16
    P_VARIABLES = 0x140,     // s16 x16
    P_TICK_COUNT = 0x160,    // u32
};

const u32 GLOBAL_VARIABLES = GA_lbl_80963A08;  // SeqPlayer's 16 globals (s16)
const u32 MML_COMMAND_PROC = 0x80470DA0;       // MmlParser::CommandProc
const u32 MML_NOTE_ON_PROC = 0x80471720;       // MmlParser::NoteOnCommandProc
const u32 MML_SEQ_TRACK_PARSE = 0x80471930;    // MmlSeqTrack::Parse

// the sound thread's mutex: the game's threads change players and tracks under it
u32 sound_mutex() {
    static u32 s_thread;
    if (!s_thread)
        s_thread = g::nw4r_snd_detail_SoundThread_GetInstance();
    return s_thread + 0x354;
}
struct SoundLock {
    SoundLock() { g::OSLockMutex(sound_mutex()); }
    ~SoundLock() { g::OSUnlockMutex(sound_mutex()); }
};

// ---- variables
u32 player_variable(u32 player, s32 i) {  // SeqPlayer::GetVariablePtr
    if (i < 0x10)
        return player + P_VARIABLES + (u32)i * 2;
    if (i < 0x20)
        return GLOBAL_VARIABLES + (u32)(i - 0x10) * 2;
    return 0;
}
u32 track_variable(u32 track, s32 i) {  // SeqTrack::GetVariablePtr
    return i < 0x10 ? track + T_VARIABLES + (u32)i * 2 : 0;
}
// a variable by its MML number: 0x00..0x1F the player's (and globals), 0x20..0x2F the track's
u32 variable(u32 player, u32 track, s32 i) {
    if (i < 0x20)
        return player_variable(player, i);
    if (i < 0x30)
        return track_variable(track, i - 0x20);
    return 0;
}

// a MoveParam's value now: from `current` to `target` over `frames`
s32 move_value_u8(GuestObj t, u32 at) {
    s32 frames = t.gets16(at + 2), frame = t.gets16(at + 4);
    if (frame >= frames)
        return t.get8(at + 1);
    u32 cur = t.get8(at);
    return (u8)(cur + NOMA_DIVW((s32)(frame * (s32)(t.get8(at + 1) - cur)), frames));
}
s32 move_value_s8(GuestObj t, u32 at) {
    s32 frames = t.gets16(at + 2), frame = t.gets16(at + 4);
    if (frame >= frames)
        return (s8)t.get8(at + 1);
    u32 cur = t.get8(at);
    return (s8)(cur + NOMA_DIVW((s32)(frame * ((s8)t.get8(at + 1) - (s8)cur)), frames));
}

// ---- MmlParser

// the read position, through the track's T_CUR
struct Reader {
    GuestObj t;
    u8 byte() const {
        u32 p = t.get32(T_CUR);
        t.set32(T_CUR, p + 1);
        return guest::load8(p);
    }
    u32 u16be() const {
        u32 hi = byte();
        return hi << 8 | byte();
    }
    u32 u24be() const {
        u32 hi = u16be();
        return hi << 8 | byte();
    }
};

enum : s32 { ARG_U8 = 1, ARG_S16 = 2, ARG_VMIDI = 3, ARG_RANDOM = 4, ARG_VARIABLE = 5 };

// MmlParser::ReadArg: an argument of a type (random and variable ones by a prefix)
u32 read_arg(u32 parser, GuestObj t, u32 player, s32 type) {
    Reader r{t};
    switch (type) {
    case ARG_U8:
        return r.byte();
    case ARG_S16:
        return r.u16be();
    case ARG_VMIDI: {
        u32 v = 0;
        u8 b;
        do {
            b = r.byte();
            v = v << 7 | (b & 0x7F);
        } while (b & 0x80);
        return v;
    }
    case ARG_VARIABLE: {
        u32 p = variable(player, t.addr, r.byte());
        return p ? (u32)(s32)(s16)guest::load16(p) : 0;
    }
    case ARG_RANDOM: {
        s32 lo = (s16)r.u16be(), hi = (s16)r.u16be();
        u32 rand = g::nw4r_snd_detail_Util_CalcRandom() & 0xFFFF;
        return (u32)((s32)(rand * (u32)(hi - lo + 1)) >> 16) + (u32)lo;
    }
    default:
        return parser;  // (what was left in r3)
    }
}

u32 command_proc(u32 parser, u32 track, u32 cmd, u32 a1, u32 a2);

// the parser's virtual CommandProc and NoteOnCommandProc (MmlParser's own, natively)
void vcall_command(u32 parser, u32 track, u32 cmd, u32 a1, u32 a2) {
    u32 fn = guest::load32(guest::load32(parser) + 0x8);
    if (fn == MML_COMMAND_PROC)
        command_proc(parser, track, cmd, a1, a2);
    else
        gcall<u32>(fn, parser, track, cmd, a1, a2);
}
void vcall_note_on(u32 parser, u32 track, u32 key, u32 velocity, u32 length, u32 tie) {
    u32 fn = guest::load32(guest::load32(parser) + 0xC);
    if (fn == MML_NOTE_ON_PROC)
        g::nw4r_snd_detail_SeqTrack_NoteOn(track, key, velocity, length, tie);
    else
        gcall<u32>(fn, parser, track, key, velocity, length, tie);
}

// MmlParser::Parse: one command (or note) at the read position; 1 at the end of the track
u32 parse(u32 parser, u32 track, u32 do_note_on) {
    GuestObj t(track);
    Reader r{t};
    const u32 player = t.get32(T_PLAYER);
    u32 cmd = r.byte();
    bool execute = true;
    s32 type2 = 0;           // the second argument's type (prefixes A3 A4 A5)
    s32 override_type = 0;   // the argument's type (prefixes A0 A1)
    if (cmd == 0xA2) {       // "if": only when the last comparison held
        execute = t.get8(T_CMP_FLAG) != 0;
        cmd = r.byte();
    }
    if (cmd == 0xA3)
        type2 = ARG_S16, cmd = r.byte();
    else if (cmd == 0xA4)
        type2 = ARG_RANDOM, cmd = r.byte();
    else if (cmd == 0xA5)
        type2 = ARG_VARIABLE, cmd = r.byte();
    if (cmd == 0xA0)
        override_type = ARG_RANDOM, cmd = r.byte();
    else if (cmd == 0xA1)
        override_type = ARG_VARIABLE, cmd = r.byte();
    auto type = [&](s32 normal) { return override_type ? override_type : normal; };

    if (!(cmd & 0x80)) {  // a note: key, velocity, length
        u32 velocity = r.byte();
        s32 length = (s32)read_arg(parser, t, player, type(ARG_VMIDI));
        s32 key = (s32)cmd + (s8)t.get8(T_TRANSPOSE);
        if (!execute)
            return 0;
        key = key > 0x7F ? 0x7F : key < 0 ? 0 : key;
        if (!t.get8(T_MUTE) && do_note_on)
            vcall_note_on(parser, track, (u32)key, velocity, length > 0 ? (u32)length : 0xFFFFFFFFu, t.get8(T_TIE));
        if (t.get8(T_NOTE_WAIT)) {
            t.set32(T_WAIT, (u32)length);
            if (length == 0)
                t.set8(T_WAIT_NOTE_END, 1);
        }
        return 0;
    }

    u32 a1 = 0, a2 = 0;
    switch (cmd & 0xF0) {
    case 0x80:
        switch (cmd) {
        case 0x80: {  // wait
            u32 w = read_arg(parser, t, player, type(ARG_VMIDI));
            if (execute)
                t.set32(T_WAIT, w);
            break;
        }
        case 0x81: {  // program
            u32 p = read_arg(parser, t, player, type(ARG_VMIDI));
            if (execute)
                vcall_command(parser, track, cmd, p, 0);
            break;
        }
        case 0x88: {  // open track: number, 24-bit offset
            u32 no = r.byte(), offset = r.u24be();
            if (execute)
                vcall_command(parser, track, cmd, no, offset);
            break;
        }
        case 0x89:  // jump
        case 0x8A: {  // call
            u32 offset = r.u24be();
            if (execute)
                vcall_command(parser, track, cmd, offset, 0);
            break;
        }
        }
        return 0;
    case 0xB0:
    case 0xC0:
    case 0xD0: {  // u8 argument (s8 for C3 C4), and one more with a prefix
        u32 v = read_arg(parser, t, player, type(ARG_U8));
        if (type2)
            a2 = read_arg(parser, t, player, type2);
        if (execute)
            vcall_command(parser, track, cmd, cmd - 0xC3 <= 1 ? (u32)(s32)(s8)(u8)v : (u32)(u8)v, a2);
        return 0;
    }
    case 0x90:
        if (execute)
            vcall_command(parser, track, cmd, 0, 0);
        return 0;
    case 0xE0: {  // s16 argument
        u32 v = (u32)(s32)(s16)read_arg(parser, t, player, type(ARG_S16));
        if (execute)
            vcall_command(parser, track, cmd, v, 0);
        return 0;
    }
    case 0xF0:
        if (cmd == 0xFE) {  // allocate tracks (a header: skipped)
            t.set32(T_CUR, t.get32(T_CUR) + 2);
            return 0;
        }
        if (cmd == 0xFF)  // end
            return execute ? 1 : 0;
        if (cmd == 0xF0) {  // extended: a second command byte
            u32 ext = r.byte();
            switch (ext & 0xF0) {
            case 0xE0:
                a1 = read_arg(parser, t, player, type(ARG_S16)) & 0xFFFF;
                if (execute)
                    vcall_command(parser, track, (cmd << 8) + ext, a1, 0);
                break;
            case 0x80:
            case 0x90:
                a1 = r.byte();
                a2 = (u32)(s32)(s16)read_arg(parser, t, player, type(ARG_S16));
                if (execute)
                    vcall_command(parser, track, (cmd << 8) + ext, a1, a2);
                break;
            }
        }
        // (and, as the original does, CommandProc with the command itself: F0 and the others)
        if (execute)
            vcall_command(parser, track, cmd, a1, a2);
        return 0;
    }
    return 0;
}

// MmlParser::CommandProc: a command's effect
u32 command_proc(u32 parser, u32 track, u32 cmd, u32 a1, u32 a2) {
    GuestObj t(track);
    const u32 player = t.get32(T_PLAYER);
    GuestObj p(player);
    const s32 v = (s32)a1;
    if (cmd > 0xFF) {
        if (cmd > 0xFFFF)
            return 0;
        u32 var = 0;
        const u32 group = cmd & 0xF0, op = cmd & 0xFF;
        if (group == 0x80 || group == 0x90) {
            var = variable(player, track, v);
            if (!var)
                return 0;
        }
        const s32 x = (s32)a2;
        auto get = [&] { return (s32)(s16)guest::load16(var); };
        auto set = [&](s32 n) { guest::store16(var, (u16)n); };
        auto flag = [&](u32 n) { t.set8(T_CMP_FLAG, (u8)n); };
        switch (op) {
        case 0x80: set(x); break;
        case 0x81: set(get() + (s16)x); break;
        case 0x82: set(get() - (s16)x); break;
        case 0x83: set(get() * (s16)x); break;
        case 0x84:
            if (x != 0)
                set(NOMA_DIVW(get(), x));
            break;
        case 0x85:
            if (x >= 0)
                set((s32)NOMA_SLW((u32)get(), (u32)x));
            else
                set(NOMA_SRAW((u32)get(), (u32)-x));
            break;
        case 0x86: {  // random 0..x (or -x..0)
            bool negative = x < 0;
            s32 range = negative ? (s16)-x : x;
            s32 n = (s32)((g::nw4r_snd_detail_Util_CalcRandom() & 0xFFFF) * (u32)(range + 1)) >> 16;
            set(negative ? -n : n);
            break;
        }
        case 0x87: set(get() & (s16)x); break;
        case 0x88: set(get() | (s16)x); break;
        case 0x89: set(get() ^ (s16)x); break;
        case 0x8A: set(~(s32)(x & 0xFFFF)); break;
        case 0x8B:
            if (x != 0)
                set(get() - NOMA_DIVW(get(), x) * x);
            break;
        // comparisons (signed), computed with the original's bit tricks
        case 0x90: flag((u32)(x - get()) == 0); break;  // ==
        case 0x91: {                                     // >=: subfc/adde
            u32 a = (u32)get(), b = (u32)x;
            flag((u32)((s32)a >> 31) + (b >> 31) + (a >= b));
            break;
        }
        case 0x92: {  // >
            u32 a = (u32)get(), d = a ^ (u32)x;
            flag(((u32)((s32)d >> 1) - (d & a)) >> 31);
            break;
        }
        case 0x93: {  // <=
            u32 a = (u32)x, b = (u32)get();
            flag((u32)((s32)a >> 31) + (b >> 31) + (a >= b));
            break;
        }
        case 0x94: {  // <
            u32 a = (u32)x, d = a ^ (u32)get();
            flag(((u32)((s32)d >> 1) - (d & a)) >> 31);
            break;
        }
        case 0x95: flag(((u32)(x - get()) | (u32)(get() - x)) >> 31); break;  // !=
        case 0xE0:  // user procedure
            g::nw4r_snd_detail_SeqPlayer_CallSeqUserprocCallback(player, a1 & 0xFFFF, track);
            break;
        }
        return 0;
    }

    // a MoveParam's new target: from its value now, over a2 frames
    auto move_to = [&](u32 at, s32 now, u32 target) {
        t.set8(at, (u8)now);
        t.set8(at + 1, (u8)target);
        t.set16(at + 2, (u16)a2);
        t.set16(at + 4, 0);
    };
    switch (cmd) {
    case 0x81:  // program
        if (v < 0x10000)
            t.set32(T_PROGRAM, a1 & 0xFFFF);
        break;
    case 0x88: {  // open track
        u32 other = v > 0xF ? 0 : guest::load32(player + P_TRACKS + a1 * 4);  // GetPlayerTrack
        if (other && other != track) {
            g::nw4r_snd_detail_SeqTrack_Close(other);
            GuestObj o(other);
            o.set32(T_BASE, t.get32(T_BASE));
            o.set32(T_CUR, t.get32(T_BASE) + a2);
            o.set8(T_WAIT_NOTE_END, 0);
            o.set8(T_STACK_DEPTH, 0);
            o.set32(T_WAIT, 0);
            o.set8(T_OPENED, 1);
        }
        break;
    }
    case 0x89:  // jump
        t.set32(T_CUR, t.get32(T_BASE) + a1);
        break;
    case 0x8A: {  // call
        u32 depth = t.get8(T_STACK_DEPTH);
        if (depth >= 3)
            break;
        u32 e = T_STACK + depth * 8;
        t.set32(e + 4, t.get32(T_CUR));
        t.set8(e, 0);
        t.set8(T_STACK_DEPTH, (u8)(depth + 1));
        t.set32(T_CUR, t.get32(T_BASE) + a1);
        break;
    }
    case 0xFD: {  // return: to the innermost call (loops on the way are left)
        u32 e = 0;
        while (u32 depth = t.get8(T_STACK_DEPTH)) {
            depth = (depth - 1) & 0xFF;
            t.set8(T_STACK_DEPTH, (u8)depth);
            if (t.get8(T_STACK + depth * 8) == 0) {
                e = T_STACK + depth * 8;
                break;
            }
        }
        if (e)
            t.set32(T_CUR, t.get32(e + 4));
        break;
    }
    case 0xD4: {  // loop start: a1 times (0: forever)
        u32 depth = t.get8(T_STACK_DEPTH);
        if (depth >= 3)
            break;
        u32 e = T_STACK + depth * 8;
        t.set32(e + 4, t.get32(T_CUR));
        t.set8(e + 1, (u8)a1);
        t.set8(e, 1);
        t.set8(T_STACK_DEPTH, (u8)(depth + 1));
        break;
    }
    case 0xFC: {  // loop end
        u32 depth = t.get8(T_STACK_DEPTH);
        if (depth == 0)
            break;
        u32 e = T_STACK + (depth - 1) * 8;
        if (!t.get8(e))
            break;
        u32 count = t.get8(e + 1);
        if (count != 0) {
            count--;
            if ((count & 0xFF) == 0) {
                t.set8(T_STACK_DEPTH, (u8)(depth - 1));
                break;
            }
        }
        t.set8(e + 1, (u8)count);
        t.set32(T_CUR, t.get32(e + 4));
        break;
    }
    case 0xB0: p.set8(P_TIMEBASE, (u8)a1); break;
    case 0xB1: t.set16(T_ENV_HOLD, (u16)(a1 & 0xFF)); break;
    case 0xB2:  // monophonic
        t.set8(0x27, a1 != 0);
        if (a1 != 0) {
            g::nw4r_snd_detail_SeqTrack_ReleaseAllChannel(track, 0xFFFFFFFFu);
            g::nw4r_snd_detail_SeqTrack_FreeAllChannel(track);
        }
        break;
    case 0xB3: t.set8(0x89, (u8)a1); break;
    case 0xB4: t.set8(T_BIQUAD_TYPE, (u8)a1); break;
    case 0xB5: t.setf(T_BIQUAD_VALUE, (f32)v / 127.0f); break;
    case 0xC0: move_to(T_PAN, move_value_s8(t, T_PAN), a1 - 0x40); break;
    case 0xC1: move_to(T_VOLUME, move_value_u8(t, T_VOLUME), a1); break;
    case 0xC2: p.set8(P_MAIN_VOLUME, (u8)a1); break;
    case 0xC3: t.set8(T_TRANSPOSE, (u8)a1); break;
    case 0xC4: move_to(T_BEND, move_value_s8(t, T_BEND), a1); break;
    case 0xC5: t.set8(T_BEND_RANGE, (u8)a1); break;
    case 0xC6: t.set8(T_PRIORITY, (u8)a1); break;
    case 0xC7: t.set8(T_NOTE_WAIT, a1 != 0); break;
    case 0xC8:  // tie
        t.set8(T_TIE, a1 != 0);
        g::nw4r_snd_detail_SeqTrack_ReleaseAllChannel(track, 0xFFFFFFFFu);
        g::nw4r_snd_detail_SeqTrack_FreeAllChannel(track);
        break;
    case 0xC9:  // portamento from a key
        t.set8(T_PORTA_FLAG, 1);
        t.set8(T_PORTA_KEY, (u8)(a1 + t.get8(T_TRANSPOSE)));
        break;
    case 0xCA: t.setf(T_LFO + 0x0, (f32)(a1 & 0xFF) * 0.0078125f); break;   // depth
    case 0xCB: t.setf(T_LFO + 0x4, 0.390625f * (f32)(a1 & 0xFF)); break;    // speed
    case 0xCC: t.set8(T_LFO_TARGET, (u8)a1); break;
    case 0xCD: t.set8(T_LFO + 0xC, (u8)a1); break;                          // range
    case 0xCE: t.set8(T_PORTA_FLAG, a1 != 0); break;
    case 0xCF: t.set8(T_PORTA_TIME, (u8)a1); break;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:  // attack, decay, sustain, release
        t.set8(T_ATTACK + (cmd - 0xD0), (u8)a1);
        break;
    case 0xD5: t.set8(T_VOLUME2, (u8)a1); break;
    case 0xD6:  // print a variable (debug output, when enabled)
        if (guest::load8(GA_lbl_80883BD0)) {
            u32 var = variable(player, track, v);
            s32 shown = v >= 0x20 ? v - 0x20 : v >= 0x10 ? v - 0x10 : v;
            u32 kind = v >= 0x20 ? GA_lbl_808825B8 : v >= 0x10 ? GA_lbl_808825BC : GA_lbl_808825C0;
            g::OSReport(GA_lbl_8085B0E8, player, t.get8(T_INDEX), kind, (u32)shown, a1,
                        (u32)(s32)(s16)guest::load16(var), 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
        break;
    case 0xD7: move_to(T_SPAN, move_value_s8(t, T_SPAN), a1); break;
    case 0xD8: t.setf(T_LPF_FREQ, (f32)(s32)(a1 - 0x40) * 0.015625f); break;
    case 0xD9: t.set8(T_FX_SEND + 0, (u8)a1); break;
    case 0xDA: t.set8(T_FX_SEND + 1, (u8)a1); break;
    case 0xDE: t.set8(T_FX_SEND + 2, (u8)a1); break;
    case 0xDB: t.set8(T_MAIN_SEND, (u8)a1); break;
    case 0xDC: t.set8(T_INIT_PAN, (u8)(a1 - 0x40)); break;
    case 0xDD: g::nw4r_snd_detail_SeqTrack_SetMute(track, a1); break;
    case 0xDF: {  // damper: on from 64 (the original's bit trick)
        u32 b = a1 & 0xFF;
        t.set8(T_DAMPER, (u8)(((b | ~0x40u) - ((b - 0x40) >> 1)) >> 31));
        break;
    }
    case 0xE0: t.set32(T_LFO + 0x8, a1 * 5); break;  // LFO delay, ms
    case 0xE1: p.set16(P_TEMPO, (u16)(v > 0x3FF ? 0x3FF : v < 0 ? 0 : v)); break;
    case 0xE3: t.setf(T_SWEEP_PITCH, (f32)v * 0.015625f); break;
    case 0xFB:  // envelope reset
        for (u32 i = 0; i < 4; i++)
            t.set8(T_ATTACK + i, 0xFF);
        t.set16(T_ENV_HOLD, 0xFF);
        break;
    }
    return 0;
}

// ---- SeqTrack

// SeqTrack::ParseNextTick: 1 while playing, -1 at the track's end, 0 if not open
s32 track_parse_next_tick(u32 track, u32 do_note_on) {
    SoundLock lock;
    GuestObj t(track);
    if (!t.get8(T_OPENED))
        return 0;
    for (u32 at : {T_VOLUME, T_PAN, T_SPAN, T_BEND}) {  // the MoveParams advance a frame
        s16 frame = t.gets16(at + 4);
        if (frame < t.gets16(at + 2))
            t.set16(at + 4, (u16)(frame + 1));
    }
    if (t.get8(T_WAIT_NOTE_END)) {
        if (t.get32(T_CHANNELS))
            return 1;
        t.set8(T_WAIT_NOTE_END, 0);
    }
    s32 wait = t.gets32(T_WAIT);
    if (wait > 0) {
        t.set32(T_WAIT, (u32)--wait);
        if (wait > 0)
            return 1;
    }
    if (t.get32(T_CUR)) {
        u32 parse_fn = guest::load32(guest::load32(track) + 0xC);
        while (t.get32(T_WAIT) == 0 && t.get8(T_WAIT_NOTE_END) == 0) {
            u32 end = parse_fn == MML_SEQ_TRACK_PARSE ? parse(t.get32(T_PARSER), track, do_note_on)
                                                      : gcall<u32>(parse_fn, track, do_note_on);
            if (end == 1)
                return -1;
        }
    }
    return 1;
}

// SeqTrack::UpdateChannelLength: notes with a length count down a tick, and are released
void track_update_channel_length(u32 track) {
    SoundLock lock;
    GuestObj t(track);
    if (!t.get8(T_OPENED))
        return;
    for (u32 ch = t.get32(T_CHANNELS); ch; ch = guest::load32(ch + 0xD4)) {
        GuestObj c(ch);
        s32 length = c.gets32(0xB0);
        if (length > 0)
            c.set32(0xB0, (u32)(length - 1));
        if (c.gets32(0xB0) == 0 && c.gets32(0x00) != 4 && t.get8(T_DAMPER) == 0)
            g::nw4r_snd_detail_Channel_Release(ch);
        if (!c.get8(0x38)) {  // the sweep, a tick at a time (Channel::UpdateSweep(1))
            s32 n = c.gets32(0x88) + 1, end = c.gets32(0x8C);
            c.set32(0x88, (u32)(n > end ? end : n));
        }
    }
}

// SeqTrack::UpdateChannelParam: the track's and player's parameters into each of its channels
void track_update_channel_param(u32 track) {
    SoundLock lock;
    GuestObj t(track);
    if (!t.get8(T_OPENED) || !t.get32(T_CHANNELS))
        return;
    GuestObj p(t.get32(T_PLAYER));
    f32 vol = (f32)(u32)move_value_u8(t, T_VOLUME) / 127.0f;
    f32 vol2 = (f32)(u32)t.get8(T_VOLUME2) / 127.0f;
    f32 main = (f32)(u32)p.get8(P_MAIN_VOLUME) / 127.0f;
    const f32 volume = 1.0f * (vol * vol) * (vol2 * vol2) * (main * main) * t.getf(T_EXT_VOLUME) * p.getf(P_VOLUME);
    const f32 bend = (f32)(u32)t.get8(T_BEND_RANGE) * ((f32)move_value_s8(t, T_BEND) * 0.0078125f);
    const f32 pitch = 1.0f * p.getf(P_PITCH) * t.getf(T_EXT_PITCH);
    f32 pan = (f32)move_value_s8(t, T_PAN) / 63.0f;
    pan = pan > 1.0f ? 1.0f : pan < -1.0f ? -1.0f : pan;
    pan = ((0.0f + pan) * t.getf(T_PAN_RANGE) * p.getf(P_PAN_RANGE)) + t.getf(T_EXT_PAN) + p.getf(P_PAN);
    f32 span = (f32)move_value_s8(t, T_SPAN) / 63.0f;
    span = span > 2.0f ? 2.0f : span < 0.0f ? 0.0f : span;
    span = 0.0f + span + t.getf(T_EXT_SPAN) + p.getf(P_SPAN);
    u32 biquad_type = t.get8(T_BIQUAD_TYPE);
    const f32 lpf = 0.0f + t.getf(T_LPF_FREQ) + p.getf(P_LPF_FREQ);
    f32 biquad_value = t.getf(T_BIQUAD_VALUE);
    if (u8 type = p.get8(P_BIQUAD_TYPE)) {
        biquad_value = p.getf(P_BIQUAD_VALUE);
        biquad_type = type;
    }
    const u8 remote_filter = p.get8(P_REMOTE_FILTER);
    const f32 main_send = 0.0f + (((f32)(u32)t.get8(T_MAIN_SEND) / 127.0f) - 1.0f) + p.getf(P_MAIN_SEND);
    f32 fx[3];
    for (u32 i = 0; i < 3; i++)
        fx[i] = (0.0f + (f32)(u32)t.get8(T_FX_SEND + i) / 127.0f) + p.getf(P_FX_SEND + 4 * i);

    for (u32 ch = t.get32(T_CHANNELS); ch; ch = guest::load32(ch + 0xD4)) {
        GuestObj c(ch);
        c.setf(0x40, volume);
        c.setf(0x80, bend);
        c.setf(0x44, pitch);
        c.setf(0x48, pan);
        c.setf(0x4C, span);
        c.setf(0x50, lpf);
        c.set8(0x3B, (u8)biquad_type);  // Channel::SetBiquadFilter
        c.setf(0x54, biquad_value);
        c.set8(0x3C, remote_filter);
        c.set32(0x58, p.get32(P_OUTPUT_LINE));
        c.setf(0x5C, p.getf(P_MAIN_OUT_VOLUME));
        c.setf(0x60, main_send);
        for (u32 i = 0; i < 3; i++)
            c.setf(0x64 + 4 * i, fx[i]);
        for (u32 i = 0; i < 4; i++)
            c.setf(0x70 + 4 * i, p.getf(P_REMOTE_VOLUME + 4 * i));
        for (s32 i = 0; i < p.gets32(P_VOICE_OUTS); i++) {  // Voice::SetVoiceOutParam
            if (u32 voice = c.get32(0xD0)) {
                GuestObj vo(voice);
                for (u32 k = 0; k < 6; k++)
                    vo.setf(V_OUT + (u32)i * 0x18 + 4 * k, p.getf(P_VOICE_OUT + (u32)i * 0x18 + 4 * k));
                vo.set16(V_SYNC, vo.get16(V_SYNC) | 0x3C);
            }
        }
        c.setf(0x1C, t.getf(T_LFO + 0x0));  // the LfoParam, as the original copies it
        c.setf(0x20, t.getf(T_LFO + 0x4));
        c.set32(0x24, t.get32(T_LFO + 0x8));
        c.set8(0x28, t.get8(T_LFO + 0xC));
        c.set16(0x29, t.get16(T_LFO + 0xD));
        c.set8(0x2B, t.get8(T_LFO + 0xF));
        c.set8(0x34, t.get8(T_LFO_TARGET));
    }
}

// ---- SeqPlayer

f32 ticks_per_ms(GuestObj p) {
    return (p.getf(P_TEMPO_RATIO) * (f32)(s32)(p.get8(P_TIMEBASE) * p.get16(P_TEMPO))) / 60000.0f;
}

// SeqPlayer::FinishPlayer: the sequence ended: callbacks off, tracks closed and freed
void finish_player(u32 player) {
    SoundLock lock;
    GuestObj p(player);
    if (p.get8(P_STARTED)) {
        g::nw4r_snd_detail_SoundThread_UnregisterPlayerCallback(g::nw4r_snd_detail_SoundThread_GetInstance(),
                                                                 player + P_PLAYER_LINK);
        p.set8(P_STARTED, 0);
    }
    if (p.get8(P_ACTIVE)) {
        g::nw4r_snd_detail_DisposeCallbackManager_UnregisterDisposeCallback(
            g::nw4r_snd_detail_DisposeCallbackManager_GetInstance(), player + P_DISPOSE_LINK);
        p.set8(P_ACTIVE, 0);
    }
    for (u32 i = 0; i < 16; i++) {
        SoundLock lock2;
        if (u32 track = p.get32(P_TRACKS + 4 * i)) {
            g::nw4r_snd_detail_SeqTrack_Close(track);
            u32 alloc = p.get32(P_ALLOCATOR);
            gcall<u32>(guest::load32(guest::load32(alloc) + 0x10), alloc, p.get32(P_TRACKS + 4 * i));
            p.set32(P_TRACKS + 4 * i, 0);
        }
    }
}

// SeqPlayer::ParseNextTick: a tick of every track; true once none is left open
bool player_parse_next_tick(u32 player, u32 do_note_on) {
    SoundLock lock;
    GuestObj p(player);
    bool active = false;
    for (u32 i = 0; i < 16; i++) {
        u32 track = p.get32(P_TRACKS + 4 * i);
        if (!track)
            continue;
        track_update_channel_length(track);
        if (track_parse_next_tick(track, do_note_on) < 0) {  // ended: closed and freed
            SoundLock lock2;
            if (u32 t = p.get32(P_TRACKS + 4 * i)) {
                g::nw4r_snd_detail_SeqTrack_Close(t);
                u32 alloc = p.get32(P_ALLOCATOR);
                gcall<u32>(guest::load32(guest::load32(alloc) + 0x10), alloc, p.get32(P_TRACKS + 4 * i));
                p.set32(P_TRACKS + 4 * i, 0);
            }
        }
        if (guest::load8(track + T_OPENED))
            active = true;
    }
    return !active;
}

// SeqPlayer::UpdateTick: ms of playback at the tempo, a tick at a time
void player_update_tick(u32 player, s32 ms) {
    GuestObj p(player);
    f32 tpm = ticks_per_ms(p);
    if (tpm == 0.0f)
        return;
    f32 rest = p.getf(P_TICK_COUNTER) / tpm;  // ms to the next tick
    f32 left = (f32)ms;
    for (;;) {
        if (!(rest < left)) {
            p.setf(P_TICK_COUNTER, (rest - left) * tpm);
            return;
        }
        left = left - rest;
        if (player_parse_next_tick(player, 1)) {
            finish_player(player);
            return;
        }
        p.set32(P_TICK_COUNT, p.get32(P_TICK_COUNT) + 1);
        tpm = ticks_per_ms(p);
        if (tpm == 0.0f)
            return;
        rest = 1.0f / tpm;
    }
}

// SeqPlayer::SkipTick: a start position: up to 0x300 ticks at once, without notes
void player_skip_tick(u32 player) {
    GuestObj p(player);
    for (u32 i = 0; i < 16; i++) {
        if (u32 track = p.get32(P_TRACKS + 4 * i)) {
            g::nw4r_snd_detail_SeqTrack_ReleaseAllChannel(track, 0x7F);
            g::nw4r_snd_detail_SeqTrack_FreeAllChannel(track);
        }
    }
    for (s32 n = 0;; n++) {
        s32 skip = p.gets32(P_SKIP_TICKS);
        if (skip == 0 && !(p.getf(P_SKIP_TIME) * ticks_per_ms(p) >= 1.0f)) {
            p.setf(P_SKIP_TIME, 0.0f);
            return;
        }
        if (n >= 0x300)
            return;
        if (skip != 0)
            p.set32(P_SKIP_TICKS, (u32)(p.gets32(P_SKIP_TICKS) - 1));
        else
            p.setf(P_SKIP_TIME, p.getf(P_SKIP_TIME) - (1.0f / ticks_per_ms(p)));
        if (player_parse_next_tick(player, 0)) {
            finish_player(player);
            return;
        }
        p.set32(P_TICK_COUNT, p.get32(P_TICK_COUNT) + 1);
    }
}

// SeqPlayer::Update: an AX frame (3 ms) of a sequence, then its tracks' parameters
void player_update(u32 player) {
    SoundLock lock;
    GuestObj p(player);
    if (!p.get8(P_ACTIVE) || !p.get8(P_STARTED))
        return;
    if (p.gets32(P_SKIP_TICKS) != 0 || p.getf(P_SKIP_TIME) > 0.0f)
        player_skip_tick(player);
    else if (!p.get8(P_PAUSED))
        player_update_tick(player, 3);
    SoundLock lock2;
    for (u32 i = 0; i < 16; i++)
        if (u32 track = p.get32(P_TRACKS + 4 * i))
            track_update_channel_param(track);
}

}  // namespace

}  // namespace engine::snd

// ---- the guest ABI (decl.h)
namespace g {

using namespace engine::snd;

u32 nw4r_snd_detail_MmlParser_Parse(u32 parser, u32 track, u32 do_note_on) {
    return parse(parser, track, do_note_on);
}
u32 nw4r_snd_detail_MmlParser_ReadArg(u32 parser, u32 cur, u32 player, u32 track, u32 type) {
    // (cur: the track's read position, track + 0x20)
    (void)cur;
    return read_arg(parser, GuestObj(track), player, (s32)type);
}
u32 nw4r_snd_detail_MmlParser_CommandProc(u32 parser, u32 track, u32 cmd, u32 a1, u32 a2) {
    return command_proc(parser, track, cmd, a1, a2);
}
u32 nw4r_snd_detail_MmlParser_NoteOnCommandProc(u32, u32 track, u32 key, u32 velocity, u32 length, u32 tie) {
    return nw4r_snd_detail_SeqTrack_NoteOn(track, key, velocity, length, tie);
}
u32 nw4r_snd_detail_MmlSeqTrack_Parse(u32 track, u32 do_note_on) {
    return parse(guest::load32(track + T_PARSER), track, do_note_on);
}
u32 nw4r_snd_detail_SeqTrack_ParseNextTick(u32 track, u32 do_note_on) {
    return (u32)track_parse_next_tick(track, do_note_on);
}
void nw4r_snd_detail_SeqTrack_UpdateChannelLength(u32 track) {
    track_update_channel_length(track);
}
void nw4r_snd_detail_SeqTrack_UpdateChannelParam(u32 track) {
    track_update_channel_param(track);
}
u32 nw4r_snd_detail_SeqTrack_GetVariablePtr(u32 track, u32 i) {
    return track_variable(track, (s32)i);
}
void nw4r_snd_detail_SeqTrack_SetSeqData(u32 track, u32 base, u32 offset) {
    GuestObj t(track);
    t.set32(T_BASE, base);
    t.set32(T_CUR, base + offset);
}
void nw4r_snd_detail_SeqTrack_Open(u32 track) {
    GuestObj t(track);
    t.set8(T_WAIT_NOTE_END, 0);
    t.set8(T_STACK_DEPTH, 0);
    t.set32(T_WAIT, 0);
    t.set8(T_OPENED, 1);
}
u32 nw4r_snd_detail_SeqPlayer_GetVariablePtr(u32 player, u32 i) {
    return player_variable(player, (s32)i);
}
u32 nw4r_snd_detail_SeqPlayer_GetPlayerTrack(u32 player, u32 i) {
    return (s32)i > 0xF ? 0 : guest::load32(player + P_TRACKS + i * 4);
}
u32 nw4r_snd_detail_SeqPlayer_Update(u32 player) {
    NOMA_PROF_SCOPE("(snd sequences)");
    player_update(player);
    return 0;
}
u32 nw4r_snd_detail_SeqPlayer_ParseNextTick(u32 player, u32 do_note_on) {
    return player_parse_next_tick(player, do_note_on);
}
void nw4r_snd_detail_SeqPlayer_UpdateTick(u32 player, u32 ms) {
    player_update_tick(player, (s32)ms);
}
void nw4r_snd_detail_SeqPlayer_SkipTick(u32 player) {
    player_skip_tick(player);
}
void nw4r_snd_detail_Channel_SetBiquadFilter(u32 ch, u32 type, f64 value) {
    GuestObj c(ch);
    c.set8(0x3B, (u8)type);
    c.setf(0x54, (f32)value);
}
void nw4r_snd_detail_Channel_UpdateSweep(u32 ch, u32 ticks) {
    GuestObj c(ch);
    s32 n = c.gets32(0x88) + (s32)ticks, end = c.gets32(0x8C);
    c.set32(0x88, (u32)(n > end ? end : n));
}
f64 nw4r_snd_detail_BasicPlayer_GetFxSend(u32 player, u32 i) {
    return GuestObj(player).getf(P_FX_SEND + i * 4);
}
f64 nw4r_snd_detail_BasicPlayer_GetRemoteOutVolume(u32 player, u32 i) {
    return GuestObj(player).getf(P_REMOTE_VOLUME + i * 4);
}
void nw4r_snd_detail_Voice_SetVoiceOutParam(u32 voice, u32 i, u32 param) {
    GuestObj v(voice), src(param);
    for (u32 k = 0; k < 6; k++)
        v.setf(V_OUT + i * 0x18 + 4 * k, src.getf(4 * k));
    v.set16(V_SYNC, v.get16(V_SYNC) | 0x3C);
}

}  // namespace g
