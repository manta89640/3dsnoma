#!/usr/bin/env python3
"""Which functions can still run: a reachability pass over dtk's assembly (build/HCIJ/asm).

    tools/decomp/reachable.py [--range LO HI] [--why NAME ...] [--dead FILE]

Nodes are functions (.fn) and data objects (.obj); an edge is any symbol a node names: calls and
tail branches, @ha/@l/@sda21 address loads, .4byte entries (vtables, callback tables). Execution
starts at __start, main and the static constructors (the .ctors table, read from main.dol: dtk's
assembly names only its first entry), and wherever the native C++ code
(engine/, runtime/, platform/) names a guest function or global (g::name, GFN(name), GA_name).
Functions whose body isn't compiled -- natives (build/decomp/natives.txt) and stubbed regions
(tools/decomp/regions.py) -- don't make their callees live. Exception tables don't either.

It prints how many functions are reached, per region; with --range, the functions in that address
range that are still reached and the first reference from outside that reaches each (with --why,
the whole chain for a name). Function pointers built from plain constants, rather than through a
symbol, aren't seen: whatever is left out on its word stays a logged stub.
"""
import argparse
import collections
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from decomp.regions import region_of  # noqa: E402
from ppc.dol import Dol  # noqa: E402

ENTRIES = ("__start", "main", "__init_user", "__init_cpp")
TOKEN = re.compile(r'"[^"]+"|[A-Za-z_.$][\w.$<>,:@-]*')
SUFFIXES = ("@ha", "@h", "@l", "@sda21", "@sda2")


def load_symbols():
    addr = {}
    for line in open(os.path.join(ROOT, "config/HCIJ/symbols.txt")):
        m = re.match(r'\s*("?[^ "=]+"?)\s*=\s*\.?[\w.]+:0x([0-9A-Fa-f]+);', line)
        if m:
            addr[m.group(1).strip('"')] = int(m.group(2), 16)
    return addr


def load_natives():
    out = set()
    for line in open(os.path.join(ROOT, "build/decomp/natives.txt")):
        p = line.split()
        if len(p) >= 2 and re.match(r"^[0-9a-fA-F]{8}$", p[0]):
            out.add(p[1])
    return out


def load_graph(addr):
    """Each node's kind ("fn"/"obj"), section, and the symbols it names."""
    refs, kind, section = collections.defaultdict(set), {}, {}
    for path in glob.glob(os.path.join(ROOT, "build/HCIJ/asm/*.s")):
        sec = cur = None
        for line in open(path, errors="replace"):
            s = line.strip()
            if s.startswith(".section"):
                sec = s.split()[1].strip(',"')
            elif s.split(" ")[0] in (".text", ".data", ".rodata", ".sdata", ".sdata2", ".sbss", ".bss",
                                      ".ctors", ".dtors", ".init"):
                sec = s.split()[0]
            if s.startswith((".fn ", ".obj ")):
                cur = s.split()[1].rstrip(",").strip('"')
                kind[cur] = "fn" if s.startswith(".fn") else "obj"
                section[cur] = sec
                continue
            if s.startswith((".endfn", ".endobj")):
                cur = None
                continue
            if cur is None:
                continue
            for t in TOKEN.findall(s.split("*/", 1)[-1]):
                t = t.strip('"')
                for suf in SUFFIXES:
                    if t.endswith(suf):
                        t = t[: -len(suf)]
                if t != cur and t in addr:
                    refs[cur].add(t)
    return refs, kind, section


def native_code_refs(addr):
    out = set()
    for d in ("engine", "runtime", "platform"):
        for path in glob.glob(os.path.join(ROOT, d, "**", "*.cpp"), recursive=True):
            for m in re.finditer(r"\bg::(\w+)|GFN\((\w+)\)|GA_(\w+)", open(path, errors="replace").read()):
                n = m.group(1) or m.group(2) or m.group(3)
                if n in addr:
                    out.add(n)
    return out


def static_constructors(addr):
    """The functions in main.dol's .ctors table (to its terminating 0)."""
    by_addr = {a: n for n, a in addr.items()}
    dol, a, out = Dol(os.path.join(ROOT, "orig/HCIJ/sys/main.dol")), addr["_ctors"], []
    while dol.read32(a):
        out.append(by_addr.get(dol.read32(a)))
        a += 4
    return [n for n in out if n]


def reach(addr, refs, kind, section, natives):
    roots = {n for n in kind if section.get(n) in (".ctors", ".dtors", ".init")}
    roots |= set(static_constructors(addr))
    roots |= {n for n in ENTRIES if n in addr} | native_code_refs(addr)
    live, why, stack = set(), {}, list(roots)
    while stack:
        n = stack.pop()
        if n in live:
            continue
        live.add(n)
        if n in natives or (kind.get(n) == "fn" and n in addr and region_of(addr[n])[1] == "stub"):
            continue  # its body isn't compiled
        for r in refs.get(n, ()):
            if r not in live and section.get(r) not in ("extab", "extabindex"):
                why.setdefault(r, n)
                stack.append(r)
    return live, why


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--range", nargs=2, metavar=("LO", "HI"))
    ap.add_argument("--why", nargs="*", default=[])
    ap.add_argument("--dead", metavar="FILE", help="write the range's unreached functions here")
    args = ap.parse_args()
    addr = load_symbols()
    refs, kind, section = load_graph(addr)
    live, why = reach(addr, refs, kind, section, load_natives())
    fns = [n for n in kind if kind[n] == "fn" and n in addr and not n.startswith("gap_")]
    dead = collections.Counter(region_of(addr[n])[0] for n in fns if n not in live)
    print("%d functions, %d not reached: %s" % (len(fns), sum(dead.values()),
                                                ", ".join("%s %d" % kv for kv in dead.most_common())))
    if args.range:
        lo, hi = (int(x, 16) for x in args.range)
        inr = sorted((addr[n], n) for n in fns if lo <= addr[n] < hi)
        alive = [(a, n) for a, n in inr if n in live]
        print("%08X-%08X: %d functions, %d reached:" % (lo, hi, len(inr), len(alive)))
        for a, n in alive:
            w = n
            while w in why and lo <= addr.get(why[w], 0) < hi:
                w = why[w]
            print("  %08X %-40s <- %s" % (a, n, why.get(w, "(entry)")))
        if args.dead:
            open(args.dead, "w").write("".join("%08X %s\n" % (a, n) for a, n in inr if n not in live))
    for n in args.why:
        chain = [n]
        while chain[-1] in why and len(chain) < 40:
            chain.append(why[chain[-1]])
        print(" <- ".join("%s (%08X)" % (x, addr.get(x, 0)) for x in chain))


if __name__ == "__main__":
    main()
