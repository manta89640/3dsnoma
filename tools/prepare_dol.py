#!/usr/bin/env python3
"""Make orig/HCIJ/sys/main.dol from the WAD's main program (content 1, LZ77-decompressed).

    tools/prepare_dol.py extracted/fs/00000025.dec orig/HCIJ/sys/main.dol

WiiLink patched a few functions by overwriting their first instructions with an early return
(for example "li r3, 0; blr"). The rest of each patched function can never run, but its
epilogue without a prologue stops decomp-toolkit's function analysis. Those dead instructions
are replaced with nops here; nothing that can execute changes. Each patch is checked against
the expected original words, so a different build of the channel fails loudly.
"""
import hashlib
import struct
import sys

NOP = 0x60000000

# (first dead address, last dead address + 4, why)
DEAD_CODE = [
    # fn_802AF08C: "li r3, 0; blr" over the prologue of a check for title 00010008-HALJ
    (0x802AF094, 0x802AF0DC, "WiiLink early return in fn_802AF08C"),
]

EXPECTED_SHA1 = "1efe619f2d1578b980271b505cf4b1075b3033c8"  # content 1 of HCIJ v1025, decompressed


def dol_offset(raw, addr):
    to = struct.unpack(">18I", raw[0:72])
    ta = struct.unpack(">18I", raw[72:144])
    ts = struct.unpack(">18I", raw[144:216])
    for off, a, s in zip(to, ta, ts):
        if s and a <= addr < a + s:
            return off + addr - a
    raise ValueError("address %08x not in the DOL" % addr)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    raw = bytearray(open(src, "rb").read())
    sha = hashlib.sha1(raw).hexdigest()
    if sha != EXPECTED_SHA1:
        sys.exit("%s: sha1 %s, expected %s (a different build of the channel?)" % (src, sha, EXPECTED_SHA1))
    for start, end, why in DEAD_CODE:
        prev = struct.unpack(">I", raw[dol_offset(raw, start - 4):dol_offset(raw, start - 4) + 4])[0]
        assert prev == 0x4E800020, "%08x: expected blr before the dead code (%s)" % (start - 4, why)
        for a in range(start, end, 4):
            o = dol_offset(raw, a)
            raw[o:o + 4] = struct.pack(">I", NOP)
    open(dst, "wb").write(raw)
    print("%s: %d dead-code patches, sha1 %s" % (dst, len(DEAD_CODE), hashlib.sha1(raw).hexdigest()))


if __name__ == "__main__":
    main()
