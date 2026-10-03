"""Find C++ vtables in a CodeWarrior DOL through RTTI.

CodeWarrior emits a vtable as {type_info*, 0 (this-adjust), fn0, fn1, ...}, where type_info
starts with a pointer to the class name. The virtual-call analysis in signatures.py needs to
know which functions can be reached from each vtable slot.
"""
import collections
import re
import struct

_NAME = re.compile(rb"^[A-Za-z_@][A-Za-z0-9_:<>,\* &@\.\(\)\-]*$")


def scan_vtables(dol):
    """{vtable address: (class name, [function addresses])}"""
    data = [s for s in dol.data]
    words = collections.defaultdict(list)  # word value -> data addresses holding it
    for s in data:
        for o in range(0, s.size - 3, 4):
            words[struct.unpack(">I", s.data[o:o + 4])[0]].append(s.addr + o)
    type_info = {}  # candidate type_info address -> class name
    for s in data:
        for o in range(0, s.size - 3, 4):
            v = struct.unpack(">I", s.data[o:o + 4])[0]
            sec = dol.section_at(v)
            if sec is None or sec.kind != "data":
                continue
            b = dol.cstring(v, 300)
            if not b or len(b) < 2 or not _NAME.match(b):
                continue
            type_info[s.addr + o] = b.decode()
    vtables = {}
    for ti, name in type_info.items():
        for va in words.get(ti, []):
            if dol.read32(va + 4) != 0:
                continue
            fns = []
            a = va + 8
            while True:
                w = dol.read32(a)
                if w is None or not dol.is_text(w):
                    break
                fns.append(w)
                a += 4
            if fns:
                vtables[va] = (name, fns)
    return vtables
