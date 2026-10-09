# 「広い世界へ」 (Hiroi Sekai e, "Into the Wide World"), recomposed: the departure cue for setting
# out across Wuhu Island. An original piece in D major, 4/4 at 132 bpm, made only of Wii no Ma's own
# instruments (BANK_BGM_ALL). A score for tools/music/rseq.py (and check.py):
#
#   python3 tools/music/rseq.py compile tools/music/hiroi_sekai.py -o hiroi_sekai.rseq --wav preview.wav
#   python3 tools/music/rseq.py header tools/music/hiroi_sekai.py -o engine/wuhu/hiroi_sekai_seq.h
#   python3 tools/music/check.py tools/music/hiroi_sekai.py        (harmony, loop seam, voices, levels)
#
# Form (bars of 4/4; the intro plays once, then the loop from A to the end, again and again):
#   I   (4)  fanfare: a brass motto (rising call) on D, answered far off, then raised on Bb and C
#   L   (4)  the launch: an A7sus4/A7 dominant whose figuration doubles each bar (quarters, eighths,
#            triplets, sixteenths) over a snare-roll crescendo, falling into the theme
#   --- loop start ---
#   A   (16) the departure theme, violins over a viola eighth-note motor; the second half adds flute,
#            a trombone counter-line, glockenspiel and the Bb - C lift to a high F#
#   B   (8)  the castle: B minor / Dorian, horn and trombone in fourths, the motor in triplets,
#            a military snare, timpani B - F# and a warm pad pedal
#   T   (8)  on the road: a second tune in G, off-beat marimba, sixteenth hats; C - Bb - A7 darkens
#   C   (8)  the volcano: D minor with a Phrygian Eb, low string ostinato, viola tremolo, stern low
#            trombone motif, toms, timpani; A7 falls deceptively onto Bb
#   D   (8)  the ancient ruins: Bb Lydian, a recorder with the channel's echo trick, marimba
#            arpeggios as the motor, harp, airy pad, pizzicato basses, shaker-soft hats
#   E   (8)  the climb: the theme's head one step higher each bar over a chromatic bass
#            C - C#dim7 - Dm - D#dim7 - Em7 - F - G - A7, the groove adding a layer every two bars
#   A3  (8)  the view: the theme's second half in full, the brass motto in its last bar
#   L   (4)  the launch again (the intro's last four bars, note for note) -> A
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from score_lib import (BAR, TB, Track, bass_pair, chord_info, chord_spans, fifth_pair, guide_line,  # noqa: E402
                       key, pad_voicing, segs, triad_voicing)

TEMPO = 132

# ---------------------------------------------------------------------------- the chord chart
INTRO_CH = ['D', 'D', 'Bb', 'C']
LAUNCH_CH = ['A7sus4', 'A7', 'A7sus4', 'A7']
A_CH = ['D', 'Bm', 'G', (('A7sus4', 2), ('A7', 2)), 'D/F#', (('G', 2), ('Gm', 2)), (('D/A', 2), ('B7', 2)),
        (('Em7', 2), ('A7', 2)),
        'D', 'F#m7', 'G', 'E7/G#', 'Bb', 'C', 'D', (('F#7sus4', 2), ('F#7', 2))]
B_CH = ['Bm', 'A/B', 'Bm', 'E/B', 'G', 'A', (('Em7', 2), ('F#m7', 2)), (('D7sus4', 2), ('D7', 2))]
T_CH = ['G', (('C', 2), ('D', 2)), 'Em7', (('Am7', 2), ('D7', 2)), 'G/B', 'C', (('C', 2), ('Bb', 2)),
        (('A7sus4', 2), ('A7', 2))]
C_CH = ['Dm', 'Eb/D', 'Dm', 'Eb/D', 'Bb', 'Gm', 'E7', 'A7']
D_CH = ['Bb', 'C/Bb', 'Bb', 'C/Bb', 'Gm7', 'Am7', 'Bbmaj7', (('Dm7', 2), ('G7', 2))]
E_CH = ['C', 'C#dim7', 'Dm', 'D#dim7', 'Em7', 'F', 'G', (('A7sus4', 2), ('A7', 2))]
A3_CH = ['D', 'F#m7', 'G', 'E7/G#', 'Bb', 'C', 'D', 'D']

CHORDS = INTRO_CH + LAUNCH_CH + A_CH + B_CH + T_CH + C_CH + D_CH + E_CH + A3_CH + LAUNCH_CH

# piece bars where each part starts
I0, L0, A0, B0, T0, C0, D0, E0, A30, L1 = 0, 4, 8, 24, 32, 40, 48, 56, 64, 72
END_BAR = 76
LOOP_BAR = A0
INTRO_BARS = LOOP_BAR
SEAM_BARS = 4
SECTIONS = [('intro', 0, INTRO_BARS), ('A', A0, 8), ('A2', A0 + 8, 8), ('castle', B0, 8), ('road', T0, 8),
            ('volcano', C0, 8), ('ruins', D0, 8), ('climb', E0, 8), ('view', A30, 8), ('launch', L1, 4)]

T = {n: Track(n) for n in ['violins', 'flute', 'trombone', 'horn', 'violas', 'basses', 'harp', 'marimba',
                           'glock', 'timpani', 'drums', 'hats', 'orch', 'pad', 'recorder']}


def spans(b0, b1):
    return chord_spans(CHORDS, 0, b0, b1)


# ---------------------------------------------------------------------------- pattern makers
def motor(track, b0, b1, floor, pattern, step, vels, gate=6, n=3, floor_of=None):
    """Broken chords: `pattern` indexes a chord voicing (n tones from `floor`), one note every
    `step` ticks, velocities cycling through `vels`."""
    for t, ln, name in spans(b0, b1):
        v = triad_voicing(name, floor_of(name) if floor_of else floor, n)
        i = 0
        for at in range(t, t + ln, step):
            track.note(at, v[pattern[i % len(pattern)]], step, vels[i % len(vels)], gate)
            i += 1


def bass_line(b0, b1, kind, vel, gate=7, octave_of=None):
    """Octave pairs on the cellos and basses: 'gallop' (R. R | R R), 'drive' (eighths, the fifth
    on beat 3), 'march' (quarters, the fifth on beat 3), 'pizz' (short quarters), 'whole'."""
    tr = T['basses']
    for t, ln, name in spans(b0, b1):
        r = octave_of(name) if octave_of else bass_pair(name)
        root, pcs, bass = chord_info(name)
        f = r if '/' in name else fifth_pair(name)
        if octave_of and '/' not in name:
            f = [r[0] + 7, r[1] + 7]
        if kind == 'gallop':
            lo, flo = r[:1], f[:1]
            if ln == 4 * TB:
                seq = [(r, 144), (lo, 48), (f, 96), (lo, 96)]
            else:
                seq = [(r, 144), (lo, 48)] if t % BAR == 0 else [(r, 96), (lo, 96)]
        elif kind == 'drive':
            lo, flo = r[:1], f[:1]
            seq = ([(r, 48), (lo, 48), (lo, 48), (lo, 48), (f, 48), (flo, 48), (lo, 48), (lo, 48)]
                   if ln == 4 * TB else [(r, 48), (lo, 48), (lo, 48), (lo, 48)])
        elif kind == 'march':
            seq = [(r, 96), (r, 96), (f, 96), (r, 96)] if ln == 4 * TB else [(r, 96), (r, 96)]
        elif kind == 'pizz':
            seq = [(r, 96)] * (ln // TB)
        elif kind == 'whole':
            seq = [(r, ln)]
        at = t
        for i, (ks, d) in enumerate(seq):
            accent = vel + (8 if i == 0 else 0)
            tr.note(at, ks, d, min(127, accent), gate)
            at += d


def pads(track, b0, b1, lo, hi, n, vel, gate=8, start=None):
    prev = start
    for t, ln, name in spans(b0, b1):
        v = pad_voicing(name, lo, hi, n, prev)
        track.note(t, v, ln, vel, gate)
        prev = v


def guide(track, b0, b1, lo, hi, start, vel, gate=8):
    for t, ln, k in guide_line(spans(b0, b1), lo, hi, start):
        track.note(t, [k], ln, vel, gate)


REL = {'timpani': 122, 'orch': 124, 'harp': 120}


def roll(track, tick, k, beats, v0, v1, div=32, gate=8):
    """Strokes every 1/div note for `beats` beats, velocity v0 -> v1, with a short release while
    they last (the strokes overlap little: fewer voices), the track's own after."""
    step = 4 * TB // div
    cnt = int(beats * TB // step)
    if track.name in REL:
        track.cmd(tick, 'release 125')
        track.cmd(tick + cnt * step, 'release %d' % REL[track.name])
    for i in range(cnt):
        track.note(tick + i * step, [k], step, round(v0 + (v1 - v0) * i / max(1, cnt - 1)), gate)


def gliss(bar, notes, vel, beat=0.0):
    """A harp glissando in 32nds (short release while it runs)."""
    t0 = bt(bar, beat)
    ns = notes.split()
    T['harp'].cmd(t0, 'release 125')
    T['harp'].cmd(t0 + 12 * len(ns), 'release %d' % REL['harp'])
    for i, n in enumerate(ns):
        T['harp'].note(t0 + 12 * i, [key(n)], 12, vel, 8)


def bt(bar, beat=0.0):
    return int(round(bar * BAR + beat * TB))


K, S, HH, OH, CR = key('c0'), key('d1'), key('f#2'), key('a#3'), key('c#7')
TL, TM, TH = key('g4'), key('b4'), key('d5')     # the kit's tom, low / mid / high
BD, SN, ROLL, CYM = key('c0'), key('d1'), key('d4'), key('c#7')   # orchestral kit (98)


def groove(b0, b1, kick_vel=96, snare_vel=88, pattern='KxSKKxSx'):
    for b in range(b0, b1):
        for i, c in enumerate(pattern):
            if c == 'K':
                T['drums'].note(bt(b, i / 2), [K], 48, kick_vel - (0 if i == 0 else 10), 8)
            elif c == 'S':
                T['drums'].note(bt(b, i / 2), [S], 48, snare_vel, 8)


def hats(b0, b1, sub=8, vels=(84, 54, 70, 54, 80, 54, 70, 58), open_every=2, crash_bars=()):
    step = 4 * TB // sub
    for b in range(b0, b1):
        n = BAR // step
        for i in range(n):
            ks = [HH]
            v = vels[i % len(vels)]
            if open_every and i == n - 1 and (b - b0) % open_every == open_every - 1 and sub == 8:
                ks = [OH]
            if i == 0 and b in crash_bars:
                ks = [HH, CR]
                v = 112
            T['hats'].note(bt(b) + i * step, ks, step, v, 8)


# ============================================================================ the launch (L)
def launch(b):
    """The four bars that end the intro and the loop alike."""
    vn, fl = T['violins'], T['flute']
    vn.cmd(bt(b + 1), 'release 124')
    fl.cmd(bt(b + 1), 'release 124')
    vn.bars(b, ['v108 a4/4 v110 d5/4 v112 e5/4 v114 a5/4',
                'v100 g5/8 e5/8 c#5/8 e5/8 a5/8 g5/8 e5/8 c#5/8',
                'v104 d5/8t e5/8t a5/8t d6/8t a5/8t e5/8t d5/8t e5/8t a5/8t d6/8t e6/8t g6/8t',
                'v108 a4/16 c#5/16 b4/16 d5/16 c#5/16 e5/16 d5/16 f#5/16 e5/16 g5/16 f#5/16 a5/16 '
                'v112 g5/16 e5/16 c#5/16 b4/16'])
    fl.bars(b + 1, ['v78 g5/8 e5/8 c#5/8 e5/8 a5/8 g5/8 e5/8 c#5/8',
                    'v84 d5/8t e5/8t a5/8t d6/8t a5/8t e5/8t d5/8t e5/8t a5/8t d6/8t e6/8t g6/8t',
                    'v88 a5/16 c#6/16 b5/16 d6/16 c#6/16 e6/16 d6/16 f#6/16 e6/16 g6/16 f#6/16 a6/16 r4'])
    T['trombone'].bars(b, ['v92 d4/1', 'v98 c#4/1', 'v104 d4/1', 'v112 c#4/2. r4'])
    T['horn'].bars(b, ['v90 a3/1', 'v96 a3/1', 'v102 a3/1', 'v108 a3/2. r4'])
    motor(T['violas'], b, b + 2, 57, [0, 2, 1, 2], 48, (92, 72, 80, 72))
    motor(T['violas'], b + 2, b + 3, 57, [0, 1, 2], 32, (86, 66, 72))
    motor(T['violas'], b + 3, b + 4, 57, [0, 1, 2, 1], 24, (92, 70, 80, 70), gate=6)
    bs = T['basses']
    a = bass_pair('A7')
    for i in range(4):
        bs.note(bt(b, i), a, 96, 106 if i == 0 else 96, 6)
    for bb in (b + 1, b + 2, b + 3):
        for i in range(8):
            bs.note(bt(bb, i / 2), a if i in (0, 4) else a[:1], 48, (100 if i == 0 else 84) + 4 * (bb - b), 6)
    tp = T['timpani']
    for i in range(4):
        tp.note(bt(b, i), [key('a2')], 96, 90 + 4 * i, 8)
    for i in range(8):
        tp.note(bt(b + 1, i / 2), [key('a2')], 48, 76 + 2 * i, 8)
    for i in range(8):
        tp.note(bt(b + 2, i / 2), [key('a2')], 48, 84 + 2 * i, 8)
    roll(tp, bt(b + 3), key('a2'), 4, 70, 124)
    gliss(b + 3, 'a3 b3 c#4 d4 e4 f#4 g4 a4 b4 c#5 d5 e5 f#5 g5 a5 b5', 96)
    # drums: kick quarters, then the backbeat, the groove, a snare fill
    for i in range(4):
        T['drums'].note(bt(b, i), [K], 48, 90, 8)
    groove(b + 1, b + 2, 84, 74, 'KxSxKxSx')
    groove(b + 2, b + 3, 90, 82)
    fill = [(0, K, 96), (0.5, S, 60), (0.75, S, 66), (1, S, 72), (1.25, S, 76), (1.5, S, 80), (1.75, S, 84),
            (2, K, 100), (2.25, S, 88), (2.5, S, 92), (2.75, S, 96), (3, S, 100), (3.25, S, 104), (3.5, S, 110),
            (3.75, S, 116)]
    for i, (beat, k, v) in enumerate(fill):
        nxt = fill[i + 1][0] if i + 1 < len(fill) else 4
        T['drums'].note(bt(b + 3, beat), [k], int((nxt - beat) * TB), v, 8)
    hats(b, b + 2, 8, (72, 50, 62, 50, 72, 50, 62, 50), open_every=0)
    hats(b + 2, b + 3, 16, (66, 40, 52, 40), open_every=0)
    for i in range(12):
        T['hats'].note(bt(b + 3, i / 4), [HH], 24, (72, 46, 58, 46)[i % 4], 8)
    # the orchestral snare roll, a crescendo over the four bars
    for i in range(16):
        T['orch'].note(bt(b, i), [ROLL], 96, int(60 + 62 * i / 15), 8)


# ============================================================================ intro (fanfare)
vn, fl, tb, hn, gl = T['violins'], T['flute'], T['trombone'], T['horn'], T['glock']
vn.cmd(0, 'release 120')
fl.cmd(0, 'release 120')
vn.bars(I0, ['v112 a4/8 d5/8 f#5/4 a5/2', 'v76 f#5/1', 'v112 bb4/8 d5/8 f5/4 bb5/2', 'v116 c5/8 e5/8 g5/4 c6/2'])
fl.bars(I0, ['r1', 'v84 a5/8 d6/8 f#6/4 a6/2', 'v92 bb5/8 d6/8 f6/4 bb6/2', 'v96 c6/8 e6/8 g6/4 c7/2'])
gl.bars(I0, ['r1', 'v78 a6/8 d7/8 f#7/4 a7/2', 'r1', 'v84 c7/2 g7/2'])
tb.bars(I0, ['v118 a3/8 d4/8 f#4/4 a4/2', 'r1', 'v118 bb3/8 d4/8 f4/4 bb4/2', 'v122 c4/8 e4/8 g4/4 c5/2'])
hn.bars(I0, ['v110 f#3/8 a3/8 d4/4 f#4/2', 'v66 a3/8 d4/8 f#4/4 a4/2', 'v110 f3/8 bb3/8 d4/4 f4/2',
             'v114 g3/8 c4/8 e4/4 g4/2'])
T['violas'].bars(I0, ['v92 [d4 f#4 a4]/1', 'v70 [d4 f#4 a4]/1', 'v94 [d4 f4 bb4]/1', 'v98 [e4 g4 c5]/1'])
T['basses'].bars(I0, ['v112 [d2 d3]/1', 'v90 [d2 d3]/1', 'v112 [bb1 bb2]/1', 'v116 [c2 c3]/1'])
gliss(I0 + 1, 'd4 e4 f#4 g4 a4 b4 c#5 d5 e5 f#5 g5 a5 b5 c#6 d6 e6', 92)
T['timpani'].bars(I0, ['v120 d3/2 r4 a2/4', 'v96 d3/4 r2.', 'v120 bb2/2 r4 f2/4', 'v122 c3/2 r4 g2/4'])
roll(T['timpani'], bt(I0 + 1, 2), key('d3'), 2, 40, 100)
T['hats'].note(bt(I0), [CR], BAR, 116, 8)
T['hats'].note(bt(I0 + 2), [CR], BAR, 112, 8)
T['hats'].note(bt(I0 + 3), [CR], BAR, 104, 8)
T['orch'].note(bt(I0), [BD], BAR, 122, 8)
for i in range(8):
    T['orch'].note(bt(I0 + 1, 2 + i / 4), [ROLL], 24, 40 + 8 * i, 8)
T['orch'].note(bt(I0 + 2), [BD], BAR // 2, 120, 8)
T['orch'].note(bt(I0 + 3), [BD], BAR // 2, 124, 8)
launch(L0)

# ============================================================================ A: the departure theme
MEL_A = ['a4/8 d5/8 f#5/4 e5/8 d5/8 a5/4',     # D
         'b5/4. a5/8 f#5/4 d5/4',              # Bm
         'd5/8 e5/8 g5/4 b5/8 a5/8 g5/4',       # G
         'e5/4. d5/8 c#5/4 r8 e5/8',           # A7sus4 A7
         'f#5/8 d5/8 a5/4 f#5/8 a5/8 d6/4',     # D/F#
         'b5/4. a5/8 bb5/4 g5/4',              # G Gm
         'a5/4. f#5/8 d#5/4 f#5/4',            # D/A B7
         'e5/8 f#5/8 g5/8 b5/8 a5/4 g5/8 e5/8',  # Em7 A7
         'a4/8 d5/8 f#5/4 e5/8 d5/8 a5/4',     # D
         'a5/4. c#6/8+4 a5/4',                 # F#m7
         'b5/4. a5/8 g5/4 d5/4',               # G
         'e5/8 f#5/8 g#5/4 b5/8 d6/8+4',        # E7/G#
         'd6/4. c6/8 bb5/4 f5/4',              # Bb
         'e6/4. d6/8 c6/4 g5/4',               # C
         'f#6/2 e6/8 d6/8 a5/4',               # D
         'b5/4. c#6/8 a#5/4 r4']               # F#7sus4 F#7


def up(text, semis):
    from score_lib import _NOTE
    out = []
    for tok in text.split():
        if '/' in tok and not tok.startswith('r') and not tok.startswith('['):
            n, ln = tok.split('/', 1)
            k = key(n) + semis
            names = ['c', 'c#', 'd', 'd#', 'e', 'f', 'f#', 'g', 'g#', 'a', 'a#', 'b']
            out.append('%s%d/%s' % (names[k % 12], k // 12 - 1, ln))
        else:
            out.append(tok)
    return ' '.join(out)


vn.cmd(bt(A0), 'release 120')
fl.cmd(bt(A0), 'release 120')
vn.bars(A0, ['v118 ' + MEL_A[0]] + ['v114 ' + x for x in MEL_A[1:8]] + ['v112 ' + MEL_A[8]] +
        ['v108 ' + x for x in MEL_A[9:12]] + ['v114 ' + x for x in MEL_A[12:15]] + ['v108 ' + MEL_A[15]])
fl.bars(A0 + 8, ['v88 ' + MEL_A[8]] + ['v88 ' + x for x in MEL_A[9:12]] + ['v96 ' + x for x in MEL_A[12:16]])
gl.bars(A0 + 12, ['v82 ' + up(MEL_A[12], 12), up(MEL_A[13], 12), up(MEL_A[14], 12)])
tb.bars(A0, ['v88 f#4/1', 'v86 f#4/1', 'v86 g4/1', 'v90 e4/2 a3/8 b3/8 c#4/8 e4/8',
             'v88 f#4/1', 'v86 g4/2 g4/2', 'v88 f#4/2 f#4/2', 'v88 g4/2 g4/4 a4/4'])
tb.bars(A0 + 8, ['v96 f#4/2 a4/2', 'v94 e4/1', 'v96 d4/2 b3/2', 'v98 d4/1',
                 'v104 bb3/4 d4/4 f4/4 bb4/4', 'v108 c4/4 e4/4 g4/4 c5/4', 'v114 d5/1', 'v104 b4/2 a#4/4 f#4/8 a#4/8'])
pads(hn, A0, A0 + 8, key('g3'), key('d5'), 2, 86)
pads(hn, A0 + 8, A0 + 16, key('g3'), key('d5'), 2, 82)
motor(T['violas'], A0, A0 + 16, 57, [0, 2, 1, 2], 48, (94, 72, 82, 72))
bass_line(A0, A0 + 16, 'gallop', 104)
# harp: rising quarter-note arpeggios in the second half
for t, ln, name in spans(A0 + 8, A0 + 16):
    v = triad_voicing(name, key('d4'), 4) if ln == BAR else triad_voicing(name, key('d4'), 2)
    for i, k in enumerate(v):
        T['harp'].note(t + i * TB, [k], TB, 78 + 4 * i, 8)
# marimba: off-beat chords (the island's colour)
for t, ln, name in spans(A0, A0 + 16):
    v = pad_voicing(name, key('f3'), key('a4'), 2)
    for at in range(t + 48, t + ln, 96):
        T['marimba'].note(at, v, 48, 78 if t < bt(A0 + 8) else 74, 8)
tp = T['timpani']
tp.bars(A0, ['v118 d3/4 r2.', 'r1', 'r1', 'r2 v96 a2/8 a2/8 a2/4',
             'v104 d3/4 r2.', 'r1', 'r1', 'r2 v100 a2/8 a2/8 a2/4',
             'v116 d3/4 r2.', 'r1', 'r1', 'v100 e3/4 r2.', 'v112 bb2/4 r2.', 'v114 c3/4 r2.', 'v118 d3/4 r2.',
             'v106 f#2/4 r2.'])
roll(tp, bt(A0 + 15, 2), key('f#2'), 2, 50, 110)
groove(A0, A0 + 16, 104, 94)
hats(A0, A0 + 16, 8, (90, 58, 74, 58, 86, 58, 74, 62), crash_bars=(A0, A0 + 8, A0 + 12))
T['orch'].note(bt(A0), [BD], 192, 120, 8)
T['orch'].note(bt(A0 + 8), [BD], 192, 112, 8)
roll(T['orch'], bt(A0 + 11, 2), CYM, 2, 24, 96, div=12)
T['orch'].note(bt(A0 + 12), [BD, CYM], 192, 118, 8)
for i in range(8):
    T['orch'].note(bt(A0 + 15, 2 + i / 4), [ROLL], 24, 50 + 9 * i, 8)

# ============================================================================ B: the castle
vn.cmd(bt(B0), 'release 124')
for b, name in zip(range(B0, B0 + 8), ['f#5', 'e5', 'f#5', 'g#5', 'b5', 'c#6', 'b5', 'a5']):
    k = key(name)
    for i in range(16):
        k2 = k
        if b == B0 + 6 and i >= 8:
            k2 = key('a5')     # Em7 -> F#m7
        if b == B0 + 7:
            k2 = key('g5') if i < 8 else key('f#5')   # D7sus4 -> D7
        vn.note(bt(b, i / 4), [k2], 24, (78 if i % 4 == 0 else 64) + 2 * (b - B0), 7)
CASTLE = ['b3/4. c#4/8 d4/4 f#4/4', 'e4/4. d4/8 c#4/4 a3/4', 'd4/4. e4/8 f#4/4 b4/4', 'g#4/2 f#4/8 e4/8 g#4/4',
          'g4/4. a4/8 b4/4 d5/4', 'c#5/2 b4/8 a4/8 e4/4', 'g4/4 e4/4 a4/4 f#4/4', 'g4/2 f#4/4 c5/4']
CASTLE_LOW = ['f#3/4. g#3/8 a3/4 c#4/4', 'b3/4. a3/8 g#3/4 e3/4', 'a3/4. b3/8 c#4/4 f#4/4', 'e4/2 b3/4 b3/4',
              'd4/4. e4/8 f#4/4 a4/4', 'e4/2 e4/8 c#4/8 a3/4', 'd4/4 b3/4 e4/4 c#4/4', 'd4/2 c4/4 a3/4']
hn.bars(B0, ['v114 ' + CASTLE[0]] + CASTLE[1:4] + ['v116 ' + CASTLE[4]] + CASTLE[5:])
tb.bars(B0, ['v98 ' + CASTLE_LOW[0]] + CASTLE_LOW[1:4] + ['v102 ' + CASTLE_LOW[4]] + CASTLE_LOW[5:])
fl.bars(B0 + 4, ['v74 ' + up(CASTLE[4], 12), up(CASTLE[5], 12), up(CASTLE[6], 12), up(CASTLE[7], 12)])
motor(T['violas'], B0, B0 + 8, 54, [0, 1, 2], 32, (92, 68, 74))
bass_line(B0, B0 + 8, 'march', 92)
T['pad'].bars(B0, ['v86 [b2 b3]/1', 'v86 [b2 b3]/1', 'v88 [b2 b3]/1', 'v90 [b2 b3]/1'])
tp.bars(B0, ['v100 b2/4 r4 f#2/4 r4', 'v90 b2/4 r4 f#2/4 r4', 'v100 b2/4 r4 f#2/4 r4', 'v94 b2/4 r4 f#2/4 r4',
             'v100 g2/4 r4 d3/4 r4', 'v100 a2/4 r4 e3/4 r4', 'v100 e3/4 r4 f#2/4 r4', 'v104 d3/4 r2.'])
roll(tp, bt(B0 + 7, 2), key('d3'), 2, 46, 106)
for b in range(B0, B0 + 8):
    if b == B0 + 7:
        T['orch'].note(bt(b), [BD, SN], 32, 96, 8)
        for i in range(1, 3):
            T['orch'].note(bt(b, i / 3), [SN], 32, 66, 8)
        T['orch'].note(bt(b, 1), [SN], 96, 84, 8)
        for i in range(8):
            T['orch'].note(bt(b, 2 + i / 4), [ROLL], 24, 46 + 10 * i, 8)
        continue
    for half in (0, 2):
        T['orch'].note(bt(b, half), [BD, SN] if half == 0 else [SN], 32, 90 if half == 0 else 80, 8)
        T['orch'].note(bt(b, half + 1 / 3), [SN], 32, 58, 8)
        T['orch'].note(bt(b, half + 2 / 3), [SN], 32, 64, 8)
        T['orch'].note(bt(b, half + 1), [SN], 96, 76, 8)
for b in range(B0, B0 + 8):
    for i in range(4):
        T['hats'].note(bt(b, i), [HH, CR] if (b == B0 and i == 0) else [HH], 96,
                       100 if (b == B0 and i == 0) else (56 if i % 2 == 0 else 46), 8)
T['drums'].note(bt(B0), [K], 96, 96, 8)

# ============================================================================ T: on the road
vn.cmd(bt(T0), 'release 120')
MEL_T = ['b4/8 d5/8 g5/4. f#5/8 g5/8 a5/8', 'c6/4 g5/8 e5/8 a5/4. f#5/8', 'g5/8 e5/8 b4/8 e5/8 g5/4 b5/4',
         'a5/8 b5/8 c6/8 a5/8 f#5/4 d5/8 e5/8', 'd5/8 g5/8 b5/4. a5/8 g5/8 d6/8', 'e6/4. d6/8 c6/8 g5/8 e5/8 g5/8',
         'c6/4 b5/8 a5/8 bb5/4 f5/4', 'e5/4. d5/8 c#5/4 e5/8 g5/8']
vn.bars(T0, ['v104 ' + MEL_T[0]] + MEL_T[1:4] + ['v108 ' + MEL_T[4]] + MEL_T[5:])
fl.bars(T0, ['v90 ' + MEL_T[0]] + MEL_T[1:4] + ['v94 ' + MEL_T[4]] + MEL_T[5:])
gl.bars(T0 + 4, ['v78 ' + up(MEL_T[4], 12), up(MEL_T[5], 12)])
guide(tb, T0, T0 + 4, key('g3'), key('d4'), key('b3'), 84)
tb.bars(T0 + 4, ['v94 d4/2 g4/2', 'e4/2 g4/4 c5/4', 'e4/2 d4/2', 'v100 d4/2 c#4/2'])
guide(hn, T0, T0 + 8, key('a3'), key('d5'), key('d4'), 86)
motor(T['violas'], T0, T0 + 8, 55, [0, 2, 1, 2], 48, (84, 64, 74, 64))
bass_line(T0, T0 + 8, 'drive', 90, gate=6)
for t, ln, name in spans(T0, T0 + 8):
    v = pad_voicing(name, key('f3'), key('a4'), 2)
    for at in range(t + 48, t + ln, 96):
        T['marimba'].note(at, v, 48, 84, 8)
tp.bars(T0, ['v108 g2/4 r2.', 'r1', 'v96 e3/4 r2.', 'r2 v92 d3/8 d3/8 a2/4', 'v104 g2/4 r2.', 'v100 c3/4 r2.',
             'v100 c3/4 r4 bb2/4 r4', 'r1'])
roll(tp, bt(T0 + 7), key('a2'), 4, 50, 118)
groove(T0, T0 + 8, 98, 90, 'KxSKxKSx')
hats(T0, T0 + 4, 8, crash_bars=(T0,))
hats(T0 + 4, T0 + 8, 16, (80, 46, 62, 46), open_every=0)
T['orch'].note(bt(T0), [BD, CYM], 192, 110, 8)
for i in range(16):
    T['orch'].note(bt(T0 + 7, i / 4), [ROLL], 24, 40 + 5 * i, 8)

# ============================================================================ C: the volcano
vn.cmd(bt(C0), 'release 124')
for b, name in zip(range(C0, C0 + 4), ['a5', 'bb5', 'a5', 'bb5']):
    for i in range(16):
        k = key(name) if not (b == C0 + 3 and i >= 8) else key('g5')
        vn.note(bt(b, i / 4), [k], 24, (70 if i % 4 == 0 else 56) + 4 * (b - C0), 7)
vn.cmd(bt(C0 + 4), 'release 120')
MEL_C = ['d4/4. a3/8 d4/4 f4/4', 'g4/4. f4/8 eb4/2', 'd4/4. a3/8 d4/4 a4/4', 'bb4/4. a4/8 g4/4 eb4/4',
         'd5/2 c5/8 bb4/8 f4/4', 'g4/4. a4/8 bb4/4 d5/4', 'e5/4. d5/8 b4/4 g#4/4', 'a4/4 c#5/4 e5/4 g5/4']
tb.bars(C0, ['v112 ' + MEL_C[0]] + MEL_C[1:4] + ['v118 ' + MEL_C[4]] + MEL_C[5:])
vn.bars(C0 + 4, ['v104 ' + up(MEL_C[4], 12), up(MEL_C[5], 12), up(MEL_C[6], 12), 'v110 ' + up(MEL_C[7], 12)])
hn.bars(C0, ['v84 [d4 a4]/1', 'v84 [eb4 g4]/1', 'v88 [d4 a4]/1', 'v88 [eb4 bb4]/1'])
pads(hn, C0 + 4, C0 + 8, key('g3'), key('c5'), 2, 92)
for t, ln, name in spans(C0, C0 + 8):
    v = triad_voicing(name, key('d4'), 2)
    for i in range(ln // 24):
        T['violas'].note(t + i * 24, [v[i % 2]], 24, 74 if i % 4 == 0 else 60, 6)
OST = {'Dm': ['d', 'd', 'a', 'd', 'd', 'd', 'c', 'd'], 'Eb/D': ['d', 'd', 'bb', 'd', 'd', 'd', 'eb', 'd']}
for b in range(C0, C0 + 4):
    for i, n in enumerate(OST[CHORDS[b]]):
        k = key(n + '2') if n in ('d', 'eb') else key(n + '2') if n in ('a', 'bb') else key(n + '3')
        T['basses'].note(bt(b, i / 2), [k, k + 12] if i in (0, 4) else [k], 48, 104 if i in (0, 4) else 88, 6)
bass_line(C0 + 4, C0 + 8, 'drive', 98, gate=6)
T['pad'].cmd(bt(C0), 'prg 89')
T['pad'].bars(C0, ['v92 d3/1', 'v92 d3/1', 'v96 d3/1', 'v96 d3/1'])
pads(T['pad'], C0 + 4, C0 + 8, key('g2'), key('d4'), 3, 84)
for b in range(C0, C0 + 6):
    hi = key('bb2') if CHORDS[b] == 'Eb/D' else key('a2')
    if b >= C0 + 4:
        hi = key('f2') if CHORDS[b] == 'Bb' else key('d3')
    lo = key('d3') if b < C0 + 4 else (key('bb2') if CHORDS[b] == 'Bb' else key('g2'))
    for beat, k, v in [(0, lo, 116), (0.5, lo, 84), (1, hi, 100), (2, lo, 110), (2.5, lo, 84), (3, hi, 100)]:
        nxt = {0: 0.5, 0.5: 1, 1: 2, 2: 2.5, 2.5: 3, 3: 4}[beat]
        tp.note(bt(b, beat), [k], int((nxt - beat) * TB), v, 8)
roll(tp, bt(C0 + 6), key('e3'), 4, 60, 110)
roll(tp, bt(C0 + 7), key('a2'), 4, 70, 124)
for b in range(C0, C0 + 8):
    pat = [(0, K, 108), (0.5, TL, 84), (0.75, TL, 76), (1, TM, 96), (1.5, TM, 80), (2, K, 104), (2.5, TL, 84),
           (2.75, TL, 76), (3, TH, 96), (3.5, TM, 88)]
    if b in (C0 + 3, C0 + 7):
        pat = pat[:6] + [(2.5, TH, 90), (2.75, TH, 94), (3, TM, 98), (3.25, TM, 102), (3.5, TL, 106), (3.75, TL, 110)]
    for i, (beat, k, v) in enumerate(pat):
        nxt = pat[i + 1][0] if i + 1 < len(pat) else 4
        T['drums'].note(bt(b, beat), [k], int((nxt - beat) * TB), v, 8)
T['hats'].note(bt(C0), [CR], BAR, 112, 8)
T['hats'].note(bt(C0 + 4), [CR], BAR, 108, 8)
T['orch'].note(bt(C0), [BD], BAR, 124, 8)
roll(T['orch'], bt(C0 + 3, 2), CYM, 2, 30, 100, div=12)
T['orch'].note(bt(C0 + 4), [BD, CYM], BAR, 120, 8)
for i in range(16):
    T['orch'].note(bt(C0 + 7, i / 4), [ROLL], 24, 44 + 5 * i, 8)

# ============================================================================ D: the ancient ruins
MEL_D = ['d5/2 f5/4. e5/8', 'e5/2. c5/8 d5/8', 'f5/4. g5/8 a5/4 c6/4', 'g5/4. e5/8+4 d5/4',
         'd5/8t e5/8t f5/8t g5/2 f5/4', 'e5/2 c5/4 a4/4', 'd5/4. f5/8 a5/2', 'f5/4. e5/8 d5/4 b4/4']
T['recorder'].bars(D0, ['v100 ' + MEL_D[0]] + MEL_D[1:4] + ['v104 ' + MEL_D[4]] + MEL_D[5:])
for t, ln, name in spans(D0, D0 + 8):
    v = triad_voicing(name, key('f3'), 4) if name != 'C/Bb' else [key('g3'), key('bb3'), key('c4'), key('e4')]
    v = [k if k <= key('bb4') else k - 12 for k in v]
    v = sorted(v)
    pat = [0, 2, 1, 3, 2, 1, 2, 3] if ln == BAR else [0, 2, 1, 3]
    for i, p in enumerate(pat):
        T['marimba'].note(t + i * 48, [v[p]], 48, 86 if i % 2 == 0 else 72, 8)
prev = None
for t, ln, name in spans(D0, D0 + 8):
    low = pad_voicing(name, key('bb2'), key('d4'), 2, prev)
    prev = low
    T['harp'].note(t, low, 192 if ln == BAR else ln, 84, 8)
    if ln == BAR:
        ups = triad_voicing(name, key('f4'), 3) + [triad_voicing(name, key('f4'), 3)[0] + 12]
        for i, k in enumerate(ups):
            T['harp'].note(t + 192 + i * 48, [k], 48, 70 + 4 * i, 8)
T['pad'].cmd(bt(D0), 'prg 88')
pads(T['pad'], D0, D0 + 8, key('bb3'), key('f5'), 3, 70)
T['pad'].cmd(bt(D0 + 8), 'prg 89')
bass_line(D0, D0 + 8, 'pizz', 76, gate=2)
gl.put(D0 + 1, 'v70 e7/2 r2')
gl.put(D0 + 3, 'v66 e7/2 r2')
gl.put(D0 + 6, 'v64 a6/2 r2')
tp.bars(D0, ['v70 bb2/2 r2', 'r1', 'r1', 'r1', 'v66 g2/2 r2', 'r1', 'r1', 'r1'])
for b in range(D0, D0 + 8):
    T['drums'].note(bt(b), [K], 144, 70, 8)
    T['drums'].note(bt(b, 1.5), [K], 48, 56, 8)
hats(D0, D0 + 8, 16, (54, 34, 44, 34), open_every=0)
roll(T['orch'], bt(D0 + 7, 2), CYM, 2, 20, 70, div=12)
vn.put(D0 + 7, 'v84 d4/8 f4/8', beat=3)

# ============================================================================ E: the climb
MEL_E = ['g4/8 c5/8 e5/4 d5/8 c5/8 g5/4', 'e5/8 g5/8 bb5/4 a5/8 g5/8 e5/4', 'a4/8 d5/8 f5/4 e5/8 d5/8 a5/4',
         'f#5/8 a5/8 c6/4 b5/8 a5/8 f#5/4', 'b4/8 e5/8 g5/4 f#5/8 e5/8 b5/4', 'c5/8 f5/8 a5/4 g5/8 f5/8 c6/4',
         'd5/8 g5/8 b5/4 a5/8 g5/8 d6/4', 'e6/4. d6/8 c#6/8 a5/8 g5/8 e5/8']
vn.bars(E0, ['v%d %s' % (106 + 2 * i, m) for i, m in enumerate(MEL_E)])
fl.bars(E0 + 4, ['v%d %s' % (84 + 4 * i, m) for i, m in enumerate(MEL_E[4:])])
gl.bars(E0 + 6, ['v80 ' + up(MEL_E[6], 12), up(MEL_E[7], 12)])
tb.bars(E0, ['v98 c4/1', 'v98 c#4/1', 'v100 d4/1', 'v100 d#4/1', 'v102 e4/1', 'v104 f4/1',
             'v108 g4/4. d4/8 g4/4 b4/4', 'v114 a4/4. e4/8 a4/4 c#5/4'])
guide(hn, E0, E0 + 6, key('g3'), key('d5'), key('e4'), 96)
hn.bars(E0 + 6, ['v102 d4/4. b3/8 d4/4 g4/4', 'v108 e4/4. a3/8 e4/4 a4/4'])
motor(T['violas'], E0, E0 + 8, 57, [0, 2, 1, 2], 48, (90, 70, 78, 70))
E_ROOTS = ['c2', 'c#2', 'd2', 'd#2', 'e2', 'f2', 'g2', 'a2']
for b in range(E0, E0 + 8):
    r = key(E_ROOTS[b - E0])
    for i in range(8):
        T['basses'].note(bt(b, i / 2), [r, r + 12] if i in (0, 4) else [r], 48, (100 if i == 0 else 86) + 2 * (b - E0), 6)
for b in (E0, E0 + 1):
    for i, p in enumerate([0, 2, 1, 3, 2, 1, 2, 3]):
        v = sorted(k if k <= key('bb4') else k - 12 for k in triad_voicing(CHORDS[b], key('f3'), 4))
        T['marimba'].note(bt(b, i / 2), [v[p]], 48, 78 - 3 * (b - E0) if i % 2 == 0 else 64, 8)
tp.bars(E0, ['v90 c3/4 r2.', 'r1', 'v96 d3/4 r2.', 'r1', 'v102 e3/4 r2.', 'v104 f2/4 r2.', 'v108 g2/4 g2/4 g2/4 g2/4',
             'r1'])
roll(tp, bt(E0 + 7), key('a2'), 4, 60, 124)
for b in (E0, E0 + 1):
    for i in range(4):
        T['drums'].note(bt(b, i), [K], 96, 84, 8)
groove(E0 + 2, E0 + 4, 88, 70, 'KxxxKxSx')
groove(E0 + 4, E0 + 6, 94, 84)
groove(E0 + 6, E0 + 7, 98, 90)
fill = [(0, K, 100), (0.5, S, 70), (1, S, 76), (1.5, K, 92), (2, S, 82), (2.25, S, 86), (2.5, S, 90), (2.75, S, 94),
        (3, S, 100), (3.25, S, 106), (3.5, S, 112), (3.75, S, 118)]
for i, (beat, k, v) in enumerate(fill):
    nxt = fill[i + 1][0] if i + 1 < len(fill) else 4
    T['drums'].note(bt(E0 + 7, beat), [k], int((nxt - beat) * TB), v, 8)
hats(E0, E0 + 2, 8, (60, 40, 50, 40, 58, 40, 50, 42), open_every=0)
hats(E0 + 2, E0 + 6, 8, (70, 46, 58, 46, 68, 46, 58, 50), open_every=2)
hats(E0 + 6, E0 + 8, 16, (82, 50, 64, 50), open_every=0)
T['orch'].note(bt(E0 + 4), [BD], 192, 104, 8)
for i in range(8):
    T['orch'].note(bt(E0 + 6, i / 2), [ROLL], 48, 40 + 4 * i, 8)
for i in range(16):
    T['orch'].note(bt(E0 + 7, i / 4), [ROLL], 24, 72 + 3 * i, 8)

# ============================================================================ A3: the view
vn.bars(A30, ['v116 ' + MEL_A[8]] + ['v112 ' + x for x in MEL_A[9:12]] + ['v118 ' + x for x in MEL_A[12:15]] +
        ['v108 d6/2. r4'])
fl.bars(A30, ['v96 ' + MEL_A[8]] + MEL_A[9:12] + ['v100 ' + x for x in MEL_A[12:15]] + ['v90 d6/2. r4'])
gl.bars(A30 + 4, ['v86 ' + up(MEL_A[12], 12), up(MEL_A[13], 12), up(MEL_A[14], 12)])
tb.bars(A30, ['v104 f#4/2 a4/2', 'v100 e4/1', 'v104 d4/2 b3/2', 'v106 d4/1',
              'v110 bb3/4 d4/4 f4/4 bb4/4', 'v114 c4/4 e4/4 g4/4 c5/4', 'v118 d5/1', 'v118 a3/8 d4/8 f#4/4 a4/2'])
pads(hn, A30, A30 + 7, key('g3'), key('d5'), 2, 88)
hn.put(A30 + 7, 'v110 f#3/8 a3/8 d4/4 f#4/2')
motor(T['violas'], A30, A30 + 8, 57, [0, 2, 1, 2], 48, (90, 68, 78, 68))
bass_line(A30, A30 + 8, 'gallop', 104)
for t, ln, name in spans(A30, A30 + 7):
    v = triad_voicing(name, key('d4'), 4)
    for i, k in enumerate(v):
        T['harp'].note(t + i * TB, [k], TB, 82 + 4 * i, 8)
for t, ln, name in spans(A30, A30 + 7):
    v = pad_voicing(name, key('f3'), key('a4'), 2)
    for at in range(t + 48, t + ln, 96):
        T['marimba'].note(at, v, 48, 78, 8)
tp.bars(A30, ['v122 d3/4 r2.', 'r1', 'r1', 'v104 e3/4 r2.', 'v116 bb2/4 r2.', 'v118 c3/4 r2.', 'v122 d3/4 r2.',
              'v120 a2/8 d3/8 d3/4 r2'])
roll(tp, bt(A30 + 6, 2), key('d3'), 2, 60, 116)
groove(A30, A30 + 7, 100, 94)
T['drums'].bars(A30 + 7, ['v112 c0/8 c0/8 c0/4 r2'])
hats(A30, A30 + 7, 8, crash_bars=(A30, A30 + 4))
T['hats'].note(bt(A30 + 7), [CR], BAR, 116, 8)
T['orch'].note(bt(A30), [BD, CYM], 192, 124, 8)
roll(T['orch'], bt(A30 + 3, 2), CYM, 2, 30, 104, div=12)
T['orch'].note(bt(A30 + 4), [BD, CYM], 192, 120, 8)
T['orch'].note(bt(A30 + 7), [BD], 192, 124, 8)

launch(L1)

# ============================================================================ the score
LOOP_TICK = LOOP_BAR * BAR
END_TICK = END_BAR * BAR
T['pad'].cmd(LOOP_TICK, 'prg 89')

TRACKS = [
    # name, program, volume, pan, release, extra
    ('violins', 48, 120, 40, 120, {}),
    ('flute', 73, 84, 54, 120, {}),
    ('trombone', 57, 104, 82, 120, {}),
    ('horn', 60, 92, 46, 122, {}),
    ('violas', 48, 92, 88, 124, {}),
    ('basses', 48, 96, 64, 124, {}),
    ('harp', 46, 96, 24, 120, {}),
    ('marimba', 12, 92, 100, 122, {'sounding': True}),
    ('glock', 9, 66, 92, 116, {'sounding': True}),
    ('timpani', 47, 92, 58, 122, {}),
    ('drums', 95, 96, 64, 118, {}),
    ('hats', 95, 78, 74, 116, {}),
    ('orch', 98, 100, 70, 124, {}),
    ('pad', 89, 76, 64, 120, {}),
    ('recorder', 74, 100, 44, 120, {'sounding': True,
                                     'echo': {'delay': 72, 'copies': [{'pan': 108, 'volume': 48}]}}),
]

SCORE = {
    'title': '広い世界へ',
    'label': 'SEQ_BGM_HIROI_SEKAI',
    'timebase': TB,
    'tempo': TEMPO,
    'main_volume': 100,
    'bar': 4,
    'loop': True,
    'tracks': [],
}
for name, prg, vol, pan, rel, extra in TRACKS:
    tr = T[name]
    d = {'name': name, 'program': prg, 'volume': vol, 'pan': pan, 'release': rel,
         'mml': tr.mml(LOOP_TICK, END_TICK)}
    d.update(extra)
    SCORE['tracks'].append(d)
