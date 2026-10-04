#!/usr/bin/env python3
"""Refuses PICA vertex shaders that leave a component of an output register unwritten.

A real 3DS GPU never finishes a draw whose vertex shader leaves any component of an output
register in its output map unwritten, even components the map drops (texcoord0's z and w): the
New 3DS hung at the first frame with gx.v.pica writing only otc0.xy, and the Jak 1 port's GPU hung
on a shader that never wrote texcoord1.zw. The emulator runs either without a word. The rule
checked here is stricter than "written somewhere": every component of every output must be written
on every path through main, so a write counts only outside ifu/ifc/for blocks, in main itself or
in a procedure main calls outside them (writing in parts, .xy then .zw, is fine).

    tools/check_shader_outputs.py [SHADER.v.pica | DIR ...]   (default: platform/3ds/shaders)

Exit status 1 if a shader breaks the rule (platform/3ds/package.sh refuses to package then)."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BLOCKS = ("ifu", "ifc", "for")  # (and .else, inside an ifu/ifc)


def check(path):
    lines = path.read_text().splitlines()
    outs = {}      # name -> output register index
    aliases = {}   # alias -> register name
    for line in lines:
        code = line.split(";")[0].strip()
        m = re.match(r"\.out\s+(\S+)\s+\w+(?:\.\w+)?(?:\s+(o\d+))?\s*$", code)
        if m:
            reg = int(m.group(2)[1:]) if m.group(2) else len(outs)
            if m.group(1) != "-":
                outs[m.group(1)] = reg
            continue
        m = re.match(r"\.alias\s+(\w+)\s+(\w+)", code)
        if m:
            aliases[m.group(1)] = m.group(2)
    by_reg = {reg: name for name, reg in outs.items()}

    def out_of(target):
        target = aliases.get(target, target)
        if target in outs:
            return target
        if re.fullmatch(r"o\d+", target):
            return by_reg.get(int(target[1:]))
        return None

    # per procedure: the components written outside blocks, and the procedures called outside them
    procs = {}
    proc, depth = None, 0
    for line in lines:
        code = line.split(";")[0].strip()
        if not code:
            continue
        word = code.split()[0].lower()
        if word == ".proc":
            proc, depth = code.split()[1], 0
            procs[proc] = {"writes": {}, "calls": []}
            continue
        if proc is None:
            continue
        if word in BLOCKS:
            depth += 1
            continue
        if word == ".else":
            continue
        if word == ".end":
            if depth == 0:
                proc = None
            else:
                depth -= 1
            continue
        if word.startswith("."):
            continue
        if depth:
            continue  # (conditional: doesn't count)
        if word == "call":
            procs[proc]["calls"].append(code.split()[1])
            continue
        m = re.match(r"[a-z0-9]+\s+([A-Za-z_]\w*)(\.[xyzw]+)?\s*,", code)
        if m:
            o = out_of(m.group(1))
            if o:
                procs[proc]["writes"].setdefault(o, set()).update(m.group(2)[1:] if m.group(2) else "xyzw")

    def written(name, seen):
        p = procs.get(name)
        if not p or name in seen:
            return {}
        seen = seen | {name}
        got = {o: set(c) for o, c in p["writes"].items()}
        for callee in p["calls"]:
            for o, c in written(callee, seen).items():
                got.setdefault(o, set()).update(c)
        return got

    if "main" not in procs:
        return [f"{path.name}: no .proc main"]
    got = written("main", frozenset())
    bad = []
    for o in outs:
        missing = set("xyzw") - got.get(o, set())
        if missing:
            bad.append(f"{path.name}: output {o} (o{outs[o]}): {''.join(c for c in 'xyzw' if c in missing)} not written "
                       f"on every path (a real GPU hangs on the draw)")
    return bad


def main():
    args = [pathlib.Path(a) for a in sys.argv[1:]] or [ROOT / "platform/3ds/shaders"]
    files = []
    for a in args:
        files += sorted(a.glob("*.v.pica")) if a.is_dir() else [a]
    bad = []
    for f in files:
        bad += check(f)
    for b in bad:
        print(b)
    if not bad:
        print(f"shader outputs: {len(files)} shader(s), every output component written on every path")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
