#!/usr/bin/env python3
"""Unpack every content of the decrypted WAD (tools/wad_extract.py output) into a file tree.

    tools/unpack_all.py extracted/wad extracted/fs

U8 archives are unpacked (recursively, including LZ77/Yaz0-compressed .arc files inside them),
the compressed main program is decompressed to main.dol, and a listing goes to files.txt.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from nintendo_formats import (U8_MAGIC, imet_names, lz77_decompress, maybe_decompress,
                              u8_extract)


def unpack_tree(root, log):
    """Unpack nested archives found under root, in place (x.arc -> x.arc.d/)."""
    changed = True
    while changed:
        changed = False
        for dirpath, _dirs, files in os.walk(root):
            for fn in files:
                p = os.path.join(dirpath, fn)
                if os.path.exists(p + ".d"):
                    continue
                with open(p, "rb") as f:
                    head = f.read(16)
                lz_name = fn.endswith((".arc", ".lz", ".LZ", ".lex", ".lz7", "_LZ.bin"))
                if head[:4] in (b"Yaz0", b"LZ77", U8_MAGIC) or (head[:1] in (b"\x10", b"\x11") and lz_name):
                    data = open(p, "rb").read()
                    try:
                        if head[:1] in (b"\x10", b"\x11"):
                            data, how = lz77_decompress(data), "lz77"
                        else:
                            data, how = maybe_decompress(data)
                    except Exception as e:  # not actually compressed
                        log.append("  %s: decompress failed (%s)" % (p, e))
                        continue
                    if data[:4] == U8_MAGIC:
                        u8_extract(data, p + ".d")
                        log.append("  %s: %s U8 -> %s.d/" % (p, how or "plain", p))
                        changed = True
                    elif how:
                        with open(p + ".dec", "wb") as f:
                            f.write(data)
                        os.makedirs(p + ".d", exist_ok=True)  # marker: done
                        log.append("  %s: %s -> .dec (%s)" % (p, how, data[:4]))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    log = []
    for fn in sorted(os.listdir(src)):
        if not fn.endswith(".app"):
            continue
        data = open(os.path.join(src, fn), "rb").read()
        name = fn[:-4]
        if data.find(b"IMET", 0, 0x100) >= 0:
            log.append("%s: banner, names %s" % (fn, imet_names(data)))
            i = data.find(b"IMET")
            body = data[i - 0x40 + 0x600:] if False else data[data.find(U8_MAGIC):]
            u8_extract(body, os.path.join(dst, name + "_banner"))
        elif data[:4] == U8_MAGIC:
            files = u8_extract(data, os.path.join(dst, name))
            log.append("%s: U8, %d files" % (fn, len(files)))
        elif data[0] in (0x10, 0x11):
            dec = lz77_decompress(data)
            out = os.path.join(dst, name + ".dec")
            open(out, "wb").write(dec)
            log.append("%s: LZ77 type %02x -> %d bytes, head %s" % (fn, data[0], len(dec), dec[:16].hex()))
        else:
            open(os.path.join(dst, fn), "wb").write(data)
            log.append("%s: raw, head %s" % (fn, data[:16].hex()))
    unpack_tree(dst, log)

    listing = []
    for dirpath, _dirs, files in os.walk(dst):
        for fn in sorted(files):
            p = os.path.join(dirpath, fn)
            listing.append("%10d  %s" % (os.path.getsize(p), os.path.relpath(p, dst)))
    listing.sort(key=lambda s: s[12:])
    open(os.path.join(dst, "files.txt"), "w").write("\n".join(listing) + "\n")
    print("\n".join(log))
    print("%d files, listing in %s" % (len(listing), os.path.join(dst, "files.txt")))


if __name__ == "__main__":
    main()
