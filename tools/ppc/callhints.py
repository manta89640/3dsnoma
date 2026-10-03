"""What the caller does with the result of each call through a pointer: int (r3), float (f1)
or nothing.

m2c types such a call's result from how the code uses r3 and f1 afterwards. A function that
takes the whole register file (varargs, or one forwarding its arguments through a pointer)
"uses" whatever f1 holds as much as a real float result does, and m2c then reads a pointer
result as a double. Here a register counts as used when an instruction reads it, a callee
takes it as a parameter, or the function returns it; being passed along the whole register
file only counts when nothing else decides. A callee's r3 parameter is weak evidence too:
the inferred prototypes include r3 whenever a caller might pass it. An instruction reading the
register beats the function returning it: after a float-returning call r3 still holds whatever
the callee left there, and a function's inferred return type is r3 whenever any path leaves a
value in it (TurnEnd's update compared a float speed through r3 and took the wrong branch).
"""
from .signatures import FPR, GPR, use_def

R3, F1 = GPR(3), FPR(1)
BOTH = R3 | F1


class Proto:
    __slots__ = ("ret", "params", "full")

    def __init__(self, ret, params, full):
        self.ret = ret          # "void", "u32", "u64", "f64"
        self.params = params    # register mask
        self.full = full        # takes the whole register file


def parse_prototype(text):
    """'u32 name(u32 r3, f64 f1)' -> Proto"""
    ret = text.split(None, 1)[0]
    inside = text[text.index("(") + 1:text.rindex(")")]
    params = 0
    full = "..." in inside
    for p in inside.split(","):
        p = p.strip().split()
        if len(p) == 2:
            name = p[1].replace("_kept", "")
            if name[0] == "r":
                params |= GPR(int(name[1:]))
            elif name[0] == "f":
                params |= FPR(int(name[1:]))
    every = sum(GPR(r) for r in range(3, 11)) | sum(FPR(f) for f in range(1, 9))
    if full:
        params = every
    # a prototype listing every argument register forwards them (it calls through a pointer
    # with what it was given, like __construct_new_array): passing them along, not taking them
    full = full or params == every
    return Proto(ret, params, full)


def _decide(read, ret, medium, weak):
    """read: by an instruction; ret: returned by the function; medium: a callee's parameter;
    weak: passed along the whole register file"""
    if read & R3:
        return "int"
    if read & F1:
        return "float"
    if ret & R3:
        return "int"
    if ret & F1:
        return "float"
    if medium & F1:
        return "float"
    if medium & R3:
        return "int"
    if weak & F1 and not weak & R3:
        return "float"
    if weak & R3:
        return "int"
    return None


def _ret_mask(proto):
    if proto is None:
        return 0
    if proto.ret in ("u32", "u64"):
        return R3
    if proto.ret == "f64":
        return F1
    return 0


def classify(si, f, ca, protos):
    """Return kind of the indirect call at address ca in function f."""
    insns = si.insns
    own = protos.get(f.start)
    blocks = f.blocks
    k0 = next(k for k, (s, e) in enumerate(blocks) if s <= ca < e)
    read = ret = medium = weak = 0
    work = [(k0, ca + 4, BOTH)]
    seen = set()
    while work:
        k, a, alive = work.pop()
        s, e = blocks[k]
        ended = False
        while a < e and alive:
            ins = insns[a]
            c = si.call_uses(a, ins)
            if c is not None and c[0] != "helper":
                kind, _uses, tgt = c
                p = protos.get(tgt) if kind == "call" else None
                if p is not None and not p.full and tgt not in si.passthrough:
                    medium |= alive & p.params
                else:
                    weak |= alive
                ended = True  # the call clobbers r3 and f1
                break
            if c is None:
                u, d = use_def(ins)
                read |= alive & u
                alive &= ~d
            a += 4
        if ended or not alive:
            continue
        last = insns[e - 4]
        lo = last.op
        if lo == "bclr" and not last.lk:  # (conditional) return
            ret |= alive & _ret_mask(own)
        if lo in ("b", "bc") and not last.lk and not (f.start <= last.target < f.end):
            p = protos.get(last.target)  # tail call
            if p is not None and not p.full and last.target not in si.passthrough:
                medium |= alive & p.params
            else:
                weak |= alive
        if lo == "bcctr" and not last.lk and not f.succ[k]:
            weak |= alive  # tail call through a pointer
        for j in f.succ[k]:
            if (j, alive) not in seen:
                seen.add((j, alive))
                work.append((j, blocks[j][0], alive))
    return _decide(read, ret, medium, weak)


def call_hints(si, protos):
    """{call address: "int" | "float"} for calls through pointers (bctrl, blrl,
    __ptmf_scall) and tail calls through pointers (whose result is the function's own)."""
    hints = {}
    for f in si.fns.values():
        if not f.blocks:
            continue
        for k, (s, e) in enumerate(f.blocks):
            for a in range(s, e, 4):
                ins = si.insns[a]
                o = ins.op
                indirect = (o in ("bcctr", "bclr") and ins.lk) or (
                    o == "b" and ins.lk and ins.target in si.passthrough)
                if indirect:
                    h = classify(si, f, a, protos)
                    if h:
                        hints[a] = h
                elif o == "bcctr" and not ins.lk and a == e - 4 and not f.succ[k]:
                    m = _ret_mask(protos.get(f.start))
                    if m == R3:
                        hints[a] = "int"
                    elif m == F1:
                        hints[a] = "float"
    return hints
