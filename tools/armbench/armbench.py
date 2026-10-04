#!/usr/bin/env python3
"""The AX voice renderer as the 3DS runs it, on a desktop: built for the ARM11, run under Unicorn.

    tools/armbench/armbench.py [--lines KIND] [--voices N]
    tools/armbench/armbench.py --cx [FILE...]
    tools/armbench/armbench.py --frame [VOICES...]
    tools/armbench/armbench.py --snd [VOICES...]

Builds tools/armbench/ax_bench.cpp and engine/audio/ax_render.cpp with devkitARM and the 3DS
build's code generation flags, then
  * counts the ARM instructions one voice's frame takes, for voices like the game's;
  * checks the renderer against engine/audio/ax_reference.h on N random voices (default 1000),
    four frames each: the 3DS-only paths (the ARM11's DSP instructions), which NOMA_SELFTEST=ax on
    the host can't reach.
--lines KIND: instead, where voice KIND's instructions go, per source line.
--frame: whole AX frames (tools/armbench/frame_bench.cpp: runtime/hw/dsp_ax.cpp's command list,
SETUP, the voices, OUTPUT) with that many voices like the room's, instructions and cycles.
--snd: nw4r::snd's voice update (tools/armbench/snd_bench.cpp: engine/snd/voice.cpp's
UpdateAllVoices over that many voices, each recalculated every frame as the game makes it).
--cycles: with the instruction counts, an estimate of ARM11 cycles (tools/armbench/arm11_timing.py:
the ARM11 MPCore TRM's issue cycles, result latencies and interlocks, the branch predictor; every
access an L1 hit), with --lines per source line too.
--cx: instead, engine/cx/uncompress.cpp's LZ and Huffman decoders (tools/armbench/cx_bench.cpp):
instructions per output byte, the natives' against the streaming code they started from
(engine/cx/cx_reference.h), and whether their outputs agree; on compressed files given (from your
extracted data, FILE or FILE@OFFSET for data inside a file, such as a font's glyph sheet; at most
512 KB in and 1 MB out), or a random LZ10 stream like the game's.

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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arm11_timing  # noqa: E402

from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_ARM, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_MODE_ARM, Uc
from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEVKITARM = os.environ.get("DEVKITARM", "/opt/devkitpro/devkitARM")
FLAGS = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft", "-D__3DS__", "-fsigned-char",
         "-ffunction-sections", "-fdata-sections", "-O2", "-g", "-DNDEBUG", "-std=gnu++17", "-fno-math-errno",
         "-fno-exceptions", "-fno-rtti"]
INCLUDES = ["-I" + ROOT, "-I" + os.path.join(ROOT, "runtime"), "-I" + os.path.join(ROOT, "build/decomp/include")]
KINDS = ["room voice", "envelope 1.0", "PCM16", "ramps", "muted", "+aux A"]
STACK, RETURN = 0x7000000, 0x7F00000


def build(out, sources=("tools/armbench/ax_bench.cpp", "engine/audio/ax_render.cpp"), entries=("count", "exact")):
    cxx = os.path.join(DEVKITARM, "bin", "arm-none-eabi-g++")
    objs = []
    for src in sources:
        obj = os.path.join(out, os.path.basename(src) + ".o")
        subprocess.check_call([cxx] + FLAGS + INCLUDES + ["-c", os.path.join(ROOT, src), "-o", obj])
        objs.append(obj)
    elf = os.path.join(out, "bench.elf")
    subprocess.check_call([cxx] + FLAGS + ["-nostartfiles", "-Wl,--gc-sections", "-Wl,-e," + entries[0]] +
                          ["-Wl,-u," + e for e in entries[1:]] + ["-Wl,-Ttext=0x10000"] + objs + ["-lc", "-lgcc", "-o", elf])
    return elf


class Machine:
    def __init__(self, elf_path, names=("count", "exact", "g_first_bad")):
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
        self.fn = {name: symtab.get_symbol_by_name(name)[0]["st_value"] for name in names}
        self.uc.mem_map(STACK, 0x100000)
        self.uc.mem_map(RETURN, 0x1000)
        # the VFP on, as on the 3DS (C++ runtime code, such as static locals' guards, uses it)
        self.uc.reg_write(UC_ARM_REG_C1_C0_2, self.uc.reg_read(UC_ARM_REG_C1_C0_2) | 0xF << 20)
        self.uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)

    def call(self, fn, *args):
        self.uc.reg_write(UC_ARM_REG_SP, STACK + 0x100000)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN)
        for reg, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.uc.reg_write(reg, v)
        self.uc.emu_start(self.fn[fn], RETURN)
        return self.uc.reg_read(UC_ARM_REG_R0)

    def instructions(self, fn, *args):
        self.uc.ctl_flush_tb()  # blocks translated before the hook was added wouldn't call it
        n = [0]
        hook = self.uc.hook_add(UC_HOOK_BLOCK, lambda uc, addr, size, _: n.__setitem__(0, n[0] + size // 4))
        self.call(fn, *args)
        self.uc.hook_del(hook)
        return n[0]

    def cycles(self, fn, *args):
        """estimated ARM11 cycles for a call (arm11_timing.py)"""
        self.uc.ctl_flush_tb()
        model, hook = arm11_timing.attach(self.uc)
        self.call(fn, *args)
        self.uc.hook_del(hook)
        return model.cycles

    def lines(self, kind, frames=16, cycles=False):
        def counted():
            # instructions per address, or with `cycles` the cycles each address's instructions took
            hits = collections.Counter()
            if not cycles:
                return hits, self.uc.hook_add(UC_HOOK_CODE, lambda uc, addr, size, _: hits.update((addr,)))
            model = arm11_timing.Arm11Timing()

            def step(uc, addr, size, _):
                t0 = model.cycles
                model.step(uc, addr)
                hits[addr] += model.cycles - t0
            return hits, self.uc.hook_add(UC_HOOK_CODE, step)
        hits, hook = counted()
        self.call("count", kind, frames, 0)
        self.uc.hook_del(hook)
        setup, hook = counted()
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


def lz10_stream(size, seed=99):
    """A random LZ10 stream like the game's data: half the tokens literals, matches of 3-18 bytes."""
    import random
    rnd = random.Random(seed)
    out, s = bytearray(), bytearray([0x10, size & 0xFF, size >> 8 & 0xFF, size >> 16 & 0xFF])
    while len(out) < size:
        flag_at, s = len(s), s + b"\0"
        for bit in range(8):
            left = size - len(out)
            if not left:
                break
            if not out or rnd.random() < 0.5 or left < 3:
                b = rnd.randrange(256) if rnd.random() < 0.8 else out[-1] if out else 0
                s.append(b), out.append(b)
                continue
            length, dist = min(rnd.randint(3, 18), left), rnd.randint(1, min(4096, len(out)))
            s[flag_at] |= 0x80 >> bit
            s += bytes([(length - 3) << 4 | (dist - 1) >> 8, (dist - 1) & 0xFF])
            for _ in range(length):
                out.append(out[-dist])
    return bytes(s)


def bench_cx(files):
    with tempfile.TemporaryDirectory() as out:
        m = Machine(build(out, ("tools/armbench/cx_bench.cpp", "engine/cx/uncompress.cpp"), ("cx",)), ("cx", "cx_in"))
        inputs = []
        for f in files:
            path, _, offset = f.partition("@")
            inputs.append((f, open(path, "rb").read()[int(offset or "0", 0):][:0x80000]))
        inputs = inputs or [("random LZ10 like the game's", lz10_stream(256 << 10))]
        bad = 0
        for name, data in inputs:
            size = int.from_bytes(data[1:4], "little") or int.from_bytes(data[4:8], "little")
            if size > 0x100000:
                print("%s: too big for the bench" % name)
                continue
            m.uc.mem_write(m.fn["cx_in"], data)
            n_ref, n_nat = (m.instructions("cx", native, len(data), 0x2000, 0) for native in (0, 1))
            same = m.call("cx", 0, len(data), 0x2000, 1) == m.call("cx", 1, len(data), 0x2000, 1)
            bad += not same
            kind = "Huff%d" % (data[0] & 0xF) if data[0] >> 4 == 2 else "LZ%d" % (10 + (data[0] & 1))
            print("%-40s %-6s %7d -> %7d bytes: %5.2f ARM instructions a byte (bit by bit: %5.2f), %s" % (
                os.path.basename(name)[-40:], kind, len(data), size, n_nat / size, n_ref / size,
                "same output" if same else "OUTPUT DIFFERS"))
        sys.exit(1 if bad else 0)


def bench_frame(counts):
    """whole AX frames (tools/armbench/frame_bench.cpp: runtime/hw/dsp_ax.cpp's command list with
    voices like the room's): instructions and estimated ARM11 cycles a frame, by voice count"""
    with tempfile.TemporaryDirectory() as out:
        m = Machine(build(out, ("tools/armbench/frame_bench.cpp", "runtime/hw/dsp_ax.cpp", "engine/audio/ax_render.cpp"),
                          ("frame", "frame_init")), ("frame", "frame_init"))
        m.call("frame_init")
        for n in counts:
            ins = (m.instructions("frame", n, 8, 0) - m.instructions("frame", n, 0, 0)) / 8
            cyc = (m.cycles("frame", n, 2, 0) - m.cycles("frame", n, 0, 0)) / 2  # (the cycle model is slow)
            print("%3d voices: %7.0f ARM instructions, %7.0f estimated ARM11 cycles a frame (%.0f us of the 3 ms "
                  "at 268 MHz, %.0f at 804); output hash %08X" % (n, ins, cyc, cyc / 268.0, cyc / 804.0,
                                                                m.call("frame", n, 4, 1)), flush=True)


def bench_snd(counts):
    """nw4r::snd's voice update (tools/armbench/snd_bench.cpp: engine/snd's UpdateAllVoices, every
    voice recalculated as the game's player makes it each frame): per voice, by voice count"""
    with tempfile.TemporaryDirectory() as out:
        m = Machine(build(out, ("tools/armbench/snd_bench.cpp", "engine/snd/voice.cpp", "engine/snd/ax_voice.cpp",
                                "engine/snd/tables.cpp"), ("snd",)), ("snd",))
        for n in counts:
            ins = (m.instructions("snd", n, 8, 0) - m.instructions("snd", n, 0, 0)) / 8
            cyc = (m.cycles("snd", n, 4, 0) - m.cycles("snd", n, 0, 0)) / 4
            print("%3d voices: %7.0f ARM instructions, %7.0f estimated ARM11 cycles a frame (%.0f a voice); "
                  "state hash %08X" % (n, ins, cyc, cyc / n, m.call("snd", n, 6, 1)), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lines", type=int, metavar="KIND")
    ap.add_argument("--voices", type=int, default=1000)
    ap.add_argument("--cx", nargs="*", metavar="FILE")
    ap.add_argument("--cycles", action="store_true")
    ap.add_argument("--frame", type=int, nargs="*", metavar="VOICES")
    ap.add_argument("--snd", type=int, nargs="*", metavar="VOICES")
    args = ap.parse_args()
    if args.cx is not None:
        return bench_cx(args.cx)
    if args.frame is not None:
        return bench_frame(args.frame or [0, 1, 8, 24, 37])
    if args.snd is not None:
        return bench_snd(args.snd or [1, 8, 20])
    with tempfile.TemporaryDirectory() as out:
        m = Machine(build(out))
        if args.lines is not None:
            per = m.lines(args.lines, cycles=args.cycles)
            print("%s: %.0f %s a frame" % (KINDS[args.lines], sum(per.values()),
                                          "estimated ARM11 cycles" if args.cycles else "instructions"))
            for name, n in sorted(per.items(), key=lambda x: -x[1])[:40]:
                line = ""
                if name.startswith("ax_render.cpp:"):
                    num = int(name.split(":")[1].split()[0])
                    line = open(os.path.join(ROOT, "engine/audio/ax_render.cpp")).read().split("\n")[num - 1].strip()
                print("%7.0f  %-28s %s" % (n, name, line[:70]))
            return
        for kind, name in enumerate(KINDS):
            n = (m.instructions("count", kind, 64, 0) - m.instructions("count", kind, 0, 0)) / 64
            if not args.cycles:
                print("%-13s %6.0f ARM instructions a frame" % (name, n))
                continue
            c = (m.cycles("count", kind, 64, 0) - m.cycles("count", kind, 0, 0)) / 64
            print("%-13s %6.0f ARM instructions, %6.0f estimated ARM11 cycles a frame (%.1f us at 268 MHz)" % (
                name, n, c, c / 268.0))
        bad = m.call("exact", args.voices)
        first = ""
        if bad:
            at = int.from_bytes(m.uc.mem_read(m.fn["g_first_bad"], 4), "little") - 1
            first = " (the first: voice %d, frame %d)" % (at // 4, at % 4)
        print("%d random voices, %d frames: %s%s" % (args.voices, 4 * args.voices,
                                                      "MISMATCH in %d frames" % bad if bad else "exact", first))
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
