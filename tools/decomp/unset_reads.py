#!/usr/bin/env python3
"""List reads of registers m2c found unset outside plain call arguments.

Calls pass every argument register the prototype names, set or not (M2C_UNSET there is
harmless: the callee does not read it). Anywhere else (arithmetic, an address, a store, a
condition) an unset read is a decompilation error. Usage: tools/decomp/unset_reads.py [src dir]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CALLEE = re.compile(r"(?:\bfn_[0-9A-F]{8}|\bgcall<[^<>]*(?:<[^<>]*>)?[^<>]*>|\b[A-Za-z_][A-Za-z0-9_:]*)\s*$")
# the compiler's register save/restore and pointer-to-member helpers use registers outside the
# ABI (r11, r12) and are replaced by native code
HELPERS = {"__save_fpr", "__restore_fpr", "__save_gpr", "__restore_gpr", "_savefpr", "_restfpr",
           "_savegpr", "_restgpr", "__ptmf_scall", "__ptmf_scall4"}
NOT_CALLS = {"M2C_FIELD", "GIDX", "M2C_BITWISE", "GS", "GV", "if", "while", "for", "switch", "return",
             "sizeof", "M2C_STRUCT_COPY", "NOMA_U64", "noma_reg_bits"}


def enclosing_call(line, pos):
    """name of the call whose argument list directly holds position pos, or None"""
    depth = 0
    i = pos - 1
    while i >= 0:
        c = line[i]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth == 0:
                m = CALLEE.search(line[:i])
                return m.group(0).strip() if m else None
            depth -= 1
        i -= 1
    return None


def direct_argument(line, pos):
    """is the expression starting at pos a whole argument (after '(' or ',', before ',' or ')');
    gcall's arguments are wrapped as (u32)(expr)"""
    before = line[:pos].rstrip()
    if before.endswith("(u32)("):
        return direct_argument(line, len(before) - len("(u32)("))
    if not before or before[-1] not in "(,":
        return False
    depth = 0
    i = pos + 5 if line.startswith("(u32)(", pos) else pos
    while i < len(line):
        c = line[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth < 0:
                return True
            if depth == 0:
                rest = line[i + 1:].lstrip()
                return rest[:1] in (",", ")")
        i += 1
    return False


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "decomp", "src")
    total = 0
    for name in sorted(os.listdir(src)):
        if not name.endswith(".cpp"):
            continue
        fn = None
        in_ref = False
        for line in open(os.path.join(src, name)):
            if line.startswith("#if 0"):
                in_ref = True
            elif line.startswith("#endif"):
                in_ref = False
            if in_ref:
                continue
            m = re.match(r"^[A-Za-z_][\w<>, ]*\s+(fn_[0-9A-F]{8}|[A-Za-z_]\w*)\(", line)
            if m and line.rstrip().endswith("{"):
                fn = m.group(1)
            for u in re.finditer(r"M2C_UNSET\(/\* Read from unset register \$(\w+) \*/\)", line):
                start = u.start()
                if line[:start].endswith("(u32)("):
                    start -= len("(u32)(")
                callee = enclosing_call(line, start)
                if callee and callee.split("<")[0] not in NOT_CALLS and direct_argument(line, start):
                    continue
                if fn in HELPERS:
                    continue
                total += 1
                print("%s %s: $%s: %s" % (name[:-4], fn, u.group(1), line.strip()[:200]))
    print("%d unset reads outside call arguments" % total)


if __name__ == "__main__":
    main()
