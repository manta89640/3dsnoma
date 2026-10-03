#!/usr/bin/env python3
"""Classify the result of every call through a pointer as int or float (see ppc/callhints.py).
Run after infer_signatures.py; decompile.py passes the result to m2c.

  tools/infer_call_hints.py [--dol orig/HCIJ/sys/main.dol] [--symbols config/HCIJ/symbols.txt]
                            [--protos build/sig/prototypes.txt] [--out build/sig/call_hints.txt]
"""
import argparse
import collections
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decomp.decompile import load_prototypes  # noqa: E402
from ppc.callhints import call_hints, parse_prototype  # noqa: E402
from ppc.dol import Dol  # noqa: E402
from ppc.signatures import SignatureInference  # noqa: E402
from ppc.symbols import SymbolTable  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dol", default="orig/HCIJ/sys/main.dol")
    ap.add_argument("--symbols", default="config/HCIJ/symbols.txt")
    ap.add_argument("--protos", default="build/sig/prototypes.txt")
    ap.add_argument("--out", default="build/sig/call_hints.txt")
    args = ap.parse_args()
    t = time.time()
    dol = Dol(args.dol)
    st = SymbolTable(args.symbols)
    protos = {a: parse_prototype("%s %s(%s)" % (r, n, p or "void"))
              for a, (r, n, p) in load_prototypes(args.protos, st).items()}
    si = SignatureInference(dol, st, {}, log=lambda *a: None)
    si.build()
    hints = call_hints(si, protos)
    with open(args.out, "w") as out:
        for a in sorted(hints):
            out.write("%08X %s\n" % (a, hints[a]))
    print("call hints:", dict(collections.Counter(hints.values())),
          "in %.1fs -> %s" % (time.time() - t, args.out))


if __name__ == "__main__":
    main()
