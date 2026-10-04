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


class _Regs:
    """the registers before the instruction, read from Unicorn only when asked for"""
    __slots__ = ("uc", "pc")

    def __init__(self, uc, addr):
        self.uc, self.pc = uc, addr + 8

    def __getitem__(self, r):
        return self.pc if r == 15 else self.uc.reg_read(UC_REGS[r])


class Arm11Timing:
    """Feed it each executed instruction (step); `cycles` is the estimate so far."""

    def __init__(self):
        self.md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
        self.md.detail = True
        self.cache = {}
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

    def decode(self, uc, addr):
        insn = self.cache.get(addr)
        if insn is None:
            insn = next(self.md.disasm(bytes(uc.mem_read(addr, 4)), addr))
            self.cache[addr] = insn
        return insn

    def _need(self, start, r, kind=0, acc=False):
        """the earliest start for an instruction that reads register r (kind 1 early, -1 late)"""
        if r is None or r == 15:
            return start
        ready = self.mac_ready[r] if acc else self.ready[r]
        return max(start, ready + kind)

    def _write(self, r, at, mac=False):
        if r is None or r == 15:
            return
        self.ready[r] = at
        self.mac_ready[r] = at - 1 if mac else at

    def step(self, uc, addr):
        insn = self.decode(uc, addr)
        regs = _Regs(uc, addr)
        iid, ops = insn.id, insn.operands
        passes = insn.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID) or cond_passes(insn.cc, uc.reg_read(UC_ARM_REG_CPSR))
        start = self.t
        if insn.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID) or iid in CARRY_IN:
            start = max(start, self.ready[FLAGS])
        issue, dests, lsu_cycles = 1, [], 0
        sets_flags = insn.update_flags and iid not in CARRY_IN or iid in COMPARE
        regs_of = [REG.get(o.reg) if o.type == A.ARM_OP_REG else None for o in ops]

        if iid in DP or iid in MEDIA1 or iid in MEDIA2 or iid in SAD:
            lat = 2 if iid in MEDIA2 else 3 if iid in SAD else 1
            first_src = 0 if iid in COMPARE else 1
            if iid in _ids("SSAT", "USAT", "SSAT16", "USAT16"):
                first_src = 2  # (rd, #sat, rm)
            srcs = []
            for k, o in enumerate(ops[first_src:], first_src):
                if o.type != A.ARM_OP_REG:
                    continue
                r = REG.get(o.reg)
                last = k == len(ops) - 1
                if o.shift.type in SFT_REG:
                    issue, lat = 2, max(lat, 2)
                    srcs.append((REG.get(o.shift.value), 1))
                    srcs.append((r, 0))
                elif o.shift.type or (last and (iid in MEDIA1_EARLY_M or iid in MEDIA2_EARLY_M or iid in SAD)):
                    srcs.append((r, 1))
                else:
                    srcs.append((r, 0))
            if issue == 2:  # register-controlled shift: the unshifted operand is a Late Reg
                srcs = [(r, -1) if kind == 0 and k == 0 and len(srcs) > 2 else (r, kind) for k, (r, kind) in enumerate(srcs)]
            if iid in SAD:
                srcs = [(r, 1) for r, _ in srcs[:2]] + srcs[2:]
            for r, kind in srcs:
                start = self._need(start, r, kind)
            if iid not in COMPARE:
                dests.append((regs_of[0], lat, False))
            if regs_of[0] == 15 and iid not in COMPARE and passes:
                issue += 6  # a data-processing write to the PC: an unpredicted branch
        elif iid in MUL:
            issue, lats, late = MUL[iid]
            n_dst = 2 if len(lats) == 2 else 1
            srcs = regs_of[n_dst:]
            accumulates = iid in MUL_ACC
            for k, r in enumerate(srcs):
                is_acc = accumulates and k == len(srcs) - 1 and n_dst == 1
                if k < 2:
                    start = self._need(start, r, 1)
                elif is_acc:
                    start = self._need(start, r, -1 if late is not None else 0, acc=True)
                else:
                    start = self._need(start, r, 0)
            if n_dst == 2 and accumulates:  # the 64-bit accumulators are the destinations
                start = self._need(start, regs_of[0], -1, acc=True)
                start = self._need(start, regs_of[1], 0, acc=True)
            for k in range(n_dst):
                dests.append((regs_of[k], lats[k], True))
            if not passes:
                issue = min(issue, max(5 - (start - self.flags_set - 1), 3))
        elif iid in LOAD or iid in STORE or iid in PLD or iid in LDRD or iid in STRD:
            size = LOAD.get(iid) or STORE.get(iid) or 1
            if iid in LDRD or iid in STRD:
                size = 8
            mem = next(o for o in ops if o.type == A.ARM_OP_MEM)
            mem_k = ops.index(mem) if False else [k for k, o in enumerate(ops) if o.type == A.ARM_OP_MEM][0]
            base = REG.get(mem.mem.base)
            post = mem_k + 1 < len(ops)  # an operand after the address: post-indexed
            index = REG.get(mem.mem.index) if mem.mem.index else None
            off_reg = None
            if post:
                o = ops[mem_k + 1]
                if o.type == A.ARM_OP_REG:
                    off_reg = REG.get(o.reg)
                    neg, shift = o.subtracted, o.shift
            elif index is not None:
                off_reg, neg, shift = index, mem.subtracted, mem.shift
            two_issue = off_reg is not None and (neg or (shift.type and not (shift.type == A.ARM_SFT_LSL and shift.value == 2)))
            issue = 2 if two_issue else 1
            # the address
            ea = regs[base] if base is not None else 0
            off = 0
            if off_reg is not None:
                off = regs[off_reg]
                if shift.type == A.ARM_SFT_LSL:
                    off = off << shift.value
                elif shift.type == A.ARM_SFT_LSR:
                    off = off >> shift.value
                elif shift.type == A.ARM_SFT_ASR:
                    off = (off - (1 << 32) if off >> 31 else off) >> shift.value
                off = -off if neg else off
            elif not post:
                off = mem.mem.disp if not mem.subtracted else -mem.mem.disp
            if not post:
                ea = (ea + off) & 0xFFFFFFFF
            align = 8 if size == 8 else size
            unaligned = size in (2, 4) and ea % size != 0
            potentially = size in (2, 4) and ((regs[base] if base is not None else 0) % size or off % size)
            if size == 8:
                lsu_cycles = 1 if ea % 8 == 0 else 2
            else:
                lsu_cycles = 2 if potentially else 1
            start = self._need(start, base, 0 if two_issue else 1)
            start = self._need(start, off_reg, 1)
            start = max(start, self.lsu)
            if iid in STORE or iid in STRD:
                for r in regs_of[:2 if iid in STRD else 1]:
                    start = self._need(start, r, 0)
            lat = (4 if two_issue else 3) + (1 if unaligned else 0)
            if iid in LOAD:
                dests.append((regs_of[0], lat, False))
            elif iid in LDRD:
                dests.append((regs_of[0], lat, False))
                dests.append((regs_of[1], lat + (0 if ea % 8 == 0 else 1), False))
            if insn.writeback or post:
                dests.append((base, 1, False))
            if iid in LOAD and regs_of[0] == 15 and passes:
                issue += 7
        elif iid in LDM or iid in STM:
            if iid in _ids("PUSH", "POP"):
                base, lst = 13, regs_of
            else:
                base, lst = regs_of[0], regs_of[1:]
            sp = regs[base]
            n = len(lst)
            if iid in _ids("PUSH", "STMDB", "LDMDB"):
                first = sp - 4 * n
            elif iid in _ids("STMIB", "LDMIB"):
                first = sp + 4
            elif iid in _ids("STMDA", "LDMDA"):
                first = sp - 4 * n + 4
            else:
                first = sp
            aligned = first % 8 == 0
            lsu_cycles = (n + 1) // 2 if aligned else (n + 2) // 2
            start = self._need(start, base, 1)
            start = max(start, self.lsu)
            if iid in STM:
                for r in lst:
                    start = self._need(start, r, 0)
            else:
                for k, r in enumerate(lst):
                    dests.append((r, 3 + (k // 2 if aligned else (k + 1) // 2), False))
            if insn.writeback or iid in _ids("PUSH", "POP"):
                dests.append((base, 1, False))
            if iid in LDM and 15 in lst and passes:
                ret = (lst[-1] == 15 and base == 13)
                issue += self._return(regs, uc, addr, insn) if ret else 8
        elif iid in B or iid in BL:
            target = ops[0].imm
            issue = self._branch(addr, target, passes, start)
            if iid in BL and passes:
                self.rstack = ([addr + 4] + self.rstack)[:3]
                dests.append((14, 1, False))
        elif iid in BX or iid in BLX:
            r = regs_of[0]
            start = self._need(start, r, 0)
            if r is None:  # BLX #imm
                issue = self._branch(addr, ops[0].imm, passes, start)
            elif not passes:
                issue = 1
            elif iid in BX and r == 14:
                issue = self._return(regs, uc, addr, insn)
            else:
                issue = 6
            if iid in BLX and passes:
                self.rstack = ([addr + 4] + self.rstack)[:3]
                dests.append((14, 1, False))
        else:
            for r in regs_of:
                start = self._need(start, r, 0)
            dests.append((regs_of[0] if regs_of else None, 1, False))

        if passes:
            for r, lat, mac in dests:
                self._write(r, start + lat, mac)
        if sets_flags and passes:
            self.ready[FLAGS] = start + 1
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

    def _return(self, regs, uc, addr, insn):
        # the real return address is what the instruction loads/branches to; the return stack holds
        # BL's return addresses
        if not self.rstack:
            return 5
        self.rstack = self.rstack[1:]
        return 4


def attach(uc):
    """a timing model fed by every instruction Unicorn executes from now on; detach with uc.hook_del"""
    model = Arm11Timing()
    hook = uc.hook_add(UC_HOOK_CODE, lambda uc, addr, size, _: model.step(uc, addr))
    return model, hook
