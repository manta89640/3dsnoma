#!/usr/bin/env python3
"""Stage Wuhu Island's sound effects from your own Wii Sports Resort: DATA/wuhu/sounds.nsfx.

    tools/wuhu/stage_sounds.py DATA [--wsr ROOT] [--wav DIR]

ROOT is your Wii Sports Resort extraction (default ../wii-sports-extract/wii-sports-resort next to
this repository; files/ under it is the disc's root). DATA is the port's data directory (on the 3DS
sdmc:/3ds/noma). The bank goes to DATA/wuhu/sounds.nsfx (engine/audio/nsfx.h), what went into it to
DATA/wuhu/sounds.json; --wav DIR also writes each sound's first variant as a .wav to audition. Nothing
of the disc goes anywhere else, and nothing this makes belongs in the repository. tools/wuhu/
stage_wuhu.py runs it too, after the island's package.

What goes in (docs/wuhu.md "Sounds" has the reasons): the sounds of Wii Sports Resort's own sound
archive (Sound/Static/Sp2Snd.brsar) that the island plays (SOUNDS below), each as the waves its notes
play -- the disc's own DSP-ADPCM, as they are (no second encoding) -- with the level and pitch nw4r::snd
gives each note. A sequence's sound (most of WSR's effects: a note or two on an instrument of an
effects bank) is played through tools/music/brsar.py's model of nw4r's sequencer (with the variables
the game sets, as Swordplay Showdown's footsteps' ground: var1) many times over: its random
transpositions give the variants (Showdown's steps alternate between two recordings for each foot),
its random pitch bends and volumes the play's spread, which the engine draws at each play as the
sequencer would. A wave sound (RWSD) is its one wave. Levels are nw4r's: velocity^2 x the
instrument's volume x the track's volume^2 x the sound's archive volume (tools/music/sndrender.py);
the engine scales them all by one level against the island's music (engine/wuhu/sounds.cpp).
"""
import argparse
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "music"))
import brsar  # noqa: E402

DEFAULT_WSR = os.path.join(os.path.dirname(ROOT), "wii-sports-extract", "wii-sports-resort")
BRSAR = "Sound/Static/Sp2Snd.brsar"
RATE = 32000          # the AX output's
SEEDS = 64            # runs of a sequence through the model, for its variants and spreads

# The grounds the footsteps tell apart, in the order of Swordplay Showdown's footstep programs
# (SE_SWF_KUMITE_FOOTNOTE_*: program 8 + var1 of bank SE_SWF_KUMITE): var1 is the KCL attribute's low
# byte less one -- 0x1101 grass, 0x1102 dirt, 0x1103 hard ground, 0x1104 sand, 0x1105 wood, 0x1106
# water -- six programs for the attributes' first six, the hard ground's waves the same recordings
# as the duel stage's and the basketball court's, the water's splashing (docs/wuhu.md "Sounds").
GROUNDS = ["grass", "dirt", "hard", "sand", "wood", "water"]

# (name, WSR's sound label, the sequence's player variables, a loop): what the island plays
# (engine/wuhu/sounds.cpp names them)
SOUNDS = []
for _i, _g in enumerate(GROUNDS):
    SOUNDS += [
        ("step.%s.walk_l" % _g, "SE_SWF_KUMITE_FOOTNOTE_WALK_L", {1: _i}, False),
        ("step.%s.walk_r" % _g, "SE_SWF_KUMITE_FOOTNOTE_WALK_R", {1: _i}, False),
        ("step.%s.run_l" % _g, "SE_SWF_KUMITE_FOOTNOTE_RUN_L", {1: _i}, False),
        ("step.%s.run_r" % _g, "SE_SWF_KUMITE_FOOTNOTE_RUN_R", {1: _i}, False),
        ("jump.%s" % _g, "SE_SWF_KUMITE_FOOTNOTE_JUMP", {1: _i}, False),
        ("land.%s" % _g, "SE_SWF_KUMITE_FOOTNOTE_LANDING", {1: _i}, False),
    ]
SOUNDS += [
    # Skydiving's: the big canopy opening over the player, and Island Flyover's wind in flight
    ("parachute.open", "SE_OMK_PARACHUTE_L_OPEN_PLAYER", {}, False),
    ("glide.wind", "SE_PLN_WIND_FLYING", {}, True),
    # into the water: Cycling's wheel landing in shallow water (two layers), a cyclist falling in,
    # a Mii knocked off Swordplay's stage into the sea
    ("splash.small", "SE_BIC_LANDING_WATER", {}, False),
    ("splash.medium", "SE_BIC_FALL_WATER_ONE", {}, False),
    ("splash.large", "SE_SWF_PLAYER_FALL_WATER", {}, False),
    # a swimming hand's stroke: Canoeing's light paddle stroke (two layers)
    ("swim.stroke", "SE_CAN_PADDLING_S", {}, False),
    # a car: Skydiving's steps on the plane's floor (in) and out of its hatch (out); the island's
    # electric cars' hum (Basketball's and Swordplay's background traffic)
    ("car.in", "SE_OMK_FOOTNOTE_TO_HATCH", {}, False),
    ("car.out", "SE_OMK_FOOTNOTE_JUMP_OUT", {}, False),
    ("car.engine", "SE_CMN_ELEC_CAR_RUNNING_1", {}, True),
    # a boat: Wakeboarding's tow boat, its engine at three speeds and its hull's water at two
    ("boat.engine.idle", "SE_JSK_BOAT_ENGINE_IDLE", {}, True),
    ("boat.engine.low", "SE_JSK_BOAT_ENGINE_LOW", {}, True),
    ("boat.engine.high", "SE_JSK_BOAT_ENGINE_HIGH", {}, True),
    ("boat.water.low", "SE_JSK_BOAT_RUN_WATER_LOW", {}, True),
    ("boat.water.high", "SE_JSK_BOAT_RUN_WATER_HIGH", {}, True),
]

MAGIC, VERSION = 0x5846534E, 1   # "NSFX" (engine/audio/nsfx.h)
ADPCM = 0
HEADER = struct.Struct("<IHHHHHHIIII")      # 32
SOUND = struct.Struct("<IHHffB3x")          # 20
VARIANT = struct.Struct("<HH")              # 4
LAYER = struct.Struct("<HHff")              # 12
WAVE = struct.Struct("<BBHIII16hhhhhII")    # 64
assert (HEADER.size, SOUND.size, VARIANT.size, LAYER.size, WAVE.size) == (32, 20, 4, 12, 64)


def be_u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


class Wave:
    """A mono DSP-ADPCM wave as the bank keeps it: its frames, coefficients and history."""

    def __init__(self, key, frames, coefs, rate, samples, loops, loop_start, hist):
        self.key, self.frames, self.coefs, self.rate = key, frames, coefs, rate
        self.samples, self.loops, self.loop_start, self.hist = samples, loops, loop_start, hist
        # the decoded samples (as the engine's decoder makes them: the same arithmetic) and the history
        # at the start of the loop's frame, where the decoder goes back to
        self.pcm = brsar.decode_adpcm(frames, coefs, samples, hist[0], hist[1])
        fs = loop_start // 14 * 14
        self.loop_hist = ((int(self.pcm[fs - 1]) if fs >= 1 else hist[0]),
                          (int(self.pcm[fs - 2]) if fs >= 2 else hist[1]))


def rwav_wave(key, w):
    """A Wave of an RWAV (brsar.Rwav): mono, DSP-ADPCM, its frames all there."""
    if w.format != 2 or w.channels != 1:
        raise SystemExit("%s: %s: only mono DSP-ADPCM waves are staged" % (key, w.describe()))
    ci = w.channel_info[0]
    n = (w.nsamples + 13) // 14 * 8
    frames = w.data[ci["data"]:ci["data"] + n]
    if len(frames) != n:
        raise SystemExit("%s: its frames run past the file (%d of %d bytes)" % (key, len(frames), n))
    a = ci["adpcm"]
    return Wave(key, frames, a["coefs"], w.rate, w.nsamples, bool(w.loop), w.loop_start if w.loop else 0,
                (a["yn1"], a["yn2"]))


def rwsd_note(r, s):
    """A wave sound's (RWSD) first note: its RWAV, pitch and volume (the note's and the sound's own
    pitch: tools/music/stage_music.py rwsd_wave; NoteInfo +0xD the volume)."""
    d = r.file_data(s.file)
    magic, _, _, blocks = brsar.block_header(d, 0)
    if magic != "RWSD" or len(blocks) > 1:
        raise SystemExit("%s: not an RWSD of a wave archive (%s, %d blocks)" % (s.label, magic, len(blocks)))
    base = blocks[0][0] + 8

    def ref(o):
        return base + be_u32(d, o + 4)

    sound = ref(base + 4 + 8 * s.wave_index)
    note = ref(ref(sound + 0x10) + 4)
    wave_index = struct.unpack_from(">i", d, note)[0]
    pitch = struct.unpack_from(">f", d, note + 0x10)[0] * (struct.unpack_from(">f", d, ref(sound))[0] or 1.0)
    volume = d[note + 0xD]
    return brsar.Rwar(r.wave_data(s.file)).wave(wave_index), wave_index, pitch, volume


class Stager:
    def __init__(self, brsar_path):
        self.r = brsar.Rsar(brsar_path)
        self.waves = []            # Wave
        self.wave_at = {}          # key -> index
        self.sounds = []           # (name, loops, gain spread, pitch spread, [[(wave, gain, pitch)]])
        self.inv = []

    def wave(self, key, make):
        if key not in self.wave_at:
            self.wave_at[key] = len(self.waves)
            self.waves.append(make())
        return self.wave_at[key]

    def stage(self, name, label, pvars, loops):
        r = self.r
        s = r.sound(label)
        if s.type == 3:
            w, index, pitch, volume = rwsd_note(r, s)
            wi = self.wave(("rwsd", s.file, index), lambda: rwav_wave("%s (wave %d)" % (label, index), w))
            gain = volume / 127.0 * s.volume / 127.0
            variants, gspread, pspread = [[(wi, gain, pitch)]], 1.0, 0.0
        elif s.type == 1:
            variants, gspread, pspread = self.sequence(name, s, pvars)
        else:
            raise SystemExit("%s: %s is a stream, not an effect" % (name, label))
        if loops and not all(len(v) == 1 and self.waves[v[0][0]].loops for v in variants):
            raise SystemExit("%s: a loop must be one looping wave" % name)
        self.sounds.append((name, loops, gspread, pspread, variants))
        self.inv.append(dict(name=name, label=label, vars=pvars, loops=loops, gain_spread=round(gspread, 4),
                             pitch_spread=round(pspread, 4),
                             variants=[[dict(wave=self.waves[w].key, gain=round(g, 4), pitch=round(p, 4),
                                             seconds=round(self.waves[w].samples / self.waves[w].rate / p, 3))
                                        for w, g, p in v] for v in variants]))

    def sequence(self, name, s, pvars):
        """(variants, gain spread, pitch spread) of a sequence's sound, from SEEDS runs of the model."""
        r = self.r
        sq = r.sequence(s.file)
        bank = r.bank(s.bank)
        pv = [0] * 16
        for k, v in pvars.items():
            pv[k] = v
        found = {}               # variant key -> each run's layers' gains
        bends = []
        for seed in range(1, SEEDS + 1):
            sim = brsar.simulate(sq.mml, s.seq_offset, loops=1, alloc_mask=s.alloc_track, player_vars=pv, seed=seed,
                                 max_ticks=48 * 4 * 8)
            notes = sorted(sim["notes"], key=lambda n: (n["track"], n["tick"]))
            if not notes:
                raise SystemExit("%s: %s plays no note" % (name, s.label))
            if any(n["tick"] != notes[0]["tick"] for n in notes):
                raise SystemExit("%s: %s's notes don't start together" % (name, s.label))
            layers = []
            for n in notes:
                ip = bank.inst(n["program"], n["key"], n["velocity"])
                if ip is None or ip.wave < 0:
                    raise SystemExit("%s: %s's note %s on program %d has no instrument" % (
                        name, s.label, brsar.note_name(n["key"]), n["program"]))
                if n["mod"].get("depth") or n["sweep"] or n["porta"] is not None:
                    print("  note: %s: %s has a modulation, sweep or portamento the bank leaves out" % (name, s.label))
                w = bank.wave(ip.wave)
                wi = self.wave((bank.label, ip.wave), lambda: rwav_wave("%s wave %d" % (bank.label, ip.wave), w))
                pitch = 2.0 ** ((n["key"] - ip.original_key) / 12.0) * ip.tune
                gain = ((n["velocity"] / 127.0) ** 2 * (ip.volume / 127.0) * (n["volume"] / 127.0) ** 2 *
                        (n["volume2"] / 127.0) ** 2 * (n["main_volume"] / 127.0) ** 2 * s.volume / 127.0)
                layers.append((wi, round(pitch, 6), gain))
                bends.append(n["bend"])
            key = tuple((w, p) for w, p, _ in layers)
            found.setdefault(key, []).append([g for _, _, g in layers])
        # a variant's layer's gain: the middle of its range over the runs that played it (geometrically:
        # its random volume's); the spread, the most a run's was off it in any of them
        variants, gspread = [], 1.0
        for key in sorted(found):
            g = np.array(found[key])
            lo, hi = g.min(axis=0), g.max(axis=0)
            gspread = max(gspread, float(np.max(np.sqrt(hi / lo))))
            variants.append([(w, float(m), p) for (w, p), m in zip(key, np.sqrt(lo * hi))])
        pspread = float(max(abs(b) for b in bends))
        return variants, gspread, pspread

    def write(self, path):
        names = b""
        name_at = []
        for name, *_ in self.sounds:
            name_at.append(len(names))
            names += name.encode() + b"\0"
        sounds, variants, layers = b"", b"", b""
        nv = nl = 0
        for (name, loops, gspread, pspread, vs), at in zip(self.sounds, name_at):
            sounds += SOUND.pack(at, nv, len(vs), gspread, pspread, 1 if loops else 0)
            for v in vs:
                variants += VARIANT.pack(nl, len(v))
                for w, gain, pitch in v:
                    layers += LAYER.pack(w, 0, gain, pitch)
                nl += len(v)
            nv += len(vs)
        data, waves = b"", b""
        for w in self.waves:
            data += b"\0" * (-len(data) % 32)
            waves += WAVE.pack(ADPCM, 1 if w.loops else 0, 0, w.rate, w.samples, w.loop_start, *w.coefs, w.hist[0],
                               w.hist[1], w.loop_hist[0], w.loop_hist[1], len(data), len(w.frames))
            data += w.frames
        tables = sounds + variants + layers + waves
        strings = HEADER.size + len(tables)
        data_at = strings + len(names)
        data_at += -data_at % 32
        head = HEADER.pack(MAGIC, VERSION, len(self.sounds), nv, nl, len(self.waves), 0, strings, data_at, len(data), 0)
        blob = head + tables + names
        blob += b"\0" * (data_at - len(blob)) + data
        with open(path, "wb") as f:
            f.write(blob)
        return len(blob)

    def wav(self, out_dir):
        """Each sound's first variant at 32 kHz, its layers mixed, to audition (a loop twice through)."""
        os.makedirs(out_dir, exist_ok=True)
        for name, loops, _, _, vs in self.sounds:
            mix = np.zeros(1, dtype=np.float64)
            for w, gain, pitch in vs[0]:
                wv = self.waves[w]
                x = wv.pcm.astype(np.float64) / 32768.0
                if loops:
                    x = np.concatenate([x, x[wv.loop_start:], x[wv.loop_start:]])
                step = wv.rate * pitch / RATE
                pos = np.arange(0, len(x) - 1, step)
                y = np.interp(pos, np.arange(len(x)), x) * gain
                if len(y) > len(mix):
                    mix = np.concatenate([mix, np.zeros(len(y) - len(mix))])
                mix[:len(y)] += y
            brsar.write_wav(os.path.join(out_dir, name + ".wav"), mix[None, :].astype(np.float32), RATE)


def stage(wsr, data_dir, wav_dir=None, say=print):
    """Stage the bank into DATA/wuhu (stage_wuhu.py calls this too); its path."""
    path = os.path.join(wsr, "files", BRSAR)
    if not os.path.exists(path):
        raise SystemExit("no %s (give --wsr the extraction's root)" % path)
    st = Stager(path)
    for name, label, pvars, loops in SOUNDS:
        st.stage(name, label, pvars, loops)
    out_dir = os.path.join(data_dir, "wuhu")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "sounds.nsfx")
    size = st.write(out)
    adpcm = sum(len(w.frames) for w in st.waves)
    with open(os.path.join(out_dir, "sounds.json"), "w") as f:
        json.dump(dict(source=BRSAR, bytes=size, sounds=st.inv,
                       waves=[dict(key=w.key, rate=w.rate, samples=w.samples, loops=w.loops,
                                   loop_start=w.loop_start, bytes=len(w.frames)) for w in st.waves]),
                  f, ensure_ascii=False, indent=1)
    if wav_dir:
        st.wav(wav_dir)
    say("wrote %s: %d sounds, %d variants, %d waves (%d bytes of the disc's DSP-ADPCM, %.1f s), %d bytes" % (
        out, len(st.sounds), sum(len(s[4]) for s in st.sounds), len(st.waves), adpcm,
        sum(w.samples / w.rate for w in st.waves), size))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("data", help="the port's data directory (the bank goes to DATA/wuhu/sounds.nsfx)")
    ap.add_argument("--wsr", default=DEFAULT_WSR, help="the Wii Sports Resort extraction's root (holding files/)")
    ap.add_argument("--wav", help="also write each sound's first variant here as a .wav")
    a = ap.parse_args(argv)
    stage(a.wsr, a.data, a.wav)
    return 0


if __name__ == "__main__":
    sys.exit(main())
