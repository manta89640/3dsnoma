#!/usr/bin/env python3
"""Compares a recording of the port (NOMA_DUMP_AUDIO=file.wav: what the audio DMA played, 32 kHz
stereo) with the model of what should have played (brsar.simulate + sndrender):

    dump_check.py score SCORE.py DUMP.wav [--near SECONDS] [--passes 2]
        where the score's piece starts in the recording (the model's note onsets against the
        recording's, cross-correlated), its level section by section against the model's (a
        constant difference: the balance is the model's), how well the onsets agree, and the
        length of a pass of its loop (the recording against itself one pass later)
    dump_check.py locate LABEL DUMP.wav --at SECONDS [--length 12]
        where in one of the archive's pieces (by its label) the recording at SECONDS is: the
        piece's position, in seconds from its start (e.g. the room's theme resuming where it
        paused rather than from its start), and the levels of the two there. The LENGTH seconds
        from SECONDS should hold that piece alone (a window running into a fade or another piece
        finds a wrong place), and a piece that repeats a phrase may be found at the repeat.

Onsets: the spectral flux of the mono mix (positive changes of the log spectrum above 150 Hz,
10 ms apart), the same for the recording and the model.
"""
import argparse
import sys
import wave

import numpy as np

import brsar
import check
import sndrender

RATE = 32000
HOP = 320    # 10 ms
FRAME = 1024


def read_wav(path):
    w = wave.open(path)
    if w.getframerate() != RATE or w.getsampwidth() != 2:
        sys.exit('%s: expected 16-bit %d Hz' % (path, RATE))
    x = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, w.getnchannels())
    return x.T.astype(np.float32) / 32768.0


def flux(audio):
    """The onset strength every 10 ms (a frame's start), from a [channels, samples] array."""
    m = audio.mean(axis=0)
    n = max(0, (len(m) - FRAME) // HOP)
    if n == 0:
        return np.zeros(0)
    idx = np.arange(FRAME)[None, :] + HOP * np.arange(n)[:, None]
    spec = np.abs(np.fft.rfft(m[idx] * np.hanning(FRAME), axis=1))
    lo = int(150 * FRAME / RATE)
    logs = np.log1p(1000 * spec[:, lo:])
    d = np.maximum(np.diff(logs, axis=0), 0).sum(axis=1)
    return np.concatenate([[0], d])


def norm_xcorr(a, b):
    """Normalised cross-correlation of a (short) at every offset into b: (best offset, value)."""
    a = (a - a.mean()) / (a.std() + 1e-9)
    n = len(a)
    if n == 0 or len(b) < n:
        return 0, 0.0
    size = 1 << (len(b) + n).bit_length()
    c = np.fft.irfft(np.fft.rfft(b, size) * np.conj(np.fft.rfft(a, size)), size)[:len(b) - n + 1]
    csum = np.concatenate([[0], np.cumsum(b)])
    csum2 = np.concatenate([[0], np.cumsum(b * b)])
    mean = (csum[n:] - csum[:-n]) / n
    std = np.sqrt(np.maximum((csum2[n:] - csum2[:-n]) / n - mean ** 2, 1e-12))
    c = c / (n * std)
    k = int(np.argmax(c))
    return k, float(c[k])


def peaks(env, rel=0.15):
    """Onset times (frames): local maxima above a share of the 99th percentile."""
    thr = rel * np.percentile(env, 99)
    return np.array([i for i in range(1, len(env) - 1) if env[i] > thr and env[i] >= env[i - 1] and
                     env[i] > env[i + 1]])


def rms_db(x):
    return 10 * np.log10(np.mean(x ** 2) + 1e-20)


def cmd_score(a):
    score, ns, mml, end, loop = check.load(a.score)
    r = brsar.Rsar(a.brsar)
    bank = r.bank(next(b['id'] for b in r.banks if b['label'] == 'BANK_BGM_ALL'))
    sim = brsar.simulate(mml, 0, loops=a.passes)
    model = sndrender.render(sim, bank, gain_db=None, volume=a.host_volume / 127.0, tail_s=1.0)
    tsec = brsar.tick_to_seconds(sim['tempo'], sim['timebase'])
    intro_s, period_s = tsec(loop), tsec(end) - tsec(loop)
    dump = read_wav(a.dump)
    fd, fm = flux(dump), flux(model)

    # where the piece starts: the model's first 30 s against the recording
    probe = fm[:3000]
    lo = 0 if a.near is None else max(0, int((a.near - 20) * 100))
    hi = len(fd) if a.near is None else min(len(fd), int((a.near + 20) * 100) + len(probe))
    k, c = norm_xcorr(probe, fd[lo:hi])
    start = (lo + k) / 100.0
    print('start: %.2f s into the recording (onsets correlate %.2f)' % (start, c))
    s0 = int(start * RATE)

    # levels and onsets by section, the passes the recording holds
    print('%-10s %8s %8s %6s %7s %7s' % ('section', 'dump dB', 'model dB', 'diff', 'onsets', 'found'))
    diffs = []
    bar = check.bar_ticks(score)
    for p in range(a.passes):
        for name, first, bars in ns['SECTIONS']:
            if name == 'intro' and p:
                continue
            b0, b1 = tsec(first * bar), tsec((first + bars) * bar)
            if name != 'intro':
                b0, b1 = b0 + p * period_s, b1 + p * period_s
            seg_d = dump[:, s0 + int(b0 * RATE):s0 + int(b1 * RATE)]
            seg_m = model[:, int(b0 * RATE):int(b1 * RATE)]
            if seg_d.shape[1] < (b1 - b0) * RATE * 0.9:
                continue
            dd, dm = rms_db(seg_d), rms_db(seg_m)
            diffs.append(dd - dm)
            # onsets: the model's (strong ones) found in the recording within 30 ms
            e_m = fm[int(b0 * 100):int(b1 * 100)]
            e_d = fd[int(start * 100) + int(b0 * 100):int(start * 100) + int(b1 * 100)]
            pm, pd = peaks(e_m, 0.25), peaks(e_d, 0.1)
            found = sum(1 for t in pm if len(pd) and np.min(np.abs(pd - t)) <= 3)
            print('%-10s %8.1f %8.1f %+6.1f %7d %6.0f%%' % ('%s%s' % (name, ' (2)' if p else ''), dd, dm, dd - dm,
                                                         len(pm), 100.0 * found / max(1, len(pm))))
    if diffs:
        print('level: the recording is %+.1f dB from the model (spread %.1f dB across sections)' % (
            np.mean(diffs), np.max(diffs) - np.min(diffs)))

    # a pass of the loop: the recording against itself one pass later
    a0 = int((start + intro_s + 5) * 100)
    ref = fd[a0:a0 + 3000]
    search0 = a0 + int((period_s - 2) * 100)
    region = fd[search0:search0 + 3000 + 400]
    if len(region) >= len(ref) + 10:
        k, c = norm_xcorr(ref, region)
        lag = (search0 + k - a0) / 100.0
        # to the sample: the waveforms around that lag
        w0 = int((start + intro_s + 5) * RATE)
        wl = dump.mean(axis=0)
        seg = wl[w0:w0 + 4 * RATE]
        c0 = w0 + int(lag * RATE) - 640
        k2, c2 = norm_xcorr(seg, wl[c0:c0 + 4 * RATE + 1280])
        lag2 = (c0 + k2 - w0) / RATE
        print('loop: a pass lasts %.3f s in the recording (onsets %.2f, waveform %.2f); the score says %.3f s' % (
            lag2, c, c2, period_s))
    else:
        print('loop: the recording ends before a second pass')


def cmd_locate(a):
    r = brsar.Rsar(a.brsar)
    sd = r.sound(a.label)
    sq = r.sequence(sd.file)
    sim = brsar.simulate(sq.mml, sd.seq_offset, alloc_mask=sd.alloc_track, loops=2)
    model = sndrender.render(sim, r.bank(sd.bank), gain_db=None, volume=sd.volume / 127.0, tail_s=0)
    dump = read_wav(a.dump)
    seg = dump[:, int(a.at * RATE):int((a.at + a.length) * RATE)]
    if seg.shape[1] < RATE:
        sys.exit('%s: less than a second of it from %.2f s (it lasts %.2f s)' % (a.dump, a.at, dump.shape[1] / RATE))
    k, c = norm_xcorr(flux(seg), flux(model))
    tsec = brsar.tick_to_seconds(sim['tempo'], sim['timebase'])
    loop_ticks = sim['loop_tick'].get(0, [(sim['ticks'], 0)])
    match = model[:, k * HOP:k * HOP + seg.shape[1]]
    print('%s: the recording at %.2f s is %.2f s into the piece (onsets correlate %.2f; a pass of its loop: '
          '%.1f s); there it is %.1f dB RMS, the model %.1f dB' % (
              a.label, a.at, k / 100.0, c, tsec(loop_ticks[0][0]) - tsec(loop_ticks[0][1]), rms_db(seg),
              rms_db(match)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--brsar', default=brsar.DEFAULT_BRSAR)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('score')
    s.add_argument('score')
    s.add_argument('dump')
    s.add_argument('--near', type=float, help='the piece starts about here (seconds into the recording)')
    s.add_argument('--passes', type=int, default=2)
    s.add_argument('--host-volume', type=int, default=125)
    s = sub.add_parser('locate')
    s.add_argument('label')
    s.add_argument('dump')
    s.add_argument('--at', type=float, required=True)
    s.add_argument('--length', type=float, default=12)
    a = ap.parse_args()
    cmd_score(a) if a.cmd == 'score' else cmd_locate(a)


if __name__ == '__main__':
    main()
