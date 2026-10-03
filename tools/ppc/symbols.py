"""Read decomp-toolkit's symbols.txt and answer "what is at this address" questions."""
import bisect
import re

_LINE = re.compile(r"^(\S+) = (\S+):0x([0-9A-Fa-f]+); // (.*)$")


class Symbol:
    __slots__ = ("name", "section", "addr", "kind", "size", "attrs")

    def __init__(self, name, section, addr, kind, size, attrs):
        self.name = name
        self.section = section
        self.addr = addr
        self.kind = kind
        self.size = size
        self.attrs = attrs

    @property
    def end(self):
        return self.addr + self.size


class SymbolTable:
    def __init__(self, path):
        self.syms = []
        for line in open(path):
            m = _LINE.match(line.strip())
            if not m:
                continue
            name, section, addr, rest = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
            attrs = dict(kv.split(":", 1) for kv in rest.split() if ":" in kv)
            self.syms.append(Symbol(name, section, addr, attrs.get("type", "?"),
                                    int(attrs.get("size", "0"), 16), attrs))
        self.syms.sort(key=lambda s: s.addr)
        self.functions = [s for s in self.syms if s.kind == "function"]
        self.fn_addrs = [s.addr for s in self.functions]
        self.by_addr = {}
        for s in self.syms:
            self.by_addr.setdefault(s.addr, s)
        self.by_name = {s.name: s for s in self.syms}

    def function_at(self, addr):
        k = bisect.bisect_right(self.fn_addrs, addr) - 1
        if k < 0:
            return None
        f = self.functions[k]
        return f if f.addr <= addr < f.addr + max(f.size, 4) else None

    def name(self, addr):
        s = self.by_addr.get(addr)
        return s.name if s else "0x%08x" % addr
