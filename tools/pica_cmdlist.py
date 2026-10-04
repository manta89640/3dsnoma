#!/usr/bin/env python3
"""Reads cmdlist.bin (NOMA_DUMP_CMDLIST=N on the 3DS: platform/3ds/gx_pica.cpp) and checks the
command lists the way the PICA will read them: each command's header, every draw's buffers (their
physical addresses inside VRAM or the linear heap, the index buffer's 28-bit offset), the render
target, the textures, and the registers written that the GPU doesn't have.

    tools/pica_cmdlist.py cmdlist.bin [--regs REGISTERS_H] [--trace FRAME]

--trace FRAME prints every register write of that frame's lists, named."""
import argparse
import os
import re
import struct
import sys

VRAM = (0x18000000, 0x18600000)


def reg_names(path):
    names = {}
    if path and os.path.exists(path):
        for m in re.finditer(r"#define\s+(GPUREG_\w+)\s+0x([0-9A-Fa-f]+)", open(path).read()):
            names.setdefault(int(m.group(2), 16), m.group(1))
    return names


def read(path):
    data = open(path, "rb").read()
    words = struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4])
    phys, lists, i = {}, [], 0
    while i < len(words):
        magic = words[i]
        if magic == 0x53594850:  # PHYS
            n = words[i + 1]
            i += 2
            for _ in range(n):
                raw = struct.pack("<3I", *words[i : i + 3])
                name = raw.split(b"\0")[0].decode()
                va, pa, size = words[i + 3 : i + 6]
                phys[name] = (va, pa, size)
                i += 6
        elif magic == 0x5453494C:  # LIST
            frame, seq, va, pa, n = words[i + 1 : i + 6]
            body = words[i + 6 : i + 6 + n]
            if len(body) < n:
                print("list %d (frame %d) cut short: %d of %d words" % (seq, frame, len(body), n))
            lists.append(dict(frame=frame, seq=seq, va=va, pa=pa, words=body))
            i += 6 + n
        else:
            sys.exit("unknown record %08X at word %d" % (magic, i))
    return phys, lists


def commands(words):
    """(index, register, value, mask, extra) per register write, and the problems found"""
    out, bad, i = [], [], 0
    while i + 1 < len(words):
        hdr = words[i + 1]
        reg, nib, extra, seq = hdr & 0xFFFF, (hdr >> 16) & 0xF, (hdr >> 20) & 0x7FF, hdr >> 31
        if (hdr >> 28) & 7:
            bad.append("word %d: header %08X has bits 28-30 set (more than 256 parameters?)" % (i + 1, hdr))
        if reg >= 0x300:
            bad.append("word %d: register %03X doesn't exist (header %08X)" % (i + 1, reg, hdr))
        mask = sum(0xFF << (8 * b) for b in range(4) if nib >> b & 1)
        for j in range(extra + 1):
            k = i if j == 0 else i + 1 + j
            if k >= len(words):
                bad.append("word %d: command runs past the list's end" % (i + 1))
                break
            out.append((i, reg + (j if seq else 0), words[k], mask, extra))
        i += 2 + extra
        i += i & 1
    return out, bad


def inside(pa, size, phys):
    if VRAM[0] <= pa and pa + size <= VRAM[1]:
        return "VRAM"
    lin = phys.get("linear")
    if lin and lin[1] <= pa and pa + size <= lin[1] + lin[2]:
        return "linear"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--regs", default=os.path.expanduser("~/devkitpro-3ds/libctru/include/3ds/gpu/registers.h"))
    ap.add_argument("--trace", type=int, default=-1)
    a = ap.parse_args()
    names = reg_names(a.regs)
    name = lambda r: names.get(r, "%03X" % r)
    phys, lists = read(a.dump)
    for k, (va, pa, size) in phys.items():
        print("%-7s va %08X pa %08X size %08X (pa end %08X)" % (k, va, pa, size, pa + size))
    total_bad = 0
    reg = [0] * 0x300  # (carried from list to list, as the GPU keeps them)
    for L in lists:
        cmds, bad = commands(L["words"])
        draws = 0
        for i, r, v, m, extra in cmds:
            if r < 0x300:
                reg[r] = (reg[r] & ~m) | (v & m)
            if L["frame"] == a.trace:
                print("  f%d l%d w%-6d %-40s = %08X (mask %08X)" % (L["frame"], L["seq"], i, name(r), v, m))
            if r in (0x22E, 0x22F) and m & 1:  # GPUREG_DRAWARRAYS, GPUREG_DRAWELEMENTS
                draws += 1
                where = "list %d draw %d" % (L["seq"], draws)
                base = (reg[0x200] & 0x1FFFFFFE) << 3
                nattr = (reg[0x202] >> 28) + 1
                n = reg[0x228]
                for b in range(12):
                    off = reg[0x203 + 3 * b] & 0x0FFFFFFF
                    cfg2 = reg[0x205 + 3 * b]
                    count = cfg2 >> 28
                    stride = (cfg2 >> 16) & 0xFF
                    if not count:
                        continue
                    pa = base + off
                    if not inside(pa, max(stride, 1), phys):
                        bad.append("%s: attribute buffer %d at pa %08X (base %08X + %07X) is outside VRAM and the linear heap"
                                   % (where, b, pa, base, off))
                if r == 0x22F:
                    ic = reg[0x227]
                    ipa = base + (ic & 0x0FFFFFFF)
                    isz = n * (2 if ic >> 31 else 1)
                    if not inside(ipa, isz, phys):
                        bad.append("%s: index buffer at pa %08X (%d indices) is outside VRAM and the linear heap"
                                   % (where, ipa, n))
                    if ic >> 31 and ipa & 1:
                        bad.append("%s: 16-bit indices at an odd address %08X" % (where, ipa))
                for u, ar in ((0, 0x085), (1, 0x095), (2, 0x09D)):
                    if reg[0x080] & (1 << u):
                        tpa = (reg[ar] & 0x0FFFFFFF) << 3
                        if not inside(tpa, 8, phys):
                            bad.append("%s: texture unit %d at pa %08X is outside VRAM and the linear heap" % (where, u, tpa))
                cpa, dpa = (reg[0x11D] & 0x0FFFFFFF) << 3, (reg[0x11C] & 0x0FFFFFFF) << 3
                if not inside(cpa, 4, phys):
                    bad.append("%s: color buffer at pa %08X" % (where, cpa))
                if (reg[0x114] or reg[0x115]) and not inside(dpa, 4, phys):
                    bad.append("%s: depth buffer at pa %08X" % (where, dpa))
                if nattr != (reg[0x2B9] & 0xF) + 1 or nattr != (reg[0x242] & 0xF) + 1:
                    bad.append("%s: %d attributes loaded, the shader told %d (2B9) / %d (242)"
                               % (where, nattr, (reg[0x2B9] & 0xF) + 1, (reg[0x242] & 0xF) + 1))
        size = len(L["words"])
        print("frame %d list %d: pa %08X, %d words, %d commands, %d draws, %d problems"
              % (L["frame"], L["seq"], L["pa"], size, len(cmds), draws, len(bad)))
        for b in bad[:40]:
            print("   ", b)
        if len(bad) > 40:
            print("    ... %d more" % (len(bad) - 40))
        total_bad += len(bad)
    print("%d lists, %d problems" % (len(lists), total_bad))


if __name__ == "__main__":
    main()
