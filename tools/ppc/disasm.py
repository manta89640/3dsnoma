"""Readable PowerPC disassembly for decompilation work.

Simplified mnemonics (li, mr, slwi, beq, blr ...), lis/addi pairs and r2/r13 small-data
accesses resolved to addresses, annotated with symbol names and strings.
"""
import re

from .decode import decode, sext16

SPR_NAMES = {1: "xer", 8: "lr", 9: "ctr", 18: "dsisr", 19: "dar", 22: "dec", 25: "sdr1",
             26: "srr0", 27: "srr1", 272: "sprg0", 273: "sprg1", 274: "sprg2", 275: "sprg3",
             282: "ear", 284: "tbl", 285: "tbu", 287: "pvr", 528: "ibat0u", 529: "ibat0l",
             912: "gqr0", 913: "gqr1", 914: "gqr2", 915: "gqr3", 916: "gqr4", 917: "gqr5",
             918: "gqr6", 919: "gqr7", 920: "hid2", 921: "wpar", 922: "dma_u", 923: "dma_l",
             936: "ummcr0", 952: "mmcr0", 953: "pmc1", 954: "pmc2", 1008: "hid0", 1009: "hid1",
             1010: "iabr", 1011: "hid4", 1013: "dabr", 1017: "l2cr", 1019: "ictc", 1020: "thrm1"}

COND = {(12, 0): "lt", (12, 1): "gt", (12, 2): "eq", (12, 3): "so",
        (4, 0): "ge", (4, 1): "le", (4, 2): "ne", (4, 3): "ns"}


def mask_str(mb, me):
    return "%d,%d" % (mb, me)


class Disassembler:
    def __init__(self, dol, symbols=None, sda=None, sda2=None):
        self.dol = dol
        self.symbols = symbols or {}
        self.sda = sda      # r13 value
        self.sda2 = sda2    # r2 value

    # ------------------------------------------------------------------ annotations
    def describe(self, addr):
        if addr in self.symbols:
            return self.symbols[addr]
        s = self.dol.section_at(addr)
        if s is None:
            if self.dol.bss_addr <= addr < self.dol.bss_addr + self.dol.bss_size:
                return "bss_%08x" % addr
            return None
        if s.kind == "text":
            return "fn_%08x" % addr if addr in self.symbols else "code_%08x" % addr
        b = self.dol.cstring(addr, 80)
        if b and len(b) >= 3 and all(32 <= c < 127 or c in (9, 10) for c in b):
            t = b.decode("ascii").replace("\n", "\\n")
            return '"%s"' % (t if len(t) < 60 else t[:57] + "...")
        if b and len(b) >= 4:
            try:
                t = b.decode("shift_jis")
                if all(ord(ch) >= 0x3000 or ch.isprintable() for ch in t):
                    return '"%s"' % t[:40]
            except UnicodeDecodeError:
                pass
        return "%s_%08x" % ("data" if s.kind == "data" else "?", addr)

    def name(self, addr):
        if addr in self.symbols:
            return self.symbols[addr]
        return "fn_%08x" % addr

    # ------------------------------------------------------------------ one instruction
    def insn_text(self, ins, addr, hi=None):
        """hi: dict reg -> upper half from a preceding lis (for pair resolution)."""
        o = ins.op
        rd, ra, rb = ins.rd, ins.ra, ins.rb
        note = None
        if o == "addi":
            if ra == 0:
                return "li r%d, %d" % (rd, ins.simm), None
            if hi and ra in hi:
                v = (hi[ra] + ins.simm) & 0xFFFFFFFF
                note = self.describe(v) or "0x%08x" % v
            elif ra == 13 and self.sda is not None:
                v = (self.sda + ins.simm) & 0xFFFFFFFF
                note = self.describe(v) or "0x%08x" % v
            elif ra == 2 and self.sda2 is not None:
                v = (self.sda2 + ins.simm) & 0xFFFFFFFF
                note = self.describe(v) or "0x%08x" % v
            if ins.simm < 0:
                return "subi r%d, r%d, %d" % (rd, ra, -ins.simm), note
            return "addi r%d, r%d, %d" % (rd, ra, ins.simm), note
        if o == "addis":
            if ra == 0:
                return "lis r%d, 0x%x" % (rd, ins.uimm), None
            return "addis r%d, r%d, 0x%x" % (rd, ra, ins.uimm), None
        if o == "ori":
            if rd == 0 and ra == 0 and ins.uimm == 0:
                return "nop", None
            if hi and rd in hi:
                v = (hi[rd] | ins.uimm) & 0xFFFFFFFF
                note = self.describe(v) or "0x%08x" % v
            return "ori r%d, r%d, 0x%x" % (ra, rd, ins.uimm), note
        if o in ("oris", "xori", "xoris", "andi.", "andis."):
            return "%s r%d, r%d, 0x%x" % (o, ra, rd, ins.uimm), None
        if o == "or" and rd == rb:
            return ("mr%s r%d, r%d" % ("." if ins.rc else "", ra, rd)), None
        if o == "nor" and rd == rb:
            return ("not%s r%d, r%d" % ("." if ins.rc else "", ra, rd)), None
        if o in ("cmpi", "cmpli", "cmp", "cmpl"):
            m = {"cmpi": "cmpwi", "cmpli": "cmplwi", "cmp": "cmpw", "cmpl": "cmplw"}[o]
            cr = "" if ins.crfd == 0 else "cr%d, " % ins.crfd
            if o == "cmpi":
                return "%s %sr%d, %d" % (m, cr, ra, ins.simm), None
            if o == "cmpli":
                return "%s %sr%d, %d" % (m, cr, ra, ins.uimm), None
            return "%s %sr%d, r%d" % (m, cr, ra, rb), None
        if o == "rlwinm":
            sh, mb, me = ins.sh, ins.mb, ins.me
            dot = "." if ins.rc else ""
            if mb == 0 and me == 31 - sh:
                return "slwi%s r%d, r%d, %d" % (dot, ra, rd, sh), None
            if me == 31 and sh == (32 - mb) % 32 and mb != 0:
                return "srwi%s r%d, r%d, %d" % (dot, ra, rd, mb), None
            if sh == 0 and me == 31:
                return "clrlwi%s r%d, r%d, %d" % (dot, ra, rd, mb), None
            if sh == 0 and mb == 0:
                return "clrrwi%s r%d, r%d, %d" % (dot, ra, rd, 31 - me), None
            if me == 31 and mb == 0:
                return "rotlwi%s r%d, r%d, %d" % (dot, ra, rd, sh), None
            return "rlwinm%s r%d, r%d, %d, %d, %d" % (dot, ra, rd, sh, mb, me), None
        if o == "rlwimi":
            return "rlwimi%s r%d, r%d, %d, %d, %d" % ("." if ins.rc else "", ra, rd, ins.sh, ins.mb, ins.me), None
        if o == "rlwnm":
            return "rlwnm%s r%d, r%d, r%d, %d, %d" % ("." if ins.rc else "", ra, rd, rb, ins.mb, ins.me), None
        if o in ("srawi",):
            return "srawi%s r%d, r%d, %d" % ("." if ins.rc else "", ra, rd, ins.sh), None
        if o in ("mfspr", "mtspr"):
            n = SPR_NAMES.get(ins.spr, "spr%d" % ins.spr)
            if o == "mfspr":
                return ("mf%s r%d" % (n, rd)) if n in ("lr", "ctr", "xer") else "mfspr r%d, %s" % (rd, n), None
            return ("mt%s r%d" % (n, rd)) if n in ("lr", "ctr", "xer") else "mtspr %s, r%d" % (n, rd), None
        if o == "mftb":
            return "mftb%s r%d" % ("u" if ins.spr == 269 else "", rd), None
        if o in ("b", "bc"):
            t = ins.target
            tgt = self.name(t) if ins.lk or t in self.symbols else "0x%08x" % t
            if o == "b":
                return ("bl %s" if ins.lk else "b %s") % tgt, None
            return self.bc_text(ins, tgt), None
        if o in ("bclr", "bcctr"):
            base = "lr" if o == "bclr" else "ctr"
            if (ins.bo & 0x14) == 0x14:
                return "b%s%s" % (base, "l" if ins.lk else ""), None
            return self.bc_text(ins, None, base), None
        if o in ("lwz", "lwzu", "lbz", "lbzu", "lhz", "lhzu", "lha", "lhau", "stw", "stwu", "stb",
                 "stbu", "sth", "sthu", "lmw", "stmw", "lfs", "lfsu", "lfd", "lfdu", "stfs",
                 "stfsu", "stfd", "stfdu"):
            reg = ("f%d" if o[1] == "f" or o.startswith("stf") else "r%d") % rd
            if hi and ra in hi:
                v = (hi[ra] + ins.simm) & 0xFFFFFFFF
                note = self.describe(v) or "0x%08x" % v
            elif ra == 13 and self.sda is not None:
                v = (self.sda + ins.simm) & 0xFFFFFFFF
                note = self.describe(v) or "0x%08x" % v
            elif ra == 2 and self.sda2 is not None:
                v = (self.sda2 + ins.simm) & 0xFFFFFFFF
                note = self.describe(v) or "0x%08x" % v
                if o in ("lfs",) and self.dol.read32(v) is not None:
                    import struct
                    f = struct.unpack(">f", struct.pack(">I", self.dol.read32(v)))[0]
                    note += " = %g" % f
                elif o in ("lfd",) and self.dol.read32(v) is not None:
                    import struct
                    b = self.dol.read_bytes(v, 8)
                    if b and len(b) == 8:
                        note += " = %g" % struct.unpack(">d", b)[0]
            return "%s %s, %d(r%d)" % (o, reg, ins.simm, ra), note
        if o.startswith("psq_"):
            if o in ("psq_l", "psq_lu", "psq_st", "psq_stu"):
                return "%s f%d, %d(r%d), %d, qr%d" % (o, rd, ins.d, ra, ins.w, ins.i), None
            return "%s f%d, r%d, r%d, %d, qr%d" % (o, rd, ra, rb, ins.w, ins.i), None
        if o == "subf":
            return "sub%s r%d, r%d, r%d" % ("." if ins.rc else "", rd, rb, ra), None
        if o in ("crxor",) and rd == ra == rb:
            return "crclr %d" % rd, None
        if o in ("creqv",) and rd == ra == rb:
            return "crset %d" % rd, None
        if o in ("cror",) and ra == rb:
            return "crmove %d, %d" % (rd, ra), None
        if o in ("fmr", "fneg", "fabs", "fnabs", "frsp", "fctiw", "fctiwz", "fres", "frsqrte",
                 "ps_mr", "ps_neg", "ps_abs", "ps_nabs", "ps_res", "ps_rsqrte"):
            return "%s%s f%d, f%d" % (o, "." if ins.rc else "", rd, rb), None
        if o in ("fadd", "fsub", "fdiv", "fadds", "fsubs", "fdivs", "ps_add", "ps_sub", "ps_div",
                 "ps_merge00", "ps_merge01", "ps_merge10", "ps_merge11"):
            return "%s f%d, f%d, f%d" % (o, rd, ra, rb), None
        if o in ("fmul", "fmuls", "ps_mul", "ps_muls0", "ps_muls1"):
            return "%s f%d, f%d, f%d" % (o, rd, ra, ins.rc_), None
        if o in ("fmadd", "fmsub", "fnmadd", "fnmsub", "fmadds", "fmsubs", "fnmadds", "fnmsubs",
                 "fsel", "ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub", "ps_madds0", "ps_madds1",
                 "ps_sum0", "ps_sum1", "ps_sel"):
            return "%s f%d, f%d, f%d, f%d" % (o, rd, ra, ins.rc_, rb), None
        if o in ("fcmpu", "fcmpo", "ps_cmpu0", "ps_cmpo0", "ps_cmpu1", "ps_cmpo1"):
            return "%s cr%d, f%d, f%d" % (o, ins.crfd, ra, rb), None
        if o in ("lfsx", "lfsux", "lfdx", "lfdux", "stfsx", "stfsux", "stfdx", "stfdux", "stfiwx"):
            return "%s f%d, r%d, r%d" % (o, rd, ra, rb), None
        if o in ("lwzx", "lwzux", "lbzx", "lbzux", "lhzx", "lhzux", "lhax", "lhaux", "stwx",
                 "stwux", "stbx", "stbux", "sthx", "sthux", "lwbrx", "lhbrx", "stwbrx", "sthbrx",
                 "lwarx", "stwcx."):
            return "%s r%d, r%d, r%d" % (o, rd, ra, rb), None
        if o in ("and", "andc", "or", "orc", "xor", "nor", "nand", "eqv", "slw", "srw", "sraw"):
            return "%s%s r%d, r%d, r%d" % (o, "." if ins.rc else "", ra, rd, rb), None
        if o in ("cntlzw", "extsb", "extsh"):
            return "%s%s r%d, r%d" % (o, "." if ins.rc else "", ra, rd), None
        if o in ("add", "addc", "adde", "subfc", "subfe", "mullw", "mulhw", "mulhwu", "divw", "divwu"):
            return "%s%s%s r%d, r%d, r%d" % (o, "o" if ins.oe else "", "." if ins.rc else "", rd, ra, rb), None
        if o in ("neg", "addze", "addme", "subfze", "subfme"):
            return "%s%s r%d, r%d" % (o, "." if ins.rc else "", rd, ra), None
        if o in ("mulli", "subfic", "addic", "addic."):
            return "%s r%d, r%d, %d" % (o, rd, ra, ins.simm), None
        if o in ("mfcr", "mfmsr"):
            return "%s r%d" % (o, rd), None
        if o in ("mtmsr",):
            return "mtmsr r%d" % rd, None
        if o == "mtcrf":
            return "mtcrf 0x%02x, r%d" % (ins.crm, rd), None
        if o in ("dcbz", "dcbf", "dcbst", "dcbi", "icbi", "dcbt", "dcbtst", "dcbz_l"):
            return "%s r%d, r%d" % (o, ra, rb), None
        if o in ("sync", "isync", "eieio", "rfi", "sc"):
            return o, None
        if o in ("crand", "cror", "crxor", "crnand", "crnor", "creqv", "crandc", "crorc"):
            return "%s %d, %d, %d" % (o, rd, ra, rb), None
        if o == "mcrf":
            return "mcrf cr%d, cr%d" % (ins.crfd, ins.crfs), None
        if o == "illegal":
            return ".word 0x%08x" % ins.word, None
        return "%s 0x%08x" % (o, ins.word), None

    def bc_text(self, ins, tgt, base=""):
        bo, bi = ins.bo, ins.bi
        crf, bit = bi >> 2, bi & 3
        crs = "" if crf == 0 else "cr%d" % crf
        lk = "l" if ins.lk else ""
        if (bo & 0x14) == 0x10 or (bo & 0x14) == 0x12 or bo in (16, 18, 0, 2, 8, 10):
            # ctr forms
            dz = "dz" if (bo & 2) else "dnz"
            extra = ""
            if not (bo & 0x10):
                extra = ("t" if bo & 8 else "f")
            s = "bd%s%s%s%s" % (dz, extra, base, lk)
            args = []
            if extra:
                args.append(str(bi))
            if tgt:
                args.append(tgt)
            return s + (" " + ", ".join(args) if args else "")
        key = (bo & 0x1C if (bo & 0x10) == 0 else bo, bit)
        c = COND.get(((bo & 0xC) if (bo & 0x10) == 0 else 0, bit))
        if c and (bo & 0x14) == 0x04:
            name = "b%s%s%s" % (c, base, lk)
            args = []
            if crs:
                args.append(crs)
            if tgt:
                args.append(tgt)
            return name + (" " + ", ".join(args) if args else "")
        return "bc%s%s %d, %d%s" % (base, lk, bo, bi, (", " + tgt) if tgt else "")

    # ------------------------------------------------------------------ ranges
    def function(self, start, end, labels=()):
        out = []
        hi = {}
        labels = set(labels)
        for a in range(start, end, 4):
            w = self.dol.read32(a)
            ins = decode(w, a)
            if a in labels:
                out.append("  L_%08x:" % a)
            text, note = self.insn_text(ins, a, hi)
            if ins.op == "addis" and ins.ra == 0:
                hi[ins.rd] = (ins.simm << 16) & 0xFFFFFFFF
            elif ins.op in ("addi", "ori") or ins.op.startswith("l"):
                pass
            if ins.op not in ("addis",) and not text.startswith(("st", "cmp", "b", "mt", "f", "ps", "dc", "ic", "sync", "isync", "nop")):
                # destination register clobbered (except the lis pair base we just consumed)
                if ins.op in ("addi", "ori"):
                    dest = ins.rd if ins.op == "addi" else ins.ra
                    src = ins.ra if ins.op == "addi" else ins.rd
                    if dest != src:
                        hi.pop(dest, None)
                    else:
                        hi.pop(dest, None)
                else:
                    hi.pop(ins.rd, None)
            if ins.op in ("b", "bclr", "bcctr") and not ins.lk:
                hi.clear()
            line = "    %08x  %-34s" % (a, text)
            if note:
                line += " # " + note
            out.append(line.rstrip())
        return "\n".join(out)
