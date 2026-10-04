#!/usr/bin/env python3
"""nw4r::snd sequences (RSEQ 1.0, as Wii no Ma's douga.brsar holds them): an assembler, an
exact disassembler, a score compiler and a previewer.

    rseq.py disasm  (FILE.rseq | --sound LABEL) [-o OUT.s]   a sequence file as assembly text
    rseq.py asm     IN.s -o OUT.rseq                         assembly text to an RSEQ file
    rseq.py compile SCORE.py -o OUT.rseq [--asm OUT.s] [--wav OUT.wav] [--seconds S] [--loops N]
    rseq.py header  SCORE.py -o OUT.h [--namespace NS]       a score's RSEQ file as a C++ byte array
    rseq.py render  FILE.rseq OUT.wav [--label NAME] [--bank BANK_BGM_ALL] [--loops N] [--seconds S]
    rseq.py audition OUT_DIR [--bank BANK_BGM_ALL] [--programs 0,48,...]   a short piece per program
    rseq.py selftest                                         round trips and note-list checks

The file (as nw4r's SeqFileReader reads it, 804728D0 / 80472940 / 80472950):
  header 0x20: 'RSEQ', u16 BOM 0xFEFF, u16 version 0x0100, u32 file size, u16 header size 0x20,
    u16 blocks 2, then {u32 offset, u32 size} of DATA (0x20) and LABL.
  DATA: 'DATA', u32 size, u32 offset of the MML from DATA (0xC), the MML byte code; the block is
    zero-padded to a multiple of 0x20.
  LABL: 'LABL', u32 size, u32 count, u32 offset[count] (from LABL+8) -> {u32 MML offset, u32
    name length, name, NUL, zero pad to 4}; labels sorted by name; the block padded to 0x20.
  A sequence starts at a label (or a sound's data offset) with `alloctrack MASK` (FE hi lo):
  MmlParser::ParseAllocTrack (804718B0) takes the tracks to allocate from it (no FE: track 0
  only) when a sequence is started from its data (SoundStartable::StartInfo's seqSoundInfo).

Assembly syntax (what `disasm` writes and `asm` reads):
  ; comment                         to the end of the line
  name:   / name::                  a label / one exported to LABL (offsets for StartInfo)
  .byte fe 00 3f                    raw bytes (hex)
  [if] COMMAND ARGS                 `if`: only when the last comparison held (A2)
  c#4 v70 51                        a note: key (c4 = 60), velocity, length in ticks
  note 60 70 51                     the same, by number
  wait 48 / prg 48 / alloctrack 0x003f / opentrack 1, label / jump label / call label
  volume 100 (u8 commands by name), tempo 120 (s16), fin, ret, loop_end, env_reset
  volume 100, 48                    a second argument (A3: s16; for C1/C0/D7/C4 a move over
                                    that many ticks), or `, rand(a,b)` (A4) / `, var(n)` (A5)
  volume rand(50,60) / volume var(3) the argument random (A0) or from a variable (A1)
  setvar 0x20, 5 ... cmp_eq, userproc 3 (F0 extended commands)
Every command's encoding follows MmlParser::Parse (80470710, engine/snd/seq.cpp).

Score files (`compile`, `header`): a Python file defining SCORE (see compile_score() and the piece in
hiroi_sekai.py next to this file): tracks written in a small MML (see Mml), compiled to MML
byte code in the channel's own converter style (notewait off, notes and waits, one loop jump
per track), every track as long as the longest so their loops stay in step. `header` writes the
compiled file as a C++ byte array (the port plays it: engine/wuhu/music.cpp); `audition` writes a
short piece per program of a bank, to hear the instruments through the whole pipeline.
"""
import argparse
import os
import re
import struct
import sys

import brsar

# ---------------------------------------------------------------------------- file container


def build_rseq(mml, labels):
    """RSEQ bytes for MML byte code and labels [(name, offset)] (sorted by name, as the channel's
    files are)."""
    mml = bytes(mml)
    data = bytearray(b'DATA' + b'\0' * 8 + mml)
    data += b'\0' * (-len(data) % 0x20)
    struct.pack_into('>II', data, 4, len(data), 0xC)
    labels = sorted(labels, key=lambda l: l[0].encode('latin1'))
    entries = bytearray()
    offsets = []
    table = 4 + 4 * len(labels)
    for name, off in labels:
        offsets.append(table + len(entries))
        nb = name.encode('latin1')
        e = struct.pack('>II', off, len(nb)) + nb + b'\0'
        e += b'\0' * (-len(e) % 4)
        entries += e
    labl = bytearray(b'LABL' + b'\0' * 4 + struct.pack('>I', len(labels)) +
                     b''.join(struct.pack('>I', o) for o in offsets) + entries)
    labl += b'\0' * (-len(labl) % 0x20)
    struct.pack_into('>I', labl, 4, len(labl))
    size = 0x20 + len(data) + len(labl)
    head = struct.pack('>4sHHIHHIIII', b'RSEQ', 0xFEFF, 0x0100, size, 0x20, 2, 0x20, len(data),
                       0x20 + len(data), len(labl))
    head += b'\0' * (0x20 - len(head))
    return head + bytes(data) + bytes(labl)


def read_rseq(data):
    """(MML bytes with the DATA block's padding, [(label, offset)])."""
    sq = brsar.Rseq(data)
    return sq.mml, sq.labels


# ---------------------------------------------------------------------------- disassembler

NOTE_RE = re.compile(r'^([a-g])(#?)(-1|[0-9])$')
NOTE_INDEX = {'c': 0, 'd': 2, 'e': 4, 'f': 5, 'g': 7, 'a': 9, 'b': 11}


def key_of(name):
    m = NOTE_RE.match(name)
    if not m:
        return None
    return NOTE_INDEX[m.group(1)] + (1 if m.group(2) else 0) + 12 * (int(m.group(3)) + 1)


def _arg(a):
    if isinstance(a, tuple):
        return 'rand(%d,%d)' % (a[1], a[2]) if a[0] == 'random' else 'var(%d)' % a[1]
    return str(a)


def _label_for(off, names):
    return names[off][0] if off in names else 'L_%04X' % off


def format_asm(c, names):
    """One decoded command (brsar.MmlReader.decode) as an assembly line."""
    pre = 'if ' if c['if'] else ''
    n = c['name']
    if n == 'note':
        return '%s%s v%d %s' % (pre, brsar.note_name(c['key']), c['velocity'], _arg(c['length']))
    if n in ('jump', 'call'):
        return '%s%s %s' % (pre, n, _label_for(c['args'][0], names))
    if n == 'opentrack':
        return '%sopentrack %d, %s' % (pre, c['args'][0], _label_for(c['args'][1], names))
    if n == 'alloctrack':
        return '%salloctrack 0x%04x' % (pre, c['args'][0])
    if n == 'userproc':
        return '%suserproc %s' % (pre, _arg(c['args'][0]))
    if n in brsar.CMD_EXT_VAR.values():
        return '%s%s %d, %s' % (pre, n, c['args'][0], _arg(c['args'][1]))
    if n.startswith('ext_'):
        return pre + n
    return pre + n + (' ' + ', '.join(_arg(a) for a in c['args']) if c['args'] else '')


def disassemble(mml, labels, extra_starts=()):
    """Assembly text for MML byte code that assembles back to the same bytes: the code reachable
    from the labels (and extra_starts) through opentrack / jump / call, in offset order, with
    any bytes between as .byte (the DATA block's trailing zero padding left to the builder)."""
    rd = brsar.MmlReader(mml)
    names = {}
    for name, off in labels:
        names.setdefault(off, []).append(name)
    exported = set(n for n, _ in labels)
    todo = [off for _, off in labels] + list(extra_starts)
    seen = {}
    targets = set()
    while todo:
        p = todo.pop()
        while 0 <= p < len(mml) and p not in seen:
            try:
                c = rd.decode(p)
            except (IndexError, struct.error):
                break
            if c['end'] > len(mml):
                break
            seen[p] = c
            if c['name'] == 'opentrack':
                targets.add(c['args'][1])
                todo.append(c['args'][1])
            elif c['name'] in ('jump', 'call'):
                targets.add(c['args'][0])
                todo.append(c['args'][0])
            if not c['if'] and c['name'] in ('fin', 'ret', 'jump'):
                break
            p = c['end']
    for t in targets:
        names.setdefault(t, ['L_%04X' % t])
    lines = []
    end_of_code = max((c['end'] for c in seen.values()), default=0)
    p = 0
    starts = sorted(seen)
    while p < len(mml):
        for nm in names.get(p, []):
            lines.append('%s%s' % (nm, '::' if nm in exported else ':'))
        if p in seen:
            c = seen[p]
            lines.append('    %s' % format_asm(c, names))
            p = c['end']
            continue
        nxt = min([s for s in starts if s > p] + [s for s in names if s > p] + [len(mml)])
        chunk = mml[p:nxt]
        if p >= end_of_code and not chunk.strip(b'\0') and nxt == len(mml):
            break   # the block's padding
        # unreachable code (the converter's `fin` after each track's loop jump): as commands if
        # the gap decodes exactly, else as bytes
        stop = nxt
        if nxt == len(mml):   # the last gap: its trailing zeros are the block's padding
            stop = p + len(chunk.rstrip(b'\0'))
        decoded, q = [], p
        try:
            while q < stop:
                c = rd.decode(q)
                decoded.append(c)
                q = c['end']
        except (IndexError, struct.error):
            q = -1
        if q == stop and decoded:
            for c in decoded:
                lines.append('    %s    ; (not reached)' % format_asm(c, names))
            if stop < nxt:
                break
        else:
            for k in range(0, len(chunk), 16):
                lines.append('    .byte ' + ' '.join('%02x' % b for b in chunk[k:k + 16]))
        p = nxt
    for nm in names.get(len(mml), []):
        lines.append('%s%s' % (nm, '::' if nm in exported else ':'))
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------------------- assembler

CMD_U8 = {v: k for k, v in brsar.CMD_U8.items()}
CMD_S16 = {v: k for k, v in brsar.CMD_S16.items()}
CMD_NONE = {v: k for k, v in brsar.CMD_NONE.items()}
CMD_EXT = {v: k for k, v in brsar.CMD_EXT_VAR.items()}
S8_CMDS = brsar.CMD_S8


class AsmError(Exception):
    pass


def vmidi(v):
    if v < 0:
        raise AsmError('negative variable-length value %d' % v)
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


ARG_RE = re.compile(r'^(rand)\((-?\w+),\s*(-?\w+)\)$|^(var)\((\w+)\)$')


def parse_arg(s):
    """('int', v) / ('random', lo, hi) / ('var', n) / ('label', name)."""
    s = s.strip()
    m = ARG_RE.match(s)
    if m:
        if m.group(1):
            return ('random', int(m.group(2), 0), int(m.group(3), 0))
        return ('var', int(m.group(5), 0))
    try:
        return ('int', int(s, 0))
    except ValueError:
        return ('label', s)


def split_args(rest):
    out, depth, cur = [], 0, ''
    for ch in rest:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [parse_arg(a) for a in out]


def assemble(text):
    """(MML bytes, [(exported label, offset)]) for assembly text."""
    out = bytearray()
    labels = {}
    exported = []
    fixups = []   # (position of a 24-bit offset, label, line number)

    def emit_value(a, kind, lineno):
        """bytes of an argument of `kind` (u8 s8 s16 vmidi) and the prefix it needs."""
        if a[0] == 'random':
            return 0xA0, struct.pack('>hh', a[1], a[2])
        if a[0] == 'var':
            return 0xA1, bytes([a[1] & 0xFF])
        if a[0] != 'int':
            raise AsmError('line %d: bad argument %r' % (lineno, a))
        v = a[1]
        if kind == 'u8':
            if not 0 <= v <= 255:
                raise AsmError('line %d: %d is out of u8 range' % (lineno, v))
            return None, bytes([v])
        if kind == 's8':
            if not -128 <= v <= 255:
                raise AsmError('line %d: %d is out of s8 range' % (lineno, v))
            return None, bytes([v & 0xFF])
        if kind == 's16':
            if not -0x8000 <= v <= 0xFFFF:
                raise AsmError('line %d: %d is out of s16 range' % (lineno, v))
            return None, struct.pack('>H', v & 0xFFFF)
        return None, vmidi(v)

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split(';', 1)[0].strip()
        if not line:
            continue
        m = re.match(r'^([A-Za-z_.][\w.]*)(::?)$', line)
        if m:
            name = m.group(1)
            if name in labels:
                raise AsmError('line %d: label %s defined twice' % (lineno, name))
            labels[name] = len(out)
            if m.group(2) == '::':
                exported.append(name)
            continue
        if line.startswith('.byte'):
            out += bytes(int(b, 16) for b in line[5:].split())
            continue
        cond = False
        if line.startswith('if '):
            cond = True
            line = line[3:].strip()
        word, _, rest = line.partition(' ')
        args = split_args(rest) if rest.strip() else []
        pre = bytearray([0xA2]) if cond else bytearray()
        key = key_of(word)
        if word == 'note' or key is not None:
            parts = [parse_arg(x) if not x.startswith('v') else ('int', int(x[1:]))
                     for x in re.findall(r'rand\([^)]*\)|var\([^)]*\)|[^\s,]+', rest)]
            if key is None:
                key = parts.pop(0)[1]
            if len(parts) != 2 or parts[0][0] != 'int':
                raise AsmError('line %d: a note is KEY vVELOCITY LENGTH' % lineno)
            if not 0 <= key <= 127 or not 0 <= parts[0][1] <= 255:
                raise AsmError('line %d: key %d / velocity %d out of range' % (lineno, key, parts[0][1]))
            p, b = emit_value(parts[1], 'vmidi', lineno)
            if p:
                pre.append(p)
            out += pre + bytes([key, parts[0][1]]) + b
            continue
        if word in ('wait', 'prg'):
            p, b = emit_value(args[0], 'vmidi', lineno)
            if p:
                pre.append(p)
            out += pre + bytes([0x80 if word == 'wait' else 0x81]) + b
            continue
        if word == 'opentrack':
            out += pre + bytes([0x88, args[0][1]])
            fixups.append((len(out), args[1], lineno))
            out += b'\0\0\0'
            continue
        if word in ('jump', 'call'):
            out += pre + bytes([0x89 if word == 'jump' else 0x8A])
            fixups.append((len(out), args[0], lineno))
            out += b'\0\0\0'
            continue
        if word == 'alloctrack':
            out += pre + bytes([0xFE]) + struct.pack('>H', args[0][1])
            continue
        if word in CMD_U8 or re.match(r'^op_[bcd][0-9a-f]$', word):
            op = CMD_U8.get(word) or int(word[3:], 16)
            p1, b1 = emit_value(args[0], 's8' if op in S8_CMDS else 'u8', lineno)
            b2 = b''
            if len(args) > 1:
                a2 = args[1]
                if a2[0] == 'random':
                    pre.append(0xA4)
                    b2 = struct.pack('>hh', a2[1], a2[2])
                elif a2[0] == 'var':
                    pre.append(0xA5)
                    b2 = bytes([a2[1] & 0xFF])
                else:
                    pre.append(0xA3)
                    b2 = struct.pack('>H', a2[1] & 0xFFFF)
            if p1:
                pre.append(p1)
            out += pre + bytes([op]) + b1 + b2
            continue
        if word in CMD_S16 or re.match(r'^op_e[0-9a-f]$', word):
            op = CMD_S16.get(word) or int(word[3:], 16)
            p, b = emit_value(args[0], 's16', lineno)
            if p:
                pre.append(p)
            out += pre + bytes([op]) + b
            continue
        if word in CMD_NONE:
            out += pre + bytes([CMD_NONE[word]])
            continue
        if re.match(r'^op_[0-9a-f]{2}$', word):
            out += pre + bytes([int(word[3:], 16)])
            continue
        if word in CMD_EXT or word == 'userproc':
            if word == 'userproc':
                p, b = emit_value(args[0], 's16', lineno)
                if p:
                    pre.append(p)
                out += pre + bytes([0xF0, 0xE0]) + b
            else:
                p, b = emit_value(args[1], 's16', lineno)
                if p:
                    pre.append(p)
                out += pre + bytes([0xF0, CMD_EXT[word], args[0][1] & 0xFF]) + b
            continue
        m = re.match(r'^ext_([0-9a-f]{2})$', word)
        if m:
            out += pre + bytes([0xF0, int(m.group(1), 16)])
            continue
        raise AsmError('line %d: unknown command %r' % (lineno, word))
    for pos, a, lineno in fixups:
        if a[0] == 'label':
            if a[1] not in labels:
                raise AsmError('line %d: no label %s' % (lineno, a[1]))
            off = labels[a[1]]
        else:
            off = a[1]
        out[pos:pos + 3] = struct.pack('>I', off)[1:]
    return bytes(out), [(n, labels[n]) for n in exported]


# ---------------------------------------------------------------------------- score compiler

# The bank's programs whose keys don't sound as written (measured: each wave's pitch against its
# region's original key, by harmonic sum and YIN): a score written in sounding pitch is shifted by
# these for them (the glockenspiel sounds two octaves up, the marimba and the recorder one, the
# finger bass one down).
SOUNDING_OFFSET = {9: 24, 12: 12, 33: -12, 74: 12}

TRACK_PARAMS = [  # score track key -> (command, how to emit)
    ('volume2', 'volume2'), ('pan', 'pan'), ('init_pan', 'init_pan'), ('span', 'span'),
    ('transpose', 'transpose'), ('bendrange', 'bendrange'), ('priority', 'prio'),
    ('velocity_range', 'velocity_range'), ('attack', 'attack'), ('decay', 'decay'), ('sustain', 'sustain'),
    ('release', 'release'), ('hold', 'env_hold'), ('mod_depth', 'mod_depth'), ('mod_speed', 'mod_speed'),
    ('mod_type', 'mod_type'), ('mod_range', 'mod_range'), ('mod_delay', 'mod_delay'),
    ('fxsend_a', 'fxsend_a'), ('fxsend_b', 'fxsend_b'), ('fxsend_c', 'fxsend_c'), ('mainsend', 'mainsend'),
    ('lpf', 'lpf_cutoff'), ('monophonic', 'monophonic'), ('porta_time', 'porta_time'), ('porta', 'porta_sw'),
]


class MmlError(Exception):
    pass


class Mml:
    """A small music macro language for one track, compiled to timed events.

      o4 > <          octave (c4 = key 60, nw4r's numbering) / up / down
      l8  l4.  l:36   default length: a note value (1 2 4 8 16 32 64, `t` triplet, dots) or ticks
      c d e f g a b   notes, `+`/`#` sharp, `-` flat, then a length: `c+8.`
      r8              rest
      c4^8 / c4&c8    tie (the second must be the same key for &)
      [c e g]4        a chord (octave changes inside last until the `]`)
      v90             velocity (0..127) for the notes that follow
      q7  q12         gate: notes sound q/8 of their length (q8: all of it; above 8 they overlap)
      @48             program change
      {volume 90, 192; pan 40}   commands (assembly syntax) at this point in time
      |               a bar line (checked against the score's bar length)
      s1 ... s0       let ring: notes played in between sound on to the next `|` or `!`
                      (as a sustain pedal or a fingerpicked guitar would), not just their length,
                      or until their key is played again (a string struck again)
      !               a ring boundary inside a bar (a chord change)
      $               the loop point (the track jumps back here at its end)
      ( ... )3        repeat
    Lengths are in ticks of the score's timebase (96: a quarter note is 96 ticks).
    """

    TOKEN = re.compile(r'''\s*(?:
        (?P<oct>o-?\d) | (?P<up>>) | (?P<down><) |
        (?P<len>l(?::\d+|\d+t?\.*)) | (?P<vel>v\d+) | (?P<gate>q\d+) | (?P<prg>@\d+) |
        (?P<note>[a-g][+#-]*(?::\d+|\d+t?\.*)?) | (?P<rest>r(?::\d+|\d+t?\.*)?) |
        (?P<tie>\^(?::\d+|\d+t?\.*)) | (?P<amp>&) |
        (?P<chord>\[) | (?P<chordend>\](?::\d+|\d+t?\.*)?) |
        (?P<cmd>\{[^}]*\}) | (?P<bar>\|) | (?P<loop>\$) | (?P<ring>s[01]) | (?P<bound>!) |
        (?P<rep>\() | (?P<repend>\)\d*)
        )''', re.X)

    def __init__(self, timebase):
        self.tb = timebase

    def length(self, s, default):
        if not s:
            return default
        if s.startswith(':'):
            return int(s[1:])
        m = re.match(r'(\d+)(t?)(\.*)$', s)
        if not m:
            raise MmlError('bad length %r' % s)
        n = int(m.group(1))
        if (4 * self.tb) % n:
            raise MmlError('length 1/%d is not a whole number of ticks' % n)
        ticks = 4 * self.tb // n
        if m.group(2):
            if ticks * 2 % 3:
                raise MmlError('triplet 1/%d is not a whole number of ticks' % n)
            ticks = ticks * 2 // 3
        add = ticks
        for _ in m.group(3):
            add //= 2
            ticks += add
        return ticks

    def tokens(self, text):
        pos = 0
        out = []
        text = re.sub(r'//[^\n]*', '', text)
        while pos < len(text):
            if text[pos:].strip() == '':
                break
            m = self.TOKEN.match(text, pos)
            if not m or m.end() == pos:
                raise MmlError('cannot read %r' % text[pos:pos + 20])
            out.append((m.lastgroup, m.group(m.lastgroup)))
            pos = m.end()
        return out

    def expand(self, toks):
        """Repeats expanded."""
        out, stack = [], [[]]
        for kind, val in toks:
            if kind == 'rep':
                stack.append([])
            elif kind == 'repend':
                body = stack.pop()
                n = int(val[1:] or 2)
                stack[-1].extend(body * n)
            else:
                stack[-1].append((kind, val))
        if len(stack) != 1:
            raise MmlError('unbalanced ( )')
        return stack[0]

    def compile(self, text, bar_ticks=None, octave=4, length=None, velocity=96, gate=8, name='?'):
        """{'notes': [(tick, key, velocity, length)], 'cmds': [(tick, asm line)], 'end': ticks,
        'loop': tick or None}."""
        toks = self.expand(self.tokens(text))
        tick = 0
        length = length or self.tb
        notes, cmds = [], []
        loop = None
        last = []        # indices of the last note(s), for ties
        chord = None     # keys of an open chord
        chord_octave = None
        pending_amp = False
        ring = False
        ringing = []     # notes to let ring to the next boundary

        def boundary(at):
            for i in ringing:
                t0, k, v, full, snd = notes[i]
                if at > t0 + snd:
                    notes[i] = (t0, k, v, full, at - t0)
            ringing.clear()

        def restrike(key, at):
            # a ringing note whose key is played again stops there, as a string struck again does
            for i in [i for i in ringing if notes[i][1] == key]:
                t0, k, v, full, snd = notes[i]
                notes[i] = (t0, k, v, full, max(1, min(snd, at - t0)))
                ringing.remove(i)

        for kind, val in toks:
            if kind == 'oct':
                octave = int(val[1:])
            elif kind == 'up':
                octave += 1
            elif kind == 'down':
                octave -= 1
            elif kind == 'len':
                length = self.length(val[1:], length)
            elif kind == 'vel':
                velocity = int(val[1:])
            elif kind == 'gate':
                gate = int(val[1:])
            elif kind == 'prg':
                cmds.append((tick, 'prg %d' % int(val[1:])))
            elif kind == 'note':
                m = re.match(r'([a-g])([+#-]*)(.*)$', val)
                key = NOTE_INDEX[m.group(1)] + m.group(2).count('+') + m.group(2).count('#') - \
                    m.group(2).count('-') + 12 * (octave + 1)
                if chord is not None:
                    chord.append(key)
                    continue
                ln = self.length(m.group(3), length)
                if pending_amp and last and notes[last[0]][1] == key:
                    notes[last[0]] = notes[last[0]][:3] + (notes[last[0]][3] + ln, notes[last[0]][4] + ln)
                    pending_amp = False
                else:
                    pending_amp = False
                    if ring:
                        restrike(key, tick)
                    notes.append((tick, key, velocity, ln, ln * gate // 8))
                    last = [len(notes) - 1]
                    if ring:
                        ringing.append(len(notes) - 1)
                tick += ln
            elif kind == 'rest':
                tick += self.length(val[1:], length)
                last = []
            elif kind == 'tie':
                ln = self.length(val[1:], length)
                for i in last:
                    t0, k, v, full, snd = notes[i]
                    notes[i] = (t0, k, v, full + ln, snd + ln)
                tick += ln
            elif kind == 'amp':
                pending_amp = True
            elif kind == 'chord':
                chord, chord_octave = [], octave
            elif kind == 'chordend':
                ln = self.length(val[1:], length)
                last = []
                for key in chord:
                    if ring:
                        restrike(key, tick)
                    notes.append((tick, key, velocity, ln, ln * gate // 8))
                    last.append(len(notes) - 1)
                    if ring:
                        ringing.append(len(notes) - 1)
                chord = None
                octave = chord_octave
                tick += ln
            elif kind == 'cmd':
                for c in val[1:-1].split(';'):
                    if c.strip():
                        cmds.append((tick, c.strip()))
            elif kind == 'ring':
                boundary(tick)
                ring = val == 's1'
            elif kind == 'bound':
                boundary(tick)
            elif kind == 'bar':
                boundary(tick)
                if bar_ticks and tick % bar_ticks:
                    raise MmlError('%s: bar line at tick %d, %d ticks into a bar' % (name, tick, tick % bar_ticks))
            elif kind == 'loop':
                loop = tick
        boundary(tick)
        return {'notes': [(t, k, v, max(1, s)) for t, k, v, full, s in notes], 'cmds': cmds, 'end': tick,
                'loop': loop}


def emit_track(lines, events, end, loop, loop_label, looping):
    """Assembly for a track's timed events [(tick, order, asm line)] in the converter's style."""
    cur = 0
    events = sorted(events, key=lambda e: (e[0], e[1]))
    done_loop = False
    for tick, order, text in events:
        if loop is not None and not done_loop and tick >= loop:
            if loop > cur:
                lines.append('    wait %d' % (loop - cur))
                cur = loop
            lines.append('%s:' % loop_label)
            done_loop = True
        if tick > cur:
            lines.append('    wait %d' % (tick - cur))
            cur = tick
        lines.append('    ' + text)
    if loop is not None and not done_loop:
        if loop > cur:
            lines.append('    wait %d' % (loop - cur))
            cur = loop
        lines.append('%s:' % loop_label)
    if end > cur:
        lines.append('    wait %d' % (end - cur))
    if looping:
        lines.append('    jump %s' % loop_label)
    else:
        lines.append('    fin')


def compile_score(score):
    """Assembly text for a score:

      {'label': 'SEQ_BGM_X'           the exported start label (StartInfo's startLocationLabel)
       'timebase': 96, 'tempo': 120,  'main_volume': 127, 'bar': 4 (beats a bar, for | checks),
       'loop': True                   loop (from each track's `$`, or the start) or end with fin
       'tracks': [{'program': 48, 'volume': 100, 'pan': 64, 'attack': .., 'release': .., ...
                   (see TRACK_PARAMS), 'sounding': True (write sounding pitch: SOUNDING_OFFSET),
                   'octave': 4, 'velocity': 96, 'mml': '...',
                   'echo': {'delay': 48, 'copies': [{'pan': 0, 'volume': 50}, ...]}} ...]}
                   (16 tracks at most, echoes included)
    Track 0 opens the others; all are padded to the longest one; their loop points must agree.
    Returns (assembly text, the end tick, the loop tick)."""
    tb = score.get('timebase', 96)
    tracks = score['tracks']
    if not 1 <= len(tracks) <= 16:
        raise MmlError('1..16 tracks')
    label = score.get('label', 'SEQ_SCORE')
    bar_ticks = int(score.get('bar', 4) * tb) if score.get('bar') else None
    mml = Mml(tb)
    compiled = []
    for i, t in enumerate(tracks):
        c = mml.compile(t.get('mml', ''), bar_ticks, octave=t.get('octave', 4), velocity=t.get('velocity', 96),
                        gate=t.get('gate', 8), name=t.get('name', 'track %d' % i))
        shift = SOUNDING_OFFSET.get(t.get('program', 0), 0) if t.get('sounding') else 0
        c['notes'] = [(tk, k - shift, v, ln) for tk, k, v, ln in c['notes']]
        for tk, k, v, ln in c['notes']:
            if not 0 <= k <= 127 or not 0 <= v <= 127:
                raise MmlError('track %d: note %d velocity %d at tick %d out of range' % (i, k, v, tk))
        compiled.append(c)
    end = max([c['end'] for c in compiled] + [int(score.get('length', 0))])
    loops = set(c['loop'] for c in compiled if c['loop'] is not None)
    if len(loops) > 1:
        raise MmlError('tracks loop from different ticks: %s' % sorted(loops))
    loop = loops.pop() if loops else 0
    looping = score.get('loop', True)
    for c in compiled:
        c['end'], c['loop'] = end, loop
    # A looping track's notes end by the loop's end: one that rang on past it would sound into the
    # loop's start on every pass but the first (the sequencer counts a note's length down on its
    # own, through the jump back).
    clipped = 0
    if looping:
        for c in compiled:
            for j, (tk, k, v, ln) in enumerate(c['notes']):
                if tk + ln > end:
                    c['notes'][j] = (tk, k, v, end - tk)
                    clipped += 1
    # Echoes, the channel's own trick (SAIZIKI_E, SHOP_MAIN, CONCIERGE_WAITING): copies of a
    # melody's track `delay`, 2 * `delay`... ticks later, panned hard left and right, quieter. Each
    # copy is the whole track delayed, its loop too (it jumps back `delay` ticks after the others,
    # to `delay` after the loop point), so it is `delay` behind its track everywhere: the first
    # pass starts with the echo of the intro's end, every later pass with the loop end's (the same,
    # for a score whose intro ends as its loop does). A copy looping with the others, its last
    # notes wrapped round to the loop point, would echo the loop's end at the start of the first
    # pass too, before that end was ever played, and the intro's end at the start of every pass.
    # The copy keeps its own pan; the track's volume changes reach it scaled by the copy's volume
    # over the track's.
    tracks = list(tracks)
    for i in range(len(tracks)):
        e = tracks[i].get('echo')
        if not e:
            continue
        for n, copy in enumerate(e['copies'], 1):
            d = e['delay'] * n
            c = compiled[i]
            scale = copy.get('volume', 100) / float(tracks[i].get('volume', 100))
            cmds = []
            for tk, cmd in c['cmds']:
                word, _, rest = cmd.partition(' ')
                if word == 'pan':
                    continue
                if word == 'volume':
                    args = [x.strip() for x in rest.split(',')]
                    args[0] = str(max(0, min(127, int(round(int(args[0], 0) * scale)))))
                    cmd = 'volume ' + ', '.join(args)
                cmds.append((tk + d, cmd))
            t2 = dict(tracks[i])
            t2.pop('echo')
            t2.update(copy)
            t2['name'] = '%s echo %d' % (tracks[i].get('name', 'track %d' % i), n)
            tracks.append(t2)
            compiled.append({'notes': [(tk + d, k, v, ln) for tk, k, v, ln in c['notes']], 'cmds': cmds,
                             'end': end + d, 'loop': loop + d})
    if len(tracks) > 16:
        raise MmlError('%d tracks with the echoes: 16 at most' % len(tracks))
    mask = (1 << len(tracks)) - 1
    lines = ['; %s, compiled by rseq.py' % score.get('title', label), '%s::' % label,
             '    alloctrack 0x%04x' % mask, '%s_Start::' % label, '    timebase %d' % tb]
    for i in range(1, len(tracks)):
        lines.append('    opentrack %d, %s_Track_%d' % (i, label, i))
    lines.append('    tempo %d' % score.get('tempo', 120))
    if 'main_volume' in score:
        lines.append('    main_volume %d' % score['main_volume'])
    for i, (t, c) in enumerate(zip(tracks, compiled)):
        if i:
            lines.append('%s_Track_%d::' % (label, i))
        lines.append('    notewait 0')
        lines.append('    prg %d' % t.get('program', 0))
        lines.append('    volume %d' % t.get('volume', 100))
        for k, cmd in TRACK_PARAMS:
            if k in t:
                v = t[k]
                if k == 'lpf':
                    v += 64
                lines.append('    %s %d' % (cmd, v))
        events = [(tk, 0, cmd) for tk, cmd in c['cmds']]
        events += [(tk, 1, '%s v%d %d' % (brsar.note_name(k), v, ln)) for tk, k, v, ln in c['notes']]
        emit_track(lines, events, c['end'], c['loop'] if looping else None, '%s_Track_%d_LoopStart' % (label, i),
                   looping)
    lines.append('%s_End::' % label)
    if clipped:
        lines.insert(1, '; %d notes cut short at the loop\'s end' % clipped)
    return '\n'.join(lines) + '\n', end, loop


# ---------------------------------------------------------------------------- C++ header


def header_text(data, score, end, loop, score_path, out_path, namespace):
    """A C++ header holding a compiled score's RSEQ file as a byte array, with its clock and loop."""
    rel = lambda p: os.path.relpath(os.path.abspath(p), brsar.ROOT)
    tb = score.get('timebase', 96)
    tempo = score.get('tempo', 120)
    bar = score.get('bar') or 4
    if bar != int(bar):   # (the engine counts a bar in whole quarter notes: music_bar_beats)
        raise MmlError('BAR_BEATS: a bar of %g quarter notes is no whole number of them' % bar)
    lines = [
        '// %s (%s): the port\'s own music, compiled to an RSEQ file' % (score.get('title', '?'),
                                                                        score.get('label', '?')),
        '// (nw4r::snd sequence data, big-endian, as the channel\'s sound archive holds its own).',
        '// Generated from %s by tools/music/rseq.py; don\'t edit it, make it again:' % rel(score_path),
        '//     python3 tools/music/rseq.py header %s -o %s' % (rel(score_path), rel(out_path)),
        '#pragma once',
        '',
        '#include "runtime/runtime.h"',
        '',
        'namespace %s {' % namespace,
        '',
        '// its clock: TIMEBASE ticks a quarter note, TEMPO quarter notes a minute, BAR_BEATS quarter notes',
        '// a bar; it loops from LOOP_START to LOOP_END (ticks from its start): %.2f s a pass' % (
            (end - loop) * 60.0 / (tempo * tb)),
        'constexpr u32 TIMEBASE = %d, TEMPO = %d, BAR_BEATS = %d;' % (tb, tempo, bar),
        'constexpr u32 LOOP_START = %d, LOOP_END = %d;' % (loop, end),
        'constexpr u32 SIZE = %d;  // bytes' % len(data),
        'alignas(32) const u8 RSEQ[SIZE] = {',
    ]
    for k in range(0, len(data), 16):
        lines.append('    ' + ' '.join('0x%02X,' % b for b in data[k:k + 16]))
    lines += ['};', '', '}  // namespace %s' % namespace, '']
    return '\n'.join(lines)


# ---------------------------------------------------------------------------- previews


def simulate_rseq(data, label=None, loops=1, max_ticks=None):
    mml, labels = read_rseq(data)
    start = 0
    if label:
        start = dict(labels)[label]
    kw = {'loops': loops}
    if max_ticks:
        kw['max_ticks'] = max_ticks
    return brsar.simulate(mml, start, **kw)


def render_rseq(data, out_wav, label=None, bank='BANK_BGM_ALL', loops=1, seconds=None, reverb=True, volume=1.0,
                brsar_path=brsar.DEFAULT_BRSAR, absolute=False):
    """Renders a sequence with a bank of the channel's archive to a .wav; returns the stats."""
    import sndrender
    r = brsar.Rsar(brsar_path)
    b = next(x for x in r.banks if bank in (x['label'], str(x['id'])))
    sim = simulate_rseq(data, label, loops)
    st = {}
    audio = sndrender.render(sim, r.bank(b['id']), max_seconds=seconds, reverb=reverb, volume=volume,
                             gain_db=None if absolute else -1.0, stats=st)
    brsar.write_wav(out_wav, audio, 32000)
    st['seconds'] = audio.shape[1] / 32000.0
    st['notes'] = len(sim['notes'])
    return st


# ---------------------------------------------------------------------------- auditions

KITS = {25, 26, 95, 96, 98}


def audition(r, bank_label, out_dir, programs=None):
    """A .wav per program of a bank, through the whole pipeline (score -> compile -> assemble ->
    simulate -> render): a pitched program plays a D major arpeggio and scale over the range the
    channel's music plays it in, a kit every key the channel plays on it."""
    import collections
    b = next(x for x in r.banks if bank_label in (x['label'], str(x['id'])))
    bank = r.bank(b['id'])
    used = collections.defaultdict(collections.Counter)
    for sd in r.sounds:
        if sd.type == 1 and sd.bank == b['id']:
            sq = r.sequence(sd.file)
            for n in brsar.simulate(sq.mml, sd.seq_offset, alloc_mask=sd.alloc_track)['notes']:
                used[n['program']][n['key']] += 1
    os.makedirs(out_dir, exist_ok=True)
    name = lambda k: 'o%d%s' % (k // 12 - 1, ['c', 'c+', 'd', 'd+', 'e', 'f', 'f+', 'g', 'g+', 'a', 'a+', 'b'][k % 12])
    for prg in range(bank.nprograms):
        if not bank.regions(prg) or (programs and prg not in programs):
            continue
        keys = used.get(prg)
        if prg in KITS:
            ks = sorted(keys) if keys else [rg[4].original_key for rg in bank.regions(prg)]
            mml = ' '.join(name(k) + '4' for k in ks)
        else:
            lo, hi = (min(keys), max(keys)) if keys else (48, 84)
            mid = (lo + hi) // 2
            base = max(mid - (mid - 2) % 12 - 12, lo - (lo - 2) % 12)   # a D an octave under the middle
            ks = [base + 12 * o + s for o in range(2) for s in (0, 4, 7, 12)]
            ks += [base + 12 + s for s in (0, 2, 4, 5, 7, 9, 11, 12)]
            mml = ' '.join(name(k) + '8' for k in ks if 0 <= k <= 127) + \
                ' [%s %s %s]2' % (name(base), name(base + 4), name(base + 7))
        score = {'label': 'AUDITION', 'tempo': 100, 'bar': None, 'loop': False,
                 'tracks': [{'program': prg, 'volume': 110, 'release': 110, 'velocity': 104, 'mml': mml}]}
        text, end, loop = compile_score(score)
        mml_bytes, labels = assemble(text)
        path = os.path.join(out_dir, 'prg%02d.wav' % prg)
        st = render_rseq(build_rseq(mml_bytes, labels), path, label='AUDITION', bank=bank_label, reverb=False,
                         brsar_path=r.path)
        print('%s: %d notes, %.1f s' % (path, st['notes'], st['seconds']))


# ---------------------------------------------------------------------------- self test


def note_key(n):
    return (n['tick'], n['track'], n['key'], n['velocity'], n['length'], n['program'], n['volume'], n['pan'],
            tuple(n['adsr']), n.get('off_tick'))


def selftest(r, verbose=True):
    """1. every RSEQ file of the archive: disassemble, assemble, build: the same bytes;
    2. every sequence sound: the assembled file plays the same notes (brsar.simulate);
    3. score path: each BGM piece's notes (one pass) re-written as a score (tracks of timed notes
       with their parameters), compiled, assembled and simulated: the same notes."""
    ok = True
    files = [f['id'] for f in r.files if r.file_kind(f['id']) == 'RSEQ']
    same = 0
    for fid in files:
        data = r.file_data(fid)
        mml, labels = read_rseq(data)
        starts = [sd.seq_offset for sd in r.sounds if sd.type == 1 and sd.file == fid]
        text = disassemble(mml, labels, starts)
        mml2, labels2 = assemble(text)
        data2 = build_rseq(mml2, labels2)
        if data2 == data:
            same += 1
        else:
            ok = False
            k = next(i for i in range(min(len(data), len(data2))) if data[i] != data2[i]) \
                if any(a != b for a, b in zip(data, data2)) else min(len(data), len(data2))
            print('file %d: differs at 0x%X (%d vs %d bytes)' % (fid, k, len(data), len(data2)))
    if verbose:
        print('round trip: %d of %d RSEQ files rebuilt byte for byte' % (same, len(files)))
    # 2 + 3
    seqs = [sd for sd in r.sounds if sd.type == 1]
    n_same = n_score = n_score_same = 0
    for sd in seqs:
        data = r.file_data(sd.file)
        mml, labels = read_rseq(data)
        a = brsar.simulate(mml, sd.seq_offset, alloc_mask=sd.alloc_track, loops=1)
        text = disassemble(mml, labels, [sd.seq_offset])
        mml2, _ = assemble(text)
        b = brsar.simulate(mml2, sd.seq_offset, alloc_mask=sd.alloc_track, loops=1)
        if [note_key(n) for n in a['notes']] == [note_key(n) for n in b['notes']]:
            n_same += 1
        else:
            ok = False
            print('%s: notes differ after reassembly' % sd.label)
        if r.players[sd.player]['label'] != 'PLAYER_BGM' or not a['notes']:
            continue
        n_score += 1
        if score_roundtrip(a, sd.label, verbose=False):
            n_score_same += 1
        else:
            ok = False
    if verbose:
        print('notes: %d of %d sequence sounds play the same notes reassembled' % (n_same, len(seqs)))
        print('score path: %d of %d BGM pieces recompiled from their note lists play the same notes' % (
            n_score_same, n_score))
    return ok


def score_roundtrip(sim, label, verbose=True):
    """Rebuilds a simulated piece as a score (each track's notes with explicit parameter changes)
    through compile_score's emitter and checks the notes match (tick, key, velocity, length,
    program, volume, pan, envelope, release tick)."""
    tb = sim['timebase']
    tempos = sim['tempo'][1:]
    tracks = {}
    for n in sim['notes']:
        tracks.setdefault(n['track'], []).append(n)
    end = sim['ticks']
    lines = ['%s_RT::' % label, '    alloctrack 0x%04x' % sim['alloc_mask'], '    timebase %d' % tb]
    order = sorted(tracks)
    if 0 not in tracks:
        tracks[0] = []
        order = [0] + order
    for no in order:
        if no:
            lines.append('    opentrack %d, T%d' % (no, no))
    first = True
    for no in order:
        if not first:
            lines.append('T%d:' % no)
        lines.append('    notewait 0')
        ev = []
        if first:
            ev += [(tk, 0, 'tempo %d' % bpm) for tk, bpm in tempos]
        state = {}
        for n in tracks[no]:
            want = [('prg', n['program']), ('volume', n['volume']), ('pan', n['pan'] + 64),
                    ('attack', n['adsr'][0]), ('decay', n['adsr'][1]), ('sustain', n['adsr'][2]),
                    ('release', n['adsr'][3]), ('main_volume', n['main_volume'])]
            for cmd, v in want:
                if state.get(cmd) != v and v <= 255:
                    ev.append((n['tick'], 0, '%s %d' % (cmd, v)))
                    state[cmd] = v
            ev.append((n['tick'], 1, '%s v%d %d' % (brsar.note_name(n['key']), n['velocity'], n['length'])))
        for tk, down in sim['damper'].get(no, []):
            ev.append((tk, 2, 'damper %d' % (127 if down else 0)))
        emit_track(lines, ev, end, None, 'T%d_loop' % no, False)
        first = False
    mml, labels = assemble('\n'.join(lines))
    b = brsar.simulate(mml, 0, loops=1)
    ka = sorted(note_key(n)[:9] + (n.get('off_tick'),) for n in sim['notes'])
    kb = sorted(note_key(n)[:9] + (n.get('off_tick'),) for n in b['notes'])
    # the score keeps the notes, not the track numbers' parameters a note didn't use
    same = ka == kb
    if not same:
        diff = next((i for i, (x, y) in enumerate(zip(ka, kb)) if x != y), min(len(ka), len(kb)))
        print('%s: score round trip differs at note %d: %s vs %s' % (
            label, diff, ka[diff] if diff < len(ka) else None, kb[diff] if diff < len(kb) else None))
    return same


# ---------------------------------------------------------------------------- CLI


def load_score(path):
    ns = {'__file__': path}
    sys.path.insert(0, os.path.dirname(os.path.abspath(path)))
    exec(compile(open(path, encoding='utf-8').read(), path, 'exec'), ns)
    return ns['SCORE']


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--brsar', default=brsar.DEFAULT_BRSAR)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('disasm')
    s.add_argument('file', nargs='?')
    s.add_argument('--sound', help='a sequence sound of the archive (label or id)')
    s.add_argument('-o', '--out')
    s = sub.add_parser('asm')
    s.add_argument('src')
    s.add_argument('-o', '--out', required=True)
    s = sub.add_parser('compile')
    s.add_argument('score')
    s.add_argument('-o', '--out', required=True)
    s.add_argument('--asm', help='also write the assembly text here')
    s.add_argument('--wav', help='also render a preview')
    s.add_argument('--seconds', type=float)
    s.add_argument('--loops', type=int, default=1)
    s.add_argument('--volume', type=float, default=1.0, help="the host sound's volume (archive volume / 127)")
    s = sub.add_parser('header', help="a score's RSEQ as a C++ header (a byte array)")
    s.add_argument('score')
    s.add_argument('-o', '--out', required=True)
    s.add_argument('--namespace', default='engine::wuhu::hiroi_sekai')
    s = sub.add_parser('render')
    s.add_argument('file')
    s.add_argument('out')
    s.add_argument('--label')
    s.add_argument('--bank', default='BANK_BGM_ALL')
    s.add_argument('--loops', type=int, default=1)
    s.add_argument('--seconds', type=float)
    s.add_argument('--dry', action='store_true', help='no reverb')
    s = sub.add_parser('audition')
    s.add_argument('out')
    s.add_argument('--bank', default='BANK_BGM_ALL')
    s.add_argument('--programs', help='comma-separated (default: all)')
    sub.add_parser('selftest')
    a = ap.parse_args(argv)

    if a.cmd == 'disasm':
        if a.sound:
            r = brsar.Rsar(a.brsar)
            sd = r.sound(a.sound)
            data = r.file_data(sd.file)
            starts = [sd.seq_offset]
        else:
            data = open(a.file, 'rb').read()
            starts = []
        mml, labels = read_rseq(data)
        text = disassemble(mml, labels, starts)
        if a.out:
            open(a.out, 'w').write(text)
        else:
            sys.stdout.write(text)
    elif a.cmd == 'asm':
        mml, labels = assemble(open(a.src).read())
        open(a.out, 'wb').write(build_rseq(mml, labels))
        print('%d bytes of MML, %d labels -> %s' % (len(mml), len(labels), a.out))
    elif a.cmd == 'compile':
        score = load_score(a.score)
        text, end, loop = compile_score(score)
        if a.asm:
            open(a.asm, 'w').write(text)
        mml, labels = assemble(text)
        data = build_rseq(mml, labels)
        open(a.out, 'wb').write(data)
        tb = score.get('timebase', 96)
        ntracks = text.count('notewait 0')
        print('%s: %d tracks (%d with echoes), %d ticks (%.1f beats, loop from beat %.1f), %d bytes of MML, '
              'file %d bytes -> %s' % (score.get('title', '?'), len(score['tracks']), ntracks, end, end / tb,
                                      loop / tb, len(mml), len(data), a.out))
        if a.wav:
            st = render_rseq(data, a.wav, label=score.get('label'), loops=a.loops, seconds=a.seconds,
                             volume=a.volume, brsar_path=a.brsar)
            print('preview: %(notes)d notes, %(seconds).1f s, up to %(voices)d voices (mean %(mean_voices).1f), '
                  'rms %(rms_db).1f dB -> ' % st + a.wav)
    elif a.cmd == 'header':
        score = load_score(a.score)
        text, end, loop = compile_score(score)
        mml, labels = assemble(text)
        data = build_rseq(mml, labels)
        open(a.out, 'w').write(header_text(data, score, end, loop, a.score, a.out, a.namespace))
        print('%s: %d bytes, loop %d..%d ticks -> %s' % (score.get('title', '?'), len(data), loop, end, a.out))
    elif a.cmd == 'render':
        data = open(a.file, 'rb').read()
        st = render_rseq(data, a.out, label=a.label, bank=a.bank, loops=a.loops, seconds=a.seconds,
                         reverb=not a.dry, brsar_path=a.brsar)
        print('%(notes)d notes, %(seconds).1f s, up to %(voices)d voices -> ' % st + a.out)
    elif a.cmd == 'audition':
        audition(brsar.Rsar(a.brsar), a.bank, a.out,
                 set(int(x) for x in a.programs.split(',')) if a.programs else None)
    elif a.cmd == 'selftest':
        ok = selftest(brsar.Rsar(a.brsar))
        print('OK' if ok else 'FAILED')
        sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
