#!/usr/bin/env python3
"""Infer the register-ABI prototype of every function in the DOL (see ppc/signatures.py).

  tools/infer_signatures.py [--dol orig/HCIJ/sys/main.dol] [--symbols config/HCIJ/symbols.txt]
                            [--out build/sig/prototypes.txt]
"""
import argparse
import collections
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppc.dol import Dol  # noqa: E402
from ppc.rtti import scan_vtables  # noqa: E402
from ppc.signatures import FPR, GPR, RET_F1, RET_R3, RET_R4, SignatureInference  # noqa: E402
from ppc.symbols import SymbolTable  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dol", default="orig/HCIJ/sys/main.dol")
    ap.add_argument("--symbols", default="config/HCIJ/symbols.txt")
    ap.add_argument("--out", default="build/sig/prototypes.txt")
    args = ap.parse_args()
    t = time.time()
    dol = Dol(args.dol)
    syms = SymbolTable(args.symbols)
    vtables = scan_vtables(dol)
    print("vtables: %d" % len(vtables))
    si = SignatureInference(dol, syms, {a: fns for a, (n, fns) in vtables.items()})
    si.solve()
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    si.write(args.out)
    shapes = collections.Counter()
    for f in si.fns.values():
        ng = sum(1 for r in range(3, 11) if f.params & GPR(r))
        nf = sum(1 for r in range(1, 9) if f.params & FPR(r))
        shapes[(ng, nf)] += 1
    rets = collections.Counter(("r3" if f.ret & RET_R3 else "") + ("r4" if f.ret & RET_R4 else "") +
                               ("f1" if f.ret & RET_F1 else "") or "void" for f in si.fns.values())
    print("parameter shapes (gprs, fprs):", sorted(shapes.items(), key=lambda x: -x[1])[:12])
    print("returns:", dict(rets))
    print("%d functions in %.1fs -> %s" % (len(si.fns), time.time() - t, args.out))


if __name__ == "__main__":
    main()
