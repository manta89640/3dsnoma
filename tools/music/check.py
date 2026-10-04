#!/usr/bin/env python3
"""Checks a score (tools/music/rseq.py's score files) before it goes into the port:

  harmony   every pitched note sounding on a beat (or held a beat or more) is a tone of the
            score's chord there (CHORDS: one a bar, or a pair changing on beat 3) or one of its
            usual tensions; notes off the beat may pass
  seam      the loop: every pass plays the same notes (the second pass against the third, and the
            first pass's loop body against the second's: what the echo copies carry over must be
            there the first time too), no note sounds on past the loop's end, every track starts
            each pass with the same parameters
  voices    the channels sounding (nw4r::snd keeps a channel from its note-on until its release
            reaches -90.4 dB or its wave ends): the most at once and the mean, over the loop, per
            section and per track; every channel costs the 3DS's sound mixer, so a budget
            (--max-voices, --max-mean) fails the check
  levels    the loudness (RMS of the preview, at the level the archive's volumes give) per section,
            next to the channel's room themes and a few of its livelier pieces

    check.py SCORE.py [--brsar douga.brsar] [--host-volume 125] [--wav-dir DIR]

--wav-dir also writes the preview (two passes) and a file per section. The renderer is
sndrender.py's model (no AX reverb or voice stealing): levels and voice counts are estimates of
what the channel's own sound system does.
"""
import argparse
import os
import re
import sys

import numpy as np

import brsar
import rseq
import sndrender

PERCUSSION = {25, 26, 47, 95, 96, 98}   # kits and strum kits: no pitch to check
PC = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
QUALITY = {'': (0, 4, 7), 'm': (0, 3, 7), '7': (0, 4, 7, 10), 'maj7': (0, 4, 7, 11), 'm7': (0, 3, 7, 10),
           'add9': (0, 2, 4, 7), '7sus4': (0, 5, 7, 10), 'sus4': (0, 5, 7)}
TENSIONS = {'': (2, 11), 'm': (2, 5, 10), '7': (2, 9), 'maj7': (2, 9, 6), 'm7': (2, 5), 'add9': (11, 9),
            '7sus4': (2, 9), 'sus4': (2, 10)}
# the channel's pieces to compare levels with: its room themes and some livelier music
REFERENCES = ['SEQ_BGM_MAIN_01', 'SEQ_BGM_MAIN_02', 'SEQ_BGM_SAIZIKI_C', 'SEQ_BGM_SAIZIKI_H', 'SEQ_BGM_SHOP_MAIN',
              'SEQ_BGM_CONCIERGE_WAITING']


def chord_tones(name):
    m = re.match(r'^([A-G])([#b]?)(maj7|m7|7sus4|sus4|add9|m|7)?(?:/([A-G])([#b]?))?$', name)
    if not m:
        raise ValueError('chord %r' % name)
    acc = {'#': 1, 'b': -1, '': 0}
    root = (PC[m.group(1)] + acc[m.group(2)]) % 12
    q = m.group(3) or ''
    tones = set((root + i) % 12 for i in QUALITY[q])
    if m.group(4):
        tones.add((PC[m.group(4)] + acc[m.group(5)]) % 12)
    return tones, set((root + i) % 12 for i in TENSIONS[q])


def load(path):
    score = rseq.load_score(path)
    ns = {'__file__': path}
    exec(compile(open(path, encoding='utf-8').read(), path, 'exec'), ns)
    text, end, loop = rseq.compile_score(score)
    mml, labels = rseq.assemble(text)
    return score, ns, mml, end, loop


def sounding_key(n):
    return n['key'] + rseq.SOUNDING_OFFSET.get(n['program'], 0)


def check_harmony(score, ns, mml, tb):
    chords = ns.get('CHORDS')
    if not chords:
        print('harmony: the score has no CHORDS')
        return 0
    sim = brsar.simulate(mml, 0, loops=1)
    bar = 4 * tb
    names = {i: t.get('name', str(i)) for i, t in enumerate(score['tracks'])}
    bad = 0
    for n in sim['notes']:
        # (the echo copies, numbered after the score's tracks, repeat what was checked)
        if n['program'] in PERCUSSION or n['track'] >= len(score['tracks']):
            continue
        b = n['tick'] // bar
        if b >= len(chords):
            continue
        pos = n['tick'] % bar
        ch = chords[b]
        if isinstance(ch, tuple):
            ch = ch[0] if pos < bar // 2 else ch[1]
        tones, tensions = chord_tones(ch)
        pc = sounding_key(n) % 12
        if pc in tones or pc in tensions:
            continue
        on_beat = pos % tb == 0
        if not on_beat and n['length'] < tb:
            continue
        bad += 1
        print('  bar %2d beat %.2f %-14s %-4s over %-7s (%s)' % (
            b + 1, pos / tb + 1, names.get(n['track'], n['track']), brsar.note_name(sounding_key(n)), ch,
            'on the beat' if on_beat else 'held'))
    print('harmony: %d notes off their chord' % bad)
    return bad


def note_sig(n, shift=0):
    return (n['tick'] - shift, n['track'], n['key'], n['velocity'], n['length'], n['program'], n['volume'],
            n['pan'], tuple(n['adsr']))


def check_seam(mml, end, loop):
    period = end - loop
    sim = brsar.simulate(mml, 0, loops=4, max_ticks=loop + 4 * period + 1000)
    problems = 0
    # each track's own pass: its notes from its own loop point (echo copies loop later)
    track_loop = {}
    for no, jumps in sim['loop_tick'].items():
        if jumps:
            track_loop[no] = jumps[0][0] - period   # the tick its first pass's loop body started
    passes = {}
    for k in range(3):
        sig = []
        for n in sim['notes']:
            start = track_loop.get(n['track'], loop) + k * period
            if start <= n['tick'] < start + period:
                sig.append(note_sig(n, k * period))
        passes[k] = sorted(sig)
    for a, b in ((0, 1), (1, 2)):
        if passes[a] != passes[b]:
            problems += 1
            sa, sb = set(passes[a]), set(passes[b])
            print('seam: pass %d and pass %d differ: %d notes only in pass %d, %d only in pass %d' % (
                a + 1, b + 1, len(sa - sb), a + 1, len(sb - sa), b + 1))
            for x in sorted(sa ^ sb)[:6]:
                print('   ', 'pass %d' % (a + 1) if x in sa else 'pass %d' % (b + 1), x)
    over = [n for n in sim['notes'] if n['tick'] < track_loop.get(n['track'], loop) + period and
            n['tick'] + n['length'] > track_loop.get(n['track'], loop) + period]
    if over:
        problems += 1
        print('seam: %d notes sound on past the loop\'s end, e.g. %s' % (len(over), over[0]))
    print('seam: %d passes compared (%d notes a pass), %d problems' % (3, len(passes[1]), problems))
    return problems


def section_spans(ns, end, loop, tb, tsec):
    """[(name, start s, end s)] from the score's SECTIONS (first pass)."""
    out = []
    for name, first, bars in ns.get('SECTIONS', []):
        out.append((name, tsec(first * 4 * tb), tsec((first + bars) * 4 * tb)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('score')
    ap.add_argument('--brsar', default=brsar.DEFAULT_BRSAR)
    ap.add_argument('--host-volume', type=int, default=125,
                    help="the archive volume of the sound the piece plays as (SEQ_BGM_SAIZIKI_D: 125)")
    ap.add_argument('--max-voices', type=int, default=32)
    ap.add_argument('--max-mean', type=float, default=18.0)
    ap.add_argument('--wav-dir', help='write the preview (2 passes) and a file per section here')
    ap.add_argument('--no-references', action='store_true', help="don't render the channel's pieces")
    a = ap.parse_args()

    score, ns, mml, end, loop = load(a.score)
    tb = score.get('timebase', 96)
    tempo = score.get('tempo', 120)
    period_s = (end - loop) * 60.0 / (tempo * tb)
    print('%s: %d tracks, %d ticks (loop %d..%d: %.1f s a pass, intro %.1f s), %d bytes of MML' % (
        score.get('title', '?'), len(score['tracks']), end, loop, end, period_s, loop * 60.0 / (tempo * tb), len(mml)))
    failures = 0
    failures += check_harmony(score, ns, mml, tb) > 0
    failures += check_seam(mml, end, loop) > 0

    r = brsar.Rsar(a.brsar)
    bank = r.bank(next(b['id'] for b in r.banks if b['label'] == 'BANK_BGM_ALL'))
    volume = a.host_volume / 127.0
    # voices and levels over two passes (the second pass is what a player hears from then on)
    sim = brsar.simulate(mml, 0, loops=2)
    st = {}
    audio = sndrender.render(sim, bank, gain_db=None, volume=volume, stats=st)
    tsec = brsar.tick_to_seconds(sim['tempo'], sim['timebase'])
    active = st['active']
    t0, t1 = tsec(loop + (end - loop)), tsec(loop + 2 * (end - loop))   # the second pass
    body = active[int(t0 * 100):int(t1 * 100)]
    print('voices: the loop (second pass): up to %d at once, %.1f on average (budget %d / %.1f)' % (
        body.max(), body.mean(), a.max_voices, a.max_mean))
    failures += body.max() > a.max_voices or body.mean() > a.max_mean
    spans = section_spans(ns, end, loop, tb, tsec)
    rate = 32000
    print('%-6s %6s %6s  %5s %5s  %s' % ('', 'from s', 'to s', 'peak', 'mean', 'rms dB'))
    for name, s0, s1 in spans:
        if name != 'intro':   # the loop body: measure the second pass
            s0, s1 = s0 + period_s, s1 + period_s
        v = active[int(s0 * 100):int(s1 * 100)]
        seg = audio[:, int(s0 * rate):int(s1 * rate)]
        print('%-6s %6.1f %6.1f  %5d %5.1f  %6.1f' % (name, s0, s1, v.max(), v.mean(),
                                                    10 * np.log10(np.mean(seg ** 2) + 1e-20)))
    loop_audio = audio[:, int(t0 * rate):int(t1 * rate)]
    loop_rms = 10 * np.log10(np.mean(loop_audio ** 2) + 1e-20)
    print('levels: the loop %.1f dB RMS (at host volume %d)' % (loop_rms, a.host_volume))
    names = {i: t.get('name', str(i)) for i, t in enumerate(score['tracks'])}
    for no in sorted(set(n['track'] for n in sim['notes'])):
        sub = dict(sim)
        sub['notes'] = [n for n in sim['notes'] if n['track'] == no]
        s2 = {}
        sndrender.render(sub, bank, gain_db=None, volume=volume, stats=s2)
        v = s2['active'][int(t0 * 100):int(t1 * 100)]
        sounding = v[v > 0]
        print('  track %2d %-22s prg %2d: up to %2d voices, %.1f on average (%.1f while sounding)' % (
            no, names.get(no, 'echo %d' % (no - len(score['tracks']) + 1)), sub['notes'][0]['program'], v.max(),
            v.mean(), sounding.mean() if len(sounding) else 0))
    if not a.no_references:
        for label in REFERENCES:
            sd = r.sound(label)
            sq = r.sequence(sd.file)
            ref = brsar.simulate(sq.mml, sd.seq_offset, alloc_mask=sd.alloc_track)
            s3 = {}
            x = sndrender.render(ref, r.bank(sd.bank), gain_db=None, volume=sd.volume / 127.0, stats=s3, tail_s=0)
            print('  %-26s %6.1f dB RMS, up to %2d voices, %.1f on average while sounding' % (
                label, 10 * np.log10(np.mean(x ** 2) + 1e-20), s3['voices'], s3['mean_voices']))
    if a.wav_dir:
        os.makedirs(a.wav_dir, exist_ok=True)
        peak = float(np.abs(audio).max())
        gain = 10 ** (-1.0 / 20) / peak   # one gain for every file: their levels compare
        brsar.write_wav(os.path.join(a.wav_dir, 'hiroi_sekai_preview.wav'), audio[:, :int((t1 + 3) * rate)] * gain,
                        rate)
        for k, (name, s0, s1) in enumerate(spans):
            if name != 'intro':
                s0, s1 = s0 + period_s, s1 + period_s
            fn = 'hiroi_sekai_%d_%s.wav' % (k, name.replace("'", 'p'))
            brsar.write_wav(os.path.join(a.wav_dir, fn), audio[:, int(s0 * rate):int((s1 + 1.5) * rate)] * gain,
                            rate)
        print('wav: the preview and %d sections -> %s' % (len(spans), a.wav_dir))
    print('OK' if not failures else 'FAILED (%d)' % failures)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
