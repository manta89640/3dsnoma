"""An estimate of ARM11 MPCore cycles for the instructions armbench.py runs under Unicorn.

The rules are the ones the ARM11 MPCore Technical Reference Manual gives in its chapter "Cycle
Timings and Interlock Behavior" (ARM DDI 0360F, chapter 15), applied to the instructions as they
execute, in order:

  * issue cycles per instruction: 1 for most, 2 for register-controlled shifts, MUL/MLA, SMLALD,
    SMMUL and loads/stores with a negative or shifted (other than LSL #2) register offset, 3 for
    SMULL/UMULL/SMLAL/UMLAL;
  * result latencies (cycles from an instruction's first cycle until a following instruction can
    use the result in its ALU stage): 1 for data processing, PKHBT, REV, SXT/UXT; 2 for SSAT/USAT,
    QADD and the saturating/halving SIMD; 3 for loads (one more for an unaligned address or a
    2-issue-cycle addressing mode) and the 16x16/32x16 multiplies (SMULxy, SMLAWy, SMLAD...); 4 for
    MUL/MLA; 4/5 for the low/high results of 64-bit multiplies. A multiply's result feeding the
    accumulator of the next multiply-accumulate comes one cycle earlier;
  * Early Regs (needed a cycle earlier: shifted operands, the multiplicand and multiplier, load and
    store base and offset registers) and Late Regs (MLA's accumulator, the unshifted operand of a
    register-controlled shift);
  * the load/store unit: a 64-bit data path, so LDM/STM/LDRD/STRD move two registers a cycle when
    64-bit aligned; an unaligned single access takes another memory cycle;
  * branches: a 128-entry dynamic predictor indexed by address bits [9:3] with 2-bit counters,
    static prediction for branches it holds no entry for (backward taken, forward not taken),
    1 cycle when predicted (a statically predicted taken branch 4), 5-7 when mispredicted
    (7 minus the cycles since the flags were set, at least 5); a 3-entry return stack (a predicted
    return 4 cycles, mispredicted 7, empty 5).

What it leaves out: caches and memory (every access hits; on the Old 3DS, which has no L2 cache,
misses to FCRAM cost far more), branch folding (a predicted branch is counted as 1 cycle, not 0),
store-buffer and write-allocate effects, and register lock latencies of single stores. So the
numbers are lower bounds for code whose data is in the 16 KB L1 data cache; they are meant for
comparing versions of the same kernel, not as measured hardware time.
"""
import capstone
from capstone import arm_const as A
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_PC)

UC_REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
           UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP,
           UC_ARM_REG_LR, UC_ARM_REG_PC]
FLAGS = 16  # the condition flags, tracked like a register

# capstone register ids -> 0..15
REG = {}
for n in range(13):
    REG[getattr(A, "ARM_REG_R%d" % n)] = n
REG.update({A.ARM_REG_SP: 13, A.ARM_REG_LR: 14, A.ARM_REG_PC: 15})


def _ids(*names):
    return {getattr(A, "ARM_INS_" + n) for n in names if hasattr(A, "ARM_INS_" + n)}


DP = _ids("AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC", "TST", "TEQ", "CMP", "CMN", "ORR", "MOV", "BIC",
          "MVN", "LSL", "LSR", "ASR", "ROR", "RRX", "CLZ")
COMPARE = _ids("TST", "TEQ", "CMP", "CMN")
CARRY_IN = _ids("ADC", "SBC", "RSC", "RRX")
MEDIA1 = _ids("SADD16", "SSUB16", "SADD8", "SSUB8", "UADD16", "USUB16", "UADD8", "USUB8", "SEL", "SASX", "SSAX",
              "UASX", "USAX", "SXTB", "SXTH", "SXTB16", "UXTB", "UXTH", "UXTB16", "SXTAB", "SXTAH", "SXTAB16", "UXTAB",
              "UXTAH", "UXTAB16", "REV", "REV16", "REVSH", "PKHBT", "PKHTB")
MEDIA1_EARLY_M = _ids("SASX", "SSAX", "UASX", "USAX", "SXTB", "SXTH", "SXTB16", "UXTB", "UXTH", "UXTB16", "SXTAB",
                      "SXTAH", "SXTAB16", "UXTAB", "UXTAH", "UXTAB16", "REV", "REV16", "REVSH", "PKHBT", "PKHTB")
MEDIA2 = _ids("QADD16", "QSUB16", "QADD8", "QSUB8", "SHADD16", "SHSUB16", "SHADD8", "SHSUB8", "UQADD16", "UQSUB16",
              "UQADD8", "UQSUB8", "UHADD16", "UHSUB16", "UHADD8", "UHSUB8", "SSAT16", "USAT16", "SSAT", "USAT",
              "QASX", "QSAX", "SHASX", "SHSAX", "UQASX", "UQSAX", "UHASX", "UHSAX", "QADD", "QSUB", "QDADD", "QDSUB")
MEDIA2_EARLY_M = _ids("SSAT", "USAT", "QASX", "QSAX", "SHASX", "SHSAX", "UQASX", "UQSAX", "UHASX", "UHSAX")
SAD = _ids("USAD8", "USADA8")
# multiplies: (issue cycles, result latency, late register operand index or None); 64-bit ones have
# two result latencies
MUL = {}
for n in ("MUL",):
    MUL.update({i: (2, (4,), None) for i in _ids(n)})
MUL.update({i: (2, (4,), 3) for i in _ids("MLA", "MLS")})
MUL.update({i: (3, (4, 5), None) for i in _ids("SMULL", "UMULL")})
MUL.update({i: (3, (4, 5), 0) for i in _ids("SMLAL", "UMLAL", "UMAAL")})
MUL.update({i: (1, (3,), None) for i in _ids("SMULBB", "SMULBT", "SMULTB", "SMULTT", "SMULWB", "SMULWT", "SMUAD",
                                               "SMUADX", "SMUSD", "SMUSDX")})
MUL.update({i: (1, (3,), None) for i in _ids("SMLABB", "SMLABT", "SMLATB", "SMLATT", "SMLAWB", "SMLAWT", "SMLAD",
                                               "SMLADX", "SMLSD", "SMLSDX")})
MUL.update({i: (2, (3, 4), 1) for i in _ids("SMLALBB", "SMLALBT", "SMLALTB", "SMLALTT", "SMLALD", "SMLALDX",
                                              "SMLSLD", "SMLSLDX")})
MUL.update({i: (2, (4,), None) for i in _ids("SMMUL", "SMMULR")})
MUL.update({i: (2, (4,), 3) for i in _ids("SMMLA", "SMMLAR", "SMMLS", "SMMLSR")})
MUL_ACC = _ids("MLA", "MLS", "SMLAL", "UMLAL", "UMAAL", "SMLABB", "SMLABT", "SMLATB", "SMLATT", "SMLAWB", "SMLAWT",
               "SMLAD", "SMLADX", "SMLSD", "SMLSDX", "SMLALBB", "SMLALBT", "SMLALTB", "SMLALTT", "SMLALD", "SMLALDX",
               "SMLSLD", "SMLSLDX", "SMMLA", "SMMLAR", "SMMLS", "SMMLSR")
LOAD = {i: s for s, names in ((4, ("LDR", "LDRT")), (1, ("LDRB", "LDRBT", "LDRSB")), (2, ("LDRH", "LDRSH")))
        for i in _ids(*names)}
STORE = {i: s for s, names in ((4, ("STR", "STRT")), (1, ("STRB", "STRBT")), (2, ("STRH",))) for i in _ids(*names)}
PLD = _ids("PLD")
LDRD, STRD = _ids("LDRD"), _ids("STRD")
LDM = _ids("LDM", "LDMDA", "LDMDB", "LDMIB", "POP")
STM = _ids("STM", "STMDA", "STMDB", "STMIB", "PUSH")
B, BL, BLX, BX = _ids("B"), _ids("BL"), _ids("BLX"), _ids("BX")

SFT_REG = {A.ARM_SFT_ASR_REG, A.ARM_SFT_LSL_REG, A.ARM_SFT_LSR_REG, A.ARM_SFT_ROR_REG, A.ARM_SFT_RRX_REG}


def cond_passes(cc, cpsr):
    n, z, c, v = cpsr >> 31 & 1, cpsr >> 30 & 1, cpsr >> 29 & 1, cpsr >> 28 & 1
    return {A.ARM_CC_EQ: z, A.ARM_CC_NE: not z, A.ARM_CC_HS: c, A.ARM_CC_LO: not c, A.ARM_CC_MI: n,
            A.ARM_CC_PL: not n, A.ARM_CC_VS: v, A.ARM_CC_VC: not v, A.ARM_CC_HI: c and not z,
            A.ARM_CC_LS: not c or z, A.ARM_CC_GE: n == v, A.ARM_CC_LT: n != v, A.ARM_CC_GT: not z and n == v,
            A.ARM_CC_LE: z or n != v}.get(cc, True)


class _Plan:
    """what an instruction needs and makes, worked out once per address"""
    __slots__ = ("cond", "flags_in", "sets_flags", "srcs", "dests", "issue", "kind", "mem", "lsu", "target",
                 "links", "is_return", "mul_fail", "ldm", "pc_write")


K_PLAIN, K_MEM, K_MEMD, K_LDM, K_B, K_BX = range(6)


class Arm11Timing:
    """Feed it each executed instruction (step); `cycles` is the estimate so far."""

    def __init__(self):
        self.md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
        self.md.detail = True
        self.plans = {}
        self.reset()

    def reset(self):
        self.t = 0                 # the first cycle the next instruction may issue in
        self.ready = [0] * 17      # cycle a register's value can be used in the ALU stage
        self.mac_ready = [0] * 17  # ... as the accumulator of a multiply-accumulate
        self.lsu = 0               # the load/store unit is free from this cycle
        self.flags_set = -10       # first cycle of the last flag-setting instruction
        self.btac = {}             # index -> [branch address, 2-bit counter]
        self.rstack = []
        self.cycles = 0

    def plan(self, uc, addr):
        insn = next(self.md.disasm(bytes(uc.mem_read(addr, 4)), addr))
        iid, ops = insn.id, insn.operands
        regs_of = [REG.get(o.reg) if o.type == A.ARM_OP_REG else None for o in ops]
        P = _Plan()
        P.cond = insn.cc if insn.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID) else None
        P.flags_in = P.cond is not None or iid in CARRY_IN
        P.sets_flags = bool(insn.update_flags and iid not in CARRY_IN or iid in COMPARE)
        P.srcs, P.dests, P.issue, P.kind, P.mem, P.lsu = [], [], 1, K_PLAIN, None, 0
        P.target, P.links, P.is_return, P.mul_fail, P.ldm, P.pc_write = None, False, False, False, None, 0
        srcs, dests = P.srcs, P.dests  # srcs: (reg, kind, acc); dests: (reg, latency, mac)
        if iid in DP or iid in MEDIA1 or iid in MEDIA2 or iid in SAD:
            lat = 2 if iid in MEDIA2 else 3 if iid in SAD else 1
            first_src = 0 if iid in COMPARE else 1
            if iid in _ids("SSAT", "USAT", "SSAT16", "USAT16"):
                first_src = 2  # (rd, #sat, rm)
            sl = []
            for k, o in enumerate(ops[first_src:], first_src):
                if o.type != A.ARM_OP_REG:
                    continue
                r = REG.get(o.reg)
                last = k == len(ops) - 1
                if o.shift.type in SFT_REG:
                    P.issue, lat = 2, max(lat, 2)
                    sl.append((REG.get(o.shift.value), 1))
                    sl.append((r, 0))
                elif o.shift.type or (last and (iid in MEDIA1_EARLY_M or iid in MEDIA2_EARLY_M or iid in SAD)):
                    sl.append((r, 1))
                else:
                    sl.append((r, 0))
            if P.issue == 2:  # register-controlled shift: the unshifted operand is a Late Reg
                sl = [(r, -1) if kind == 0 and k == 0 and len(sl) > 2 else (r, kind) for k, (r, kind) in enumerate(sl)]
            if iid in SAD:
                sl = [(r, 1) for r, _ in sl[:2]] + sl[2:]
            srcs += [(r, kind, False) for r, kind in sl]
            if iid not in COMPARE:
                dests.append((regs_of[0], lat, False))
                if regs_of[0] == 15:
                    P.pc_write = 6  # a data-processing write to the PC: an unpredicted branch
        elif iid in MUL:
            P.issue, lats, late = MUL[iid]
            P.mul_fail = True
            n_dst = 2 if len(lats) == 2 else 1
            ms = regs_of[n_dst:]
            accumulates = iid in MUL_ACC
            for k, r in enumerate(ms):
                is_acc = accumulates and k == len(ms) - 1 and n_dst == 1
                if k < 2:
                    srcs.append((r, 1, False))
                elif is_acc:
                    srcs.append((r, -1 if late is not None else 0, True))
                else:
                    srcs.append((r, 0, False))
            if n_dst == 2 and accumulates:  # the 64-bit accumulators are the destinations
                srcs.append((regs_of[0], -1, True))
                srcs.append((regs_of[1], 0, True))
            for k in range(n_dst):
                dests.append((regs_of[k], lats[k], True))
        elif iid in LOAD or iid in STORE or iid in PLD or iid in LDRD or iid in STRD:
            size = LOAD.get(iid) or STORE.get(iid) or 1
            if iid in LDRD or iid in STRD:
                size = 8
            mem_k = [k for k, o in enumerate(ops) if o.type == A.ARM_OP_MEM][0]
            mem = ops[mem_k]
            base = REG.get(mem.mem.base)
            post = mem_k + 1 < len(ops)  # an operand after the address: post-indexed
            off_reg, neg, sh_t, sh_v, imm = None, False, 0, 0, 0
            if post:
                o = ops[mem_k + 1]
                if o.type == A.ARM_OP_REG:
                    off_reg, neg, sh_t, sh_v = REG.get(o.reg), o.subtracted, o.shift.type, o.shift.value
            elif mem.mem.index:
                off_reg, neg, sh_t, sh_v = REG.get(mem.mem.index), mem.subtracted, mem.shift.type, mem.shift.value
            else:
                imm = mem.mem.disp if not mem.subtracted else -mem.mem.disp
            two = off_reg is not None and (neg or (sh_t and not (sh_t == A.ARM_SFT_LSL and sh_v == 2)))
            P.issue = 2 if two else 1
            P.kind = K_MEMD if size == 8 else K_MEM
            P.mem = (base, off_reg, neg, sh_t, sh_v, imm, post, size, two,
                     iid in LOAD or iid in LDRD, iid in LDRD, regs_of[0], regs_of[1] if size == 8 else None,
                     bool(insn.writeback or post))
            srcs.append((base, 0 if two else 1, False))
            if off_reg is not None:
                srcs.append((off_reg, 1, False))
            if iid in STORE or iid in STRD:
                for r in regs_of[:2 if iid in STRD else 1]:
                    srcs.append((r, 0, False))
            if iid in LOAD and regs_of[0] == 15:
                P.pc_write = 7
        elif iid in LDM or iid in STM:
            if iid in _ids("PUSH", "POP"):
                base, lst = 13, regs_of
            else:
                base, lst = regs_of[0], regs_of[1:]
            mode = 0  # first address: base (IA), base + 4 (IB), base - 4n (DB/PUSH), base - 4n + 4 (DA)
            if iid in _ids("PUSH", "STMDB", "LDMDB"):
                mode = 2
            elif iid in _ids("STMIB", "LDMIB"):
                mode = 1
            elif iid in _ids("STMDA", "LDMDA"):
                mode = 3
            P.kind = K_LDM
            P.ldm = (base, tuple(lst), mode, iid in LDM, bool(insn.writeback or iid in _ids("PUSH", "POP")))
            srcs.append((base, 1, False))
            if iid in STM:
                for r in lst:
                    srcs.append((r, 0, False))
            if iid in LDM and 15 in lst:
                P.is_return = lst[-1] == 15 and base == 13
                P.pc_write = 1
        elif iid in B or iid in BL:
            P.kind, P.target, P.links = K_B, ops[0].imm, iid in BL
        elif iid in BX or iid in BLX:
            r = regs_of[0]
            if r is None:  # BLX #imm
                P.kind, P.target, P.links = K_B, ops[0].imm, True
            else:
                P.kind, P.links = K_BX, iid in BLX
                P.is_return = iid in BX and r == 14
                srcs.append((r, 0, False))
        else:
            for r in regs_of:
                srcs.append((r, 0, False))
            dests.append((regs_of[0] if regs_of else None, 1, False))
        srcs[:] = [x for x in srcs if x[0] is not None and x[0] != 15]
        dests[:] = [x for x in dests if x[0] is not None and x[0] != 15]
        self.plans[addr] = P
        return P

    def step(self, uc, addr):
        P = self.plans.get(addr) or self.plan(uc, addr)
        passes = P.cond is None or cond_passes(P.cond, uc.reg_read(UC_ARM_REG_CPSR))
        ready, mac_ready = self.ready, self.mac_ready
        start = self.t
        if P.flags_in and ready[FLAGS] > start:
            start = ready[FLAGS]
        for r, kind, acc in P.srcs:
            v = (mac_ready[r] if acc else ready[r]) + kind
            if v > start:
                start = v
        issue, lsu_cycles, extra = P.issue, P.lsu, []
        k = P.kind
        if k == K_MEM or k == K_MEMD:
            (base, off_reg, neg, sh_t, sh_v, imm, post, size, two, is_load, is_ldrd, rt, rt2, wb) = P.mem
            breg = uc.reg_read(UC_REGS[base]) if base != 15 else addr + 8
            if off_reg is not None:
                off = uc.reg_read(UC_REGS[off_reg]) if off_reg != 15 else addr + 8
                if sh_t == A.ARM_SFT_LSL:
                    off = off << sh_v
                elif sh_t == A.ARM_SFT_LSR:
                    off = off >> sh_v
                elif sh_t == A.ARM_SFT_ASR:
                    off = (off - (1 << 32) if off >> 31 else off) >> sh_v
                off = -off if neg else off
            else:
                off = imm
            ea = breg if post else (breg + off) & 0xFFFFFFFF
            if size == 8:
                lsu_cycles = 1 if ea % 8 == 0 else 2
            else:
                lsu_cycles = 2 if size > 1 and (breg % size or off % size) else 1
            if self.lsu > start:
                start = self.lsu
            lat = (4 if two else 3) + (1 if size in (2, 4) and ea % size else 0)
            if is_load and rt is not None and rt != 15:
                extra.append((rt, lat, False))
                if is_ldrd and rt2 is not None:
                    extra.append((rt2, lat + (0 if ea % 8 == 0 else 1), False))
            if wb:
                extra.append((base, 1, False))
            if P.pc_write and passes:
                issue += P.pc_write
        elif k == K_LDM:
            base, lst, mode, is_load, wb = P.ldm
            sp = uc.reg_read(UC_REGS[base])
            n = len(lst)
            first = sp - 4 * n if mode == 2 else sp + 4 if mode == 1 else sp - 4 * n + 4 if mode == 3 else sp
            aligned = first % 8 == 0
            lsu_cycles = (n + 1) // 2 if aligned else (n + 2) // 2
            if self.lsu > start:
                start = self.lsu
            if is_load:
                for j, r in enumerate(lst):
                    if r is not None and r != 15:
                        extra.append((r, 3 + (j // 2 if aligned else (j + 1) // 2), False))
            if wb:
                extra.append((base, 1, False))
            if P.pc_write and passes:
                issue += self._return() if P.is_return else 8
        elif k == K_B:
            issue = self._branch(addr, P.target, passes, start)
            if P.links and passes:
                self.rstack = ([addr + 4] + self.rstack)[:3]
                extra.append((14, 1, False))
        elif k == K_BX:
            if not passes:
                issue = 1
            elif P.is_return:
                issue = self._return()
            else:
                issue = 6
            if P.links and passes:
                self.rstack = ([addr + 4] + self.rstack)[:3]
                extra.append((14, 1, False))
        else:
            if P.pc_write and passes:
                issue += P.pc_write
            if P.mul_fail and not passes:
                issue = min(issue, max(5 - (start - self.flags_set - 1), 3))
        if passes:
            for r, lat, mac in P.dests:
                ready[r] = start + lat
                mac_ready[r] = start + lat - 1 if mac else start + lat
            for r, lat, mac in extra:
                ready[r] = start + lat
                mac_ready[r] = start + lat
        if P.sets_flags and passes:
            ready[FLAGS] = start + 1
            self.flags_set = start
        if lsu_cycles:
            self.lsu = start + lsu_cycles
        self.t = start + issue
        self.cycles = self.t

    def _branch(self, addr, target, taken, start):
        idx = (addr >> 3) & 127
        e = self.btac.get(idx)
        dist = start - self.flags_set - 1
        miss = max(7 - dist, 5)
        if e and e[0] == addr:
            predicted = e[1] >= 2
            cost = 1 if predicted == taken else miss
        else:
            predicted = target < addr
            cost = (4 if taken else 1) if predicted == taken else miss
        if taken:
            if e and e[0] == addr:
                e[1] = min(3, e[1] + 1)
            else:
                self.btac[idx] = [addr, 2]
        elif e and e[0] == addr:
            e[1] = max(0, e[1] - 1)
        return cost

    def _return(self):
        # the return stack holds BL's return addresses (whether the one popped is the one returned
        # to isn't checked: the code measured returns where it was called from)
        if not self.rstack:
            return 5
        self.rstack = self.rstack[1:]
        return 4


def attach(uc):
    """a timing model fed by every instruction Unicorn executes from now on; detach with uc.hook_del"""
    model = Arm11Timing()
    hook = uc.hook_add(UC_HOOK_CODE, lambda uc, addr, size, _: model.step(uc, addr))
    return model, hook
