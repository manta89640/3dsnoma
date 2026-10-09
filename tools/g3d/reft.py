"""REFT: nw4r::ef's texture files (file version 11), as Wii Sports Resort's effect archives carry them
(Effect/<scene>/effect.carc: WS2_effect_<x>.breft beside the effects' .breff).

    textures = reft.read(data)        # {name: Texture}, in the file's order
    rgba = textures["rk_hamon2"].rgba()   # level 0 as (h, w, 4) uint8, row 0 at the top (gxtex.decode)

The file (big-endian): a 16-byte header ("REFT", the byte order mark FEFF, the version, the file's size,
the header's size, the block count), then one block ("REFT", its size; at +8 the offset, from there, of
its name table). The name table: its size, the entry count (u16, then two bytes of padding), then a
texture an entry: the name's length (u16, its NUL included), the name, the texture's offset from the
table and its image's size. A texture is a 0x40-byte header (version 11's: the library's 0x20 and 0x20
more), its image, then its palette: at +4 the width and height (u16), +8 the image's size, +0xC the GX
format, +0xD the palette's format, +0xE its entries (u16), +0x10 its size, +0x14 whether the image has
mip levels.
"""
import struct

import gxtex

VERSION = 11
TEXTURE_HEADER = 0x40


class Texture:
    """One texture of a REFT file: name, width, height, fmt (GX's), mipmap (whether `image` has smaller
    levels after level 0), image (its texel bytes as GX keeps them), palette (bytes, or None) and
    palette_fmt."""

    def __init__(self, name, width, height, fmt, mipmap, image, palette, palette_fmt):
        self.name, self.width, self.height, self.fmt, self.mipmap = name, width, height, fmt, mipmap
        self.image, self.palette, self.palette_fmt = image, palette, palette_fmt

    def rgba(self):
        """Level 0 as (h, w, 4) uint8 RGBA: intensity formats as GX's TEV reads them (I8: alpha =
        intensity; IA8: its own alpha)."""
        pal = gxtex.decode_palette(self.palette, self.palette_fmt) if self.palette else None
        return gxtex.decode(self.image, self.width, self.height, self.fmt, pal)


def read(data):
    """A REFT file's bytes -> {name: Texture}."""
    if data[:4] != b"REFT" or data[4:6] != b"\xfe\xff":
        raise ValueError("not a big-endian REFT file (%r)" % bytes(data[:6]))
    version, size = struct.unpack_from(">HI", data, 6)
    if version != VERSION:
        raise ValueError("REFT version %d (this reads %d)" % (version, VERSION))
    if size != len(data):
        raise ValueError("REFT: the header says %d bytes, the file has %d" % (size, len(data)))
    block = struct.unpack_from(">H", data, 0xC)[0]
    if data[block:block + 4] != b"REFT":
        raise ValueError("REFT: no block at %#x" % block)
    table = block + 8 + struct.unpack_from(">I", data, block + 8)[0]
    count = struct.unpack_from(">H", data, table + 4)[0]
    out, at = {}, table + 8
    for _ in range(count):
        n = struct.unpack_from(">H", data, at)[0]
        name = bytes(data[at + 2:at + 2 + n]).split(b"\0")[0].decode("ascii")
        offset, image_size = struct.unpack_from(">II", data, at + 2 + n)
        at += 2 + n + 8
        h = table + offset
        width, height, size2, fmt, palette_fmt, _, palette_size, mipmap = struct.unpack_from(">HHIBBHIB", data, h + 4)
        if size2 != image_size or h + TEXTURE_HEADER + image_size + palette_size > len(data):
            raise ValueError("REFT: texture %s (%d bytes at %#x) runs past the file or disagrees with the "
                             "table (%d)" % (name, size2, h, image_size))
        image = bytes(data[h + TEXTURE_HEADER:h + TEXTURE_HEADER + image_size])
        palette = bytes(data[h + TEXTURE_HEADER + image_size:h + TEXTURE_HEADER + image_size + palette_size]) \
            if palette_size else None
        out[name] = Texture(name, width, height, fmt, bool(mipmap), image, palette, palette_fmt)
    return out
