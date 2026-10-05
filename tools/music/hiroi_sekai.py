# 「広い世界へ」 (Hiroi Sekai e, "Into the Wide World"): the music of the port's Wuhu Island, a
# stately, sweeping 3/4 in the manner of a classic orchestral JRPG overworld / kingdom theme, made
# only of Wii no Ma's own instruments (BANK_BGM_ALL in snd/douga.brsar). An original piece: no
# borrowed melody, motif or progression. A score for tools/music/rseq.py:
#
#   python3 tools/music/rseq.py compile tools/music/hiroi_sekai.py -o hiroi_sekai.rseq --wav preview.wav
#   python3 tools/music/rseq.py header tools/music/hiroi_sekai.py -o engine/wuhu/hiroi_sekai_seq.h
#   python3 tools/music/check.py tools/music/hiroi_sekai.py        (harmony, loop seam, voices, levels)
#
# D major -> E major, 3/4, 108 bpm. A 6-bar fanfare (played once): brass calls on I and IV/I,
# then bVI - bVII (Bb - C) and A7sus4 - A7 with a harp glissando into the loop:
#   A   (16) the song on the violins over harp arpeggios and a stepwise falling bass
#            (D - C# - B - A - G - F# - E - A), brass answering in the held bars; the second half
#            adds the horn an octave below and the flute, and climbs through G - G#dim7 to the top
#   B1  (8)  B minor: flute and violins, horn counter-line, harp, soft timpani
#   B2  (8)  the brass take a rising sequence (bass G - A - B - C - D), violins sweep in arpeggios,
#            snare and timpani rolls, cymbal swell: bVII (D) -> I of E
#   A'  (16) the song a whole step up in E: violins and flute, trombone and horn in unison an
#            octave below, a second trombone in harmony, waltz afterbeats, cymbals, glockenspiel
#   L   (4)  E -> Em7 brass calls, then the fanfare's last two bars (A7sus4 - A7) back to D
# 52 bars, 86.7 s a pass; the fanfare's last two bars are the loop's last two bars, so the loop is
# entered the same way both times.
# 13 tracks + 1 echo copy of the trombone (the channel's echo trick, delay 32: the whole track a
# triplet eighth later, so every pass, the first too, echoes alike): violins, trombone, horn,
# trombone 2, violas, cellos/basses (octave pairs), harp, timpani, snare, bass drum (98), cymbals
# (crashes on the drum kit's crash 95, swells on the orchestral cymbal 98), flute, glockenspiel.
# Checked by model (check.py): no note off its chord; passes identical, the echo 32 ticks behind
# the trombone throughout, no note held over the seam; up to 34 voices (mean 16.7; the room's
# themes peak at 35 and 32); the loop -16.8 dB RMS at host volume 125, 4.7 dB above the room's
# themes, within the channel's pieces' -23.0 .. -12.5.
#
# Notes are written NAME OCTAVE / LENGTH (c4 = key 60, `bb4` B-flat, `f#5` F-sharp): `c5/4.` a
# dotted quarter, `/8t` a triplet eighth, `/2.` a whole 3/4 bar; `[a4 c5]/4` a chord; rests, ties,
# `v`elocities, `q` gates, `{commands}`, `@program`, `|` and `$` are Mml's own (rseq.py). Bass
# parts use capital names for octave pairs (`D/2.` = d2 + d3, `A/2.` = a1 + a2).
import re

TB = 96   # ticks a quarter note (the channel's music always uses 96)
BAR = 3   # quarter notes a bar: 3/4

# ---------------------------------------------------------------------------- notation helpers
_PC = {'c': 0, 'd': 2, 'e': 4, 'f': 5, 'g': 7, 'a': 9, 'b': 11}
_NOTE = re.compile(r'^([a-g])([#b]?)(\d)$')
_SHARP = ['c', 'c#', 'd', 'd#', 'e', 'f', 'f#', 'g', 'g#', 'a', 'a#', 'b']


def key(n):
    mm = _NOTE.match(n)
    if not mm:
        raise ValueError('bad note %r' % n)
    return _PC[mm.group(1)] + {'#': 1, 'b': -1, '': 0}[mm.group(2)] + 12 * (int(mm.group(3)) + 1)


def name(k):
    return '%s%d' % (_SHARP[k % 12], k // 12 - 1)


def _mml_note(n):
    mm = _NOTE.match(n)
    if not mm:
        raise ValueError('bad note %r' % n)
    return 'o%s%s%s' % (mm.group(3), mm.group(1), {'#': '+', 'b': '-', '': ''}[mm.group(2)])


def m(text):
    """'f#5/4. [d4 f#4 a4]/2. r4 |' -> Mml with explicit octaves."""
    out = []
    for tok in re.findall(r'\[[^\]]*\]/\S+|\{[^}]*\}|\S+', text):
        if tok.startswith('['):
            notes, ln = tok[1:].split(']/')
            out.append('[' + ' '.join(_mml_note(n) for n in notes.split()) + ']' + ln)
        elif re.match(r'^[a-g][#b]?\d/', tok):
            n, ln = tok.split('/', 1)
            out.append(_mml_note(n) + ln)
        else:
            out.append(tok)
    return ' '.join(out)


_NOTE_TOK = re.compile(r'(?:(?<=\s)|(?<=\[)|^)([a-g][#b]?\d)(?=/|\s|\])')


def tr(x, semis):
    """Transposes the notes of a bar string (or a list of them)."""
    if isinstance(x, list):
        return [tr(b, semis) for b in x]
    return _NOTE_TOK.sub(lambda mo: name(key(mo.group(1)) + semis), x)


_LOW1 = {'e', 'f', 'f#', 'gb', 'g', 'g#', 'ab', 'a', 'a#', 'bb', 'b'}


def bass(x):
    """Capital note names -> octave pairs: 'D/2.' -> '[d2 d3]/2.', 'A/2.' -> '[a1 a2]/2.'."""
    if isinstance(x, list):
        return [bass(b) for b in x]

    def pair(mo):
        nm = mo.group(1).lower()
        o = 1 if nm in _LOW1 else 2
        return '[%s%d %s%d]/' % (nm, o, nm, o + 1)
    return re.sub(r'(?:(?<=\s)|^)([A-G][#b]?)/', pair, x)


def run(notes, ln):
    return ' '.join('%s/%s' % (n, ln) for n in notes.split())


def roll(n, beats, v0, v1, div=32):
    """A roll: `n` struck every 1/div note for `beats` beats, the velocity going v0 -> v1."""
    cnt = int(beats * div // 4)
    return ' '.join('v%d %s/%d' % (round(v0 + (v1 - v0) * i / max(1, cnt - 1)), n, div) for i in range(cnt))


def arp(notes):
    """A bar of harp in eighths."""
    return run(notes, 8)


def wz(x, y=None):
    """Afterbeats on beats 2 and 3."""
    return 'r4 [%s]/4 [%s]/4' % (x, y or x)


def track(intro, loop):
    return m(' | '.join(intro) + ' | $ ' + ' | '.join(loop) + ' |')


R = 'r2.'

# ---------------------------------------------------------------------------- the song (D major)
MEL_A = ['a4/2 f#5/4',            # 1  D
         'e5/4. d5/8 e5/4',       # 2  A/C#
         'f#5/2 b5/4',            # 3  Bm
         'a5/2.',                 # 4  Bm/A
         'b5/2 g5/4',             # 5  G
         'f#5/4. e5/8 d5/4',      # 6  D/F#
         'g5/2 f#5/4',            # 7  Em7
         'e5/2.',                 # 8  A7
         'a4/2 f#5/4',            # 9  D
         'e5/4. d5/8 e5/4',       # 10 A/C#
         'f#5/2 b5/4',            # 11 Bm
         'b5/4. c#6/8 d6/4',      # 12 G
         'f6/2 d6/4',             # 13 G#dim7
         'a5/2 f#5/4',            # 14 D/A
         'e5/2 g5/4',             # 15 A7sus4 A7
         'f#5/2.']                # 16 D
MEL_B1 = ['b4/2 d5/4', 'f#5/4. e5/8 d5/4', 'e5/2 g5/4', 'f#5/2.',
          'b5/2 a5/4', 'g5/4. a5/8 b5/4', 'e6/2 d6/4', 'c#6/2.']
MEL_B2 = ['b4/2 g5/4', 'f#5/4. e5/8 d5/4', 'c#5/2 a5/4', 'g#5/4. f#5/8 e5/4',
          'd5/2 b5/4', 'a5/4. g5/8 f#5/4', 'e5/2 c6/4', 'd5/8t d5/8t d5/8t f#5/8t f#5/8t f#5/8t a5/4']
MEL_A3 = tr(MEL_A, 2)             # A' in E
# the fanfare's brass (lead / horn / second trombone), the link's calls
INTRO_LEAD = ['a4/8t a4/8t a4/8t d5/2', 'b4/8t b4/8t b4/8t d5/2', 'f5/2.', 'g5/2.', 'a5/2.', 'g5/8 r8 r4 r4']
INTRO_HORN = ['f#4/8t f#4/8t f#4/8t a4/2', 'g4/8t g4/8t g4/8t b4/2', 'd5/2.', 'e5/2.', 'd5/2.', 'c#5/8 r8 r4 r4']
INTRO_TB2 = ['d4/8t d4/8t d4/8t f#4/2', 'd4/8t d4/8t d4/8t g4/2', 'bb4/2.', 'c5/2.', 'e5/2.', 'e5/8 r8 r4 r4']
LINK_LEAD = ['b4/8t b4/8t b4/8t e5/2', 'b4/8t b4/8t b4/8t d5/4 g5/4']
LINK_HORN = ['g#4/8t g#4/8t g#4/8t b4/2', 'g4/8t g4/8t g4/8t b4/4 d5/4']
LINK_TB2 = ['e4/8t e4/8t e4/8t g#4/2', 'e4/8t e4/8t e4/8t g4/4 b4/4']

# ---------------------------------------------------------------------------- violins
# (runs and pickups get a short release, {release 124}; the song rings, {release 118})
VLN_INTRO = ['v104 f#5/2.', 'g5/2.', 'd6/2.', 'e6/2.', 'v104 e6/2.', 'v112 c#6/8 r8 r4 {release 124} e4/8t f#4/8t g4/8t']
VLN_B2 = ['g5/8 b5/8 d6/8 g6/8 d6/8 b5/8',
          'g5/8 b5/8 d6/8 e6/8 d6/8 b5/8',
          'a5/8 c#6/8 e6/8 a6/8 e6/8 c#6/8',
          'a5/8 c#6/8 e6/8 f#6/8 e6/8 c#6/8',
          'b5/8 d6/8 f#6/8 b6/8 f#6/8 d6/8',
          'b5/8 d6/8 f#6/8 a6/8 f#6/8 d6/8',
          'c6/8 e6/8 g6/8 c7/8 g6/8 e6/8',
          '{release 124} d6/8t d6/8t d6/8t f#6/8t f#6/8t f#6/8t a6/4']
VIOLINS = track(VLN_INTRO,
                ['{release 118} v116 ' + MEL_A[0]] + MEL_A[1:8] + ['v118 ' + MEL_A[8]] + MEL_A[9:] +
                ['v108 ' + MEL_B1[0]] + MEL_B1[1:] +
                ['{release 122} v96 ' + VLN_B2[0], VLN_B2[1], 'v102 ' + VLN_B2[2], VLN_B2[3], 'v108 ' + VLN_B2[4], VLN_B2[5],
                 'v114 ' + VLN_B2[6], 'v120 ' + VLN_B2[7]] +
                ['{release 118} v120 ' + MEL_A3[0]] + MEL_A3[1:] +
                ['v108 g#5/2.', 'g5/2.'] + VLN_INTRO[4:])

# ---------------------------------------------------------------------------- trombone (lead)
A_ANSWERS = [R, R, R, 'r4 v100 d5/8t c#5/8t b4/8t a4/4', R, R, R, 'r4 e5/8t d5/8t c#5/8t a4/4',
             R, R, R, R, R, R, R, 'r4 a4/8t b4/8t c#5/8t d5/4']
LEAD = track(['v112 q7 ' + INTRO_LEAD[0]] + INTRO_LEAD[1:4] + ['v116 ' + INTRO_LEAD[4], INTRO_LEAD[5]],
             A_ANSWERS +
             [R] * 8 +
             ['v104 ' + tr(MEL_B2[0], -12)] + tr(MEL_B2[1:4], -12) + ['v110 ' + tr(MEL_B2[4], -12)] +
             tr(MEL_B2[5:7], -12) + ['v118 ' + tr(MEL_B2[7], -12)] +
             ['v120 ' + tr(MEL_A3[0], -12)] + tr(MEL_A3[1:], -12) +
             ['v112 ' + LINK_LEAD[0], LINK_LEAD[1], 'v116 ' + INTRO_LEAD[4], INTRO_LEAD[5]])

# ---------------------------------------------------------------------------- horn
HORN_A1 = ['f#4/2.', 'e4/2.', 'd4/2.', 'c#4/2.', 'b3/2 d4/4', 'a3/2.', 'b3/2 d4/4', 'c#4/2.']
HORN_B1 = ['d4/2.', 'd4/2.', 'e4/2.', 'e4/2 a#3/4', 'd4/2.', 'd4/2.', 'e4/2 g4/4', 'f#4/2 e4/4']
HORN = track(['v108 q7 ' + INTRO_HORN[0]] + INTRO_HORN[1:],
             ['v80 q8 ' + HORN_A1[0]] + HORN_A1[1:] +
             ['v96 q7 ' + tr(MEL_A[8], -12)] + tr(MEL_A[9:], -12) +
             ['v84 q8 ' + HORN_B1[0]] + HORN_B1[1:] +
             ['v100 q7 ' + tr(MEL_B2[0], -12)] + tr(MEL_B2[1:], -12) +
             ['v112 ' + tr(MEL_A3[0], -12)] + tr(MEL_A3[1:], -12) +
             ['v108 ' + LINK_HORN[0], LINK_HORN[1]] + INTRO_HORN[4:])

# ---------------------------------------------------------------------------- second trombone
TB2_A = [R, R, R, 'r4 v92 b4/8t a4/8t g4/8t f#4/4', R, R, R, 'r4 c#5/8t b4/8t a4/8t e4/4',
         R, R, R, R, R, R, R, 'r4 f#4/8t g4/8t a4/8t a4/4']
TB2_B2 = ['g3/2 d4/4', 'd4/4. b3/8 b3/4', 'a3/2 e4/4', 'e4/4. c#4/8 c#4/4',
          'b3/2 f#4/4', 'f#4/4. d4/8 d4/4', 'c4/2 g4/4', 'a3/8t a3/8t a3/8t d4/8t d4/8t d4/8t f#4/4']
TB2_A3 = ['g#3/2 e4/4', 'd#4/4. c#4/8 d#4/4', 'e4/2 g#4/4', 'g#4/2.',
          'a4/2 e4/4', 'e4/4. d#4/8 b3/4', 'f#4/2 e4/4', 'd#4/2.',
          'g#3/2 e4/4', 'd#4/4. c#4/8 d#4/4', 'e4/2 g#4/4', 'a4/4. b4/8 c#5/4',
          'e5/2 c#5/4', 'g#4/2 e4/4', 'e4/2 d#4/4', 'e4/2.']
TROMBONE2 = track(['v104 q7 ' + INTRO_TB2[0]] + INTRO_TB2[1:],
                  TB2_A + [R] * 8 +
                  ['v96 ' + TB2_B2[0]] + TB2_B2[1:4] + ['v102 ' + TB2_B2[4]] + TB2_B2[5:7] + ['v110 ' + TB2_B2[7]] +
                  ['v108 ' + TB2_A3[0]] + TB2_A3[1:] +
                  ['v104 ' + LINK_TB2[0], LINK_TB2[1]] + INTRO_TB2[4:])

# ---------------------------------------------------------------------------- violas: chords, afterbeats
VLA_A = ['[d4 f#4 a4]/2.', '[c#4 e4 a4]/2.', '[d4 f#4 b4]/2.', '[d4 f#4 b4]/2.',
         '[d4 g4 b4]/2.', '[d4 f#4 a4]/2.', '[d4 g4 b4]/2.', '[c#4 e4 g4]/2.',
         '[d4 f#4 a4]/2.', '[c#4 e4 a4]/2.', '[d4 f#4 b4]/2.', '[d4 g4 b4]/2.',
         '[d4 f4 g#4 b4]/2.', '[d4 f#4 a4]/2.', '[d4 e4 g4]/2 [c#4 e4 g4]/4', '[d4 f#4 a4]/2.']
VLA_B1 = ['[d4 f#4 b4]/2.', '[d4 f#4 a4]/2.', '[e4 g4 b4]/2.', '[e4 a#4 c#5]/2.',
          '[d4 f#4 b4]/2.', '[d4 g4 b4]/2.', '[d4 g4 b4]/2.', '[e4 a#4 c#5]/2.']
VLA_B2 = ['[g4 b4]/2.', '[g4 b4]/2.', '[a4 c#5]/2.', '[a4 c#5]/2.',
          '[b4 d5]/2.', '[a4 d5]/2.', '[c5 e5]/2.', '[a4 d5]/2.']
VLA_A3 = [wz('g#4 b4'), wz('f#4 b4'), wz('g#4 c#5'), wz('g#4 c#5'), wz('a4 c#5'), wz('g#4 b4'),
          wz('a4 c#5'), wz('a4 d#5'), wz('g#4 b4'), wz('f#4 b4'), wz('g#4 c#5'), wz('a4 c#5'),
          wz('g4 c#5'), wz('g#4 b4'), wz('a4 e5', 'a4 d#5'), wz('g#4 b4')]
VLA_INTRO = ['[d4 f#4 a4]/2.', '[d4 g4 b4]/2.', '[d4 f4 bb4]/2.', '[e4 g4 c5]/2.', '[d4 e4 g4]/2.',
             '[e4 g4 c#5]/8 r8 r4 r4']
VIOLAS = track(['v100 ' + VLA_INTRO[0]] + VLA_INTRO[1:4] + ['v104 ' + VLA_INTRO[4], 'v112 ' + VLA_INTRO[5]],
               ['v84 ' + VLA_A[0]] + VLA_A[1:] +
               ['v88 ' + VLA_B1[0]] + VLA_B1[1:] +
               ['v96 ' + VLA_B2[0]] + VLA_B2[1:] +
               ['v108 q6 ' + VLA_A3[0]] + VLA_A3[1:] +
               ['v100 q8 [g#4 b4 e5]/2.', '[g4 b4 d5]/2.', 'v104 ' + VLA_INTRO[4], 'v112 ' + VLA_INTRO[5]])

# ---------------------------------------------------------------------------- cellos and basses (octaves)
BASS_A = ['D/2.', 'C#/2.', 'B/2.', 'A/2.', 'G/2.', 'F#/2.', 'E/2.', 'A/2.',
          'D/2.', 'C#/2.', 'B/2.', 'G/2.', 'G#/2.', 'A/2.', 'A/2.', 'D/2.']
BASS_B1 = ['B/2.', 'A/2.', 'G/2.', 'F#/2.', 'B/2.', 'G/2.', 'E/2.', 'F#/2.']
BASS_B2 = ['G/2.', 'G/2.', 'A/2.', 'A/2.', 'B/2.', 'B/2.', 'C/2.', 'D/2.']
BASS_A3 = [b.replace('/2.', '/2 r4') for b in BASS_A]
BASSES = track(bass(['v112 D/2.', 'D/2.', 'Bb/2.', 'C/2.', 'A/2.', 'A/8 r8 r4 r4']),
               bass(['v100 ' + BASS_A[0]] + BASS_A[1:] + ['v96 ' + BASS_B1[0]] + BASS_B1[1:] +
                    ['v108 ' + BASS_B2[0]] + BASS_B2[1:]) +
               ['v118 ' + tr(bass(BASS_A3[0]), 2)] + tr(bass(BASS_A3[1:]), 2) +
               bass(['v112 E/2.', 'E/2.', 'A/2.', 'A/8 r8 r4 r4']))

# ---------------------------------------------------------------------------- harp
# (arpeggios ring, {release 118}; glissandi get a short release, {release 125})
HARP_A = [arp('d3 a3 d4 f#4 a4 f#4'), arp('c#3 a3 c#4 e4 a4 e4'), arp('b2 f#3 b3 d4 f#4 d4'),
          arp('a2 f#3 b3 d4 f#4 d4'), arp('g2 d3 g3 b3 d4 b3'), arp('f#2 d3 f#3 a3 d4 a3'),
          arp('e2 b2 e3 g3 d4 g3'), arp('a2 e3 g3 c#4 e4 c#4'),
          arp('d3 a3 d4 f#4 a4 f#4'), arp('c#3 a3 c#4 e4 a4 e4'), arp('b2 f#3 b3 d4 f#4 d4'),
          arp('g2 d3 g3 b3 d4 b3'), arp('g#2 d3 f3 b3 d4 b3'), arp('a2 d3 f#3 a3 d4 a3'),
          arp('a2 e3 a3 d4 c#4 e4'), arp('d3 a3 d4 f#4 a4 d5')]
HARP_B1 = [arp('b2 f#3 b3 d4 f#4 d4'), arp('a2 f#3 b3 d4 f#4 d4'), arp('g2 e3 g3 b3 e4 b3'),
           arp('f#2 c#3 e3 a#3 c#4 a#3'), arp('b2 f#3 b3 d4 f#4 b4'), arp('g2 d3 g3 b3 d4 g4'),
           arp('e2 b2 e3 g3 d4 g4'), arp('f#2 c#3 e3 a#3 c#4 e4')]
GLISS_A7 = 'r4 {release 125} ' + run('e4 f#4 g4 a4 b4 c#5 d5 e5 f#5 g5 a5 b5 c#6 d6 e6 f#6', 32) + ' {release 118}'
GLISS_D = 'r4 {release 125} ' + run('d4 e4 f#4 g4 a4 b4 c5 d5 e5 f#5 g5 a5 b5 c6 d6 e6', 32) + ' {release 118}'
HARP_INTRO = [arp('d3 a3 d4 f#4 a4 d5'), arp('d3 g3 b3 d4 g4 b4'), arp('bb2 f3 bb3 d4 f4 bb4'),
              arp('c3 g3 c4 e4 g4 c5'), arp('a2 e3 a3 d4 e4 g4'), 'v104 ' + GLISS_A7]
HARP = track(['v96 ' + HARP_INTRO[0]] + HARP_INTRO[1:],
             ['v90 ' + HARP_A[0]] + HARP_A[1:] +
             ['v92 ' + HARP_B1[0]] + HARP_B1[1:] +
             [R] * 7 + ['v104 ' + GLISS_D] +
             ['v100 ' + tr(HARP_A[0], 2)] + tr(HARP_A[1:], 2) +
             ['v100 ' + arp('e3 b3 e4 g#4 b4 e5'), arp('e3 b3 d4 g4 b4 e5'), 'v96 ' + HARP_INTRO[4],
              HARP_INTRO[5]])

# ---------------------------------------------------------------------------- timpani
# (rolls: struck every 32nd with a short release; single strokes ring)


def troll(n, beats, v0, v1):
    return '{release 125} ' + roll(n, beats, v0, v1) + ' {release 122}'


TIMP_A = ['v100 d3/2.', R, R, R, 'g2/2.', R, R, 'a2/4 r4 v88 a2/8t a2/8t a2/8t',
          'v104 d3/2.', R, R, 'g2/2.', R, 'a2/2.', 'a2/4 a2/4 a2/4', 'd3/4 r4 r4']
TIMP_I6 = 'v112 a2/8 r8 ' + troll('a2', 2, 30, 110)
TIMPANI = track(['v100 d3/8t d3/8t d3/8t v116 a2/2', 'v100 d3/8t d3/8t d3/8t v116 g2/2', 'v116 bb2/2.', 'c3/2.',
                 'a2/2.', TIMP_I6],
                TIMP_A +
                ['v72 b2/2.', R, R, 'f#2/2.', 'b2/2.', R, R, 'f#2/2.'] +
                ['v100 g2/2.', R, 'a2/2.', R, 'v108 b2/2.', R, 'c3/4 c3/4 c3/4', troll('d3', 3, 40, 122)] +
                ['v120 ' + tr(TIMP_A[0][5:], 2)] + tr(TIMP_A[1:], 2) +
                ['v100 e3/8t e3/8t e3/8t v116 b2/2', 'v100 e3/8t e3/8t e3/8t v112 b2/4 e3/4', 'v116 a2/2.', TIMP_I6])

# ---------------------------------------------------------------------------- snare (prg 98: d1, d4 roll)
SN_A3 = 'v100 d1/8 v64 d1/16 d1/16 v80 d1/4 v76 d1/4'
SN_I6 = 'v112 d1/8 r8 v70 d4/4 v100 d4/4'
SNARE = track(['v76 d4/4 v104 d1/8 r8 r4', 'v76 d4/4 v104 d1/8 r8 r4', 'v88 d4/2.', 'v100 d4/2.', R, SN_I6],
              [R] * 16 + [R] * 8 +
              ['v60 d1/8 d1/16 d1/16 d1/4 d1/4', 'v64 d1/8 d1/16 d1/16 d1/4 d1/4',
               'v68 d1/8 d1/16 d1/16 d1/4 d1/4', 'v72 d1/8 d1/16 d1/16 d1/4 d1/4',
               'v78 d1/8 d1/16 d1/16 d1/4 d1/4', 'v84 d1/8 d1/16 d1/16 d1/4 d1/4',
               'v72 d4/4 v84 d4/4 v96 d4/4', 'v104 d4/4 v114 d4/4 v124 d4/4'] +
              [SN_A3] * 7 + ['v90 d4/4 v100 d4/4 v110 d4/4'] + [SN_A3] * 6 + ['v90 d4/4 v100 d4/4 v110 d4/4'] +
              ['v112 d1/8 r8 r4 r4'] +
              ['v76 d4/4 v104 d1/8 r8 r4', 'v76 d4/4 v104 d1/8 r8 r4', R, SN_I6])

# ---------------------------------------------------------------------------- bass drum (prg 98: c0)
BASSDRUM = track(['v124 c0/2.', 'v116 c0/2.', 'v124 c0/2.', 'c0/2.', 'v116 c0/2.', 'v120 c0/8 r8 r4 r4'],
                 ['v84 c0/2.', R, R, R, 'v76 c0/2.', R, R, R, 'v88 c0/2.', R, R, R, 'v84 c0/2.', R, R, R] +
                 ['v72 c0/2.', R, R, R, 'c0/2.', R, R, R] +
                 ['v96 c0/2.', 'v90 c0/2.', 'v100 c0/2.', 'v94 c0/2.', 'v106 c0/2.', 'v100 c0/2.',
                  'v108 c0/4 c0/4 c0/4', 'v116 c0/4 c0/4 c0/4'] +
                 ['v124 c0/4 r4 r4'] + ['v108 c0/4 r4 r4'] * 7 + ['v124 c0/4 r4 r4'] + ['v108 c0/4 r4 r4'] * 6 +
                 ['v120 c0/4 r4 r4'] +
                 ['v120 c0/2.', 'v112 c0/2.', 'v116 c0/2.', 'v120 c0/8 r8 r4 r4'])


# ---------------------------------------------------------------------------- cymbals
# crashes on the drum kit's crash (prg 95, c#7); swells on the orchestral suspended cymbal (prg 98)
def swell(beats, v0, v1):
    return '@98 {release 125} ' + roll('c#7', beats, v0, v1, div=12) + ' @95 {release 116}'


CYMBALS = track(['v120 c#7/2.', R, 'v124 c#7/2.', 'v120 c#7/2.', R, R],
                ['v108 c#7/2.', R, R, R, R, R, R, 'r4 ' + swell(2, 30, 90),
                 'v100 c#7/2.', R, R, 'r4 ' + swell(2, 30, 96), 'v104 c#7/2.', R, R, R] +
                [R] * 7 + ['r4 ' + swell(2, 30, 100)] +
                ['v112 c#7/2.', R, R, R, 'v116 c#7/2.', R, R, swell(3, 30, 120)] +
                ['v127 c#7/2.', R, R, R, 'v108 c#7/2.', R, R, R,
                 'v120 c#7/2.', R, R, R, 'v116 c#7/2.', R, R, 'v124 c#7/2.'] +
                ['v112 c#7/2.', R, R, R])

# ---------------------------------------------------------------------------- flute
FLUTE = track([R, R, 'v96 f6/2.', 'g6/2.', 'a6/2.', 'g6/8 r8 r4 r4'],
              [R] * 8 + ['v92 ' + MEL_A[8]] + MEL_A[9:] +
              ['v100 ' + MEL_B1[0]] + MEL_B1[1:] +
              ['v96 ' + MEL_B2[0]] + MEL_B2[1:4] + ['v104 ' + MEL_B2[4]] + MEL_B2[5:7] + ['{release 124} ' + MEL_B2[7]] +
              ['{release 118} v108 ' + MEL_A3[0]] + MEL_A3[1:] +
              ['v100 e6/2.', 'd6/2.', 'v96 a6/2.', 'g6/8 r8 r4 r4'])

# ---------------------------------------------------------------------------- glockenspiel (sounding pitch)
GLOCK = track([R, R, 'v100 d7/2.', 'e7/2.', R, 'c#7/8 r8 r4 r4'],
              [R] * 11 + ['v92 ' + tr(MEL_A[11], 12), tr(MEL_A[12], 12)] + [R] * 3 +
              [R] * 8 +
              [R] * 8 +
              ['v100 ' + tr(MEL_A3[0], 12), tr(MEL_A3[1], 12), R, R, tr(MEL_A3[4], 12), tr(MEL_A3[5], 12), R, R,
               R, R, R, tr(MEL_A3[11], 12), tr(MEL_A3[12], 12), tr(MEL_A3[13], 12), R, R] +
              ['v100 e7/4 r4 r4', R, R, 'c#7/8 r8 r4 r4'])

SCORE = {
    'title': '広い世界へ',
    'label': 'SEQ_BGM_HIROI_SEKAI',
    'timebase': TB,
    'tempo': 108,
    'main_volume': 106,
    'bar': BAR,
    'loop': True,
    'tracks': [
        {'name': 'violins', 'program': 48, 'volume': 127, 'pan': 34, 'release': 118, 'mml': VIOLINS},
        {'name': 'trombone (lead)', 'program': 57, 'volume': 112, 'pan': 80, 'release': 122, 'mml': LEAD,
         'echo': {'delay': 32, 'copies': [{'pan': 20, 'volume': 46}]}},
        {'name': 'horn', 'program': 60, 'volume': 82, 'pan': 44, 'release': 122, 'mml': HORN},
        {'name': 'trombone 2', 'program': 57, 'volume': 86, 'pan': 100, 'release': 122, 'mml': TROMBONE2},
        {'name': 'violas', 'program': 48, 'volume': 100, 'pan': 90, 'release': 122, 'mml': VIOLAS},
        {'name': 'cellos and basses', 'program': 48, 'volume': 88, 'pan': 64, 'release': 122, 'mml': BASSES},
        {'name': 'harp', 'program': 46, 'volume': 104, 'pan': 22, 'release': 118, 'mml': HARP},
        {'name': 'timpani', 'program': 47, 'volume': 92, 'pan': 56, 'release': 122, 'mml': TIMPANI},
        {'name': 'snare', 'program': 98, 'volume': 92, 'pan': 70, 'release': 124, 'mml': SNARE},
        {'name': 'bass drum', 'program': 98, 'volume': 127, 'pan': 60, 'release': 118, 'mml': BASSDRUM},
        {'name': 'cymbals', 'program': 95, 'volume': 127, 'pan': 78, 'release': 116, 'mml': CYMBALS},
        {'name': 'flute', 'program': 73, 'volume': 80, 'pan': 52, 'release': 118, 'mml': FLUTE},
        {'name': 'glockenspiel', 'program': 9, 'volume': 70, 'pan': 88, 'release': 108, 'sounding': True,
         'mml': GLOCK},
    ],
}

# The chords bar by bar from the piece's start, for checks: a tuple of (chord, beats) pairs splits
# the bar.
_A = ['D', 'A/C#', 'Bm', 'Bm/A', 'G', 'D/F#', 'Em7', 'A7',
      'D', 'A/C#', 'Bm', 'G', 'G#dim7', 'D/A', (('A7sus4', 2), ('A7', 1)), 'D']
_B1 = ['Bm', 'Bm/A', 'Em/G', 'F#7', 'Bm', 'G', 'Em7', 'F#7']
_B2 = ['G', 'Em7/G', 'A', 'F#m7/A', 'Bm', 'Bm7', 'C', 'D']
_A3 = ['E', 'B/D#', 'C#m', 'C#m/B', 'A', 'E/G#', 'F#m7', 'B7',
       'E', 'B/D#', 'C#m', 'A', 'A#dim7', 'E/B', (('B7sus4', 2), ('B7', 1)), 'E']
CHORDS = ['D', 'G/D', 'Bb', 'C', 'A7sus4', 'A7'] + _A + _B1 + _B2 + _A3 + ['E', 'Em7', 'A7sus4', 'A7']

# The sections (name, first bar from the piece's start, bars), for checks: the fanfare, then the
# loop's (A and A' by their halves).
INTRO_BARS = 6
SECTIONS = [('intro', 0, INTRO_BARS)] + [(name, INTRO_BARS + first, bars) for name, first, bars in (
    ('A', 0, 8), ('A2', 8, 8), ('B1', 16, 8), ('B2', 24, 8), ("A'", 32, 8), ("A'2", 40, 8), ('L', 48, 4))]

# The fanfare's last bars are the loop's last bars (so the loop is always entered the same way).
SEAM_BARS = 2
