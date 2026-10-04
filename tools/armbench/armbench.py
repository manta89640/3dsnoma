#!/usr/bin/env python3
"""The AX voice renderer as the 3DS runs it, on a desktop: built for the ARM11, run under Unicorn.

    tools/armbench/armbench.py [--lines KIND] [--voices N]
    tools/armbench/armbench.py --cx [FILE...]
    tools/armbench/armbench.py --frame [VOICES...]
    tools/armbench/armbench.py --snd [VOICES...]
    tools/armbench/armbench.py --snd-diff [REV]

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
--snd-diff: engine/snd's voice update in the working tree against git revision REV's (default
HEAD) on random scenarios: the state after every frame must be the same.
--cycles: with the instruction counts, an estimate of ARM11 cycles (tools/armbench/arm11_timing.py:
the ARM11 MPCore TRM's issue cycles, result latencies and interlocks, the branch predictor; every
access an L1 hit), with --lines per source line too.
--gx: instead, the GPU thread's per-draw work (runtime/hw/gx/tnl.cpp, tools/armbench/gx_bench.cpp): ARM
instructions per draw and per vertex for draws like the room's (layout quads, model draws built
into the list cache, cached draws' palette slots, fixed and skinned), with a hash of what was
built; --gx-funcs KIND: where kind KIND's instructions go, per function.
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
        sp = STACK + 0x100000 - 64
        for i, v in enumerate(args[4:]):  # (arguments past the fourth on the stack, as AAPCS has them)
            self.uc.mem_write(sp + 4 * i, (v & 0xFFFFFFFF).to_bytes(4, "little"))
        self.uc.reg_write(UC_ARM_REG_SP, sp)
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


def snd_diff(rev, seeds=200):
    """engine/snd's voice update (tools/armbench/snd_bench.cpp's random scenarios) as the working tree
    has it against git revision `rev`'s: the same state after every frame, or the seeds that differ"""
    with tempfile.TemporaryDirectory() as out:
        old_root = os.path.join(out, "old")
        os.makedirs(os.path.join(old_root, "engine", "snd"))
        for f in ("voice.cpp", "ax_voice.cpp", "channel.cpp", "tables.cpp", "voice.h", "ax_voice.h", "guest_obj.h",
                  "tables.h"):
            data = subprocess.run(["git", "-C", ROOT, "show", "%s:engine/snd/%s" % (rev, f)], capture_output=True).stdout
            open(os.path.join(old_root, "engine", "snd", f), "wb").write(data)
        builds = []
        for name, root in (("old", old_root), ("new", ROOT)):
            d = os.path.join(out, name + "_build")
            os.makedirs(d)
            srcs = ["tools/armbench/snd_bench.cpp"] + [os.path.relpath(os.path.join(root, "engine", "snd", f), ROOT)
                                                       for f in ("voice.cpp", "ax_voice.cpp", "channel.cpp", "tables.cpp")]
            builds.append(Machine(build(d, srcs, ("snd_random", "chan_random")), ("snd_random", "chan_random")))
        bad, bad_ch = [], []
        for seed in range(1, seeds + 1):
            a, b = (m.call("snd_random", seed, 6 + seed % 10, 12) for m in builds)
            if a != b:
                bad.append(seed)
            a, b = (m.call("chan_random", seed, 6 + seed % 10, 12) for m in builds)
            if a != b:
                bad_ch.append(seed)
        print("engine/snd against %s: %d random scenarios (6-15 voices, 12 frames each): %s; with channels: %s" % (
            rev, seeds, "the same" if not bad else "DIFFERENT in seeds %s" % bad[:10],
            "the same" if not bad_ch else "DIFFERENT in seeds %s" % bad_ch[:10]))
        bad += bad_ch
        sys.exit(1 if bad else 0)


def bench_snd(counts):
    """nw4r::snd's voice update (tools/armbench/snd_bench.cpp: engine/snd's UpdateAllVoices, every
    voice recalculated as the game's player makes it each frame) and channel update (Channel::Update
    for each, with its envelope, LFO and the voice setters): per voice or channel, by count"""
    with tempfile.TemporaryDirectory() as out:
        m = Machine(build(out, ("tools/armbench/snd_bench.cpp", "engine/snd/voice.cpp", "engine/snd/ax_voice.cpp",
                                "engine/snd/channel.cpp", "engine/snd/tables.cpp"), ("snd", "chan")), ("snd", "chan"))
        for n in counts:
            ins = (m.instructions("chan", n, 12, 0) - m.instructions("chan", n, 4, 0)) / 8
            cyc = (m.cycles("chan", n, 12, 0) - m.cycles("chan", n, 4, 0)) / 8
            print("%3d channels: %7.0f ARM instructions, %7.0f estimated ARM11 cycles a frame (%.0f a channel); "
                  "state hash %08X" % (n, ins, cyc, cyc / n, m.call("chan", n, 6, 1)), flush=True)
        for n in counts:  # (frames 4 to 12: the mixes have reached their targets, as in the game)
            ins = (m.instructions("snd", n, 12, 0) - m.instructions("snd", n, 4, 0)) / 8
            cyc = (m.cycles("snd", n, 12, 0) - m.cycles("snd", n, 4, 0)) / 8
            print("%3d voices: %7.0f ARM instructions, %7.0f estimated ARM11 cycles a frame (%.0f a voice); "
                  "state hash %08X" % (n, ins, cyc, cyc / n, m.call("snd", n, 6, 1)), flush=True)


GX_KINDS = [("layout quad, immediate", 4), ("model draw, into the cache", 8), ("cached draw: fixed slots", 8),
            ("cached draw: skinned slots", 8), ("cached draws, frame by frame", 8)]
GX_SOURCES = ("tools/armbench/gx_bench.cpp", "runtime/hw/gx/tnl.cpp", "runtime/hw/gx/vertex.cpp",
              "runtime/hw/gx/pica_vsh.cpp", "runtime/hw/gx/texture.cpp")
# textures like the room's: GX format, width, height (non-powers of two are padded)
GX_TEXTURES = [("I4 glyph sheet", 0, 256, 256), ("IA8 layout part", 3, 120, 60), ("RGB5A3 icon", 5, 96, 96),
               ("RGBA8 picture", 6, 200, 150), ("CMPR model texture", 14, 128, 128), ("C8 (RGB565 palette)", 9, 64, 64)]


def bench_gx(funcs_kind=None, draws=64, flush_every=7):
    with tempfile.TemporaryDirectory() as out:
        elf = build(out, GX_SOURCES, ("gx", "tex"))
        m = Machine(elf, ("gx", "tex"))
        if funcs_kind is not None:
            name, verts = GX_KINDS[funcs_kind]
            syms = sorted((s["st_value"] & ~1, s["st_size"], s.name) for s in
                          ELFFile(open(elf, "rb")).get_section_by_name(".symtab").iter_symbols()
                          if s["st_info"]["type"] == "STT_FUNC" and s["st_size"])
            starts = [a for a, _, _ in syms]
            import bisect

            def per_func(n_draws):
                hits = collections.Counter()
                m.uc.ctl_flush_tb()
                hook = m.uc.hook_add(UC_HOOK_BLOCK, lambda uc, addr, size, _: hits.update({addr: size // 4}))
                m.call("gx", funcs_kind, n_draws, verts, flush_every)
                m.uc.hook_del(hook)
                per = collections.Counter()
                for a, n in hits.items():
                    i = bisect.bisect_right(starts, a) - 1
                    per[syms[i][2] if i >= 0 and a < syms[i][0] + syms[i][1] else "?"] += n
                return per
            per = per_func(draws)
            per.subtract(per_func(0))
            demangle = subprocess.run([os.path.join(DEVKITARM, "bin", "arm-none-eabi-c++filt")], input="\n".join(per),
                                      capture_output=True, text=True).stdout.split("\n")
            names = dict(zip(per, demangle))
            total = sum(per.values()) / draws
            print("%s (%d vertices): %.0f instructions a draw" % (name, verts, total))
            for f, n in sorted(per.items(), key=lambda x: -x[1])[:25]:
                if n > 0:
                    print("%8.1f  %s" % (n / draws, names[f][:110]))
            return
        for kind, (name, verts) in enumerate(GX_KINDS):
            n = (m.instructions("gx", kind, draws, verts, flush_every) - m.instructions("gx", kind, 0, verts, flush_every)) / draws
            print("%-30s %6.0f ARM instructions a draw of %d vertices (%5.0f a vertex)  hash %08x" % (
                name, n, verts, n / verts, m.call("gx", kind, draws, verts, flush_every)))
        m.call("tex", 0, 0, 8, 8, 0)  # (fills the input once)
        for name, fmt, w, h in GX_TEXTURES:
            old, new = (m.instructions("tex", direct, fmt, w, h, 0) for direct in (0, 1))
            same = m.call("tex", 0, fmt, w, h, 1) == m.call("tex", 1, fmt, w, h, 1)
            print("%-22s %4dx%-4d %5.1f -> %5.1f ARM instructions a texel (decode + tile -> direct), %s" % (
                name, w, h, old / (w * h), new / (w * h), "same texture" if same else "TEXTURE DIFFERS"))


# --gxsdk: engine/gx/gx_sdk.cpp's natives against the decompiled functions they replace
GXSDK_FUNCS = ["__GXSetDirtyState", "__GXSetSUTexRegs", "__GXSetVCD", "__GXSetVAT", "__GXCalculateVLim",
               "__GXSetMatrixIndex", "GXSetTevColorS10", "GXSetTevKColor", "GXSetTevSwapModeTable", "GXLoadTexMtxImm",
               "GXLoadTexObjPreLoaded"]
DECOMP_FLAGS = ["-O1", "-w", "-fno-strict-aliasing", "-fwrapv", "-ftrivial-auto-var-init=zero"]


def decompiled_functions(names, natives):
    """The decompiled source of `names` and of what they call that isn't a native (by name), the
    natives' decompiled versions renamed dec_*."""
    import re
    src_dir = os.path.join(ROOT, "build/decomp/src")
    texts = {}
    for fn in sorted(os.listdir(src_dir)):
        if fn.endswith(".cpp"):
            texts[fn] = open(os.path.join(src_dir, fn), errors="replace").read()
    def find(name):
        pat = re.compile(r"^[A-Za-z_][\w<> ]* " + re.escape(name) + r"\(.*?^}\n", re.S | re.M)
        for t in texts.values():
            m = pat.search(t)
            if m:
                return m.group(0)
        return None
    decl = open(os.path.join(ROOT, "build/decomp/include/decl.h")).read()
    known = set(re.findall(r"^[\w<> ]+? (\w+)\(", decl, re.M))
    out, todo, seen = [], list(names), set()
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        body = find(name)
        if body is None:
            raise SystemExit("no decompiled %s" % name)
        for callee in set(re.findall(r"\b(\w+)\(", body)):
            # (a native's own name, renamed below to its decompiled copy, isn't looked for)
            if callee in known and callee != name and callee not in seen and (callee not in natives or callee in names):
                todo.append(callee)
        out.append(body)
    text = "\n".join(out)
    text = re.sub(r"\b(" + "|".join(map(re.escape, natives)) + r")\(", r"dec_\1(", text)
    # (the renamed ones declared first: one may call another defined after it)
    protos = "".join(m.group(1) + ";\n" for m in re.finditer(r"^([A-Za-z_][\w<> ]* dec_\w+\([^)]*\))\s*\{", text, re.M))
    return '#include "noma_decomp.h"\nnamespace g {\n' + protos + text + "}  // namespace g\n", sorted(seen)


def bench_gxsdk(trials=3000):
    cxx = os.path.join(DEVKITARM, "bin", "arm-none-eabi-g++")
    base = [f for f in FLAGS if f != "-O2"]
    with tempfile.TemporaryDirectory() as out:
        dec, pulled = decompiled_functions(GXSDK_FUNCS, GXSDK_FUNCS)
        open(os.path.join(out, "dec.cpp"), "w").write(dec)
        print("decompiled functions in the bench: %s" % ", ".join(pulled))
        objs = []
        for src, opt in ((os.path.join(out, "dec.cpp"), DECOMP_FLAGS), (os.path.join(ROOT, "engine/gx/gx_sdk.cpp"), ["-O2"]),
                         (os.path.join(ROOT, "tools/armbench/gxsdk_bench.cpp"), ["-O2"])):
            obj = os.path.join(out, os.path.basename(src) + ".o")
            subprocess.check_call([cxx] + base + opt + INCLUDES + ["-I" + os.path.join(ROOT, "engine"), "-c", src, "-o", obj])
            objs.append(obj)
        elf = os.path.join(out, "bench.elf")
        subprocess.check_call([cxx] + base + ["-O2", "-nostartfiles", "-Wl,--gc-sections", "-Wl,-e,run", "-Wl,-u,count", "-Wl,-Ttext=0x10000"] +
                              objs + ["-lc", "-lgcc", "-o", elf])
        m = Machine(elf, ("run", "count", "gx_sent", "gx_sent_n", "gx_mem1"))
        # the DOL's sections into MEM1 (the GX library reads its own tables)
        dol = open(os.path.join(ROOT, "orig/HCIJ/sys/main.dol"), "rb").read()
        word = lambda o: int.from_bytes(dol[o:o + 4], "big")
        for k in range(18):
            off, addr, size = word(4 * k), word(0x48 + 4 * k), word(0x90 + 4 * k)
            if size and 0x80000000 <= addr and addr + size <= 0x80900000:
                m.uc.mem_write(m.fn["gx_mem1"] + addr - 0x80000000, dol[off:off + size])
        bad = 0
        for i, name in enumerate(GXSDK_FUNCS):
            same = m.call("run", i, 0, 0, trials) == m.call("run", i, 1, 0, trials)
            n = [(m.instructions("count", i, nat, 256) - m.instructions("count", i, nat, 0)) / 256 for nat in (0, 1)]
            print("%-24s decompiled %6.1f, native %6.1f ARM instructions a call; %d random states: %s" % (
                name, n[0], n[1], trials, "same" if same else "DIFFERENT"))
            if same:
                continue
            bad += 1
            lo, hi = 0, trials  # the first state they differ on
            while hi - lo > 1:
                mid = (lo + hi) // 2
                if m.call("run", i, 0, lo, mid - lo) != m.call("run", i, 1, lo, mid - lo):
                    hi = mid
                else:
                    lo = mid
            outs = []
            for nat in (0, 1):
                m.call("run", i, nat, lo, 1)
                n_sent = int.from_bytes(m.uc.mem_read(m.fn["gx_sent_n"], 4), "little")
                sent = bytes(m.uc.mem_read(m.fn["gx_sent"], n_sent))
                mem = bytes(m.uc.mem_read(m.fn["gx_mem1"] + 0x800000, 0x11000))
                outs.append((sent, mem))
            (s0, m0), (s1, m1) = outs
            print("    state %d: sent %s | %s" % (lo, s0[:64].hex(), s1[:64].hex()) if s0 != s1 else "    state %d: same bytes sent (%d)" % (lo, len(s0)))
            diffs = [k for k in range(len(m0)) if m0[k] != m1[k]][:8]
            for k in diffs:
                print("    memory %08X: decompiled %02X native %02X" % (0x80800000 + k, m0[k], m1[k]))
        sys.exit(1 if bad else 0)


def bench_lyt(trials=4000):
    """engine/lyt/quad.cpp's quads against the decompiled nw4r::lyt::detail::DrawQuad and its alpha
    variant, on random quads: the same FIFO bytes; instructions a quad"""
    cxx = os.path.join(DEVKITARM, "bin", "arm-none-eabi-g++")
    base = [f for f in FLAGS if f != "-O2"]
    # the decompiled ones are kept as *_orig (decompile.py's PATCHED_NAMES); calls between them go to
    # their dec_ copies too
    origs = ["nw4r_lyt_detail_DrawQuad_orig", "nw4r_lyt_detail_DrawQuad_Alpha_orig"]
    with tempfile.TemporaryDirectory() as out:
        dec, pulled = decompiled_functions(origs, origs + ["nw4r_lyt_detail_DrawQuad"])
        dec = dec.replace("dec_nw4r_lyt_detail_DrawQuad(", "dec_nw4r_lyt_detail_DrawQuad_orig(")
        open(os.path.join(out, "dec.cpp"), "w").write(dec)
        print("decompiled functions in the bench: %s" % ", ".join(pulled))
        objs = []
        for src, opt in ((os.path.join(out, "dec.cpp"), DECOMP_FLAGS), (os.path.join(ROOT, "engine/lyt/quad.cpp"), ["-O2"]),
                         (os.path.join(ROOT, "tools/armbench/lyt_bench.cpp"), ["-O2"])):
            obj = os.path.join(out, os.path.basename(src) + ".o")
            subprocess.check_call([cxx] + base + opt + INCLUDES + ["-c", src, "-o", obj])
            objs.append(obj)
        elf = os.path.join(out, "bench.elf")
        subprocess.check_call([cxx] + base + ["-O2", "-nostartfiles", "-Wl,--gc-sections", "-Wl,-e,run", "-Wl,-u,count",
                                              "-Wl,-Ttext=0x10000"] + objs + ["-lc", "-lgcc", "-o", elf])
        m = Machine(elf, ("run", "count", "gx_mem1"))
        dol = open(os.path.join(ROOT, "orig/HCIJ/sys/main.dol"), "rb").read()
        word = lambda o: int.from_bytes(dol[o:o + 4], "big")
        for k in range(18):
            off, addr, size = word(4 * k), word(0x48 + 4 * k), word(0x90 + 4 * k)
            if size and 0x80000000 <= addr and addr + size <= 0x80900000:
                m.uc.mem_write(m.fn["gx_mem1"] + addr - 0x80000000, dol[off:off + size])
        same = m.call("run", 0, 0, trials) == m.call("run", 1, 0, trials)
        print("%d random quads: %s" % (trials, "the same FIFO bytes" if same else "DIFFERENT"))
        for alpha in (0, 1):
            for n_tex in (0, 1, 2):
                n = [(m.instructions("count", nat, alpha, n_tex, 64) - m.instructions("count", nat, alpha, n_tex, 0)) / 64
                     for nat in (0, 1)]
                print("DrawQuad%s, %d texture coordinate sets: decompiled %5.0f, native %5.0f ARM instructions a quad" % (
                    " (alpha)" if alpha else "", n_tex, n[0], n[1]))
        sys.exit(0 if same else 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lines", type=int, metavar="KIND")
    ap.add_argument("--voices", type=int, default=1000)
    ap.add_argument("--cx", nargs="*", metavar="FILE")
    ap.add_argument("--cycles", action="store_true")
    ap.add_argument("--frame", type=int, nargs="*", metavar="VOICES")
    ap.add_argument("--snd", type=int, nargs="*", metavar="VOICES")
    ap.add_argument("--snd-diff", nargs="?", const="HEAD", metavar="REV")
    ap.add_argument("--gx", action="store_true")
    ap.add_argument("--gx-funcs", type=int, metavar="KIND")
    ap.add_argument("--gxsdk", action="store_true")
    ap.add_argument("--lyt", action="store_true")
    args = ap.parse_args()
    if args.gxsdk:
        return bench_gxsdk()
    if args.lyt:
        return bench_lyt()
    if args.gx or args.gx_funcs is not None:
        return bench_gx(args.gx_funcs)
    if args.cx is not None:
        return bench_cx(args.cx)
    if args.frame is not None:
        return bench_frame(args.frame or [0, 1, 8, 24, 37])
    if args.snd is not None:
        return bench_snd(args.snd or [1, 8, 20])
    if args.snd_diff is not None:
        return snd_diff(args.snd_diff)
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
