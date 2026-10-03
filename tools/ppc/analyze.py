"""Control-flow analysis of a DOL for static recompilation.

Finds function boundaries and switch jump tables:
  * strong starts: the entry point, bl targets, exception-index entries;
  * medium starts: code pointers in the data sections (vtables, callbacks, ctors) and lis/addi
    pairs in code that build a code address, when the target looks like a function start;
  * weak starts: code after an unconditional end that nothing in the function branches to.
Jump tables are found from the bctr pattern (lis/addi table, lwzx, mtctr, bctr) and kept out
of the code-pointer scan.
"""
import bisect
import struct
from collections import defaultdict

from .decode import decode, sext16

SPR_LR, SPR_CTR = 8, 9


def is_uncond_end(ins):
    """True when control never falls through to the next instruction."""
    o = ins.op
    if o == "b":
        return not ins.lk
    if o in ("bclr", "bcctr"):
        return (ins.bo & 0x14) == 0x14 and not ins.lk
    if o == "rfi":
        return True
    return False


class Function:
    __slots__ = ("start", "end", "labels", "falls_through", "tail_calls", "calls", "reasons",
                 "jumptables", "indirect_jumps", "xjumps")

    def __init__(self, start, end):
        self.start = start
        self.end = end
        self.labels = set()
        self.falls_through = False
        self.tail_calls = set()
        self.calls = set()
        self.reasons = set()
        self.jumptables = {}      # bctr address -> list of target addresses
        self.indirect_jumps = []  # bctr without a known table (indirect tail calls)
        self.xjumps = []          # branches into the middle of another function


class Analysis:
    def __init__(self, dol, log=print):
        self.dol = dol
        self.log = log
        self.insns = {}
        for s in dol.text:
            for o in range(0, s.size, 4):
                a = s.addr + o
                self.insns[a] = decode(struct.unpack(">I", s.data[o:o + 4])[0], a)
        self.text_ranges = [(s.addr, s.end) for s in dol.text]
        self.starts = defaultdict(set)
        self.jumptables = {}      # bctr addr -> (table addr, targets)
        self.jt_words = set()     # data addresses inside jump tables
        self.jt_targets = set()
        self.functions = {}
        self.extab_funcs = []

    # -------------------------------------------------------------------------------------
    def in_text(self, a):
        for lo, hi in self.text_ranges:
            if lo <= a < hi:
                return True
        return False

    def run(self, extra_starts=(), not_starts=()):
        self.find_jumptables()
        self.add_start(self.dol.entry, "entry")
        for s in self.dol.text:
            self.add_start(s.addr, "section")
        for a, ins in self.insns.items():
            if ins.op in ("b", "bc") and ins.lk and self.in_text(ins.target):
                self.add_start(ins.target, "bl")
        self.scan_extabindex()
        self.scan_data_pointers()
        self.scan_code_pointers()
        for a in extra_starts:
            self.add_start(a, "config")
        for a in not_starts:
            self.starts.pop(a, None)
        self.build_functions()
        for _ in range(20):
            if not self.refine():
                break
            self.build_functions()
        self.finalize()
        return self

    def add_start(self, a, why):
        if a & 3 or not self.in_text(a):
            return False
        self.starts[a].add(why)
        return True

    # -------------------------------------------------------------------------------------
    def find_jumptables(self):
        insns = self.insns
        for a, ins in insns.items():
            if ins.op != "bcctr" or ins.lk:
                continue
            # backward: mtctr rX <- lwzx rX, rT, rI <- addi rT, rH, lo <- lis rH, hi
            want_ctr = None
            ld = None
            table = None
            idx_reg = None
            bound = None
            b = a - 4
            steps = 0
            regs_tbl = None
            while steps < 40 and b in insns:
                i = insns[b]
                o = i.op
                if want_ctr is None:
                    if o == "mtspr" and i.spr == SPR_CTR:
                        want_ctr = i.rd
                elif ld is None:
                    if o == "lwzx" and i.rd == want_ctr:
                        ld = i
                        regs_tbl = {i.ra, i.rb}
                elif table is None or bound is None:
                    if table is None and o == "addi" and i.rd in regs_tbl:
                        # find the lis for i.ra
                        c = b - 4
                        for _ in range(30):
                            if c not in insns:
                                break
                            j = insns[c]
                            if j.op == "addis" and j.ra == 0 and j.rd == i.ra:
                                table = ((j.simm << 16) + i.simm) & 0xFFFFFFFF
                                break
                            c -= 4
                        if table is not None:
                            other = (regs_tbl - {i.rd})
                            if other:
                                idx_reg = other.pop()
                    elif o == "rlwinm" and idx_reg is not None and i.ra == idx_reg and i.sh == 2:
                        idx_reg = i.rd  # slwi rI, rN, 2
                    elif o == "cmpli" and idx_reg is not None and i.ra == idx_reg:
                        bound = i.uimm
                        break
                if is_uncond_end(i) or (o in ("b", "bc") and i.lk):
                    break
                b -= 4
                steps += 1
            if table is None:
                continue
            targets = []
            n = (bound + 1) if bound is not None else 512
            for k in range(n):
                v = self.dol.read32(table + 4 * k)
                if v is None or not self.in_text(v):
                    break
                if bound is None and targets and abs(v - a) > 0x10000:
                    break
                targets.append(v)
            if not targets:
                continue
            self.jumptables[a] = (table, targets)
            for k in range(len(targets)):
                self.jt_words.add(table + 4 * k)
            self.jt_targets.update(targets)

    def scan_extabindex(self):
        # CodeWarrior extabindex: {function address, function size, extab entry}
        for s in self.dol.data:
            if s.addr >= 0x80004000 and s.addr < 0x8000E000 and s.size % 12 == 0 and s.size >= 12:
                ok = True
                entries = []
                for o in range(0, s.size, 12):
                    fa, fs, ea = struct.unpack(">III", s.data[o:o + 12])
                    if not self.in_text(fa) or fs == 0 or fs > 0x100000:
                        ok = False
                        break
                    entries.append((fa, fs, ea))
                if ok:
                    for fa, fs, ea in entries:
                        self.add_start(fa, "extab")
                    self.extab_funcs = entries

    def plausible_start(self, t):
        if t & 3 or not self.in_text(t) or t in self.jt_targets:
            return False
        ins = self.insns.get(t)
        if ins is None or ins.op == "illegal":
            return False
        if t in self.starts:
            return True
        prev = self.insns.get(t - 4)
        if prev is None:
            return True
        return is_uncond_end(prev) or prev.word == 0

    def scan_data_pointers(self):
        for s in self.dol.data:
            for o in range(0, s.size - 3, 4):
                a = s.addr + o
                if a in self.jt_words:
                    continue
                v = struct.unpack(">I", s.data[o:o + 4])[0]
                if self.in_text(v) and self.plausible_start(v):
                    self.starts[v].add("data")

    def scan_code_pointers(self):
        hi = {}
        for a in sorted(self.insns):
            ins = self.insns[a]
            o = ins.op
            if o == "addis" and ins.ra == 0:
                hi[ins.rd] = ins.simm << 16
                continue
            if o in ("addi", "ori") and (ins.ra if o == "addi" else ins.rd) in hi:
                src = ins.ra if o == "addi" else ins.rd
                v = (hi[src] + (ins.simm if o == "addi" else ins.uimm)) & 0xFFFFFFFF
                if self.in_text(v) and self.plausible_start(v):
                    self.starts[v].add("code")
                dst = ins.rd if o == "addi" else ins.ra
                hi.pop(dst, None)
                continue
            # any other write invalidates the register (approximation: rd/ra fields)
            if o not in ("stw", "stwu", "stb", "sth", "stfs", "stfd", "cmpi", "cmpli", "cmp", "cmpl",
                         "b", "bc", "bclr", "bcctr"):
                hi.pop(ins.rd, None)
                if o in ("ori", "oris", "xori", "xoris", "andi.", "andis.", "rlwinm", "rlwimi",
                         "rlwnm", "and", "or", "xor", "nor", "andc", "orc", "eqv", "nand", "slw",
                         "srw", "sraw", "srawi", "cntlzw", "extsb", "extsh"):
                    hi.pop(ins.ra, None)
            if is_uncond_end(ins):
                hi.clear()

    # -------------------------------------------------------------------------------------
    def build_functions(self):
        starts = sorted(self.starts)
        self.sorted_starts = starts
        self.functions = {}
        for k, s in enumerate(starts):
            sec_end = None
            for lo, hi in self.text_ranges:
                if lo <= s < hi:
                    sec_end = hi
            e = starts[k + 1] if k + 1 < len(starts) and starts[k + 1] < sec_end else sec_end
            f = Function(s, e)
            f.reasons = set(self.starts[s])
            self.functions[s] = f

    def function_containing(self, a):
        k = bisect.bisect_right(self.sorted_starts, a) - 1
        if k < 0:
            return None
        f = self.functions[self.sorted_starts[k]]
        return f if f.start <= a < f.end else None

    def explore(self, f):
        """Walk the reachable code of f; returns (reached set, cross jumps, falls_through)."""
        reached = set()
        work = [f.start]
        xj = []
        falls = False
        while work:
            a = work.pop()
            while True:
                if a in reached:
                    break
                if not (f.start <= a < f.end):
                    if a == f.end:
                        falls = True
                    break
                ins = self.insns.get(a)
                if ins is None:
                    break
                reached.add(a)
                o = ins.op
                if o == "b":
                    if ins.lk:
                        a += 4
                        continue
                    t = ins.target
                    if f.start <= t < f.end:
                        work.append(t)
                    elif t not in self.starts:
                        xj.append((a, t))
                    break
                if o == "bc":
                    if not ins.lk:
                        t = ins.target
                        if f.start <= t < f.end:
                            work.append(t)
                        elif t not in self.starts:
                            xj.append((a, t))
                    if (ins.bo & 0x14) == 0x14 and not ins.lk:
                        break
                    a += 4
                    continue
                if o == "bcctr" and not ins.lk:
                    jt = self.jumptables.get(a)
                    if jt:
                        for t in jt[1]:
                            if f.start <= t < f.end:
                                work.append(t)
                            elif t not in self.starts:
                                xj.append((a, t))
                    if (ins.bo & 0x14) == 0x14:
                        break
                    a += 4
                    continue
                if is_uncond_end(ins):
                    break
                a += 4
        return reached, xj, falls

    def refine(self):
        """Add weak starts for unreachable code and split/merge on cross-function jumps."""
        changed = False
        for s in list(self.sorted_starts):
            f = self.functions[s]
            reached, xj, falls = self.explore(f)
            # cross jumps: merge a weak following function, otherwise split the target
            for (src, t) in xj:
                g = self.function_containing(t)
                if g is None:
                    continue
                if g.start > f.start and self.weak_chain(f, g):
                    # remove weak starts between f and t (inclusive of g.start)
                    for st in [x for x in self.sorted_starts if f.start < x <= t]:
                        if self.is_weak(st):
                            del self.starts[st]
                            changed = True
                elif t not in self.starts:
                    self.starts[t].add("split")
                    changed = True
            if changed:
                return True
            # unreachable tail after an unconditional end -> new function
            a = f.start
            last = max(reached) if reached else f.start
            b = last + 4
            while b < f.end and self.insns[b].word == 0:
                b += 4
            if b < f.end and b not in reached:
                ins = self.insns[b]
                if ins.op != "illegal":
                    self.starts[b].add("sweep")
                    changed = True
        return changed

    def is_weak(self, a):
        r = self.starts.get(a, set())
        return not (r & {"entry", "bl", "extab", "section", "config"})

    def weak_chain(self, f, g):
        for st in self.sorted_starts:
            if f.start < st <= g.start and not self.is_weak(st):
                return False
        return True

    def finalize(self):
        for s in self.sorted_starts:
            f = self.functions[s]
            reached, xj, falls = self.explore(f)
            f.falls_through = falls
            f.xjumps = xj
            for a in sorted(reached):
                ins = self.insns[a]
                o = ins.op
                if o in ("b", "bc"):
                    t = ins.target
                    if ins.lk:
                        f.calls.add(t)
                    elif f.start <= t < f.end:
                        f.labels.add(t)
                    else:
                        f.tail_calls.add(t)
                elif o == "bcctr" and not ins.lk:
                    jt = self.jumptables.get(a)
                    if jt:
                        f.jumptables[a] = jt[1]
                        for t in jt[1]:
                            if f.start <= t < f.end:
                                f.labels.add(t)
                            else:
                                f.tail_calls.add(t)
                    else:
                        f.indirect_jumps.append(a)

    # -------------------------------------------------------------------------------------
    def stats(self):
        n = len(self.functions)
        reasons = defaultdict(int)
        for f in self.functions.values():
            for r in f.reasons:
                reasons[r] += 1
        ft = sum(1 for f in self.functions.values() if f.falls_through)
        xj = sum(len(f.xjumps) for f in self.functions.values())
        ij = sum(len(f.indirect_jumps) for f in self.functions.values())
        sizes = sorted((f.end - f.start) for f in self.functions.values())
        return {
            "functions": n,
            "reasons": dict(reasons),
            "falls_through": ft,
            "cross_jumps": xj,
            "indirect_jumps": ij,
            "jumptables": len(self.jumptables),
            "largest": sizes[-5:],
            "extab_funcs": len(self.extab_funcs),
        }
