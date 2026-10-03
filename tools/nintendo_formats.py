"""Small readers for the Nintendo container formats inside the Wii no Ma WAD.

LZ77 (types 0x10 and 0x11, plus the "LZ77" magic prefix), U8 archives, Yaz0 and the IMET
banner header. Used by tools/unpack_all.py and the asset converters.
"""
import os
import struct


def lz77_decompress(data):
    """Nintendo LZ77 type 0x10 or 0x11 (LZ11). Accepts an optional b'LZ77' prefix."""
    if data[:4] == b"LZ77":
        data = data[4:]
    kind = data[0]
    size = data[1] | data[2] << 8 | data[3] << 16
    pos = 4
    if size == 0:
        size = struct.unpack("<I", data[4:8])[0]
        pos = 8
    out = bytearray()
    if kind == 0x10:
        while len(out) < size:
            flags = data[pos]; pos += 1
            for bit in range(8):
                if len(out) >= size:
                    break
                if flags & (0x80 >> bit):
                    b0, b1 = data[pos], data[pos + 1]; pos += 2
                    length = (b0 >> 4) + 3
                    disp = ((b0 & 0xF) << 8 | b1) + 1
                    for _ in range(length):
                        out.append(out[-disp])
                else:
                    out.append(data[pos]); pos += 1
    elif kind == 0x11:
        while len(out) < size:
            flags = data[pos]; pos += 1
            for bit in range(8):
                if len(out) >= size:
                    break
                if flags & (0x80 >> bit):
                    b0 = data[pos]
                    ind = b0 >> 4
                    if ind == 0:
                        b1, b2 = data[pos + 1], data[pos + 2]; pos += 3
                        length = ((b0 & 0xF) << 4 | b1 >> 4) + 0x11
                        disp = ((b1 & 0xF) << 8 | b2) + 1
                    elif ind == 1:
                        b1, b2, b3 = data[pos + 1], data[pos + 2], data[pos + 3]; pos += 4
                        length = ((b0 & 0xF) << 12 | b1 << 4 | b2 >> 4) + 0x111
                        disp = ((b2 & 0xF) << 8 | b3) + 1
                    else:
                        b1 = data[pos + 1]; pos += 2
                        length = ind + 1
                        disp = ((b0 & 0xF) << 8 | b1) + 1
                    start = len(out) - disp
                    if disp >= length:
                        out += out[start:start + length]
                    else:
                        for i in range(length):
                            out.append(out[start + i])
                else:
                    out.append(data[pos]); pos += 1
    else:
        raise ValueError("not LZ77 (type %02x)" % kind)
    return bytes(out[:size])


def yaz0_decompress(data):
    assert data[:4] == b"Yaz0"
    size = struct.unpack(">I", data[4:8])[0]
    pos = 16
    out = bytearray()
    while len(out) < size:
        flags = data[pos]; pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                out.append(data[pos]); pos += 1
            else:
                b0, b1 = data[pos], data[pos + 1]; pos += 2
                disp = ((b0 & 0xF) << 8 | b1) + 1
                n = b0 >> 4
                if n == 0:
                    n = data[pos] + 0x12; pos += 1
                else:
                    n += 2
                start = len(out) - disp
                for i in range(n):
                    out.append(out[start + i])
    return bytes(out)


def maybe_decompress(data):
    """Returns (data, how) with LZ77/Yaz0 layers removed."""
    if data[:4] == b"Yaz0":
        return yaz0_decompress(data), "yaz0"
    if data[:4] == b"LZ77":
        return lz77_decompress(data), "lz77"
    return data, None


U8_MAGIC = b"\x55\xAA\x38\x2D"


def u8_list(data):
    """Yields (path, offset, size) for each file and (path/, None, None) for each directory."""
    assert data[:4] == U8_MAGIC, "not a U8 archive"
    root_off, header_size, data_off = struct.unpack(">III", data[4:16])
    n_nodes = struct.unpack(">I", data[root_off + 8:root_off + 12])[0]
    str_tab = root_off + n_nodes * 12

    def name_at(off):
        end = data.index(b"\0", str_tab + off)
        return data[str_tab + off:end].decode("shift_jis", "replace")

    # Directory stack: (end node index, path)
    stack = [(n_nodes, "")]
    for i in range(1, n_nodes):
        while i >= stack[-1][0]:
            stack.pop()
        node = data[root_off + i * 12:root_off + i * 12 + 12]
        is_dir = node[0]
        name_off = node[1] << 16 | node[2] << 8 | node[3]
        a, b = struct.unpack(">II", node[4:12])
        path = stack[-1][1] + name_at(name_off)
        if is_dir:
            yield path + "/", None, None
            stack.append((b, path + "/"))
        else:
            yield path, a, b


def u8_extract(data, out_dir):
    files = []
    for path, off, size in u8_list(data):
        dest = os.path.join(out_dir, path)
        if off is None:
            os.makedirs(dest, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data[off:off + size])
        files.append(path)
    return files


def imet_names(data):
    """The channel names (Japanese, English, ...) from an IMET header at offset 0x40 or 0x80."""
    i = data.find(b"IMET")
    if i < 0:
        return None
    names = []
    for k in range(10):
        raw = data[i + 0x1C + k * 0x54:i + 0x1C + (k + 1) * 0x54]
        names.append(raw.decode("utf-16-be").split("\0")[0])
    return names
