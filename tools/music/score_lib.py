# Helpers for writing a score for tools/music/rseq.py: a note-text parser, per-track event lists
# placed by bar, chord voicing (motor figures, guide-tone lines, pads, basses) and serialisation
# to rseq.py's MML (explicit octaves and tick lengths).
import itertools
import re
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check  # noqa: E402  (chord vocabulary shared with the checker)

TB = 96
BEATS = 4
BAR = BEATS * TB

_PC = {'c': 0, 'd': 2, 'e': 4, 'f': 5, 'g': 7, 'a': 9, 'b': 11}
_NOTE = re.compile(r'^([a-g])(#|b)?(-?\d)$')
_MML = ['c', 'c+', 'd', 'd+', 'e', 'f', 'f+', 'g', 'g+', 'a', 'a+', 'b']


def key(n):
    m = _NOTE.match(n)
    if not m:
        raise ValueError('bad note %r' % n)
    return _PC[m.group(1)] + {'#': 1, 'b': -1, None: 0}[m.group(2)] + 12 * (int(m.group(3)) + 1)


def ticks(s):
    """'8' 48, '4.' 144, '8t' 32, '16' 24, '8+4' a tie: 144."""
    total = 0
    for part in s.split('+'):
        m = re.match(r'^(\d+)(t?)(\.*)$', part)
        if not m:
            raise ValueError('bad length %r' % s)
        t = 4 * TB // int(m.group(1))
        if m.group(2):
            t = t * 2 // 3
        add = t
        for _ in m.group(3):
            add //= 2
            t += add
        total += t
    return total


def parse(text, vel=96, gate=8):
    """'a4/8 [d4 f#4]/2 r8 v90 q6 b4/4.' -> ([(tick, keys, length, vel, gate)], total ticks)."""
    evs, t = [], 0
    for tok in re.findall(r'\[[^\]]*\]/\S+|\S+', text):
        if re.match(r'^v\d+$', tok):
            vel = int(tok[1:])
        elif re.match(r'^q\d+$', tok):
            gate = int(tok[1:])
        elif tok[0] == 'r':
            t += ticks(tok[1:].lstrip('/'))
        else:
            if tok[0] == '[':
                notes, ln = tok[1:].split(']/')
                ks = [key(n) for n in notes.split()]
            else:
                n, ln = tok.split('/', 1)
                ks = [key(n)]
            ln = ticks(ln)
            evs.append((t, ks, ln, vel, gate))
            t += ln
    return evs, t


class Track:
    def __init__(self, name):
        self.name = name
        self.events = []   # (tick, keys, length, vel, gate)
        self.cmds = []     # (tick, text)

    def put(self, bar, text, vel=96, gate=8, beat=0.0):
        evs, total = parse(text, vel, gate)
        base = int(round(bar * BAR + beat * TB))
        for t, ks, ln, v, g in evs:
            self.events.append((base + t, ks, ln, v, g))
        return total

    def bars(self, bar, texts, vel=96, gate=8):
        """One text a bar from `bar`; each must fill its bar (or be empty / 'r1')."""
        for i, tx in enumerate(texts):
            if tx is None:
                continue
            total = self.put(bar + i, tx, vel, gate)
            if total != BAR:
                raise ValueError('%s bar %d: %d ticks, not %d: %r' % (self.name, bar + i, total, BAR, tx))

    def note(self, tick, keys, length, vel, gate=8):
        self.events.append((int(tick), list(keys) if isinstance(keys, (list, tuple)) else [keys], int(length),
                            int(vel), gate))

    def cmd(self, tick, text):
        self.cmds.append((int(tick), text))

    def mml(self, loop_tick, end_tick):
        evs = sorted(self.events, key=lambda e: e[0])
        for a, b in zip(evs, evs[1:]):
            if b[0] < a[0] + a[2]:
                raise ValueError('%s: overlap at tick %d (bar %d beat %.2f)' % (
                    self.name, b[0], b[0] // BAR, (b[0] % BAR) / TB + 1))
        marks = sorted(set([t for t, _ in self.cmds] + [loop_tick]))
        out, cur, vel, gate = [], 0, None, None
        cmds = sorted(self.cmds)
        ci = 0

        def rest_to(t):
            nonlocal cur
            while cur < t:
                nxt = min([m for m in marks if m > cur] + [t])
                out.append('r:%d' % (nxt - cur))
                cur = nxt
                flush_marks()

        def flush_marks():
            nonlocal ci
            if cur == loop_tick and '$' not in out:
                out.append('$')
            while ci < len(cmds) and cmds[ci][0] == cur:
                out.append('{%s}' % cmds[ci][1])
                ci += 1

        flush_marks()
        for t, ks, ln, v, g in evs:
            rest_to(t)
            if t < loop_tick < t + ln:
                raise ValueError('%s: a note at tick %d spans the loop point' % (self.name, t))
            if ci < len(cmds) and cmds[ci][0] < t + ln and cmds[ci][0] > t:
                raise ValueError('%s: a command at %d inside a note at %d' % (self.name, cmds[ci][0], t))
            if v != vel:
                out.append('v%d' % v)
                vel = v
            if g != gate:
                out.append('q%d' % g)
                gate = g
            names = ['o%d%s' % (k // 12 - 1, _MML[k % 12]) for k in ks]
            if len(names) == 1:
                out.append('%s:%d' % (names[0], ln))
            else:
                out.append('[%s]:%d' % (' '.join(names), ln))
            cur = t + ln
            flush_marks()
        rest_to(end_tick)
        if loop_tick == end_tick and '$' not in out:
            out.append('$')
        return ' '.join(out)


# ---------------------------------------------------------------------------- chords


def chord_info(name):
    """(root pc, [chord-tone pcs in order], bass pc)."""
    m = check._CHORD.match(name)
    if not m:
        raise ValueError('chord %r' % name)
    root = (check.PC[m.group(1)] + check._ACC[m.group(2)]) % 12
    ivs = check.QUALITY[m.group(3) or ''][0]
    pcs = [(root + i) % 12 for i in ivs]
    bass = (check.PC[m.group(4)] + check._ACC[m.group(5)]) % 12 if m.group(4) else root
    return root, pcs, bass


def segs(spec):
    """A bar's chord spec ('D' or (('A7sus4', 2), ('A7', 2))) -> [(beat, beats, name)]."""
    if isinstance(spec, str):
        return [(0, BEATS, spec)]
    out, at = [], 0
    for name, beats in spec:
        out.append((at, beats, name))
        at += beats
    return out


def tones_between(name, lo, hi, with_bass=False):
    root, pcs, bass = chord_info(name)
    want = set(pcs) | ({bass} if with_bass else set())
    return [k for k in range(lo, hi + 1) if k % 12 in want]


def triad_voicing(name, lo, n=3):
    """The first n chord tones (distinct pitch classes) from `lo` up; a 7th chord drops its 5th
    when n < 4 (sus chords keep theirs)."""
    root, pcs, bass = chord_info(name)
    use = list(pcs)
    if len(use) > n and len(use) >= 4:
        fifth = (root + 7) % 12
        if fifth in use and '7sus4' not in name:
            use.remove(fifth)
    out, seen = [], set()
    k = lo
    while len(out) < n:
        if k % 12 in use and (k % 12 not in seen or len(seen) == len(use)):
            out.append(k)
            seen.add(k % 12)
        k += 1
    return out


def bass_pair(name, low=None):
    """The bass note of a chord as an octave pair (E..B in octave 1, C..D# in octave 2)."""
    root, pcs, bass = chord_info(name)
    pc = bass if low is None else low
    o = 1 if pc >= 4 else 2
    k = 12 * (o + 1) + pc
    return [k, k + 12]


def fifth_pair(name):
    root, pcs, bass = chord_info(name)
    fifth = [p for p in pcs if (p - root) % 12 in (6, 7, 8)]
    pc = fifth[0] if fifth else root
    b = bass_pair(name)[0]
    k = b + ((pc - b) % 12)
    if k - b > 7:
        k -= 12
    return [k, k + 12]


def guide_line(chords, lo, hi, start, prefer=(3, 4, 10, 11, 5)):
    """A smooth line of long notes through a list of (tick, length, chord): each chord's 3rd/7th
    (or 4th of a sus) nearest the previous note, held for the chord; a repeat is tied on."""
    out, prev = [], start
    for t, ln, name in chords:
        root, pcs, bass = chord_info(name)
        best = None
        for k in range(lo, hi + 1):
            if k % 12 not in pcs:
                continue
            iv = (k - root) % 12
            cost = abs(k - prev) + (0 if iv in prefer else 2.5)
            if best is None or cost < best[0]:
                best = (cost, k)
        k = best[1]
        if out and out[-1][2] == k and out[-1][0] + out[-1][1] == t:
            out[-1] = (out[-1][0], out[-1][1] + ln, k)
        else:
            out.append((t, ln, k))
        prev = k
    return out


def pad_voicing(name, lo, hi, n, prev=None):
    """n chord tones (distinct pcs, adjacent gaps <= 9) in [lo, hi] moving least from prev."""
    root, pcs, bass = chord_info(name)
    cands = [k for k in range(lo, hi + 1) if k % 12 in pcs]
    best = None
    for combo in itertools.combinations(cands, n):
        if len(set(k % 12 for k in combo)) < n:
            continue
        if any(b - a > 9 for a, b in zip(combo, combo[1:])):
            continue
        # keep the 3rd (or the sus 4th) in
        thirds = [p for p in pcs if (p - root) % 12 in (3, 4, 5)]
        if thirds and not any(k % 12 in thirds for k in combo):
            continue
        cost = sum(abs(a - b) for a, b in zip(combo, prev)) if prev else sum(combo) / 50.0
        if best is None or cost < best[0]:
            best = (cost, list(combo))
    return best[1]


def chord_spans(chords, first_bar, bar_from, bar_to):
    """[(tick, length, name)] for piece bars bar_from..bar_to-1 of a CHORDS list."""
    out = []
    for b in range(bar_from, bar_to):
        for beat, beats, name in segs(chords[b]):
            out.append((b * BAR + beat * TB, beats * TB, name))
    return out
