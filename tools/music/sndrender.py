#!/usr/bin/env python3
"""A small numpy sampler that plays nw4r sequences with a bank's own waves, to audition them.

It renders the note list of brsar.simulate() (a sequence played through a model of nw4r's
sequencer) the way nw4r::snd voices them, closely enough to judge notes, instruments and balance:

  - the instrument: Bank::NoteOn (8046BA80) -> BankFileReader::GetInstParam (8046BD10) for the
    track's program, the key and the velocity; its wave from the bank's RWAR;
  - pitch: 2^((key - originalKey + bend) / 12) * tune * waveRate / outRate (Channel's pitch),
    plus the track's LFO when it modulates pitch;
  - volume: (velocity/127)^2 * (instVolume/127) (Bank::NoteOn's init volume) *
    (volume/127)^2 * (volume2/127)^2 * (mainVolume/127)^2 (SeqTrack::UpdateChannelParam);
  - pan: (instPan - 64)/63 + the track's pan/63 (+ init pan), constant power;
  - envelope: nw4r's EnvGenerator in 0.1 dB per ms: attack multiplies -90.4 dB by the attack
    table's factor each ms until above -0.003 dB, hold (hold+1)^2/4 ms, decay and release
    subtract CalcRelease(x) per ms (127: at once, 126: 24, <50: (2x+1)/128/5, else
    60/(126-x)/5), sustain is the sustain table's level; the track's attack/decay/sustain/
    release/hold (when <= 127) override the instrument's (SeqTrack::NoteOn);
  - a note is released at its 'off_tick' (its length in ticks, or later while the track's
    damper is down: brsar.simulate), unless the instrument ignores note-off; a looped wave loops
    from its loop start, an unlooped one ends with its samples;
  - optionally (reverb=True) a plain feedback-delay reverb fed by each track's fxsend A, as a
    stand-in for the channel's AX reverb on aux A (its settings are not modelled).

Not modelled: the AX mixer's own filters and reverb (the channel's FX sends), portamento,
sweeps, monophonic/tie, the voice-stealing of a full channel pool. Output is normalised.
"""
import numpy as np

import brsar

# EnvGenerator's attack factors per ms (the DOL's table at 0x8070C498, by attack 0..127)
ATTACK_TABLE = [
    0.999218, 0.998433, 0.997645, 0.996855, 0.996063, 0.995268, 0.99447, 0.99367, 0.992868, 0.992063, 0.991255,
    0.990444, 0.989631, 0.988815, 0.987997, 0.987175, 0.986351, 0.985524, 0.984695, 0.983863, 0.983027, 0.982189,
    0.981348, 0.980505, 0.979658, 0.978808, 0.977956, 0.9771, 0.976241, 0.97538, 0.974515, 0.973647, 0.972776,
    0.971902, 0.971025, 0.970145, 0.969261, 0.968374, 0.967484, 0.966591, 0.965694, 0.964794, 0.963891, 0.962984,
    0.962074, 0.96116, 0.960243, 0.959323, 0.958398, 0.957471, 0.956539, 0.955604, 0.954665, 0.953723, 0.952777,
    0.951827, 0.950873, 0.949916, 0.948954, 0.947989, 0.94702, 0.946046, 0.945069, 0.944088, 0.943102, 0.942112,
    0.941119, 0.940121, 0.939118, 0.938112, 0.937101, 0.936086, 0.935066, 0.934042, 0.933013, 0.93198, 0.930942,
    0.9299, 0.928852, 0.9278, 0.926744, 0.925682, 0.924616, 0.923544, 0.922468, 0.921386, 0.9203, 0.919208,
    0.918111, 0.917009, 0.915902, 0.914789, 0.91367, 0.912547, 0.911417, 0.910282, 0.909141, 0.907995, 0.906843,
    0.905684, 0.90452, 0.90335, 0.902174, 0.900992, 0.899803, 0.898608, 0.897407, 0.896199, 0.894984, 0.89006,
    0.882462, 0.875925, 0.869186, 0.863641, 0.853579, 0.843019, 0.828614, 0.81491, 0.800217, 0.778066, 0.755475,
    0.724213, 0.682824, 0.632917, 0.559214, 0.455141, 0.329877, 0.0]

# EnvGenerator's sustain levels in 0.1 dB (engine/snd/tables.cpp kSustainTable)
SUSTAIN_TABLE = [
    -723, -722, -721, -651, -601, -562, -530, -503, -480, -460, -442, -425, -410, -396, -383, -371, -360, -349,
    -339, -330, -321, -313, -305, -297, -289, -282, -276, -269, -263, -257, -251, -245, -239, -234, -229, -224,
    -219, -214, -210, -205, -201, -196, -192, -188, -184, -180, -176, -173, -169, -165, -162, -158, -155, -152,
    -149, -145, -142, -139, -136, -133, -130, -127, -125, -122, -119, -116, -114, -111, -109, -106, -103, -101,
    -99, -96, -94, -91, -89, -87, -85, -82, -80, -78, -76, -74, -72, -70, -68, -66, -64, -62, -60, -58, -56,
    -54, -52, -50, -49, -47, -45, -43, -42, -40, -38, -36, -35, -33, -31, -30, -28, -27, -25, -23, -22, -20, -19,
    -17, -16, -14, -13, -11, -10, -8, -7, -6, -4, -3, -1, 0]

FLOOR = -904.0  # 0.1 dB: -90.4 dB, where a released note ends


def calc_release(x):
    """EnvGenerator::CalcRelease (8046F650): 0.1 dB per ms."""
    if x >= 127:
        return 65535.0
    if x == 126:
        return 24.0
    if x < 50:
        return (2 * x + 1) / 128.0 / 5.0
    return 60.0 / (126 - x) / 5.0


def envelope_db(attack, hold, decay, sustain, release, off_ms, total_ms):
    """The envelope in 0.1 dB, one value per ms, for total_ms; released at off_ms (None: never)."""
    n = int(total_ms)
    v = np.empty(n, dtype=np.float64)
    coef = ATTACK_TABLE[max(0, min(127, attack))]
    if coef <= 0.0:
        n_attack = 0
    else:
        n_attack = int(np.ceil(np.log(0.03125 / 904.0) / np.log(coef)))
    hold_ms = ((hold + 1) * (hold + 1)) >> 2
    sus = SUSTAIN_TABLE[max(0, min(127, sustain))]
    dec = calc_release(decay)
    t = np.arange(n, dtype=np.float64)
    # attack: -904 * coef^(t+1)
    a_end = min(n, n_attack)
    if a_end:
        v[:a_end] = FLOOR * np.power(coef, t[:a_end] + 1)
    h_end = min(n, a_end + hold_ms)
    v[a_end:h_end] = 0.0
    if h_end < n:
        d = -dec * (t[h_end:] - h_end + 1)
        v[h_end:] = np.maximum(d, sus)
    if off_ms is not None and off_ms < n:
        o = int(off_ms)
        start = v[o - 1] if o > 0 else FLOOR
        rel = calc_release(release)
        v[o:] = np.minimum(v[o:], start - rel * (t[o:] - o + 1))
    return np.maximum(v, FLOOR)


class Sampler:
    def __init__(self, bank, rate=32000):
        self.bank, self.rate = bank, rate

    def note(self, prg, key, velocity, dur_s, adsr=(255,) * 4, hold=255, bend=0.0, mod=None, max_tail_s=4.0):
        """(stereo float32 [2, n], or None) for one note held dur_s seconds (None: until its end)."""
        ip = self.bank.inst(prg, key, velocity)
        if ip is None or ip.wave < 0:
            return None, None
        wv = self.bank.wave(ip.wave)
        pcm = wv.pcm()
        ratio = 2.0 ** ((key - ip.original_key + bend) / 12.0) * ip.tune * wv.rate / self.rate
        attack = adsr[0] if adsr[0] <= 127 else ip.attack
        decay = adsr[1] if adsr[1] <= 127 else ip.decay
        sustain = adsr[2] if adsr[2] <= 127 else ip.sustain
        release = adsr[3] if adsr[3] <= 127 else ip.release
        hold = hold if hold <= 127 else ip.hold
        if ip.note_off:
            dur_s = None
        if wv.loop:
            rel_s = 0.0 if dur_s is None else min(max_tail_s, 90.4 / max(calc_release(release) * 100, 1e-6))
            total_s = (dur_s or max_tail_s) + rel_s
        else:
            total_s = (wv.nsamples / wv.rate) / (ratio * self.rate / wv.rate)
            if dur_s is not None:
                total_s = min(total_s, dur_s + min(max_tail_s, 90.4 / max(calc_release(release) * 100, 1e-6)))
        n = max(1, int(total_s * self.rate))
        pos = np.arange(n, dtype=np.float64) * ratio
        if mod and mod.get('depth') and mod.get('type', 0) == 0:
            speed = 0.390625 * mod.get('speed', 16)
            t = np.arange(n) / self.rate
            delay = mod.get('delay', 0) * 5 / 1000.0
            lfo = np.where(t >= delay, np.sin(2 * np.pi * speed * (t - delay)), 0.0)
            semis = lfo * mod['depth'] / 128.0 * mod.get('range', 1)
            step = ratio * 2.0 ** (semis / 12.0)
            pos = np.concatenate([[0.0], np.cumsum(step[:-1])])
        if wv.loop and wv.nsamples > wv.loop_start + 1:
            span = wv.nsamples - wv.loop_start
            over = pos >= wv.nsamples
            pos[over] = wv.loop_start + np.mod(pos[over] - wv.loop_start, span)
        else:
            keep = pos < wv.nsamples - 1
            pos = pos[keep]
            n = len(pos)
            if n == 0:
                return None, None
        i0 = np.floor(pos).astype(np.int64)
        frac = (pos - i0).astype(np.float32)
        i1 = i0 + 1
        if wv.loop:
            i1[i1 >= wv.nsamples] = wv.loop_start
        else:
            i1 = np.minimum(i1, wv.nsamples - 1)
        ms = int(np.ceil(n / self.rate * 1000)) + 1
        env = envelope_db(attack, hold, decay, sustain, release,
                          None if dur_s is None else dur_s * 1000.0, ms)
        env_s = np.interp(np.arange(n) * 1000.0 / self.rate, np.arange(ms), env)
        amp = np.power(10.0, env_s / 200.0).astype(np.float32)
        last = np.nonzero(env_s > FLOOR + 1)[0]
        if len(last):
            n = min(n, last[-1] + 1)
        chans = []
        for c in range(min(2, pcm.shape[0])):
            s = pcm[c]
            chans.append((s[i0[:n]] * (1 - frac[:n]) + s[i1[:n]] * frac[:n]) * amp[:n])
        if len(chans) == 1:
            chans = chans * 2
        return np.stack(chans), ip


def reverb_ir(rate, rt60=1.6, predelay_ms=20.0, seed=7):
    """A stereo impulse response: decaying noise (a stand-in for the AX reverb on aux A)."""
    rng = np.random.RandomState(seed)
    n = int(rate * rt60 * 1.2)
    t = np.arange(n) / rate
    env = np.power(10.0, -3.0 * t / rt60)
    ir = rng.standard_normal((2, n)) * env
    pre = int(rate * predelay_ms / 1000.0)
    ir = np.concatenate([np.zeros((2, pre)), ir], axis=1)
    ir /= np.sqrt((ir ** 2).sum(axis=1, keepdims=True))
    return ir.astype(np.float32)


def convolve(x, ir):
    n = x.shape[-1] + ir.shape[-1] - 1
    size = 1 << (n - 1).bit_length()
    return np.fft.irfft(np.fft.rfft(x, size) * np.fft.rfft(ir, size), size)[..., :n]


def render(sim, bank, rate=32000, tail_s=3.0, max_seconds=None, tracks=None, gain_db=-1.0, volume=1.0,
           reverb=False, stats=None):
    """Stereo float32 [2, n] of a brsar.simulate() result.

    gain_db: normalise the peak to this (None: absolute level, where 1.0 is a full-scale voice,
    times `volume`, the sound's own volume: its archive volume / 127). reverb: add a reverb fed
    by each note's fxsend A. stats: a dict that gets 'voices' (the most channels sounding at
    once, by the rendered lengths), 'mean_voices', 'rms_db' and 'peak' (before normalising)."""
    tsec = brsar.tick_to_seconds(sim['tempo'], sim['timebase'])
    smp = Sampler(bank, rate)
    end_s = tsec(sim['ticks']) + tail_s
    if max_seconds:
        end_s = min(end_s, max_seconds)
    out = np.zeros((2, int(end_s * rate) + rate), dtype=np.float32)
    wet = np.zeros_like(out) if reverb else None
    active = np.zeros(int(end_s * 100) + 101, dtype=np.int32)   # channels sounding per 10 ms
    for nt in sim['notes']:
        if tracks is not None and nt['track'] not in tracks:
            continue
        t0 = tsec(nt['tick'])
        if t0 >= end_s:
            continue
        off = nt.get('off_tick', nt['tick'] + nt['length'] if nt['length'] > 0 else None)
        dur = None if off is None else tsec(off) - t0
        snd, ip = smp.note(nt['program'], nt['key'], nt['velocity'], dur, nt['adsr'], nt['hold'], nt['bend'],
                           nt['mod'])
        if snd is None:
            continue
        vol = (nt['velocity'] / 127.0) ** 2 * (ip.volume / 127.0)
        vol *= (nt['volume'] / 127.0) ** 2 * (nt['volume2'] / 127.0) ** 2 * (nt['main_volume'] / 127.0) ** 2
        vol *= volume
        pan = np.clip((ip.pan - 64) / 63.0 + nt['pan'] / 63.0, -1, 1)
        left, right = np.cos((pan + 1) * np.pi / 4), np.sin((pan + 1) * np.pi / 4)
        s = int(t0 * rate)
        e = min(out.shape[1], s + snd.shape[1])
        if e <= s:
            continue
        out[0, s:e] += snd[0, :e - s] * vol * left * np.sqrt(2)
        out[1, s:e] += snd[1, :e - s] * vol * right * np.sqrt(2)
        send = nt.get('fx', [0, 0, 0])[0] / 127.0
        if wet is not None and send > 0:
            wet[0, s:e] += snd[0, :e - s] * vol * left * send
            wet[1, s:e] += snd[1, :e - s] * vol * right * send
        active[int(t0 * 100):int(e / rate * 100) + 1] += 1
    if wet is not None and np.any(wet):
        ir = reverb_ir(rate)
        out += 0.5 * convolve(wet, ir)[:, :out.shape[1]].astype(np.float32)
    out = out[:, :int(end_s * rate)]
    peak = float(np.abs(out).max())
    if stats is not None:
        stats['active'] = active   # channels sounding, every 10 ms
        stats['voices'] = int(active.max())
        sounding = active[active > 0]
        stats['mean_voices'] = float(sounding.mean()) if len(sounding) else 0.0
        stats['peak'] = peak
        stats['rms_db'] = float(10 * np.log10(np.mean(out ** 2) + 1e-20))
    if gain_db is not None and peak > 0:
        out *= (10 ** (gain_db / 20.0)) / peak
    return out


def main():
    import argparse
    ap = argparse.ArgumentParser(description='render a sound of a BRSAR (a sequence) to .wav')
    ap.add_argument('sound', help='label or id')
    ap.add_argument('out')
    ap.add_argument('--brsar', default=brsar.DEFAULT_BRSAR)
    ap.add_argument('--seconds', type=float, default=None)
    ap.add_argument('--loops', type=int, default=1)
    ap.add_argument('--tracks', help='comma-separated track numbers to keep')
    ap.add_argument('--reverb', action='store_true', help='add a reverb fed by the tracks\' fxsend A')
    ap.add_argument('--absolute', action='store_true',
                    help="don't normalise: the level the archive's volumes give (to compare pieces)")
    a = ap.parse_args()
    r = brsar.Rsar(a.brsar)
    sd = r.sound(a.sound)
    sq = r.sequence(sd.file)
    sim = brsar.simulate(sq.mml, sd.seq_offset, loops=a.loops, alloc_mask=sd.alloc_track)
    bank = r.bank(sd.bank)
    tracks = set(int(x) for x in a.tracks.split(',')) if a.tracks else None
    st = {}
    audio = render(sim, bank, max_seconds=a.seconds, tracks=tracks, reverb=a.reverb, volume=sd.volume / 127.0,
                   gain_db=None if a.absolute else -1.0, stats=st)
    brsar.write_wav(a.out, audio, 32000)
    print('%s: %d notes, %.1f s, up to %d voices (mean %.1f), rms %.1f dB, peak %.2f -> %s' % (
        sd.label, len(sim['notes']), audio.shape[1] / 32000.0, st['voices'], st['mean_voices'], st['rms_db'],
        st['peak'], a.out))


if __name__ == '__main__':
    main()
