#!/usr/bin/env python3
"""Write a Wii Mii database (RFL_DB.dat) with test Miis that all look different.

    tools/make_mii_db.py OUT [--keep DB] [--count N]

--keep copies the Miis of an existing database first (so saves that name one keep working); the
presets below fill the rest up to --count (default 4). The layout is WiiBrew's "Mii Data"
(RFLCharData, 0x4A bytes, big-endian bit fields) and the database runtime/mii.cpp writes:
"RNOD", 100 slots, flags at 0x1CEC, "RNHD" with 10000 empty entries, and a CRC-16 that makes the
whole file's CRC 0.
"""
import argparse
import struct

SIZE = 0x1F1E0
SLOT = 0x4A

# Each preset is a different person: build, colours, and every face part.
PRESETS = [
    dict(name="たろう", male=True, month=4, day=1, color=0, height=70, weight=60,
         face=0, skin=1, feature=0, hair=1, hair_color=0, flip=0,
         brow=(0, 6, 0, 4, 10, 2), eye=(2, 4, 12, 0, 4, 2), nose=(1, 4, 9), lips=(0, 0, 4, 13),
         glasses=(0, 0, 4, 10), facial=(0, 0, 0, 4, 10), mole=(0, 4, 20, 2)),
    dict(name="はなこ", male=False, month=7, day=7, color=7, height=50, weight=45,
         face=2, skin=0, feature=1, hair=12, hair_color=1, flip=1,
         brow=(5, 5, 1, 4, 10, 2), eye=(7, 4, 12, 1, 4, 2), nose=(2, 3, 9), lips=(5, 1, 4, 13),
         glasses=(0, 0, 4, 10), facial=(0, 0, 0, 4, 10), mole=(0, 4, 20, 2)),
    dict(name="じろう", male=True, month=11, day=23, color=4, height=100, weight=90,
         face=5, skin=3, feature=0, hair=40, hair_color=2, flip=0,
         brow=(10, 6, 2, 5, 9, 3), eye=(14, 3, 12, 2, 3, 3), nose=(6, 6, 9), lips=(10, 0, 5, 14),
         glasses=(1, 0, 4, 10), facial=(1, 2, 2, 4, 10), mole=(0, 4, 20, 2)),
    dict(name="ゆき", male=False, month=12, day=25, color=5, height=30, weight=30,
         face=6, skin=4, feature=0, hair=60, hair_color=6, flip=1,
         brow=(15, 5, 6, 3, 10, 2), eye=(30, 4, 11, 4, 5, 2), nose=(9, 2, 9), lips=(15, 2, 3, 12),
         glasses=(3, 4, 4, 10), facial=(0, 0, 0, 4, 10), mole=(1, 3, 20, 8)),
]


def char_data(p, index, system_id):
    """RFLCharData for preset p (its Mii ID made unique by index)."""
    w = bytearray(SLOT)
    struct.pack_into(">H", w, 0x00, (0 if p["male"] else 1) << 14 | p["month"] << 10 | p["day"] << 5 |
                     p["color"] << 1)
    name = p["name"].encode("utf-16-be")[:20]
    w[0x02:0x02 + len(name)] = name
    w[0x16], w[0x17] = p["height"], p["weight"]
    struct.pack_into(">I", w, 0x18, 0x80000000 | (0x0C0F1000 + 0x10 * index))  # made on this console
    w[0x1C:0x20] = system_id
    struct.pack_into(">H", w, 0x20, p["face"] << 13 | p["skin"] << 10 | p["feature"] << 6)
    struct.pack_into(">H", w, 0x22, p["hair"] << 9 | p["hair_color"] << 6 | p["flip"] << 5)
    t, rot, col, size, y, x = p["brow"]
    struct.pack_into(">I", w, 0x24, t << 27 | rot << 22 | col << 13 | size << 9 | y << 4 | x)
    t, rot, y, col, size, x = p["eye"]
    struct.pack_into(">I", w, 0x28, t << 26 | rot << 21 | y << 16 | col << 13 | size << 9 | x << 5)
    t, size, y = p["nose"]
    struct.pack_into(">H", w, 0x2C, t << 12 | size << 8 | y << 3)
    t, col, size, y = p["lips"]
    struct.pack_into(">H", w, 0x2E, t << 11 | col << 9 | size << 5 | y)
    t, col, size, y = p["glasses"]
    struct.pack_into(">H", w, 0x30, t << 12 | col << 9 | size << 5 | y)
    mustache, beard, col, size, y = p["facial"]
    struct.pack_into(">H", w, 0x32, mustache << 14 | beard << 12 | col << 9 | size << 5 | y)
    on, size, y, x = p["mole"]
    struct.pack_into(">H", w, 0x34, on << 15 | size << 11 | y << 6 | x << 1)
    return bytes(w)


def rfl_crc(data, crc=0):
    """The CRC RFL checks: per bit, shift left, add the polynomial if the top bit fell out, shift
    the data bit in at the bottom (as runtime/mii.cpp's rfl_crc)."""
    for byte in data:
        for b in range(7, -1, -1):
            top = crc & 0x8000
            crc = (crc << 1) & 0xFFFF
            if top:
                crc ^= 0x1021
            if (byte >> b) & 1:
                crc ^= 1
    return crc


def database(miis):
    db = bytearray(SIZE)
    db[0:4] = b"RNOD"
    for i, m in enumerate(miis[:100]):
        db[4 + SLOT * i:4 + SLOT * (i + 1)] = m
    struct.pack_into(">I", db, 0x1CEC, 0x80000000)
    db[0x1D00:0x1D04] = b"RNHD"
    struct.pack_into(">HH", db, 0x1D04, 0xFFFF, 0xFFFF)
    for i in range(10000):
        struct.pack_into(">HH", db, 0x1D08 + 12 * i + 8, 0x7FFF, 0x7FFF)
    crc = rfl_crc(bytes(db[:0x1F1DE]) + b"\0\0")
    struct.pack_into(">H", db, 0x1F1DE, crc)
    assert rfl_crc(bytes(db)) == 0
    return bytes(db)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--keep", help="a database whose Miis come first")
    ap.add_argument("--count", type=int, default=4)
    args = ap.parse_args()
    miis, system_id = [], bytes.fromhex("51abcdef")
    if args.keep:
        old = open(args.keep, "rb").read()
        for i in range(100):
            m = old[4 + SLOT * i:4 + SLOT * (i + 1)]
            if m[2] or m[3]:  # a named slot
                miis.append(m)
                system_id = m[0x1C:0x20]
    for i, p in enumerate(PRESETS):
        if len(miis) >= args.count:
            break
        miis.append(char_data(p, i, system_id))
    open(args.out, "wb").write(database(miis))
    for m in miis:
        print("  %s" % m[2:0x16].decode("utf-16-be").split("\0")[0])
    print("%d Miis -> %s" % (len(miis), args.out))


if __name__ == "__main__":
    main()
