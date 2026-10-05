#!/usr/bin/env python3
"""Wii Miis (RFLCharData) authored by named parameters.

    from mii import Mii
    m = Mii(name="ミドハ", hair=30, eye=30, eye_color=4, ...)   # every field has the Mii Channel's default
    record = m.to_bytes(mii_id, system_id)                       # the 0x4A bytes RFL reads
    Mii.from_bytes(record).params()                              # and back

    mii.py --dump DB.dat     every Mii of a Wii Mii database (RFL_DB.dat), as named parameters

The record is WiiBrew's "Mii Data" (0x4A bytes, big-endian bit fields), checked against the
channel's own RFL code (RFLiConvertRaw2Info 0x804CD650 reads it into RFLiCharInfo,
RFLiCheckValidInfo 0x804CEA80 gives the ranges below; a Mii out of range is refused by RFL):

  0x00 u16  invalid:1 sex:1 (1 female) month:4 day:5 fav_color:4 (0-11) favorite:1
  0x02      name, 10 UTF-16BE characters
  0x16 u8   height 0-127        0x17 u8 weight 0-127
  0x18 u32  Mii ID (top bits 100: made on this console; RFL wants the create ID, this and the
            next 4 bytes, not all zero)                 0x1C 4 bytes: console (system) ID
  0x20 u16  face:3 (0-7) skin:3 (0-5) feature:4 (0-11) ...:3 mingle_off:1 ...:1 downloaded:1
  0x22 u16  hair:7 (0-71) hair_color:3 (0-7) hair_flip:1
  0x24 u32  brow:5 (0-23) brow_rot:5 (0-11) ...:3 brow_color:3 (0-7) brow_size:4 (0-8) brow_y:5 (3-18) brow_x:4 (0-12)
  0x28 u32  eye:6 (0-47) eye_rot:5 (0-7) eye_y:5 (0-18) eye_color:3 (0-5) eye_size:4 (0-7) eye_x:4 (0-12) ...:5
  0x2C u16  nose:4 (0-11) nose_size:4 (0-8) nose_y:5 (0-18) ...:3
  0x2E u16  mouth:5 (0-23) mouth_color:2 (0-2) mouth_size:4 (0-8) mouth_y:5 (0-18)
  0x30 u16  glasses:4 (0-8) glasses_color:3 (0-5) glasses_size:4 (0-7) glasses_y:5 (0-20)
  0x32 u16  mustache:2 (0-3) beard:2 (0-3) beard_color:3 (0-7) mustache_size:4 (0-8) mustache_y:5 (0-16)
  0x34 u16  mole:1 mole_size:4 (0-8) mole_y:5 (0-30) mole_x:5 (0-16) ...:1
  0x36      creator name, 10 UTF-16BE characters

Part numbers are the indices of RFL's resource file (RFL_Res.dat): hair 30 is the bald head (no
hair shape, the skin-coloured skull alone), what a Mii with a hairstyle of its own made outside
RFL (tools/mii/midoha_parts.py) wears under it.

Rotations. RFL draws an eye at 11.25 degrees x ((eye_rot + EYE_ROT_OFFSET[eye]) mod 32) and a
brow at 11.25 x ((brow_rot + BROW_ROT_OFFSET[brow]) mod 32) (the face texture's layout,
fn_804CBB30; the offsets are the tables at 0x8071F850 and 0x8071F888 of the DOL). A type's own
angle (its texture upright) is rot = 32 - offset; that is what eye_rot / brow_rot default to here
(None), as the Mii Channel sets them when a type is picked.
"""
import argparse
import json
import struct

SLOT = 0x4A

# where each eye / brow type's texture stands upright (see Rotations)
EYE_ROT_OFFSET = [29, 28, 28, 28, 29, 28, 28, 28, 29, 28, 28, 28, 28, 29, 29, 28, 28, 28, 29, 29, 28, 29, 28, 29,
                  29, 28, 29, 28, 28, 29, 28, 28, 28, 29, 29, 29, 28, 28, 29, 29, 29, 28, 28, 29, 29, 29, 29, 29]
BROW_ROT_OFFSET = [26, 26, 27, 25, 26, 25, 26, 25, 28, 25, 26, 24, 27, 27, 26, 26, 25, 25, 26, 26, 27, 26, 25, 27]

EYE_COLOR_NAMES = ["black", "grey", "brown", "hazel", "blue", "green"]
FAV_COLOR_NAMES = ["red", "orange", "yellow", "light green", "green", "blue", "light blue", "pink", "purple",
                   "brown", "white", "black"]

# name -> (lowest, highest, default; None: derived, see Mii)
FIELDS = {
    "female": (0, 1, 0), "month": (0, 12, 0), "day": (0, 31, 0), "fav_color": (0, 11, 0), "favorite": (0, 1, 0),
    "height": (0, 127, 64), "weight": (0, 127, 64),
    "face": (0, 7, 0), "skin": (0, 5, 0), "feature": (0, 11, 0), "mingle_off": (0, 1, 0),
    "hair": (0, 71, 33), "hair_color": (0, 7, 1), "hair_flip": (0, 1, 0),
    "brow": (0, 23, 0), "brow_rot": (0, 11, None), "brow_color": (0, 7, None), "brow_size": (0, 8, 4),
    "brow_y": (3, 18, 10), "brow_x": (0, 12, 2),
    "eye": (0, 47, 0), "eye_rot": (0, 7, None), "eye_y": (0, 18, 12), "eye_color": (0, 5, 0), "eye_size": (0, 7, 4),
    "eye_x": (0, 12, 2),
    "nose": (0, 11, 1), "nose_size": (0, 8, 4), "nose_y": (0, 18, 9),
    "mouth": (0, 23, 23), "mouth_color": (0, 2, 0), "mouth_size": (0, 8, 4), "mouth_y": (0, 18, 13),
    "glasses": (0, 8, 0), "glasses_color": (0, 5, 0), "glasses_size": (0, 7, 4), "glasses_y": (0, 20, 10),
    "mustache": (0, 3, 0), "beard": (0, 3, 0), "beard_color": (0, 7, None), "mustache_size": (0, 8, 4),
    "mustache_y": (0, 16, 10),
    "mole": (0, 1, 0), "mole_size": (0, 8, 4), "mole_y": (0, 30, 20), "mole_x": (0, 16, 2),
}


class Mii:
    def __init__(self, name="Mii", creator="", **kw):
        unknown = set(kw) - set(FIELDS)
        if unknown:
            raise ValueError("unknown Mii fields: %s" % ", ".join(sorted(unknown)))
        self.name, self.creator = name, creator
        self.v = {k: kw.get(k, d[2]) for k, d in FIELDS.items()}
        v = self.v
        if v["eye_rot"] is None:
            v["eye_rot"] = (32 - EYE_ROT_OFFSET[v["eye"]]) & 31
        if v["brow_rot"] is None:
            v["brow_rot"] = (32 - BROW_ROT_OFFSET[v["brow"]]) & 31
        if v["brow_color"] is None:
            v["brow_color"] = v["hair_color"]
        if v["beard_color"] is None:
            v["beard_color"] = v["hair_color"]
        for k, (lo, hi, _) in FIELDS.items():
            if not lo <= v[k] <= hi:
                raise ValueError("%s=%r out of range %d..%d (RFLiCheckValidInfo)" % (k, v[k], lo, hi))
        if len(name) > 10 or len(creator) > 10:
            raise ValueError("names are at most 10 characters")

    def params(self):
        d = dict(name=self.name)
        if self.creator:
            d["creator"] = self.creator
        d.update(self.v)
        return d

    def to_bytes(self, mii_id, system_id):
        """The RFLCharData record: `mii_id` (u32, top bits 100 for "made on this console") and the
        4-byte `system_id` make its RFLCreateID."""
        v = self.v
        w = bytearray(SLOT)
        struct.pack_into(">H", w, 0x00, v["female"] << 14 | v["month"] << 10 | v["day"] << 5 | v["fav_color"] << 1 |
                         v["favorite"])
        n = self.name.encode("utf-16-be")
        w[0x02:0x02 + len(n)] = n
        w[0x16], w[0x17] = v["height"], v["weight"]
        struct.pack_into(">I", w, 0x18, mii_id)
        w[0x1C:0x20] = system_id
        struct.pack_into(">H", w, 0x20, v["face"] << 13 | v["skin"] << 10 | v["feature"] << 6 | v["mingle_off"] << 2)
        struct.pack_into(">H", w, 0x22, v["hair"] << 9 | v["hair_color"] << 6 | v["hair_flip"] << 5)
        struct.pack_into(">I", w, 0x24, v["brow"] << 27 | v["brow_rot"] << 22 | v["brow_color"] << 13 |
                         v["brow_size"] << 9 | v["brow_y"] << 4 | v["brow_x"])
        struct.pack_into(">I", w, 0x28, v["eye"] << 26 | v["eye_rot"] << 21 | v["eye_y"] << 16 | v["eye_color"] << 13 |
                         v["eye_size"] << 9 | v["eye_x"] << 5)
        struct.pack_into(">H", w, 0x2C, v["nose"] << 12 | v["nose_size"] << 8 | v["nose_y"] << 3)
        struct.pack_into(">H", w, 0x2E, v["mouth"] << 11 | v["mouth_color"] << 9 | v["mouth_size"] << 5 | v["mouth_y"])
        struct.pack_into(">H", w, 0x30, v["glasses"] << 12 | v["glasses_color"] << 9 | v["glasses_size"] << 5 |
                         v["glasses_y"])
        struct.pack_into(">H", w, 0x32, v["mustache"] << 14 | v["beard"] << 12 | v["beard_color"] << 9 |
                         v["mustache_size"] << 5 | v["mustache_y"])
        struct.pack_into(">H", w, 0x34, v["mole"] << 15 | v["mole_size"] << 11 | v["mole_y"] << 6 | v["mole_x"] << 1)
        c = self.creator.encode("utf-16-be")
        w[0x36:0x36 + len(c)] = c
        return bytes(w)

    @staticmethod
    def from_bytes(w):
        def u16(o):
            return struct.unpack_from(">H", w, o)[0]

        def u32(o):
            return struct.unpack_from(">I", w, o)[0]

        def bits(x, lo, n):
            return (x >> lo) & ((1 << n) - 1)

        a, f, h = u16(0x00), u16(0x20), u16(0x22)
        b, e = u32(0x24), u32(0x28)
        no, mo, gl, fa, ml = u16(0x2C), u16(0x2E), u16(0x30), u16(0x32), u16(0x34)
        kw = dict(female=bits(a, 14, 1), month=bits(a, 10, 4), day=bits(a, 5, 5), fav_color=bits(a, 1, 4),
                  favorite=bits(a, 0, 1), height=w[0x16], weight=w[0x17],
                  face=bits(f, 13, 3), skin=bits(f, 10, 3), feature=bits(f, 6, 4), mingle_off=bits(f, 2, 1),
                  hair=bits(h, 9, 7), hair_color=bits(h, 6, 3), hair_flip=bits(h, 5, 1),
                  brow=bits(b, 27, 5), brow_rot=bits(b, 22, 5), brow_color=bits(b, 13, 3), brow_size=bits(b, 9, 4),
                  brow_y=bits(b, 4, 5), brow_x=bits(b, 0, 4),
                  eye=bits(e, 26, 6), eye_rot=bits(e, 21, 5), eye_y=bits(e, 16, 5), eye_color=bits(e, 13, 3),
                  eye_size=bits(e, 9, 4), eye_x=bits(e, 5, 4),
                  nose=bits(no, 12, 4), nose_size=bits(no, 8, 4), nose_y=bits(no, 3, 5),
                  mouth=bits(mo, 11, 5), mouth_color=bits(mo, 9, 2), mouth_size=bits(mo, 5, 4), mouth_y=bits(mo, 0, 5),
                  glasses=bits(gl, 12, 4), glasses_color=bits(gl, 9, 3), glasses_size=bits(gl, 5, 4),
                  glasses_y=bits(gl, 0, 5),
                  mustache=bits(fa, 14, 2), beard=bits(fa, 12, 2), beard_color=bits(fa, 9, 3),
                  mustache_size=bits(fa, 5, 4), mustache_y=bits(fa, 0, 5),
                  mole=bits(ml, 15, 1), mole_size=bits(ml, 11, 4), mole_y=bits(ml, 6, 5), mole_x=bits(ml, 1, 5))
        name = w[0x02:0x16].decode("utf-16-be").split("\0")[0]
        creator = w[0x36:0x4A].decode("utf-16-be").split("\0")[0]
        return Mii(name=name, creator=creator, **kw)


def read_db(path):
    """The named Miis of a Wii Mii database ("RNOD", 100 slots of 0x4A from offset 4)."""
    d = open(path, "rb").read()
    out = []
    for i in range(100):
        m = d[4 + SLOT * i:4 + SLOT * (i + 1)]
        if m[2] or m[3]:
            out.append((m, Mii.from_bytes(m)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dump", required=True, help="print a database's Miis as named parameters")
    args = ap.parse_args()
    for raw, m in read_db(args.dump):
        print(json.dumps(dict(m.params(), create_id=raw[0x18:0x20].hex()), ensure_ascii=False))


if __name__ == "__main__":
    main()
