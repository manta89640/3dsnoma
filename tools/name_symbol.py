#!/usr/bin/env python3
"""Give symbols proper names.

    tools/name_symbol.py OLD=NEW [OLD=NEW ...] [--file NAMES] [--no-split]

Renames each symbol in config/HCIJ/symbols.txt and wherever hand-written code uses it: engine/, runtime/,
platform/, the name lists in tools/decomp/decompile.py, config/HCIJ/prototype_overrides.txt and the
README (also the forms the decompiled code derives from a name: GA_<name>, <name>_orig). Then dtk
splits the program again, so its assembly uses the new names. Prototypes and call hints are keyed
by address: tools/infer_*.py needn't run again, only tools/decomp/decompile.py (it decompiles again
the objects whose assembly changed).

Names: the SDK's, MSL's and NW4R's own names where the function is one of theirs; for the channel's
C++ classes, the class name from RTTI with "::" as "_" and the method, e.g. g3d_miisel_Mii_HitTest.
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMBOLS = os.path.join(ROOT, "config/HCIJ/symbols.txt")
DTK = os.environ.get("DTK", os.path.expanduser("~/devkitpro-3ds/decomp-tools/dtk"))
SOURCES = ["engine", "runtime", "platform"]
FILES = ["tools/decomp/decompile.py", "config/HCIJ/prototype_overrides.txt", "README.md"]
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def source_files():
    for d in SOURCES:
        for dirpath, _dirs, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                if f.endswith((".cpp", ".h", ".c", ".py", ".txt", ".md")):
                    yield os.path.join(dirpath, f)
    for f in FILES:
        p = os.path.join(ROOT, f)
        if os.path.exists(p):
            yield p


def main():
    ap = argparse.ArgumentParser(description="rename symbols (OLD=NEW) everywhere they are used")
    ap.add_argument("pairs", nargs="*", metavar="OLD=NEW")
    ap.add_argument("--file", help="more OLD=NEW pairs, one per line (# comments)")
    ap.add_argument("--no-split", action="store_true", help="don't run dtk dol split afterwards")
    args = ap.parse_args()
    if args.file:
        for line in open(args.file):
            line = line.split("#", 1)[0].strip()
            if line:
                args.pairs.append(line)
    if not args.pairs:
        ap.error("nothing to rename")

    lines = open(SYMBOLS).read().split("\n")
    names = {}
    for i, line in enumerate(lines):
        m = re.match(r"^(\S+) = ", line)
        if m:
            names.setdefault(m.group(1), i)
    renames = []
    for pair in args.pairs:
        old, sep, new = pair.partition("=")
        if not sep or not IDENT.match(new):
            sys.exit("bad rename %r (want OLD=NEW, NEW a C identifier)" % pair)
        if old not in names:
            sys.exit("no symbol %s in %s" % (old, SYMBOLS))
        if new in names or new in (n for _o, n in renames):
            sys.exit("%s is already a symbol" % new)
        if "__" in new.strip("_"):
            sys.exit("%s: no double underscores inside a name (reserved in C++)" % new)
        renames.append((old, new))

    for old, new in renames:
        i = names[old]
        lines[i] = new + lines[i][len(old):]
    open(SYMBOLS, "w").write("\n".join(lines))

    changed = []
    for path in source_files():
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        new_text = text
        for old, new in renames:
            new_text = re.sub(r"(?<![A-Za-z0-9_])(GA_)?%s(_orig)?(?![A-Za-z0-9_])" % re.escape(old),
                              lambda m: (m.group(1) or "") + new + (m.group(2) or ""), new_text)
        if new_text != text:
            open(path, "w", encoding="utf-8", errors="surrogateescape").write(new_text)
            changed.append(os.path.relpath(path, ROOT))
    for old, new in renames:
        print("%s -> %s" % (old, new))
    if changed:
        print("updated: " + ", ".join(changed))

    if not args.no_split:
        asm_dir = os.path.join(ROOT, "build/HCIJ/asm")
        before = time.time() - 1
        r = subprocess.run([DTK, "dol", "split", "config/HCIJ/config.yml", "build/HCIJ"], cwd=ROOT,
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("dtk dol split failed:\n" + r.stdout + r.stderr)
        # dtk names an object after its first function: a renamed one leaves its old file behind
        stale = [f for f in os.listdir(asm_dir)
                 if f.endswith(".s") and os.path.getmtime(os.path.join(asm_dir, f)) < before]
        for f in stale:
            os.remove(os.path.join(asm_dir, f))
        print("dtk: split again (%d stale objects removed); next: tools/decomp/decompile.py" % len(stale))


if __name__ == "__main__":
    main()
