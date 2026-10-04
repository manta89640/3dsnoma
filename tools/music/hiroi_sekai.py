# 「広い世界へ」 (Hiroi Sekai e, "Into the Wide World"): the music of the port's Wuhu Island, our own
# composition in the manner of Wii no Ma's own music, for the channel's instruments (BANK_BGM_ALL in
# snd/douga.brsar). A score for tools/music/rseq.py:
#
#   python3 tools/music/rseq.py compile tools/music/hiroi_sekai.py -o hiroi_sekai.rseq --wav preview.wav
#   python3 tools/music/rseq.py header tools/music/hiroi_sekai.py -o engine/wuhu/hiroi_sekai_seq.h
#   python3 tools/music/check.py tools/music/hiroi_sekai.py        (voices, harmony, loop seam, levels)
#
# D major, 4/4, 104 quarter notes a minute: a walking pace, between the room's themes (65 and 70)
# and the channel's livelier pieces (108 to 150). The player hears it for as long as they walk, so
# the loop is long (42 bars, 97 s) and changes its colour and weight as it goes:
#
#   bars  1-2   intro (once)  guitar, a string swell, the glockenspiel; the flute's pickup
#   bars  3-10  A    quiet    the flute's tune (with the channel's left/right echo copies) over the
#                             fingerpicked guitar, the finger bass and a light hi-hat
#   bars 11-18  A'            the tune again, ending home: strings and the piano join, the kit plays
#   bars 19-26  B    full     the chorus, the flute up high over the accordion's counter-line,
#                             strings, the piano's arpeggios, glockenspiel, the whole kit
#   bars 27-34  C    breath   the flute rests: the piano sings a quiet tune in B minor over the
#                             electric piano, the bass and the hi-hat alone
#   bars 35-42  A''           the tune at its fullest: glockenspiel doubling it, the recorder's
#                             descant, strings, guitar, the kit
#   bars 43-44  tag           bVI - bVII (Bbmaj7, Cadd9), and home to D where the loop starts again
#
# The harmony keeps the channel's habits (research: audio.md §4): a bass stepping down under
# I - V/3 - vi7 - vi7/5 - IVmaj7, maj7 and add9 colour, secondary dominants (B7 = V/ii), V sus4
# resolving to V7, a deceptive cadence (V - vi) into the B minor interlude.
#
# Notation: notes are NAME OCTAVE / LENGTH (`f+5/4.` is F#5, a dotted quarter; c4 is key 60),
# chords [d4 f+4 a4]/2; everything else is rseq.Mml's own: r rests, ^ ties, | bar lines (checked),
# $ the loop point, s1 ... s0 let ring (to the next | or !), ! a ring boundary, {commands}, v
# velocity, q gate (eighths of the written length).
import re

TB = 96  # ticks a quarter note (the channel's music always uses 96)

# The chords bar by bar, by section (a pair where the chord changes on beat 3), for check.py.
SECTION_CHORDS = [
    ('intro', ['Dadd9', 'A7sus4']),
    ('A', ['D', 'A/C#', 'Bm7', 'Bm7/A', 'Gmaj7', 'F#m7', 'Em7', ('A7sus4', 'A7')]),
    ("A'", ['D', 'A/C#', 'Bm7', 'Bm7/A', 'Gmaj7', ('F#m7', 'B7'), ('Em7', 'A7'), 'D']),
    ('B', ['Gmaj7', 'A/G', 'F#m7', 'B7', 'Em7', 'F#m7', 'Gmaj7', ('A7sus4', 'A7')]),
    ('C', ['Bm7', 'Bm7/A', 'Gmaj7', 'D/F#', 'Em7', 'F#m7', 'Gmaj7', 'A7sus4']),
    ("A''", ['D', 'A/C#', 'Bm7', 'Bm7/A', 'Gmaj7', ('F#m7', 'B7'), ('Em7', 'A7'), 'D']),
    ('tag', ['Bbmaj7', 'Cadd9']),
]
CHORDS = [c for _, cs in SECTION_CHORDS for c in cs]
SECTIONS = []  # (name, first bar (from 0), bars)
_bar = 0
for _name, _cs in SECTION_CHORDS:
    SECTIONS.append((_name, _bar, len(_cs)))
    _bar += len(_cs)


def m(text):
    """'f+5/4. [d4 f+4 a4]/1 r4 |' in rseq.Mml, with explicit octaves."""
    out = []
    for tok in re.findall(r'\[[^\]]*\]/\S+|\{[^}]*\}|\S+', text):
        if tok.startswith('['):
            notes, ln = tok[1:].split(']/')
            out.append('[' + ' '.join(_note(n) for n in notes.split()) + ']' + ln)
        elif re.match(r'^[a-g][+#-]*-?\d', tok):
            name, _, ln = tok.partition('/')
            out.append(_note(name) + ln)
        else:
            out.append(tok)
    return ' '.join(out)


def _note(n):
    mm = re.match(r'^([a-g][+#-]*)(-?\d)$', n)
    if not mm:
        raise ValueError('bad note %r' % n)
    return 'o%s%s' % (mm.group(2), mm.group(1))


def rest(bars):
    return ' r1 |' * bars


def arp(x1, x2, x3, x4):
    """A bar of fingerpicked guitar: x1 x2 x3 x4 x2 x3 x4 x3 in eighths."""
    return m(' '.join('%s/8' % n for n in (x1, x2, x3, x4, x2, x3, x4, x3))) + ' |'


def arp_half(a, b):
    """A bar of two chords, four eighths each (a ring boundary between them)."""
    return m(' '.join('%s/8' % n for n in a)) + ' ! ' + m(' '.join('%s/8' % n for n in b)) + ' |'


# ---------------------------------------------------------------- the flute (the tune)
A_HEAD = """
 f+5/4. e5/8 d5/8 e5/8 f+5/8 a5/8 | ^2. e5/8 f+5/8 |
 d5/4. c+5/8 b4/8 c+5/8 d5/8 f+5/8 | ^2. r8 a4/8 |"""
A_TAIL = """
 b4/8 d5/8 g5/4. f+5/8 e5/8 d5/8 | c+5/4. a4/8 ^4 r8 c+5/8 |
 b4/8 c+5/8 d5/8 e5/4 g5/8 f+5/8 e5/8 | e5/2 a4/4 c+5/8 e5/8 |"""
A2_TAIL = """
 b4/8 d5/8 g5/4. f+5/8 e5/8 d5/8 | c+5/4 a5/4. g5/8 f+5/8 d+5/8 |
 e5/4. f+5/8 g5/8 f+5/8 e5/8 c+5/8 | d5/2. r4 |"""
# B: the chorus, broad and high (a sixth's leap up into it)
B_TUNE = """
 b5/2. a5/8 b5/8 | c+6/4. b5/8 a5/2 | a5/4 f+5/4 c+6/4. b5/8 | a5/2 f+5/4 d+5/4 |
 g5/2. f+5/8 g5/8 | a5/4. e5/8 c+6/2 | b5/4. a5/8 b5/4 d6/4 | e6/2 c+6/4 a5/4 |"""
TAG_TUNE = """
 d5/4 f5/4 a5/2 | g5/2 e5/4 d5/8 e5/8 |"""

FLUTE = m("""
 v88 r1 | r2 r4 d5/8 e5/8 | $
 v86""" + A_HEAD + A_TAIL + """
 v94""" + A_HEAD + A2_TAIL + """
 v104""" + B_TUNE + rest(7) + """
 r2 r4 v92 d5/8 e5/8 |
 v100""" + A_HEAD + A2_TAIL + """
 v100""" + TAG_TUNE)

# ---------------------------------------------------------------- the piano: A''s soft chords, B's
# arpeggios, C's tune
PIANO = m(' r1 | r1 | $' + rest(8) + """
 v60 [a3 f+4]/2 [a3 d4]/2 | [a3 e4]/2 [a3 c+4]/2 | [a3 d4]/2 [f+3 b3]/2 |
 [f+3 d4]/2 [f+3 b3]/2 | [b3 f+4]/2 [b3 d4]/2 | [a3 c+4]/2 [a3 d+4]/2 |
 [g3 d4]/2 [g3 c+4]/2 | [a3 f+4]/1 |
 v66 q12 g2/8 d3/8 b3/8 f+4/8 a4/8 f+4/8 d4/8 b3/8 | g2/8 e3/8 c+4/8 a4/8 e5/8 a4/8 c+4/8 e3/8 |
 f+2/8 c+3/8 a3/8 e4/8 c+5/8 e4/8 a3/8 c+3/8 | b1/8 f+2/8 d+3/8 a3/8 f+4/8 a3/8 d+3/8 f+2/8 |
 e2/8 b2/8 g3/8 d4/8 b4/8 d4/8 g3/8 b2/8 | f+2/8 c+3/8 a3/8 e4/8 c+5/8 e4/8 a3/8 c+3/8 |
 g2/8 d3/8 b3/8 f+4/8 d5/8 f+4/8 b3/8 d3/8 | a2/8 e3/8 a3/8 d4/8 ! a2/8 e3/8 g3/8 c+4/8 |
 q8 v100 f+5/4. e5/8 d5/2 | c+5/4. d5/8 e5/2 | d5/4. c+5/8 b4/2 | a4/2. r4 |
 g4/4 b4/4 e5/4 g5/4 | f+5/2 e5/4 c+5/4 | d5/4 b4/4 f+5/4 d5/4 | e5/2 a4/2 |""" + rest(8) + """
 v64 [a+2 f3 d4]/1 | [c3 g3 e4]/1 |""")

# ---------------------------------------------------------------- the electric piano (C)
EPIANO = m(' r1 | r1 | $' + rest(24) + """
 v56 [a3 d4 f+4]/2 [a3 d4 f+4]/2 | [b3 d4 f+4]/2 [a3 d4 e4]/2 | [f+3 b3 d4]/2 [f+3 b3 d4]/2 |
 [f+3 a3 d4]/2 [f+3 a3 e4]/2 | [g3 b3 d4]/2 [g3 b3 e4]/2 | [e3 a3 c+4]/2 [e3 a3 c+4]/2 |
 [f+3 b3 d4]/2 [g3 b3 e4]/2 | [g3 d4 e4]/1 |""" + rest(8) + rest(2))

# ---------------------------------------------------------------- the accordion: B's counter-line
ACCORDION = m(' r1 | r1 | $' + rest(16) + """
 v88 d5/1 | e5/2 c+5/2 | c+5/1 | d+5/2 b4/2 |
 b4/1 | a4/2 c+5/2 | d5/2 f+5/2 | d5/2 c+5/2 |""" + rest(16) + """
 v76 f4/1 | e4/1 |""")

# ---------------------------------------------------------------- the glockenspiel (sounding pitch)
GLOCK = m("""
 v96 f+6/8 a6/8 e7/4 d7/2 | b6/8 d7/8 e7/2. | $""" + rest(16) + """
 v100 b6/2 r2 | r1 | c+7/2 r2 | r1 | g6/2 r2 | r1 | b6/4. a6/8 b6/4 d7/4 | e7/2 r2 |
 r1 | r1 | r1 | r2 r4 v70 a6/4 | r1 | r1 | r1 | r1 |
 v80 f+6/4. e6/8 d6/8 e6/8 f+6/8 a6/8 | ^2. r4 | r1 | r1 |
 b5/8 d6/8 g6/4. f+6/8 e6/8 d6/8 | c+6/4 a6/4. g6/8 f+6/8 d+6/8 | r1 | r2 d6/8 a6/8 d7/4 |
 v88 d6/8 f6/8 a6/8 d7/8 f7/2 | e6/8 g6/8 c7/8 d7/8 e7/2 |""")

# ---------------------------------------------------------------- the recorder's descant (sounding), A''
RECORDER = m(' r1 | r1 | $' + rest(32) + """
 v76 d6/1 | c+6/1 | b5/1 | a5/2 b5/4 c+6/4 |
 d6/2. c+6/8 b5/8 | c+6/2 d+6/2 | e6/2 c+6/2 | d6/2. r4 |""" + rest(2))

# ---------------------------------------------------------------- the guitar (fingerpicked, let ring)
D = ('d3', 'a3', 'd4', 'f+4')
A_C = ('c+3', 'a3', 'c+4', 'e4')
BM7 = ('b2', 'f+3', 'a3', 'd4')
BM7_A = ('a2', 'f+3', 'b3', 'd4')
GMA7 = ('g2', 'd3', 'f+3', 'b3')
FSM7 = ('f+2', 'c+3', 'e3', 'a3')
EM7 = ('e3', 'b3', 'd4', 'g4')
A7SUS = ('a2', 'e3', 'a3', 'd4')
A7 = ('a2', 'e3', 'g3', 'c+4')
B7 = ('b2', 'f+3', 'a3', 'd+4')
BBMA7 = ('a+2', 'f3', 'a3', 'd4')
CADD9 = ('c3', 'g3', 'd4', 'e4')
DADD9 = ('d3', 'a3', 'e4', 'f+4')
A9SUS = ('a2', 'g3', 'b3', 'd4')

GUITAR_A = ' '.join([arp(*D), arp(*A_C), arp(*BM7), arp(*BM7_A), arp(*GMA7), arp(*FSM7), arp(*EM7),
                     arp_half(A7SUS, A7)])
GUITAR_A2 = ' '.join([arp(*D), arp(*A_C), arp(*BM7), arp(*BM7_A), arp(*GMA7), arp_half(FSM7, B7),
                      arp_half(EM7, A7), arp(*D)])
GUITAR = ('s1 v80 ' + arp(*DADD9) + ' ' + arp(*A9SUS) + ' $ ' +
          'v76 ' + GUITAR_A + ' v82 ' + GUITAR_A2 + rest(16) + ' v84 ' + GUITAR_A2 + ' ' +
          arp(*BBMA7) + ' ' + arp(*CADD9))

# ---------------------------------------------------------------- the finger bass (sounding pitch)
BASS = m("""
 v96 r1 | a1/1 | $
 v92 d2/2. a1/4 | c+2/2. a1/4 | b1/2. f+1/4 | a1/2. e1/4 |
 g1/2. d2/4 | f+1/2. c+2/4 | e1/2. b1/4 | a1/2 a1/4 c+2/4 |
 v100 d2/2 f+2/4 a2/4 | c+2/2 e2/4 a2/4 | b1/2 d2/4 f+2/4 | a1/2 e2/4 a1/4 |
 g1/2 d2/4 g2/4 | f+1/4 c+2/4 b1/4 d+2/4 | e2/4 b1/4 a1/4 c+2/4 | d2/2 a1/4 f+1/4 |
 v108 g1/4 d2/4 g2/4 d2/4 | g1/4 e2/4 g2/4 e2/4 | f+1/4 c+2/4 f+2/4 c+2/4 | b1/4 f+2/4 a2/4 d+2/4 |
 e2/4 b1/4 e1/4 g1/4 | f+1/4 c+2/4 f+2/4 a1/4 | g1/4 d2/4 g2/4 b1/4 | a1/4 e2/4 a1/4 c+2/4 |
 v88 b1/2 f+2/2 | a1/2 e2/2 | g1/2 d2/2 | f+1/2 a1/2 |
 e1/2 b1/2 | f+1/2 c+2/2 | g1/2 d2/2 | a1/2 e1/4 a1/4 |
 v104 d2/2 f+2/4 a2/4 | c+2/2 e2/4 a2/4 | b1/2 d2/4 f+2/4 | a1/2 e2/4 a1/4 |
 g1/2 d2/4 g2/4 | f+1/4 c+2/4 b1/4 d+2/4 | e2/4 b1/4 a1/4 c+2/4 | d2/2 a1/4 f+1/4 |
 a+1/2 f2/4 a+1/4 | c2/2 g1/4 c2/4 |""")

# ---------------------------------------------------------------- the strings (pads)
STRINGS = m("""
 {volume 24; volume 72, 384} v84 [d4 f+4 a4]/1 | [d4 e4 g4 b4]/1 | $
 {volume 72}""" + rest(8) + """
 v76 [d4 f+4 a4]/1 | [c+4 e4 a4]/1 | [b3 d4 f+4]/1 | [a3 d4 f+4]/1 |
 [b3 d4 f+4]/1 | [a3 c+4 e4]/2 [a3 d+4 f+4]/2 | [g3 b3 e4]/2 [g3 c+4 e4]/2 | [f+3 a3 d4]/1 |
 v92 [b3 d4 f+4]/1 | [a3 c+4 e4]/1 | [a3 c+4 e4]/1 | [a3 d+4 f+4]/1 |
 [g3 b3 d4]/1 | [a3 c+4 e4]/1 | [b3 d4 f+4]/1 | [a3 d4 e4]/2 [g3 c+4 e4]/2 |""" + rest(8) + """
 v92 [d4 f+4 a4]/1 | [c+4 e4 a4]/1 | [b3 d4 f+4]/1 | [a3 d4 f+4]/1 |
 [b3 d4 f+4]/1 | [a3 c+4 e4]/2 [a3 d+4 f+4]/2 | [g3 b3 e4]/2 [g3 c+4 e4]/2 | [f+3 a3 d4]/1 |
 v96 [a3 d4 f4]/1 | [g3 c4 e4]/1 |""")

# ---------------------------------------------------------------- the kit (prg 95): kick c0, snare d1,
# toms f4..c5, crash c+7; the hi-hats (closed f+2, open a+3) on their own track
K_A = m('v64 c0/2 r2 |')
K_A2 = m('v80 c0/4 v56 d1/4 v70 c0/4 v58 d1/4 |')
K_B = m('v100 c0/4 v88 d1/4 v70 c0/8 v90 c0/8 v88 d1/4 |')
K_A3 = m('v96 c0/4 v82 d1/4 v66 c0/8 v86 c0/8 v82 d1/4 |')
DRUMS = (m('r1 | r2. v48 d1/16 v54 d1/16 v60 d1/8 | $') + ' ' + ' '.join([K_A] * 8) + ' ' +
         ' '.join([K_A2] * 7) + ' ' + m('v80 c0/4 v56 d1/4 v66 d1/8 v70 d1/8 v76 d1/8 v84 d1/8 |') + ' ' +
         m('v96 [c0 c+7]/4 v80 d1/4 v66 c0/8 v84 c0/8 v80 d1/4 |') + ' ' + ' '.join([K_B] * 6) + ' ' +
         m('v92 c0/4 v80 d1/4 v66 c0/4 r4 |') + ' ' +
         rest(7) + ' ' + m('r2 v60 c5/8 v62 a4/8 v66 f4/8 v72 d1/8 |') + ' ' +
         m('v92 [c0 c+7]/4 v74 d1/4 v62 c0/8 v80 c0/8 v74 d1/4 |') + ' ' + ' '.join([K_A3] * 7) + ' ' +
         m('v90 [c0 c+7]/4 v70 d1/4 v76 c0/4 v70 d1/4 | v80 c0/4 v66 d1/4 v72 c0/4 v54 d1/8 v60 d1/8 |'))
HH_A = m('v64 f+2/4 v46 f+2/4 v64 f+2/4 v46 f+2/4 |')
HH_8 = m('v72 f+2/8 v50 f+2/8 v72 f+2/8 v50 f+2/8 v72 f+2/8 v50 f+2/8 v72 f+2/8 v50 f+2/8 |')
HH_B = m('v86 f+2/8 v60 f+2/8 v86 f+2/8 v60 f+2/8 v86 f+2/8 v60 f+2/8 v86 f+2/8 v76 a+3/8 |')
HH_C = m('v54 f+2/4 v38 f+2/4 v54 f+2/4 v38 f+2/4 |')
HIHAT = (m('r1 | v36 f+2/8 v42 f+2/8 v48 f+2/8 v54 f+2/8 v60 f+2/8 v66 f+2/8 v72 f+2/8 v78 f+2/8 | $') + ' ' +
         ' '.join([HH_A] * 8) + ' ' + ' '.join([HH_8] * 8) + ' ' + ' '.join([HH_B] * 8) + ' ' +
         ' '.join([HH_C] * 7) + ' ' + m('v54 f+2/4 v38 f+2/4 r2 |') + ' ' + ' '.join([HH_B] * 8) + ' ' +
         HH_8 + ' ' + m('v72 f+2/4 v54 f+2/4 v64 f+2/4 r4 |'))

SCORE = {
    'title': '広い世界へ',
    'label': 'SEQ_BGM_HIROI_SEKAI',
    'timebase': TB,
    'tempo': 104,
    'main_volume': 112,
    'bar': 4,
    'loop': True,
    'tracks': [
        # the tune, with the channel's echo: two copies an eighth and a quarter later, hard left and
        # right, quieter (as SEQ_BGM_CONCIERGE_WAITING's flute: 72 / 40 / 30)
        {'name': 'flute', 'program': 73, 'volume': 80, 'pan': 64, 'attack': 127, 'release': 122, 'mml': FLUTE,
         'echo': {'delay': 48, 'copies': [{'pan': 0, 'volume': 44}, {'pan': 127, 'volume': 34}]}},
        {'name': 'guitar', 'program': 28, 'volume': 84, 'pan': 42, 'release': 122, 'mml': GUITAR},
        {'name': 'bass', 'program': 33, 'volume': 96, 'pan': 64, 'release': 124, 'sounding': True, 'mml': BASS},
        {'name': 'kick and snare', 'program': 95, 'volume': 104, 'pan': 64, 'mml': DRUMS},
        {'name': 'hi-hat', 'program': 95, 'volume': 78, 'pan': 86, 'mml': HIHAT},
        {'name': 'strings', 'program': 48, 'volume': 72, 'pan': 76, 'attack': 80, 'release': 116, 'mml': STRINGS},
        {'name': 'piano', 'program': 0, 'volume': 84, 'pan': 54, 'release': 118, 'mml': PIANO},
        {'name': 'electric piano', 'program': 4, 'volume': 76, 'pan': 74, 'release': 118, 'mml': EPIANO},
        {'name': 'accordion', 'program': 20, 'volume': 80, 'pan': 44, 'release': 116, 'mml': ACCORDION},
        {'name': 'glockenspiel', 'program': 9, 'volume': 60, 'pan': 96, 'release': 120, 'sounding': True,
         'mml': GLOCK},
        {'name': 'recorder', 'program': 74, 'volume': 64, 'pan': 88, 'release': 122, 'sounding': True,
         'mml': RECORDER},
    ],
}
