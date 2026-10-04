#!/usr/bin/env python3
"""Checks a score (tools/music/rseq.py's score files) before it goes into the port:

  harmony   every pitched note sounding on a beat (or held a beat or more) is a tone of the
            score's chord there or one of its usual tensions; notes off the beat may pass. CHORDS
            holds one entry a bar from the piece's start (bars of the score's metre, SCORE['bar']
            quarter notes, 4 if it doesn't say): a chord name ('D', 'F#m7', 'G#dim7', 'Bm7b5',
            'Dsus2', 'A7sus4', 'Cadd9', 'E6', 'Gmaj7', 'D/F#'...), a tuple of names sharing the bar
            equally (('A7sus4', 'A7'): the second from half the bar), or a tuple of (name, beats)
            pairs splitting it as given ((('A7sus4', 2), ('A7', 1)) in 3/4)
  seam      the loop: every pass plays the same notes (the second pass against the third, and the
            first pass's loop body against the second's: what the echo copies carry over must be
            there the first time too), no note sounds on past the loop's end, every track starts
            each pass with the same parameters; every echo copy plays its track's notes exactly
            its delay later, through the intro and three passes; with SEAM_BARS, the intro's last
            bars play what the loop's last bars play (so the loop is entered the same way every
            time, echoes included)
  voices    the channels sounding (nw4r::snd keeps a channel from its note-on until its release
            reaches -90.4 dB or its wave ends): the most at once and the mean, over the loop, per
            section and per track; every channel costs the port's sound mixer (the CPU renders
            each one), so a budget (--max-voices, --max-mean) fails the check
  levels    the loudness (RMS of the preview, at the level the archive's volumes give) per section,
            next to the channel's room themes and a few of its livelier pieces: the loop must lie
            within the range of those pieces (not quieter than the quietest, not louder than the
            loudest)

    check.py SCORE.py [--brsar douga.brsar] [--host-volume 125] [--wav-dir DIR]

SECTIONS in the score, (name, first bar from the piece's start, bars), name the parts the voices
and levels are given for ('intro' for the part played once). --wav-dir also writes the preview (two
passes) and a file per section. The renderer is sndrender.py's model (no AX reverb or voice
stealing): levels and voice counts are estimates of what the channel's own sound system does.
"""
import argparse
import os
import re
import sys

import numpy as np

import brsar
import rseq
import sndrender

PERCUSSION = {25, 26, 47, 95, 96, 98}   # kits, strum kits and the timpani (two waves): pitch not checked
PC = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
# A chord's quality: its tones and the tensions a held or on-beat note may add, as intervals from
# its root.
QUALITY = {
    '': ((0, 4, 7), (2, 11)),
    'm': ((0, 3, 7), (2, 5, 10)),
    '7': ((0, 4, 7, 10), (2, 9)),
    'maj7': ((0, 4, 7, 11), (2, 9, 6)),
    'm7': ((0, 3, 7, 10), (2, 5)),
    'mmaj7': ((0, 3, 7, 11), (2, 5)),
    '6': ((0, 4, 7, 9), (2,)),
    'm6': ((0, 3, 7, 9), (2, 5)),
    'add9': ((0, 2, 4, 7), (11, 9)),
    'madd9': ((0, 2, 3, 7), (5, 10)),
    '9': ((0, 2, 4, 7, 10), (9,)),
    'maj9': ((0, 2, 4, 7, 11), (9, 6)),
    'm9': ((0, 2, 3, 7, 10), (5,)),
    'sus2': ((0, 2, 7), (5, 11)),
    'sus4': ((0, 5, 7), (2, 10)),
    '7sus4': ((0, 5, 7, 10), (2, 9)),
    'dim': ((0, 3, 6), ()),
    'dim7': ((0, 3, 6, 9), (2, 5, 8, 11)),   # the diminished scale's other four
    'm7b5': ((0, 3, 6, 10), (5, 8)),
    'aug': ((0, 4, 8), (2,)),
}
_CHORD = re.compile(r'^([A-G])([#b]?)(%s)?(?:/([A-G])([#b]?))?$' % '|'.join(
    sorted((re.escape(q) for q in QUALITY if q), key=len, reverse=True)))
_ACC = {'#': 1, 'b': -1, '': 0}
# the channel's pieces to compare levels with: its room themes (first) and some livelier music
REFERENCES = ['SEQ_BGM_MAIN_01', 'SEQ_BGM_MAIN_02', 'SEQ_BGM_SAIZIKI_C', 'SEQ_BGM_SAIZIKI_H', 'SEQ_BGM_SHOP_MAIN',
              'SEQ_BGM_CONCIERGE_WAITING']
ROOM_THEMES = 2


def chord_tones(name):
    """(tones, tensions) of a chord name, as pitch classes; a slash chord's bass is a tone."""
    m = _CHORD.match(name)
    if not m:
        raise ValueError('chord %r' % name)
    root = (PC[m.group(1)] + _ACC[m.group(2)]) % 12
    tones, tensions = QUALITY[m.group(3) or '']
    out = set((root + i) % 12 for i in tones)
    if m.group(4):
        out.add((PC[m.group(4)] + _ACC[m.group(5)]) % 12)
    return out, set((root + i) % 12 for i in tensions)


def chord_at(entry, pos, bar, tb):
    """The chord name of a CHORDS entry `pos` ticks into its bar."""
    if not isinstance(entry, tuple):
        return entry
    if isinstance(entry[0], tuple):          # ((name, beats), ...)
        at = 0
        for name, beats in entry:
            at += beats * tb
            if pos < at:
                return name
        return entry[-1][0]
    return entry[min(len(entry) - 1, pos * len(entry) // bar)]   # names sharing the bar equally


def bar_ticks(score):
    """Ticks a bar: the score's 'bar' (quarter notes a bar, 4 if it doesn't say) times its timebase."""
    return int((score.get('bar') or 4) * score.get('timebase', 96))


def load(path):
    score = rseq.load_score(path)
    ns = {'__file__': path}
    exec(compile(open(path, encoding='utf-8').read(), path, 'exec'), ns)
    text, end, loop = rseq.compile_score(score)
    mml, labels = rseq.assemble(text)
    return score, ns, mml, end, loop


def sounding_key(n):
    return n['key'] + rseq.SOUNDING_OFFSET.get(n['program'], 0)


def check_harmony(score, ns, mml, end):
    chords = ns.get('CHORDS')
    if not chords:
        print('harmony: the score has no CHORDS')
        return 0
    tb = score.get('timebase', 96)
    bar = bar_ticks(score)
    sim = brsar.simulate(mml, 0, loops=1)
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
        ch = chord_at(chords[b], pos, bar, tb)
        tones, tensions = chord_tones(ch)
        pc = sounding_key(n) % 12
        if pc in tones or pc in tensions:
            continue
        on_beat = pos % tb == 0
        if not on_beat and n['length'] < tb:
            continue
        bad += 1
        print('  bar %2d beat %.2f %-18s %-4s over %-8s (%s)' % (
            b + 1, pos / tb + 1, names.get(n['track'], n['track']), brsar.note_name(sounding_key(n)), ch,
            'on the beat' if on_beat else 'held'))
    print('harmony: %d chords for %d bars of %d quarter notes, %d notes off their chord' % (
        len(chords), -(-end // bar), bar // tb, bad))
    return bad


def note_sig(n, shift=0):
    return (n['tick'] - shift, n['track'], n['key'], n['velocity'], n['length'], n['program'], n['volume'],
            n['pan'], tuple(n['adsr']))


def echo_tracks(score):
    """[(echo track, its track, its delay)]: compile_score numbers the copies after the score's
    tracks, in the order of their tracks and copies."""
    out, no = [], len(score['tracks'])
    for i, t in enumerate(score['tracks']):
        e = t.get('echo')
        for k in range(len(e['copies']) if e else 0):
            out.append((no, i, e['delay'] * (k + 1)))
            no += 1
    return out


def check_seam(score, ns, mml, end, loop):
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

    # the echo copies: their track's notes (key, velocity, length, program, envelope) `delay` later
    horizon = loop + 3 * period
    for no, src, delay in echo_tracks(score):
        def sig(track, shift):
            return sorted((n['tick'] + shift, n['key'], n['velocity'], n['length'], n['program'], tuple(n['adsr']))
                          for n in sim['notes'] if n['track'] == track and n['tick'] + shift < horizon)
        want, got = sig(src, delay), sig(no, 0)
        if want != got:
            problems += 1
            sw, sg = set(want), set(got)
            print('echo: track %d is not track %d %d ticks later: %d notes missing, %d extra, e.g. %s' % (
                no, src, delay, len(sw - sg), len(sg - sw), sorted(sw ^ sg)[:2]))
        else:
            print('echo: track %d plays track %d %d ticks later (%d notes: the intro and 3 passes)' % (
                no, src, delay, len(got)))

    # the intro's last bars against the loop's last bars, every track (the echoes heard at the
    # loop's start are then the same on every pass)
    seam_bars = ns.get('SEAM_BARS')
    if seam_bars:
        w = seam_bars * bar_ticks(score)

        def window(t0):
            return sorted((n['tick'] - t0, n['track'], n['key'], n['velocity'], n['length'], n['program'],
                           n['volume'], n['pan'], tuple(n['adsr'])) for n in sim['notes'] if t0 <= n['tick'] < t0 + w)
        w1, w2 = window(loop - w), window(end - w)
        if w1 != w2:
            problems += 1
            print('seam: the intro\'s last %d bars and the loop\'s last %d differ (%d notes against %d)' % (
                seam_bars, seam_bars, len(w1), len(w2)))
        else:
            print('seam: the intro\'s last %d bars play the loop\'s last %d (%d notes)' % (
                seam_bars, seam_bars, len(w1)))
    return problems


def section_spans(score, ns, tsec):
    """[(name, start s, end s)] from the score's SECTIONS (first pass)."""
    bar = bar_ticks(score)
    return [(name, tsec(first * bar), tsec((first + bars) * bar)) for name, first, bars in ns.get('SECTIONS', [])]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('score')
    ap.add_argument('--brsar', default=brsar.DEFAULT_BRSAR)
    ap.add_argument('--host-volume', type=int, default=125,
                    help="the archive volume of the sound the piece plays as (SEQ_BGM_SAIZIKI_D: 125)")
    # The budget: no more than the room's own themes cost the mixer, which the island's music
    # replaces. In this model SEQ_BGM_MAIN_01 peaks at 35 channels and MAIN_02 at 32, 15.7 and 16.7
    # on average while sounding; a peak is a moment (10 ms) of overlapping releases, the mean is
    # what the mixer spends.
    ap.add_argument('--max-voices', type=int, default=35)
    ap.add_argument('--max-mean', type=float, default=18.0)
    ap.add_argument('--wav-dir', help='write the preview (2 passes) and a file per section here')
    ap.add_argument('--no-references', action='store_true', help="don't render the channel's pieces")
    a = ap.parse_args()

    score, ns, mml, end, loop = load(a.score)
    tb = score.get('timebase', 96)
    tempo = score.get('tempo', 120)
    bar = bar_ticks(score)
    period_s = (end - loop) * 60.0 / (tempo * tb)
    print('%s: %d tracks, %d/4 at %d bpm, %d ticks (loop %d..%d: %g bars, %.2f s a pass; intro %g bars, %.2f s), '
          '%d bytes of MML' % (score.get('title', '?'), len(score['tracks']), bar // tb, tempo, end, loop, end,
                               (end - loop) / bar, period_s, loop / bar, loop * 60.0 / (tempo * tb), len(mml)))
    failures = 0
    failures += check_harmony(score, ns, mml, end) > 0
    failures += check_seam(score, ns, mml, end, loop) > 0

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
    spans = section_spans(score, ns, tsec)
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
    print('levels: the loop %.1f dB RMS, peak %.2f (at host volume %d)' % (
        loop_rms, float(np.abs(loop_audio).max()), a.host_volume))
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
        refs = []
        for label in REFERENCES:
            sd = r.sound(label)
            sq = r.sequence(sd.file)
            ref = brsar.simulate(sq.mml, sd.seq_offset, alloc_mask=sd.alloc_track)
            s3 = {}
            x = sndrender.render(ref, r.bank(sd.bank), gain_db=None, volume=sd.volume / 127.0, stats=s3, tail_s=0)
            refs.append(10 * np.log10(np.mean(x ** 2) + 1e-20))
            print('  %-26s %6.1f dB RMS, up to %2d voices, %.1f on average while sounding' % (
                label, refs[-1], s3['voices'], s3['mean_voices']))
        lo, hi = min(refs), max(refs)
        inside = lo <= loop_rms <= hi
        room = float(np.mean(refs[:ROOM_THEMES]))
        print('levels: the loop at %.1f dB is %s the range of those pieces (%.1f .. %.1f dB), %+.1f dB from the '
              'room themes' % (loop_rms, 'within' if inside else 'OUTSIDE', lo, hi, loop_rms - room))
        failures += not inside
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
