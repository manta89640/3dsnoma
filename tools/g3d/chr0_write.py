#!/usr/bin/env python3
"""A small BRRES writer for node animations: CHR0 (version 5) in an AnmChr(NW4R) folder.

    import chr0_write
    clip = chr0_write.Clip("Jump", frames=36, loop=False)
    clip.node("Skl_Root", rot=[rx, ry, rz], trans=[tx, ty, tz])   # per-frame values, frames + 1 each
    data = chr0_write.brres([clip, ...])

What nw4r reads, as Nintendo's files lay it out (tools/g3d/brres.py reads them back):
  - the file: "bres", 0xFEFF, u16 0, u32 size, u16 0x10 (the root), u16 sections (the root and each
    subfile); the root block ("root", u32 size) holds the root's dictionary (one folder) and the
    folder's (one entry a clip); then the clips; then the strings, each a u32 length, its characters
    and a NUL, padded to four bytes (a name points at its characters: nw4r's ResName reads the length
    before them).
  - a dictionary (nw4r ResDic): u32 size, u32 count, count + 1 entries {u16 ref, u16 flag, u16 left,
    u16 right, s32 name, s32 data} (offsets from the dictionary), entry 0 the root of a Patricia tree
    whose refs are bit indices into the name (byte ref >> 3 from the name's start, bit ref & 7) and
    fall going down from the root (ResDic::Get: down while the ref falls, left or right by that bit
    of the name; a byte past the name's end reads 0). Entries are inserted in the order given, as
    Nintendo's tools did: the trees of the channel's own files come out the same (check()).
  - a CHR0: magic, size, version 5, s32 to the BRRES, s32 the node dictionary, s32 0 (user data),
    s32 name, s32 0 (original path), u16 frames, u16 nodes, u32 loop, u32 scaling rule 2 (Maya: what
    every clip of Wii no Ma's uses); then the node dictionary, the nodes' data and their tracks.
  - a node's data: s32 its name (from the data), u32 flags, a word a component present: a constant
    (an f32) or the offset of its track (from the data). Flags: 1 present, 8 scale one, 0x10 scale
    uniform, 0x2000 << i the i-th of the nine components (scale, rotation, translation; x y z)
    constant, 0x800000 rotation present, 0x1000000 translation present, the rotation's track format
    at bit 27 and the translation's at 30; bits 0x400 and 0x800 (segment scale compensation) as the
    caller gives them.
  - tracks: a rotation (degrees) as FRM8 (format 4: f32 step, f32 base, a u8 a frame for frames 0 ..
    frames: base + step x the byte) where that is within ROT8_ERROR of the values, else FRM32
    (format 6: an f32 a frame); a translation as FVS96 (format 3: u16 keys, u16 0, f32 one over the
    keys' frame range, then {f32 frame, value, slope} a key: a key a frame, its slope the values'
    central difference). All three formats are in the channel's and Wii Sports Resort's own clips.
"""
import struct

ROT8_ERROR = 0.35   # degrees: the most an FRM8 rotation may be off its value
FOLDER = "AnmChr(NW4R)"

F_PRESENT, F_S_ONE, F_S_UNIFORM = 0x1, 0x8, 0x10
F_HAS_R, F_HAS_T = 0x800000, 0x1000000
FMT_FVS96, FMT_FRM8, FMT_FRM32 = 3, 4, 6


class Clip:
    def __init__(self, name, frames, loop):
        self.name, self.frames, self.loop = name, int(frames), bool(loop)
        self.nodes = []      # (name, rot [3 lists], trans [3 lists] or None, extra flags)

    def node(self, name, rot, trans=None, extra_flags=0):
        """rot: three lists of frames + 1 degrees (x, y, z); trans: three lists, or None for none: nw4r
        then takes the translation as zero (not the model's), so a bone's own is given as constants."""
        for c in list(rot) + (list(trans) if trans else []):
            assert len(c) == self.frames + 1, "%s/%s: %d values for %d frames" % (self.name, name, len(c), self.frames)
        self.nodes.append((name, [list(map(float, c)) for c in rot],
                           [list(map(float, c)) for c in trans] if trans else None, extra_flags))


# ------------------------------------------------------------------ the dictionary (Patricia tree)

def _bit(name, ref):
    i = ref >> 3
    return (name[i] >> (ref & 7)) & 1 if i < len(name) else 0


def _first_difference(a, b):
    """The highest bit index at which the names differ (bytes past an end read 0)."""
    for i in range(max(len(a), len(b)) - 1, -1, -1):
        x = (a[i] if i < len(a) else 0) ^ (b[i] if i < len(b) else 0)
        if x:
            return (i << 3) | (x.bit_length() - 1)
    raise ValueError("two entries named %r" % a)


def dict_tree(names):
    """[(ref, left, right)] for the root and each name (entry i + 1), inserted in order."""
    names = [n.encode() if isinstance(n, str) else n for n in names]
    ent = [[0xFFFF, 0, 0, b""]]
    for k, name in enumerate(names):
        idx = k + 1
        # the entry the search for the name ends at, and the highest bit they differ at
        x, y = ent[0], ent[ent[0][1]]
        while x[0] > y[0]:
            x, y = y, ent[y[2] if _bit(name, y[0]) else y[1]]
        ref = _first_difference(name, y[3])
        # down again to where that bit belongs
        px, x = 0, ent[0][1]
        while ent[px][0] > ent[x][0] and ent[x][0] > ref:
            px, x = x, ent[x][2] if _bit(name, ent[x][0]) else ent[x][1]
        e = [ref, 0, 0, name]
        if _bit(name, ref):
            e[1], e[2] = x, idx
        else:
            e[1], e[2] = idx, x
        ent.append(e)
        if px == 0 or not _bit(name, ent[px][0]):
            ent[px][1] = idx
        else:
            ent[px][2] = idx
    return [(e[0], e[1], e[2]) for e in ent]


def dict_lookup(tree, names, name):
    """nw4r ResDic::Get over dict_tree's entries: the entry index found for `name`, or None."""
    name = name.encode() if isinstance(name, str) else name
    names = [b""] + [n.encode() if isinstance(n, str) else n for n in names]
    x, y = 0, tree[0][1]
    while tree[x][0] > tree[y][0]:
        x = y
        y = tree[y][2] if _bit(name, tree[y][0]) else tree[y][1]
    return y if y and names[y] == name else None


# ------------------------------------------------------------------ the file

class _Out:
    def __init__(self):
        self.b = bytearray()
        self.fix = []          # (where, kind, value, base): offsets patched once the strings are placed

    def align(self, n):
        while len(self.b) % n:
            self.b.append(0)

    def u8(self, v):
        self.b += struct.pack(">B", v)

    def u16(self, v):
        self.b += struct.pack(">H", v)

    def u32(self, v):
        self.b += struct.pack(">I", v & 0xFFFFFFFF)

    def s32(self, v):
        self.b += struct.pack(">i", v)

    def f32(self, v):
        self.b += struct.pack(">f", v)

    def name_ref(self, s, base):
        """an s32 to the string `s` from `base` (patched later)"""
        self.fix.append((len(self.b), s, base))
        self.s32(0)

    def put_s32(self, at, v):
        struct.pack_into(">i", self.b, at, v)

    def put_u32(self, at, v):
        struct.pack_into(">I", self.b, at, v)


def _dictionary(o, names, datas_at):
    """A dictionary at the current position; returns where each entry's data word is."""
    base = len(o.b)
    tree = dict_tree(names)
    o.u32(8 + 16 * (len(names) + 1))
    o.u32(len(names))
    words = []
    for i, (ref, left, right) in enumerate(tree):
        o.u16(ref)
        o.u16(0)
        o.u16(left)
        o.u16(right)
        if i == 0:
            o.s32(0)
            o.s32(0)
        else:
            o.name_ref(names[i - 1], base)
            words.append(len(o.b))
            o.s32(0)
    datas_at.append((base, words))
    return base


def _frm8(values):
    """(step, base, bytes) if FRM8 holds the values within ROT8_ERROR, else None"""
    lo, hi = min(values), max(values)
    step = (hi - lo) / 255.0
    q = [int(round((v - lo) / step)) if step > 0 else 0 for v in values]
    if max(abs(lo + step * k - v) for k, v in zip(q, values)) > ROT8_ERROR:
        return None
    return step, lo, q


def _trans_track(o, values):
    n = len(values)
    o.u16(n)
    o.u16(0)
    o.f32(1.0 / max(n - 1, 1))
    for f, v in enumerate(values):
        a, b = values[max(f - 1, 0)], values[min(f + 1, n - 1)]
        slope = (b - a) / max(min(f + 1, n - 1) - max(f - 1, 0), 1)
        o.f32(float(f))
        o.f32(v)
        o.f32(slope)


def _constant(values, tol=1e-4):
    return max(values) - min(values) <= tol


def _chr0(o, clip, file_start):
    start = len(o.b)
    o.b += b"CHR0"
    o.u32(0)                       # size (patched)
    o.u32(5)
    o.s32(file_start - start)
    o.s32(0x2C)                    # the node dictionary
    o.s32(0)                       # user data
    o.name_ref(clip.name, start)
    o.s32(0)                       # original path
    o.u16(clip.frames)
    o.u16(len(clip.nodes))
    o.u32(1 if clip.loop else 0)
    o.u32(2)
    assert len(o.b) - start == 0x2C
    dicts = []
    _dictionary(o, [n[0] for n in clip.nodes], dicts)
    node_dict, words = dicts[0]
    # the nodes' data, then their tracks
    slots = []
    for (name, rot, trans, extra), word in zip(clip.nodes, words):
        at = len(o.b)
        o.put_s32(word, at - node_dict)
        o.name_ref(name, at)
        flags = F_PRESENT | F_S_ONE | F_S_UNIFORM | F_HAS_R | extra
        comps = [(3 + i, rot[i]) for i in range(3)]
        if trans:
            flags |= F_HAS_T
            comps += [(6 + i, trans[i]) for i in range(3)]
        flags_at = len(o.b)
        o.u32(0)
        mine = []
        for k, vals in comps:
            if _constant(vals):
                flags |= 0x2000 << k
                o.f32(vals[0])
            else:
                mine.append((len(o.b), k, vals))
                o.s32(0)
        slots.append((at, flags_at, flags, mine))
    for at, flags_at, flags, mine in slots:
        # (one rotation format a node: FRM8 if it holds all its rotations, else FRM32)
        rots = [vals for _, k, vals in mine if k < 6]
        packed = [_frm8(v) for v in rots]
        rot_fmt = FMT_FRM8 if rots and all(packed) else FMT_FRM32
        for word, k, vals in mine:
            o.align(4)
            track = len(o.b)
            if k >= 6:
                _trans_track(o, vals)
            elif rot_fmt == FMT_FRM8:
                step, base, q = _frm8(vals)
                o.f32(step)
                o.f32(base)
                for v in q:
                    o.u8(v)
            else:
                for v in vals:
                    o.f32(v)
            o.put_s32(word, track - at)
        if rots:
            flags |= rot_fmt << 27
        if any(k >= 6 for _, k, _ in mine):
            flags |= FMT_FVS96 << 30
        o.put_u32(flags_at, flags)
    o.align(4)
    o.put_u32(start + 4, len(o.b) - start)
    return start


def brres(clips):
    """The BRRES file of these clips (bytes)."""
    o = _Out()
    o.b += b"bres"
    o.u16(0xFEFF)
    o.u16(0)
    o.u32(0)                       # size (patched)
    o.u16(0x10)
    o.u16(1 + len(clips))
    root = len(o.b)
    o.b += b"root"
    o.u32(0)                       # size (patched)
    dicts = []
    _dictionary(o, [FOLDER], dicts)
    folder = _dictionary(o, [c.name for c in clips], dicts)
    o.put_u32(root + 4, len(o.b) - root)
    root_dict, (folder_word,) = dicts[0]
    o.put_s32(folder_word, folder - root_dict)
    _, words = dicts[1]
    for clip, word in zip(clips, words):
        o.align(0x10)
        at = _chr0(o, clip, 0)
        o.put_s32(word, at - folder)
    # the strings
    o.align(4)
    placed = {}
    for where, s, base in o.fix:
        if s not in placed:
            o.align(4)
            raw = s.encode()
            o.u32(len(raw))
            placed[s] = len(o.b)
            o.b += raw + b"\0"
        o.put_s32(where, placed[s] - base)
    o.align(4)
    o.put_u32(8, len(o.b))
    return bytes(o.b)


def check(paths):
    """dict_tree against the trees of existing files: True if every dictionary comes out the same."""
    import brres as rd
    ok = True
    for p in paths:
        b = rd.Buf(open(p, "rb").read())
        def walk(off):
            nonlocal ok
            n = b.u32(off + 4)
            entries = [(b.u16(off + 8 + 16 * i), b.u16(off + 12 + 16 * i), b.u16(off + 14 + 16 * i))
                       for i in range(n + 1)]
            names = [nm for nm, _ in rd.read_dict(b, off)]
            if dict_tree(names) != entries:
                ok = False
                print("%s: the tree at %#x differs" % (p, off))
            return rd.read_dict(b, off)
        root = b.u16(0x0C)
        for _, fo in walk(root + 8):
            for _, so in walk(fo):
                if bytes(b.d[so:so + 4]) == b"CHR0":
                    walk(so + b.s32(so + 0x10))
    return ok


if __name__ == "__main__":
    import sys
    sys.exit(0 if check(sys.argv[1:]) else 1)
