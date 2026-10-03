"""PowerPC 750CL (Gekko/Broadway) instruction decoder.

decode(word) returns an Insn with the mnemonic and every field the recompiler needs. The
field names follow the PowerPC manuals: rd/rs/ra/rb, frd/fra/frb/frc, simm/uimm, bo/bi, crfd...
Unknown encodings decode to mnemonic "illegal".
"""

__all__ = ["Insn", "decode", "sext16", "sext"]


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v & (m - 1)) - (v & m)


def sext16(v):
    return sext(v & 0xFFFF, 16)


class Insn:
    __slots__ = ("word", "op", "rd", "ra", "rb", "rc_", "simm", "uimm", "rc", "oe", "lk", "aa",
                 "bo", "bi", "target", "crfd", "crfs", "l", "sh", "mb", "me", "spr", "crm",
                 "fm", "w", "i", "d", "xo")

    def __init__(self, word):
        self.word = word
        self.op = "illegal"
        self.rd = (word >> 21) & 31
        self.ra = (word >> 16) & 31
        self.rb = (word >> 11) & 31
        self.rc_ = (word >> 6) & 31      # FRC for A-form
        self.simm = sext16(word)
        self.uimm = word & 0xFFFF
        self.rc = word & 1
        self.oe = (word >> 10) & 1
        self.lk = word & 1
        self.aa = (word >> 1) & 1
        self.bo = self.rd
        self.bi = self.ra
        self.target = None              # branch target (absolute or relative, see aa)
        self.crfd = (word >> 23) & 7
        self.crfs = (word >> 18) & 7
        self.l = (word >> 21) & 1
        self.sh = self.rb
        self.mb = (word >> 6) & 31
        self.me = (word >> 1) & 31
        self.spr = ((word >> 16) & 31) | (((word >> 11) & 31) << 5)
        self.crm = (word >> 12) & 0xFF
        self.fm = (word >> 17) & 0xFF
        self.w = 0
        self.i = 0
        self.d = 0
        self.xo = 0

    def __repr__(self):
        return "<%s %08x>" % (self.op, self.word)


_OP_D = {
    7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.",
    14: "addi", 15: "addis", 20: "rlwimi", 21: "rlwinm", 23: "rlwnm",
    24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis.",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
    40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
    3: "twi",
}

_OP31_X = {
    0: "cmp", 4: "tw", 19: "mfcr", 20: "lwarx", 23: "lwzx", 24: "slw", 26: "cntlzw", 28: "and",
    32: "cmpl", 54: "dcbst", 55: "lwzux", 60: "andc", 83: "mfmsr", 86: "dcbf", 87: "lbzx",
    119: "lbzux", 124: "nor", 144: "mtcrf", 146: "mtmsr", 150: "stwcx.", 151: "stwx",
    183: "stwux", 210: "mtsr", 215: "stbx", 242: "mtsrin", 246: "dcbtst", 247: "stbux",
    278: "dcbt", 279: "lhzx", 284: "eqv", 306: "tlbie", 310: "eciwx", 311: "lhzux", 316: "xor",
    339: "mfspr", 343: "lhax", 371: "mftb", 375: "lhaux", 407: "sthx", 412: "orc", 438: "ecowx",
    439: "sthux", 444: "or", 467: "mtspr", 470: "dcbi", 476: "nand", 512: "mcrxr", 533: "lswx",
    534: "lwbrx", 535: "lfsx", 536: "srw", 566: "tlbsync", 567: "lfsux", 595: "mfsr", 597: "lswi",
    598: "sync", 599: "lfdx", 631: "lfdux", 659: "mfsrin", 661: "stswx", 662: "stwbrx",
    663: "stfsx", 695: "stfsux", 725: "stswi", 727: "stfdx", 759: "stfdux", 790: "lhbrx",
    792: "sraw", 824: "srawi", 854: "eieio", 918: "sthbrx", 922: "extsh", 954: "extsb",
    982: "icbi", 983: "stfiwx", 1014: "dcbz",
}

_OP31_XO = {
    8: "subfc", 10: "addc", 11: "mulhwu", 40: "subf", 75: "mulhw", 104: "neg", 136: "subfe",
    138: "adde", 200: "subfze", 202: "addze", 232: "subfme", 234: "addme", 235: "mullw",
    266: "add", 459: "divwu", 491: "divw",
}

_OP19 = {
    0: "mcrf", 16: "bclr", 33: "crnor", 50: "rfi", 129: "crandc", 150: "isync", 193: "crxor",
    225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr",
}

_OP59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 24: "fres", 25: "fmuls", 28: "fmsubs",
         29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}

_OP63_A = {18: "fdiv", 20: "fsub", 21: "fadd", 23: "fsel", 25: "fmul", 26: "frsqrte",
           28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}

_OP63_X = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1",
           40: "fneg", 64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs",
           264: "fabs", 583: "mffs", 711: "mtfsf"}

_OP4_A = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0",
          15: "ps_madds1", 18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res",
          25: "ps_mul", 26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub",
          31: "ps_nmadd"}

_OP4_X = {0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1", 72: "ps_mr",
          96: "ps_cmpo1", 136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01",
          592: "ps_merge10", 624: "ps_merge11", 1014: "dcbz_l"}

_OP4_PSQX = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}


def decode(word, addr=0):
    ins = Insn(word)
    opcd = word >> 26
    xo10 = (word >> 1) & 0x3FF
    xo5 = (word >> 1) & 31
    ins.xo = xo10
    if opcd in _OP_D:
        ins.op = _OP_D[opcd]
        if opcd in (20, 21, 23):
            pass  # rlwimi/rlwinm/rlwnm: rs=rd field, ra, sh/rb, mb, me, rc
    elif opcd == 18:
        li = sext(word & 0x03FFFFFC, 26)
        ins.op = "b"
        ins.target = (li if ins.aa else addr + li) & 0xFFFFFFFF
    elif opcd == 16:
        bd = sext(word & 0xFFFC, 16)
        ins.op = "bc"
        ins.target = (bd if ins.aa else addr + bd) & 0xFFFFFFFF
    elif opcd == 17:
        ins.op = "sc"
    elif opcd == 19:
        ins.op = _OP19.get(xo10, "illegal")
    elif opcd == 31:
        if xo10 in _OP31_X:
            ins.op = _OP31_X[xo10]
        elif (xo10 & 0x1FF) in _OP31_XO:
            ins.op = _OP31_XO[xo10 & 0x1FF]
    elif opcd == 59:
        ins.op = _OP59.get(xo5, "illegal")
    elif opcd == 63:
        if xo5 in _OP63_A:
            ins.op = _OP63_A[xo5]
        else:
            ins.op = _OP63_X.get(xo10, "illegal")
    elif opcd == 4:
        xo6 = (word >> 1) & 0x3F
        if xo6 in _OP4_PSQX:
            ins.op = _OP4_PSQX[xo6]
            ins.w = (word >> 10) & 1
            ins.i = (word >> 7) & 7
        elif xo5 in _OP4_A:
            ins.op = _OP4_A[xo5]
        else:
            ins.op = _OP4_X.get(xo10, "illegal")
    elif opcd in (56, 57, 60, 61):
        ins.op = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}[opcd]
        ins.w = (word >> 15) & 1
        ins.i = (word >> 12) & 7
        ins.d = sext(word & 0xFFF, 12)
    return ins


# ---------------------------------------------------------------------------------------------
# Instruction classes used by the analyzer

LOADS_INT = {"lwz", "lwzu", "lwzx", "lwzux", "lbz", "lbzu", "lbzx", "lbzux", "lhz", "lhzu",
             "lhzx", "lhzux", "lha", "lhau", "lhax", "lhaux", "lwbrx", "lhbrx", "lwarx", "lmw",
             "lswi", "lswx"}
STORES_INT = {"stw", "stwu", "stwx", "stwux", "stb", "stbu", "stbx", "stbux", "sth", "sthu",
              "sthx", "sthux", "stwbrx", "sthbrx", "stwcx.", "stmw", "stswi", "stswx"}
LOADS_FP = {"lfs", "lfsu", "lfsx", "lfsux", "lfd", "lfdu", "lfdx", "lfdux", "psq_l", "psq_lu",
            "psq_lx", "psq_lux"}
STORES_FP = {"stfs", "stfsu", "stfsx", "stfsux", "stfd", "stfdu", "stfdx", "stfdux", "stfiwx",
             "psq_st", "psq_stu", "psq_stx", "psq_stux"}


def fmt(ins, addr=0):
    """A short disassembly string (for comments and logs)."""
    o = ins.op
    if o in ("b", "bc"):
        s = o + ("l" if ins.lk else "") + ("a" if ins.aa else "")
        if o == "bc":
            return "%s %d,%d,0x%08x" % (s, ins.bo, ins.bi, ins.target)
        return "%s 0x%08x" % (s, ins.target)
    if o in ("bclr", "bcctr"):
        return "%s%s %d,%d" % (o, "l" if ins.lk else "", ins.bo, ins.bi)
    if o in _D_MEM:
        return "%s r%d,%d(r%d)" % (o, ins.rd, ins.simm, ins.ra)
    if o in ("addi", "addis", "addic", "addic.", "mulli", "subfic"):
        return "%s r%d,r%d,%d" % (o, ins.rd, ins.ra, ins.simm)
    if o in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
        return "%s r%d,r%d,0x%x" % (o, ins.ra, ins.rd, ins.uimm)
    if o in ("cmpi", "cmpli"):
        return "%s cr%d,r%d,%d" % (o, ins.crfd, ins.ra, ins.simm if o == "cmpi" else ins.uimm)
    if o in ("rlwinm", "rlwimi", "rlwnm"):
        return "%s%s r%d,r%d,%d,%d,%d" % (o, "." if ins.rc else "", ins.ra, ins.rd, ins.sh, ins.mb, ins.me)
    if o in ("mfspr", "mtspr"):
        return "%s r%d,spr%d" % (o, ins.rd, ins.spr)
    if o.startswith("psq_"):
        return "%s f%d,%d(r%d),%d,qr%d" % (o, ins.rd, ins.d, ins.ra, ins.w, ins.i)
    return "%s%s %d,%d,%d" % (o, "." if ins.rc and o not in ("stwcx.",) else "", ins.rd, ins.ra, ins.rb)


_D_MEM = {"lwz", "lwzu", "lbz", "lbzu", "lhz", "lhzu", "lha", "lhau", "stw", "stwu", "stb", "stbu",
          "sth", "sthu", "lmw", "stmw", "lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu"}
