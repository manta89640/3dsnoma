"""Infer a C prototype for every function of the DOL from register liveness.

Parameters: argument registers (r3-r10, f1-f8) that a function reads before writing them,
taking its callees into account (a call reads the callee's parameters and clobbers the
volatile registers; a tail call reads the target's parameters).
Return value: r3 / r3:r4 / f1, when some caller reads it right after the call.
Virtual calls (lwz rV, vptr(rObj); lwz r12, slot(rV); mtctr; bctrl) read the union of the
parameters of every function found in that vtable slot.
Both are computed together to a fixpoint over the whole call graph.

Output: one line per function, "addr name ret params" (see write()).
"""
import collections
import re
import struct

from .decode import decode

GPR = lambda n: 1 << n
FPR = lambda n: 1 << (32 + n)
ARG_GPRS = [3, 4, 5, 6, 7, 8, 9, 10]
ARG_FPRS = [1, 2, 3, 4, 5, 6, 7, 8]
ARG_MASK = sum(GPR(r) for r in ARG_GPRS) | sum(FPR(f) for f in ARG_FPRS)
VOLATILE = GPR(0) | sum(GPR(r) for r in range(3, 13)) | sum(FPR(f) for f in range(0, 14))
RET_R3, RET_R4, RET_F1 = GPR(3), GPR(4), FPR(1)
RET_ANY = RET_R3 | RET_R4 | RET_F1
RET_REGS = (RET_R3, RET_R4, RET_F1)

# CodeWarrior runtime helpers with fixed prototypes (return mask, parameter mask). Inference
# gets some wrong: __mod2u returns its dividend untouched when it is below the divisor, so r4 is
# not written on every path and the r3:r4 result would be cut to r3.
_G = lambda *rs: sum(GPR(r) for r in rs)
KNOWN_PROTOTYPES = {
    "__div2u": (RET_R3 | RET_R4, _G(3, 4, 5, 6)),
    "__div2i": (RET_R3 | RET_R4, _G(3, 4, 5, 6)),
    "__mod2u": (RET_R3 | RET_R4, _G(3, 4, 5, 6)),
    "__mod2i": (RET_R3 | RET_R4, _G(3, 4, 5, 6)),
    "__shl2i": (RET_R3 | RET_R4, _G(3, 4, 5)),
    "__shr2u": (RET_R3 | RET_R4, _G(3, 4, 5)),
    "__shr2i": (RET_R3 | RET_R4, _G(3, 4, 5)),
    "__cvt_sll_dbl": (RET_F1, _G(3, 4)),
    "__cvt_ull_dbl": (RET_F1, _G(3, 4)),
    "__cvt_sll_flt": (RET_F1, _G(3, 4)),
    "__cvt_ull_flt": (RET_F1, _G(3, 4)),
    "__cvt_dbl_sll": (RET_R3 | RET_R4, FPR(1)),
    "__cvt_dbl_ull": (RET_R3 | RET_R4, FPR(1)),
    "__cvt_fp2unsigned": (RET_R3, FPR(1)),
}

_D_LOAD = {"lwz", "lbz", "lhz", "lha", "lmw"}
_D_LOAD_U = {"lwzu", "lbzu", "lhzu", "lhau"}
_D_STORE = {"stw", "stb", "sth", "stmw"}
_D_STORE_U = {"stwu", "stbu", "sthu"}
_X_LOAD = {"lwzx", "lbzx", "lhzx", "lhax", "lwbrx", "lhbrx", "lwarx"}
_X_LOAD_U = {"lwzux", "lbzux", "lhzux", "lhaux"}
_X_STORE = {"stwx", "stbx", "sthx", "stwbrx", "sthbrx", "stwcx."}
_X_STORE_U = {"stwux", "stbux", "sthux"}
_F_LOAD = {"lfs", "lfd"}
_F_LOAD_U = {"lfsu", "lfdu"}
_F_LOADX = {"lfsx", "lfdx"}
_F_LOADX_U = {"lfsux", "lfdux"}
_F_STORE = {"stfs", "stfd"}
_F_STORE_U = {"stfsu", "stfdu"}
_F_STOREX = {"stfsx", "stfdx", "stfiwx"}
_F_STOREX_U = {"stfsux", "stfdux"}
_LOGIC_X = {"and", "andc", "or", "orc", "xor", "nor", "nand", "eqv", "slw", "srw", "sraw"}
_UNARY_X = {"cntlzw", "extsb", "extsh", "srawi"}
_XO_2 = {"add", "addc", "adde", "subf", "subfc", "subfe", "mullw", "mulhw", "mulhwu", "divw", "divwu"}
_XO_1 = {"neg", "addze", "addme", "subfze", "subfme"}
_F_AB = {"fadd", "fsub", "fdiv", "fadds", "fsubs", "fdivs", "ps_add", "ps_sub", "ps_div",
         "ps_merge00", "ps_merge01", "ps_merge10", "ps_merge11"}
_F_AC = {"fmul", "fmuls", "ps_mul", "ps_muls0", "ps_muls1"}
_F_ABC = {"fmadd", "fmsub", "fnmadd", "fnmsub", "fmadds", "fmsubs", "fnmadds", "fnmsubs", "fsel",
          "ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub", "ps_madds0", "ps_madds1", "ps_sum0",
          "ps_sum1", "ps_sel"}
_F_B = {"fmr", "fneg", "fabs", "fnabs", "frsp", "fctiw", "fctiwz", "fres", "frsqrte", "ps_mr",
        "ps_neg", "ps_abs", "ps_nabs", "ps_res", "ps_rsqrte"}
_F_CMP = {"fcmpu", "fcmpo", "ps_cmpu0", "ps_cmpo0", "ps_cmpu1", "ps_cmpo1"}


def ra0(r):
    return 0 if r == 0 else GPR(r)


def fill_holes(m):
    """Arguments are positional: a call that passes r5 passes r4 too (whatever it holds), and
    f3 means f1 and f2. Caller-side evidence gets the registers below the highest it saw."""
    for regs, bit in ((ARG_GPRS, GPR), (ARG_FPRS, FPR)):
        top = [r for r in regs if m & bit(r)]
        if top:
            for r in regs:
                if r <= top[-1]:
                    m |= bit(r)
    return m


def use_def(ins):
    """(uses, defs) bitmasks of GPRs/FPRs for a non-call instruction."""
    o = ins.op
    rd, ra, rb = ins.rd, ins.ra, ins.rb
    if o in ("addi", "addis"):
        return ra0(ra), GPR(rd)
    if o in ("addic", "addic.", "subfic", "mulli"):
        return GPR(ra), GPR(rd)
    if o in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
        return GPR(rd), GPR(ra)
    if o == "rlwinm":
        return GPR(rd), GPR(ra)
    if o == "rlwimi":
        return GPR(rd) | GPR(ra), GPR(ra)
    if o == "rlwnm":
        return GPR(rd) | GPR(rb), GPR(ra)
    if o in ("cmpi", "cmpli"):
        return GPR(ra), 0
    if o in ("cmp", "cmpl"):
        return GPR(ra) | GPR(rb), 0
    if o in _LOGIC_X:
        return GPR(rd) | GPR(rb), GPR(ra)
    if o in _UNARY_X:
        return GPR(rd), GPR(ra)
    if o in _XO_2:
        return GPR(ra) | GPR(rb), GPR(rd)
    if o in _XO_1:
        return GPR(ra), GPR(rd)
    if o in _D_LOAD:
        if o == "lmw":
            return ra0(ra), sum(GPR(r) for r in range(rd, 32))
        return ra0(ra), GPR(rd)
    if o in _D_LOAD_U:
        return GPR(ra), GPR(rd) | GPR(ra)
    if o in _D_STORE:
        if o == "stmw":
            return ra0(ra) | sum(GPR(r) for r in range(rd, 32)), 0
        return GPR(rd) | ra0(ra), 0
    if o in _D_STORE_U:
        return GPR(rd) | GPR(ra), GPR(ra)
    if o in _X_LOAD:
        return ra0(ra) | GPR(rb), GPR(rd)
    if o in _X_LOAD_U:
        return GPR(ra) | GPR(rb), GPR(rd) | GPR(ra)
    if o in _X_STORE:
        return GPR(rd) | ra0(ra) | GPR(rb), 0
    if o in _X_STORE_U:
        return GPR(rd) | GPR(ra) | GPR(rb), GPR(ra)
    if o in _F_LOAD:
        return ra0(ra), FPR(rd)
    if o in _F_LOAD_U:
        return GPR(ra), FPR(rd) | GPR(ra)
    if o in _F_LOADX:
        return ra0(ra) | GPR(rb), FPR(rd)
    if o in _F_LOADX_U:
        return GPR(ra) | GPR(rb), FPR(rd) | GPR(ra)
    if o in _F_STORE:
        return FPR(rd) | ra0(ra), 0
    if o in _F_STORE_U:
        return FPR(rd) | GPR(ra), GPR(ra)
    if o in _F_STOREX:
        return FPR(rd) | ra0(ra) | GPR(rb), 0
    if o in _F_STOREX_U:
        return FPR(rd) | GPR(ra) | GPR(rb), GPR(ra)
    if o in ("psq_l",):
        return ra0(ra), FPR(rd)
    if o in ("psq_lu",):
        return GPR(ra), FPR(rd) | GPR(ra)
    if o in ("psq_lx",):
        return ra0(ra) | GPR(rb), FPR(rd)
    if o in ("psq_lux",):
        return GPR(ra) | GPR(rb), FPR(rd) | GPR(ra)
    if o in ("psq_st",):
        return FPR(rd) | ra0(ra), 0
    if o in ("psq_stu",):
        return FPR(rd) | GPR(ra), GPR(ra)
    if o in ("psq_stx",):
        return FPR(rd) | ra0(ra) | GPR(rb), 0
    if o in ("psq_stux",):
        return FPR(rd) | GPR(ra) | GPR(rb), GPR(ra)
    if o in _F_AB:
        return FPR(ra) | FPR(rb), FPR(rd)
    if o in _F_AC:
        return FPR(ra) | FPR(ins.rc_), FPR(rd)
    if o in _F_ABC:
        return FPR(ra) | FPR(rb) | FPR(ins.rc_), FPR(rd)
    if o in _F_B:
        return FPR(rb), FPR(rd)
    if o in _F_CMP:
        return FPR(ra) | FPR(rb), 0
    if o == "mffs":
        return 0, FPR(rd)
    if o == "mtfsf":
        return FPR(rb), 0
    if o in ("mfspr", "mfcr", "mfmsr", "mftb", "mfsr", "mfsrin"):
        return 0, GPR(rd)
    if o in ("mtspr", "mtcrf", "mtmsr", "mtsr", "mtsrin"):
        return GPR(rd), 0
    if o in ("dcbz", "dcbf", "dcbst", "dcbi", "icbi", "dcbt", "dcbtst", "dcbz_l"):
        return ra0(ra) | GPR(rb), 0
    if o in ("tw",):
        return GPR(ra) | GPR(rb), 0
    if o in ("twi",):
        return GPR(ra), 0
    if o in ("lswi",):
        return ra0(ra), sum(GPR((rd + k) % 32) for k in range((ins.rb or 32) // 4 + 1))
    if o in ("stswi",):
        return ra0(ra) | sum(GPR((rd + k) % 32) for k in range((ins.rb or 32) // 4 + 1)), 0
    return 0, 0


class FnInfo:
    __slots__ = ("start", "end", "name", "blocks", "succ", "params", "ret", "varargs",
                 "forwarder", "own", "calls", "insn_list", "single", "double", "kept")

    def __init__(self, start, end, name):
        self.start = start
        self.end = end
        self.name = name
        self.params = 0
        self.ret = 0
        self.varargs = False
        # varargs because it forwards the register file to an unknown target (a tail call
        # through a pointer, or a thunk to such a function): the registers it reads itself
        # are still certain parameters (own), unlike a C varargs function's
        self.forwarder = False
        self.own = 0
        self.kept = 0          # r3/f1 arguments left unchanged for callers that read them after
        self.calls = []        # (insn addr, kind, target or slot)
        self.single = 0        # FPR params used in single-precision ops
        self.double = 0


class SignatureInference:
    def __init__(self, dol, symtab, vtables, log=print):
        self.dol = dol
        self.st = symtab
        self.log = log
        self.insns = {}
        for s in dol.text:
            for o in range(0, s.size, 4):
                a = s.addr + o
                self.insns[a] = decode(struct.unpack(">I", s.data[o:o + 4])[0], a)
        self.fns = {}
        for f in symtab.functions:
            if f.section not in (".text", ".init") or f.size == 0:
                continue
            self.fns[f.addr] = FnInfo(f.addr, f.addr + f.size, f.name)
        # jump tables: dtk names them jumptable_<addr> with a size
        self.jumptables = {}
        for s in symtab.syms:
            if s.name.startswith("jumptable_") and s.size:
                self.jumptables[s.addr] = [self.dol.read32(s.addr + 4 * k) for k in range(s.size // 4)]
        # vtable slot -> functions
        self.slot_fns = collections.defaultdict(set)
        for va, fns in vtables.items():
            for k, fa in enumerate(fns):
                if fa in self.fns:
                    self.slot_fns[8 + 4 * k].add(fa)
        self.vcall_slot = {}   # call insn addr -> slot offset
        self.use_unions = False
        self.slot_union = {}
        # functions that forward every argument register to an unknown target (CodeWarrior's
        # pointer-to-member call helper): their callers pass what they set, like an indirect call
        self.passthrough = {s.addr for s in symtab.syms if s.name in ("__ptmf_scall", "__ptmf_scall4")}
        # _savegpr_N/_restgpr_N/_savefpr_N/_restfpr_N: entry points inside the CodeWarrior
        # register save/restore helpers; they only touch r11 and callee-saved registers.
        self.helpers = set()
        for s in symtab.syms:
            if re.match(r"_(save|rest)(gpr|fpr)_\d+$", s.name) or s.name in (
                    "__save_gpr", "__restore_gpr", "__save_fpr", "__restore_fpr"):
                self.helpers.add(s.addr)
        self.ret_users = collections.defaultdict(int)
        self.tail_edges = set()  # (function, target of its tail call)
        self.prod_may, self.prod_must = {}, {}
        self.reads = {r: collections.Counter() for r in RET_REGS}   # call sites reading each
        self.sreads = {r: collections.Counter() for r in RET_REGS}  # ... with an instruction
        self.ret_edges = set()  # (function, callee, registers it only returns from that callee)
        # functions whose address is taken (stored in data or built with lis/addi): they are
        # called through pointers, where no call site tells us whether the result is read
        self.addr_taken = set()
        for sec in self.dol.data:
            for o in range(0, sec.size - 3, 4):
                w = struct.unpack(">I", sec.data[o:o + 4])[0]
                if w in self.fns:
                    self.addr_taken.add(w)

    # -------------------------------------------------------------------------------------
    def build(self):
        for f in self.fns.values():
            self.build_fn(f)

    def build_fn(self, f):
        insns = self.insns
        leaders = {f.start}
        jts = []
        hi = {}
        last_table = None
        for a in range(f.start, f.end, 4):
            ins = insns[a]
            o = ins.op
            if o == "addis" and ins.ra == 0:
                hi[ins.rd] = (ins.simm << 16) & 0xFFFFFFFF
            elif (o == "addi" and ins.ra in hi) or (o == "ori" and ins.rd in hi):  # lis + addi/ori
                v = ((hi[ins.ra] + ins.simm) if o == "addi" else (hi[ins.rd] | ins.uimm)) & 0xFFFFFFFF
                if v in self.jumptables:
                    jts.append(v)
                elif v in self.fns:
                    self.addr_taken.add(v)
                else:
                    last_table = v
            if o == "bcctr" and not ins.lk and last_table is not None and last_table not in self.jumptables:
                # a switch table with a compiler name (not dtk's jumptable_): entries are
                # addresses inside this function
                ents = []
                while len(ents) < 512:
                    w = self.dol.read32(last_table + 4 * len(ents))
                    if w is None or not (f.start <= w < f.end):
                        break
                    ents.append(w)
                if ents:
                    self.jumptables[last_table] = ents
                    jts.append(last_table)
            if o in ("b", "bc") and not ins.lk:
                if f.start <= ins.target < f.end:
                    leaders.add(ins.target)
                leaders.add(a + 4)
            elif o in ("bclr", "bcctr") and not ins.lk:
                leaders.add(a + 4)
        jt_targets = set()
        for v in jts:
            for t in self.jumptables[v]:
                if t is not None and f.start <= t < f.end:
                    jt_targets.add(t)
        leaders |= jt_targets
        starts = sorted(x for x in leaders if f.start <= x < f.end)
        blocks = []
        for k, s in enumerate(starts):
            e = starts[k + 1] if k + 1 < len(starts) else f.end
            blocks.append((s, e))
        index = {s: k for k, (s, e) in enumerate(blocks)}
        succ = []
        for (s, e) in blocks:
            last = insns[e - 4]
            o = last.op
            out = []
            fall = True
            if o == "b" and not last.lk:
                fall = False
                if last.target in index:
                    out.append(index[last.target])
            elif o == "bc" and not last.lk:
                if last.target in index:
                    out.append(index[last.target])
                if (last.bo & 0x14) == 0x14:
                    fall = False
            elif o == "bclr" and not last.lk:
                if (last.bo & 0x14) == 0x14:
                    fall = False
            elif o == "bcctr" and not last.lk:
                if (last.bo & 0x14) == 0x14:
                    fall = False
                    for t in jt_targets:
                        out.append(index[t])
            elif o == "rfi":
                fall = False
            if fall and e in index:
                out.append(index[e])
            succ.append(out)
        f.blocks = blocks
        f.succ = succ
        # a tail call through a pointer (bctr that isn't a switch): the function forwards its
        # arguments to a target we don't know, so it takes the whole register file, like a
        # varargs function (MEMAllocFromAllocator: return a->func->alloc(a, size))
        for k, (bs, be) in enumerate(blocks):
            last = insns[be - 4]
            if last.op == "bcctr" and not last.lk and not succ[k]:
                f.varargs = f.forwarder = True
        # varargs: CodeWarrior saves f1-f8 behind "bne cr1" in the prologue
        for a in range(f.start, min(f.end, f.start + 0x60), 4):
            ins = insns[a]
            if ins.op == "bc" and ins.bi == 6 and (ins.bo & 0x1C) == 0x04:
                nxt = insns.get(a + 4)
                if nxt is not None and nxt.op == "stfd" and nxt.rd == 1:
                    f.varargs = True
                break
        # virtual-call slots
        for a in range(f.start, f.end, 4):
            ins = insns[a]
            if ins.op == "bcctr" and ins.lk:
                slot = self.find_vcall_slot(a, f.start)
                if slot is not None:
                    self.vcall_slot[a] = slot

    def find_vcall_slot(self, a, fstart):
        """lwz r12, SLOT(rV) ... mtctr r12 ... bctrl -> SLOT"""
        ctr_reg = None
        b = a - 4
        for _ in range(8):
            if b < fstart:
                return None
            ins = self.insns[b]
            if ctr_reg is None:
                if ins.op == "mtspr" and ins.spr == 9:
                    ctr_reg = ins.rd
            elif ins.op == "lwz" and ins.rd == ctr_reg:
                return ins.simm if ins.simm >= 8 else None
            elif ins.rd == ctr_reg and ins.op not in ("stw", "stb", "sth", "cmpi", "cmpli", "cmp", "cmpl"):
                return None
            b -= 4
        return None

    # -------------------------------------------------------------------------------------
    def call_uses(self, a, ins):
        """(kind, uses mask) for calls / tail calls; None for ordinary instructions."""
        o = ins.op
        if o in ("b", "bc"):
            t = ins.target
            if ins.lk:
                if t in self.helpers:
                    return ("helper", 0, t)
                g = self.fns.get(t)
                if g is None:
                    return ("call", ("evidence", 0), t)
                if g.varargs or t in self.passthrough:
                    return ("call", ("evidence", g.own), t)
                return ("call", g.params, t)
            return None
        if o == "bcctr" and ins.lk:
            slot = self.vcall_slot.get(a)
            if slot is not None and self.slot_fns.get(slot):
                extra = self.slot_union.get(slot, 0) if self.use_unions else 0
                return ("vcall", ("evidence", extra), slot)
            return ("icall", ("evidence", 0), None)
        if o == "bclr" and ins.lk:
            return ("icall", ("evidence", 0), None)
        return None

    def tail_uses(self, f, s, e, t):
        g = self.fns.get(t)
        if g is not None and not g.varargs and t not in self.passthrough:
            return g.params
        m = GPR(3)
        for b in range(s, e - 4, 4):
            ins = self.insns[b]
            if self.call_uses(b, ins) is not None:
                m = GPR(3)
                continue
            u, d = use_def(ins)
            m |= d & ARG_MASK
        return fill_holes(m | (g.own if g is not None else 0))

    def produces_all(self, f):
        """Return registers written on EVERY path from entry to a blr (must-analysis).
        64-bit and float returns are always set on all paths, unlike a stray r4/f1 temp."""
        insns = self.insns
        nb = len(f.blocks)
        RET = RET_R3 | RET_R4 | RET_F1
        preds = collections.defaultdict(list)
        for k, ss in enumerate(f.succ):
            for j in ss:
                preds[j].append(k)
        outm = [RET] * nb  # optimistic start for the intersection
        result = None
        changed = True
        while changed:
            changed = False
            result = None
            for k in range(nb):
                if k == 0:
                    # a float argument returned unchanged (sin(x) = x for tiny x); r3 passing
                    # through is handled by the caller, and r4 would only add false u64s
                    m = f.params & RET_F1
                else:
                    ps = preds[k]
                    m = RET  # (an unreachable block, with no predecessors, constrains nothing)
                    for p_ in ps:
                        m &= outm[p_]
                s_, e = f.blocks[k]
                for a in range(s_, e, 4):
                    ins = insns[a]
                    c = self.call_uses(a, ins)
                    if c is not None:
                        kind, uses, tgt = c
                        if kind == "helper":
                            continue
                        m &= ~RET
                        if kind == "call" and tgt in self.fns:
                            m |= self.prod_must.get(tgt, 0)
                        continue
                    u, d = use_def(ins)
                    m |= d & RET
                last = insns[e - 4]
                ends = None
                if last.op == "bclr" and not last.lk:
                    ends = m
                if last.op in ("b", "bc") and not last.lk and not (f.start <= last.target < f.end):
                    ends = self.prod_must.get(last.target, 0)
                if ends is not None:
                    result = ends if result is None else (result & ends)
                if m != outm[k]:
                    outm[k] = m
                    changed = True
        return result or 0

    def produces_ret(self, f):
        """Like produces_all, but a call leaves only the registers its callee returns by its
        current prototype (plus the r3/f1 it keeps): its scratch registers hold nothing."""
        insns = self.insns
        nb = len(f.blocks)
        if not nb:
            return 0
        RET = RET_ANY
        preds = collections.defaultdict(list)
        for k, ss in enumerate(f.succ):
            for j in ss:
                preds[j].append(k)
        outm = [RET] * nb
        result = None
        changed = True
        while changed:
            changed = False
            result = None
            for k in range(nb):
                if k == 0:
                    m = f.params & RET_F1
                else:
                    m = RET
                    for p_ in preds[k]:
                        m &= outm[p_]
                s_, e = f.blocks[k]
                for a in range(s_, e, 4):
                    ins = insns[a]
                    c = self.call_uses(a, ins)
                    if c is not None:
                        kind, uses, tgt = c
                        if kind == "helper":
                            continue
                        g = self.fns.get(tgt) if kind == "call" else None
                        # an indirect call may return anything (its call site hint decides)
                        m = ((g.ret | (m & g.kept)) & RET) if g is not None else RET
                        continue
                    u, d = use_def(ins)
                    m |= d & RET
                last = insns[e - 4]
                ends = None
                if last.op == "bclr" and not last.lk:
                    ends = m
                if last.op in ("b", "bc") and not last.lk and not (f.start <= last.target < f.end):
                    g = self.fns.get(last.target)
                    ends = ((g.ret | (m & g.kept)) & RET) if g is not None else 0
                if ends is not None:
                    result = ends if result is None else (result & ends)
                if m != outm[k]:
                    outm[k] = m
                    changed = True
        return result or 0

    def float_wins(self, f, own, ireg):
        """Callers read both f1 and an integer result (ireg: r3, or r4 for r3:r4): True to keep
        f1. Decided by what the function itself computes last for its callers (fresh_unread),
        then by what it sets on every path, then by the call sites reading the result with an
        instruction, then by all reading call sites."""
        a = f.start
        fresh = self.fresh_unread_cached(f)
        ff, fi = bool(fresh & RET_F1), bool(fresh & ireg)
        if ff != fi:
            return ff
        fo, io = bool(own & RET_F1), bool(own & ireg)
        if fo != io:
            return fo
        sf, si = self.sreads[RET_F1][a], self.sreads[ireg][a]
        if sf != si:
            return sf > si
        return self.reads[RET_F1][a] > self.reads[ireg][a]

    def fresh_unread(self, f):
        """r3/r4/f1 that, on every path to a blr, one of the function's own instructions writes
        after its last call and nothing reads before the return: values computed for the
        caller. (Holding a callee's result is weaker evidence: `bl g; lfs f1, x(r31); blr`
        returns the float, whatever g left in r3.)"""
        insns = self.insns
        nb = len(f.blocks)
        if not nb:
            return 0
        RET = RET_R3 | RET_R4 | RET_F1
        preds = collections.defaultdict(list)
        for k, ss in enumerate(f.succ):
            for j in ss:
                preds[j].append(k)
        outm = [RET] * nb
        result = None
        changed = True
        while changed:
            changed = False
            result = None
            for k in range(nb):
                m = 0 if k == 0 else RET
                if k:
                    for p_ in preds[k]:
                        m &= outm[p_]
                s_, e = f.blocks[k]
                for a in range(s_, e, 4):
                    ins = insns[a]
                    c = self.call_uses(a, ins)
                    if c is not None:
                        if c[0] != "helper":
                            m = 0
                        continue
                    u, d = use_def(ins)
                    m = (m & ~u) | (d & RET)
                last = insns[e - 4]
                if last.op == "bclr" and not last.lk:
                    result = m if result is None else (result & m)
                if last.op in ("b", "bc") and not last.lk and not (f.start <= last.target < f.end):
                    result = 0  # tail call: the target decides
                if last.op == "bcctr" and not last.lk and not f.succ[k]:
                    result = 0
                if m != outm[k]:
                    outm[k] = m
                    changed = True
        return result or 0

    def fresh_unread_cached(self, f):
        if not hasattr(self, "_fresh_unread"):
            self._fresh_unread = {}
        v = self._fresh_unread.get(f.start)
        if v is None:
            v = self._fresh_unread[f.start] = self.fresh_unread(f)
        return v

    def untouched(self, f):
        """r3/r4/f1 that no path from entry to a blr writes (and no call clobbers): a caller
        reading them after the call reads its own argument (a constructor's "this", or the
        {r3, r4} pair a small struct is returned in)."""
        insns = self.insns
        nb = len(f.blocks)
        if not nb:
            return 0
        KEEP = RET_R3 | RET_R4 | RET_F1
        preds = collections.defaultdict(list)
        for k, ss in enumerate(f.succ):
            for j in ss:
                preds[j].append(k)
        outm = [KEEP] * nb
        result = None
        changed = True
        while changed:
            changed = False
            result = None
            for k in range(nb):
                if k == 0:
                    m = KEEP
                else:
                    m = KEEP
                    for p_ in preds[k]:
                        m &= outm[p_]
                s_, e = f.blocks[k]
                for a in range(s_, e, 4):
                    ins = insns[a]
                    c = self.call_uses(a, ins)
                    if c is not None:
                        if c[0] != "helper":
                            m = 0
                        continue
                    u, d = use_def(ins)
                    m &= ~d
                last = insns[e - 4]
                if last.op == "bclr" and not last.lk:
                    result = m if result is None else (result & m)
                if last.op in ("b", "bc") and not last.lk and not (f.start <= last.target < f.end):
                    result = 0  # tail call: the target decides
                if last.op == "bcctr" and not last.lk and not f.succ[k]:
                    result = 0
                if m != outm[k]:
                    outm[k] = m
                    changed = True
        return result or 0

    def passes_r3(self, f):
        """True if some path from entry to a blr leaves r3 as it came in (no write, no call):
        such a function returns its r3 argument to a caller that reads r3 afterwards
        (an empty "blr" accessor, or "if (x) return; ..." with r3 still the argument)."""
        insns = self.insns
        nb = len(f.blocks)
        if not nb:
            return False
        preds = collections.defaultdict(list)
        for k, ss in enumerate(f.succ):
            for j in ss:
                preds[j].append(k)
        keep_in = [False] * nb
        keep_in[0] = True
        work = [0]
        seen = set()
        while work:
            k = work.pop()
            if k in seen:
                continue
            seen.add(k)
            s_, e = f.blocks[k]
            alive = keep_in[k]
            for a in range(s_, e, 4):
                if not alive:
                    break
                ins = insns[a]
                c = self.call_uses(a, ins)
                if c is not None:
                    if c[0] != "helper":
                        alive = False
                    continue
                u, d = use_def(ins)
                if d & RET_R3:
                    alive = False
            if not alive:
                continue
            last = insns[e - 4]
            if last.op == "bclr" and not last.lk:
                return True
            for j in f.succ[k]:
                if not keep_in[j]:
                    keep_in[j] = True
                    seen.discard(j)
                work.append(j)
        return False

    def produces(self, f):
        """Return registers (r3/r4/f1) written on some path from entry to a blr."""
        insns = self.insns
        nb = len(f.blocks)
        RET = RET_R3 | RET_R4 | RET_F1
        preds = collections.defaultdict(list)
        for k, ss in enumerate(f.succ):
            for j in ss:
                preds[j].append(k)
        outm = [0] * nb
        result = 0
        changed = True
        while changed:
            changed = False
            for k in range(nb):
                m = 0
                for p in preds[k]:
                    m |= outm[p]
                s, e = f.blocks[k]
                for a in range(s, e, 4):
                    ins = insns[a]
                    c = self.call_uses(a, ins)
                    if c is not None:
                        kind, uses, tgt = c
                        if kind == "helper":
                            continue
                        m &= ~RET
                        if kind == "call" and tgt in self.fns:
                            m |= self.prod_may.get(tgt, 0)
                        elif kind == "vcall":
                            for t in self.slot_fns.get(tgt, ()):
                                m |= self.prod_may.get(t, 0)
                        continue
                    u, d = use_def(ins)
                    m |= d & RET
                last = insns[e - 4]
                if last.op == "bclr" and not last.lk:
                    result |= m
                if last.op in ("b", "bc") and not last.lk and not (f.start <= last.target < f.end):
                    result |= self.prod_may.get(last.target, 0)
                if m != outm[k]:
                    outm[k] = m
                    changed = True
        return result

    def liveness(self, f, record=False, rets=True):
        """Live-in arg registers of f; when record, note which callees' returns are read.

        Recording also tells apart how a call's result is read: by an instruction of f
        ("strong" reads), only by being returned to f's own callers (a ret edge, judged by
        what those callers do), or only through the parameters of a later callee, whose
        parameter sets can be over-approximations (vtable slot unions, full register files)."""
        insns = self.insns
        nb = len(f.blocks)
        live_in = [0] * nb
        s_in = [0] * nb  # read by an instruction before being written
        r_in = [0] * nb  # reaching our own return
        ret_live = (f.ret | f.kept) if rets else 0  # (not: what a forwarder reads itself)

        def walk(k, rec):
            s, e = f.blocks[k]
            out = sl = rl = 0
            for j in f.succ[k]:
                out |= live_in[j]
                sl |= s_in[j]
                rl |= r_in[j]
            last = insns[e - 4]
            lo = last.op
            if lo == "bclr" and not last.lk:
                out |= ret_live
                rl |= ret_live
            if lo in ("b", "bc") and not last.lk and not (f.start <= last.target < f.end):
                out |= self.tail_uses(f, s, e, last.target)
                if rec:  # a tail call returns the target's result to our callers
                    self.tail_edges.add((f.start, last.target))
            if lo == "bcctr" and not last.lk and not f.succ[k]:
                out |= GPR(3)
            live = out
            for a in range(e - 4, s - 1, -4):
                ins = insns[a]
                c = self.call_uses(a, ins)
                if c is not None and c[0] == "helper":
                    continue
                if c is not None:
                    kind, uses, tgt = c
                    if rec and kind in ("call", "vcall"):
                        used = live & RET_ANY
                        if used:
                            strong = sl & used
                            via_ret = rl & used & ~strong
                            targets = [tgt] if kind == "call" else list(self.slot_fns.get(tgt, ()))
                            # a virtual call may reach any function in its vtable slot, of any
                            # class: its reads count for each of them in proportion
                            w = 1.0 / len(targets) if targets else 0.0
                            for t in targets:
                                self.ret_users[t] |= used
                                for reg in RET_REGS:
                                    if used & reg:
                                        self.reads[reg][t] += w
                                    if strong & reg:
                                        self.sreads[reg][t] += w
                                if via_ret:
                                    self.ret_edges.add((f.start, t, via_ret))
                    if isinstance(uses, tuple):  # caller-side evidence: r3 + args set in this block
                        m = GPR(3) | uses[1]
                        for b in range(s, a, 4):
                            u, d = use_def(insns[b])
                            if self.call_uses(b, insns[b]) is not None and insns[b].op != "bcctr":
                                m &= ~VOLATILE
                                m |= GPR(3)
                                continue
                            m |= d & ARG_MASK
                        uses = fill_holes(m)
                    live = (live & ~VOLATILE) | uses
                    sl &= ~VOLATILE
                    rl &= ~VOLATILE
                    continue
                u, d = use_def(ins)
                live = (live & ~d) | u
                sl = (sl & ~d) | u
                rl &= ~d
            return live, sl, rl

        changed = True
        while changed:
            changed = False
            for k in range(nb - 1, -1, -1):
                live, sl, rl = walk(k, False)
                if live != live_in[k] or (record and (sl != s_in[k] or rl != r_in[k])):
                    live_in[k], s_in[k], r_in[k] = live, sl, rl
                    changed = True
        if record:
            for k in range(nb):
                walk(k, True)
        return live_in[0] & ARG_MASK if nb else 0

    def untouched_cached(self, f):
        if not hasattr(self, "_untouched"):
            self._untouched = {}
        v = self._untouched.get(f.start)
        if v is None:
            v = self._untouched[f.start] = self.untouched(f)
        return v

    def passes_r3_cached(self, f):
        if not hasattr(self, "_passes_r3"):
            self._passes_r3 = {}
        v = self._passes_r3.get(f.start)
        if v is None:
            v = self._passes_r3[f.start] = self.passes_r3(f)
        return v

    def callers(self):
        """function -> functions that call it, tail-call it or may reach it via a vtable slot"""
        if getattr(self, "_callers", None) is None:
            self._callers = collections.defaultdict(set)
            for f in self.fns.values():
                for a in range(f.start, f.end, 4):
                    ins = self.insns[a]
                    c = self.call_uses(a, ins)
                    if c is not None and c[0] == "call" and c[2] in self.fns:
                        self._callers[c[2]].add(f.start)
                    elif c is not None and c[0] == "vcall":
                        for t in self.slot_fns.get(c[2], ()):
                            self._callers[t].add(f.start)
                    elif ins.op in ("b", "bc") and not ins.lk and ins.target in self.fns \
                            and not (f.start <= ins.target < f.end):
                        self._callers[ins.target].add(f.start)
        return self._callers

    def compute_produced(self):
        """Return registers each function may write (on some path to its return) and must
        write (on all paths), with calls contributing what their callee produces. A worklist
        over the call graph: a function is looked at again when one of its callees changes."""
        RET = RET_R3 | RET_R4 | RET_F1
        callers = self.callers()
        self.prod_may = {a: 0 for a in self.fns}
        self.prod_must = {a: RET for a in self.fns}  # optimistic start for the intersection
        work = collections.deque(self.fns)
        queued = set(work)
        steps = 0
        while work:
            a = work.popleft()
            queued.discard(a)
            f = self.fns[a]
            may, must = self.produces(f), self.produces_all(f)
            steps += 1
            if may != self.prod_may[a] or must != self.prod_must[a]:
                self.prod_may[a], self.prod_must[a] = may, must
                for c in callers.get(a, ()):
                    if c not in queued:
                        queued.add(c)
                        work.append(c)
        self.log("produced registers: %d function visits" % steps)

    def thunk_target(self, f):
        """The target of a thunk, else None: a this-adjusting one (CodeWarrior's: addi r3,r3,-N /
        addi r4,r4,-N for a method returning a struct, or the virtual-base form lwz r12,0(r3);
        lwz r12,N(r12); add r3,r3,r12) or a method handing the call to a member
        (lwz r3,N(r3)), then b target. A thunk passes every other register on."""
        if f.end - f.start > 0x10:
            return None
        last = self.insns[f.end - 4]
        if last.op != "b" or last.lk:
            return None
        for a in range(f.start, f.end - 4, 4):
            ins = self.insns[a]
            if ins.op == "addi" and ins.rd == ins.ra and ins.rd in (3, 4):
                continue
            if ins.op == "lwz" and ins.rd == 12:
                continue
            if ins.op == "add" and ins.rd == 3 and ins.ra == 3 and ins.rb == 12:
                continue
            if ins.op == "lwz" and ins.rd == 3 and ins.ra == 3:
                continue
            return None
        return last.target

    def solve(self, max_iter=30):
        self.build()
        # a thunk to a function that forwards the whole register file forwards it too (its own
        # callers then pass what they set, as for the target); other targets' parameters reach
        # the thunk through the tail call
        moved = True
        while moved:
            moved = False
            for f in self.fns.values():
                t = self.thunk_target(f)
                g = self.fns.get(t)
                if not f.varargs and g is not None and (g.varargs or t in self.passthrough):
                    f.varargs = moved = True
                    f.forwarder = g.forwarder
        self.use_unions = False
        self.fixpoint(max_iter)
        clean = {a: f.params for a, f in self.fns.items()}
        self.slot_union = {}
        for slot, fns in self.slot_fns.items():
            m = 0
            for t in fns:
                m |= clean[t]
            self.slot_union[slot] = m
        self.use_unions = True
        self.fixpoint(max_iter)

    def fixpoint(self, max_iter):
        fns = list(self.fns.values())
        self.flips = collections.Counter()
        self.frozen = set()
        for f in fns:
            if f.varargs:
                f.params = ARG_MASK
            if f.name in KNOWN_PROTOTYPES:
                f.ret, f.params = KNOWN_PROTOTYPES[f.name]
        changed = 1
        for it in range(max_iter):
            if changed:
                self.compute_produced()  # arguments count as produced: redo when they change
            changed = 0
            for f in fns:
                if f.varargs:
                    # a forwarder's own reads: not what it only hands back to its callers
                    own = self.liveness(f, rets=False) if f.forwarder else 0
                    if own != f.own:
                        f.own = own
                        changed += 1
                    p = ARG_MASK
                else:
                    p = self.liveness(f)
                if f.name in KNOWN_PROTOTYPES:
                    p = KNOWN_PROTOTYPES[f.name][1]
                if p != f.params:
                    f.params = p
                    changed += 1
            self.ret_users.clear()
            self.tail_edges = set()
            self.ret_edges = set()
            self.reads = {r: collections.Counter() for r in RET_REGS}
            self.sreads = {r: collections.Counter() for r in RET_REGS}
            for f in fns:
                self.liveness(f, record=True)
            # functions called through pointers may have whatever they produce read
            for a in self.addr_taken:
                self.ret_users[a] |= RET_R3 | RET_F1
            # what f's callers read, a function f tail-calls returns to them. (A result f only
            # returns from a call is not counted as read by f's callers' instructions: f's own
            # type may be wrong, and the reads would spread it.)
            moved = True
            while moved:
                moved = False
                for src, dst in self.tail_edges:
                    r = self.ret_users.get(dst, 0) | self.ret_users.get(src, 0)
                    if r != self.ret_users.get(dst, 0):
                        self.ret_users[dst] = r
                        moved = True
                    for reg in RET_REGS:
                        for cnt in (self.reads[reg], self.sreads[reg]):
                            if cnt[src] > cnt[dst]:
                                cnt[dst] = cnt[src]
                                moved = True
            rchanged = 0
            changes = []
            for f in fns:
                users = self.ret_users.get(f.start, 0)
                # r4 that no path changes: callers read back their own argument (a struct
                # {r3, r4} "returned" by a function that only stores its arguments)
                keep4 = users & RET_R4 & self.untouched_cached(f) if users & RET_R4 else 0
                r = users & ~keep4
                if r & RET_R4:
                    r |= RET_R3
                own = None
                if r:
                    must = self.prod_must[f.start]
                    passthrough = RET_R3 if (r & RET_R3) and self.passes_r3_cached(f) else 0
                    if r & RET_R4 and must & RET_R4:
                        # r4 written on every path, but by a callee as scratch is no result
                        own = self.produces_ret(f)
                        must &= own | ~RET_R4
                    # f1 written on some path only still counts when a caller's instruction reads
                    # it (a float function with a path through a callee whose type is unknown)
                    f1_used = RET_F1 if self.sreads[RET_F1][f.start] >= 1 else 0
                    r &= (self.prod_may[f.start] & (RET_R3 | f1_used)) | (must & (RET_R4 | RET_F1)) | \
                        (f.params & RET_R3) | passthrough
                if (r & RET_R4) and (r & RET_F1):
                    # r3:r4 or f1: a float function's address temporaries in r3/r4 look read
                    # where a caller passes them on to an over-approximated callee
                    if own is None:
                        own = self.produces_ret(f)
                    r &= ~RET_R4 if self.float_wins(f, own, RET_R4) else ~RET_F1
                # registers no path changes are "kept": callers use their own argument, and the
                # function can return something else (or nothing) at the same time
                kept = keep4
                if r & (RET_R3 | RET_F1) and not (r & RET_R4):
                    k = r & self.untouched_cached(f)
                    kept |= k
                    r &= ~k
                if (r & RET_R3) and (r & RET_F1) and not (r & RET_R4):
                    # both read by different callers: keep the one the function really sets
                    if own is None:
                        own = self.produces_ret(f)
                    r &= ~RET_R3 if self.float_wins(f, own, RET_R3) else ~RET_F1
                if f.name in KNOWN_PROTOTYPES:
                    r, kept = KNOWN_PROTOTYPES[f.name][0], 0
                if (r != f.ret or kept != f.kept) and self.flips[f.start] >= 4:
                    # flips back and forth with others (decisions that feed each other's read
                    # counts): settle on the current one
                    if f.start not in self.frozen:
                        self.frozen.add(f.start)
                        self.log("  return of %s keeps flipping: kept as %x/%x" % (f.name, f.ret, f.kept))
                    continue
                if r != f.ret or kept != f.kept:
                    self.flips[f.start] += 1
                    changes.append((f, f.ret, f.kept, r, kept))
                    f.ret = r
                    f.kept = kept
                    rchanged += 1
            if 0 < len(changes) <= 8:
                for f, r0, k0, r1, k1 in changes:
                    self.log("  return of %s: %x/%x -> %x/%x" % (f.name, r0, k0, r1, k1))
            self.log("signature pass %d: %d params changed, %d returns changed" % (it, changed, rchanged))
            if not changed and not rchanged:
                break

    # -------------------------------------------------------------------------------------
    @staticmethod
    def c_proto(f, name=None):
        ret = "void"
        if f.ret & RET_R4:
            ret = "u64"
        elif f.ret & RET_R3 and f.ret & RET_F1:
            ret = "u32"  # both read by different callers; f1 users are rare false positives
        elif f.ret & RET_R3:
            ret = "u32"
        elif f.ret & RET_F1:
            ret = "f64"
        params = []
        last_g = max([r for r in ARG_GPRS if f.params & GPR(r)] or [0])
        if f.kept & RET_R4:
            last_g = max(last_g, 4)  # a kept register is passed through, so it is a parameter
        last_f = max([r for r in ARG_FPRS if f.params & FPR(r)] or [0])
        for r in ARG_GPRS:
            if r <= last_g:
                kept = "_kept" if (r == 3 and f.kept & RET_R3) or (r == 4 and f.kept & RET_R4) else ""
                params.append("u32 r%d%s" % (r, kept))
        for r in ARG_FPRS:
            if r <= last_f:
                kept = "_kept" if r == 1 and f.kept & RET_F1 else ""
                params.append("f64 f%d%s" % (r, kept))
        if f.varargs:
            params = ["u32 r3", "..."]
        return "%s %s(%s)" % (ret, name or f.name, ", ".join(params) or "void")

    def write(self, path):
        with open(path, "w") as out:
            for a in sorted(self.fns):
                f = self.fns[a]
                out.write("%08X %s;\n" % (a, self.c_proto(f)))
