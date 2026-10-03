#!/usr/bin/env python3
"""The AX voice renderer as the 3DS runs it, on a desktop: built for the ARM11, run under Unicorn.

    tools/armbench/armbench.py [--lines KIND] [--voices N]

Builds tools/armbench/ax_bench.cpp and engine/audio/ax_render.cpp with devkitARM and the 3DS
build's code generation flags, then
  * counts the ARM instructions one voice's frame takes, for voices like the game's;
  * checks the renderer against engine/audio/ax_reference.h on N random voices (default 1000),
    four frames each: the 3DS-only paths (the ARM11's DSP instructions), which NOMA_SELFTEST=ax on
    the host can't reach.
--lines KIND: instead, where voice KIND's instructions go, per source line.

Instruction counts are not cycles (multiply and load latencies are not modelled), but they track
the work. Needs devkitARM ($DEVKITARM, default /opt/devkitpro/devkitARM), the decompiled headers
(build/decomp/include) and `pip install unicorn pyelftools`.
"""
import argparse
import collections
import os
import subprocess
import sys
import tempfile

from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_ARM, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_MODE_ARM, Uc
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEVKITARM = os.environ.get("DEVKITARM", "/opt/devkitpro/devkitARM")
FLAGS = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft", "-D__3DS__", "-fsigned-char",
         "-ffunction-sections", "-fdata-sections", "-O2", "-g", "-DNDEBUG", "-std=gnu++17", "-fno-math-errno",
         "-fno-exceptions", "-fno-rtti"]
INCLUDES = ["-I" + ROOT, "-I" + os.path.join(ROOT, "runtime"), "-I" + os.path.join(ROOT, "build/decomp/include")]
KINDS = ["room voice", "envelope 1.0", "PCM16", "ramps", "muted", "+aux A"]
STACK, RETURN = 0x7000000, 0x7F00000


def build(out):
    cxx = os.path.join(DEVKITARM, "bin", "arm-none-eabi-g++")
    objs = []
    for src in ["tools/armbench/ax_bench.cpp", "engine/audio/ax_render.cpp"]:
        obj = os.path.join(out, os.path.basename(src) + ".o")
        subprocess.check_call([cxx] + FLAGS + INCLUDES + ["-c", os.path.join(ROOT, src), "-o", obj])
        objs.append(obj)
    elf = os.path.join(out, "bench.elf")
    subprocess.check_call([cxx] + FLAGS + ["-nostartfiles", "-Wl,--gc-sections", "-Wl,-e,count", "-Wl,-u,exact",
                                           "-Wl,-Ttext=0x10000"] + objs + ["-lc", "-lgcc", "-o", elf])
    return elf


class Machine:
    def __init__(self, elf_path):
        self.elf_path = elf_path
        elf = ELFFile(open(elf_path, "rb"))
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        for seg in elf.iter_segments():
            if seg["p_type"] == "PT_LOAD":
                lo = seg["p_vaddr"] & ~0xFFF
                hi = (seg["p_vaddr"] + seg["p_memsz"] + 0xFFF) & ~0xFFF
                self.uc.mem_map(lo, hi - lo)
                self.uc.mem_write(seg["p_vaddr"], seg.data())
        symtab = elf.get_section_by_name(".symtab")
        self.fn = {name: symtab.get_symbol_by_name(name)[0]["st_value"] for name in ("count", "exact")}
        self.uc.mem_map(STACK, 0x100000)
        self.uc.mem_map(RETURN, 0x1000)

    def call(self, fn, *args):
        self.uc.reg_write(UC_ARM_REG_SP, STACK + 0x100000)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN)
        for reg, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
            self.uc.reg_write(reg, v)
        self.uc.emu_start(self.fn[fn], RETURN)
        return self.uc.reg_read(UC_ARM_REG_R0)

    def instructions(self, fn, *args):
        n = [0]
        hook = self.uc.hook_add(UC_HOOK_BLOCK, lambda uc, addr, size, _: n.__setitem__(0, n[0] + size // 4))
        self.call(fn, *args)
        self.uc.hook_del(hook)
        return n[0]

    def lines(self, kind, frames=16):
        hits = collections.Counter()
        hook = self.uc.hook_add(UC_HOOK_CODE, lambda uc, addr, size, _: hits.update((addr,)))
        self.call("count", kind, frames, 0)
        setup = collections.Counter()
        self.uc.hook_del(hook)
        hook = self.uc.hook_add(UC_HOOK_CODE, lambda uc, addr, size, _: setup.update((addr,)))
        self.call("count", kind, 0, 0)
        self.uc.hook_del(hook)
        hits.subtract(setup)
        addrs = sorted(a for a in hits if hits[a] > 0)
        names = subprocess.run([os.path.join(DEVKITARM, "bin", "arm-none-eabi-addr2line"), "-e", self.elf_path] +
                               ["%x" % a for a in addrs], capture_output=True, text=True).stdout.split("\n")
        per = collections.Counter()
        for a, name in zip(addrs, names):
            per[name.split("/")[-1]] += hits[a]
        return {k: v / frames for k, v in per.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lines", type=int, metavar="KIND")
    ap.add_argument("--voices", type=int, default=1000)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as out:
        m = Machine(build(out))
        if args.lines is not None:
            per = m.lines(args.lines)
            print("%s: %.0f instructions a frame" % (KINDS[args.lines], sum(per.values())))
            for name, n in sorted(per.items(), key=lambda x: -x[1])[:40]:
                line = ""
                if name.startswith("ax_render.cpp:"):
                    num = int(name.split(":")[1].split()[0])
                    line = open(os.path.join(ROOT, "engine/audio/ax_render.cpp")).read().split("\n")[num - 1].strip()
                print("%7.0f  %-28s %s" % (n, name, line[:70]))
            return
        for kind, name in enumerate(KINDS):
            n = (m.instructions("count", kind, 64, 0) - m.instructions("count", kind, 0, 0)) / 64
            print("%-13s %6.0f ARM instructions a frame" % (name, n))
        bad = m.call("exact", args.voices)
        print("%d random voices, %d frames: %s" % (args.voices, 4 * args.voices,
                                                    "MISMATCH in %d frames" % bad if bad else "exact"))
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
