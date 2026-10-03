"""DOL executable loader (GameCube/Wii)."""
import struct


class Section:
    def __init__(self, kind, index, offset, addr, data):
        self.kind = kind          # "text" or "data"
        self.index = index
        self.offset = offset
        self.addr = addr
        self.data = data
        self.size = len(data)
        self.end = addr + len(data)

    def __repr__(self):
        return "%s%d %08x-%08x" % (self.kind, self.index, self.addr, self.end)


class Dol:
    def __init__(self, path):
        raw = open(path, "rb").read()
        self.raw = raw
        to = struct.unpack(">7I", raw[0:28])
        do = struct.unpack(">11I", raw[28:72])
        ta = struct.unpack(">7I", raw[72:100])
        da = struct.unpack(">11I", raw[100:144])
        ts = struct.unpack(">7I", raw[144:172])
        ds = struct.unpack(">11I", raw[172:216])
        self.bss_addr, self.bss_size, self.entry = struct.unpack(">3I", raw[216:228])
        self.sections = []
        for i in range(7):
            if ts[i]:
                self.sections.append(Section("text", i, to[i], ta[i], raw[to[i]:to[i] + ts[i]]))
        for i in range(11):
            if ds[i]:
                self.sections.append(Section("data", i, do[i], da[i], raw[do[i]:do[i] + ds[i]]))
        self.text = [s for s in self.sections if s.kind == "text"]
        self.data = [s for s in self.sections if s.kind == "data"]

    def section_at(self, addr):
        for s in self.sections:
            if s.addr <= addr < s.end:
                return s
        return None

    def is_text(self, addr):
        for s in self.text:
            if s.addr <= addr < s.end:
                return True
        return False

    def read32(self, addr):
        s = self.section_at(addr)
        if s is None or addr + 4 > s.end:
            return None
        o = addr - s.addr
        return struct.unpack(">I", s.data[o:o + 4])[0]

    def read_bytes(self, addr, n):
        s = self.section_at(addr)
        if s is None:
            return None
        o = addr - s.addr
        return s.data[o:o + n]

    def cstring(self, addr, limit=256):
        b = self.read_bytes(addr, limit)
        if b is None:
            return None
        end = b.find(b"\0")
        return b[:end if end >= 0 else limit]
