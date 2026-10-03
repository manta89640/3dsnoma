#!/usr/bin/env python3
"""Decompile main.dol into C++ for the guest memory model (runtime/guest.h).

    tools/decomp/decompile.py [--jobs N] [--only FILTER]

Inputs (made by tools/setup_decomp.sh): config/HCIJ/symbols.txt, dtk's assembly in
build/HCIJ/asm, the inferred prototypes in build/sig/prototypes.txt.
Outputs in build/decomp:
  src/<object>.cpp     decompiled functions, one file per dtk object
  src/stubs.cpp        stand-ins for middleware we don't run (Flash, Mobiclip, ATOK, ...)
  src/functable.cpp    guest code address -> host function, for calls through pointers
  include/decl.h       one prototype per function, in the register ABI
  include/gsyms.h      GA_<symbol> = guest address of every symbol
  report.txt           what was decompiled, what failed and why

Each function has one of these treatments:
  decomp   decompiled with the patched m2c (the default)
  native   written by hand in runtime/ (context switches, exception vectors, ...)
  stub     returns 0 (middleware that the 3DS port replaces or leaves out)
A decompiled function can also be patched (PATCHED_NAMES): it is emitted as <name>_orig, and
runtime/patches/ defines <name>, which changes what it does and calls the original otherwise.
"""
import argparse
import collections
import concurrent.futures
import threading
import hashlib
import contextlib
import io
import multiprocessing
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from ppc.decode import decode  # noqa: E402
from ppc.dol import Dol  # noqa: E402
from ppc.symbols import SymbolTable  # noqa: E402
from decomp.regions import REGIONS, region_of  # noqa: E402

M2C_DIR = os.environ.get("M2C_DIR", os.path.expanduser("~/devkitpro-3ds/decomp-tools/m2c"))
OUT = os.path.join(ROOT, "build", "decomp")

# Functions written by hand in runtime/ (by name). Anything using rfi/sc or saving the whole
# register file is added automatically (see find_native()).
NATIVE_NAMES = {
    "__start", "__init_registers", "__init_data", "__init_hardware", "__flush_cache",
    "__my_flush_cache", "__check_pad3", "__set_debug_bba", "__get_debug_bba",
    "memcpy", "memset", "__fill_mem",
    "OSSaveContext", "OSLoadContext", "OSSaveFPUContext", "__OSSaveFPUContext",
    "__OSLoadFPUContext", "OSSwitchFPUContext", "OSExceptionVector", "__OSDBIntegrator",
    "__OSDBJump", "OSGetStackPointer", "OSSwitchStack", "OSSwitchFiber", "OSSwitchFiberEx",
    "__OSSwitchThread",
    # machine state the host doesn't have: decrementer, FPSCR, locked cache
    "PPCMtdec", "PPCMffpscr", "PPCMtfpscr", "PPCSetFpNonIEEEMode", "__OSFPRInit", "__LCEnable",
    # WiiLink's patch returning a value in r6 (runtime/patches/wiilink.cpp)
    "WiiLink_GetStrapLanguageCode", "wii_Strap_LoadImage",
    # debug output, printed to our log (runtime/hle/report.cpp)
    "OSReport", "printf", "OSPanic",
    # trap instructions (debugger breakpoints)
    "TRKAccessFile", "TRKOpenFile", "TRKCloseFile", "TRKPositionFile",
    # __AI_SRC_INIT: times the audio interface's sample counter against a few microseconds,
    # which only real hardware meets (runtime/natives.cpp)
    "__AI_SRC_INIT",
    # OSInitContext: also tells the thread emulation that the context starts over
    "OSInitContext",
    # cache maintenance, one loop iteration per 32-byte line: memory is coherent here (the sound
    # thread spent half its time invalidating stream buffers line by line on the 3DS)
    "DCInvalidateRange", "DCFlushRangeNoSync", "DCZeroRange", "ICInvalidateRange", "DCStoreRangeNoSync",
    # ---- the engine: SDK functions replaced by native 3DS code (engine/)
    # engine/os: interrupt control and the clock
    "OSDisableInterrupts", "OSEnableInterrupts", "OSRestoreInterrupts", "OSGetTime", "OSGetTick",
    # engine/cw: CodeWarrior's 64-bit integer helpers
    "__div2u", "__div2i", "__mod2u", "__mod2i", "__shl2i",
    # engine/cx: the SDK's streaming decompression (the loaders' hottest code)
    "CXReadUncompLZ", "CXReadUncompHuffman",
    # engine/math: the SDK's MTX library and nw4r::math, native (single precision, no paired singles)
    "PSMTXIdentity", "PSMTXCopy", "PSMTXConcat", "PSMTXConcatArray", "PSMTXInverse", "PSMTXInvXpose",
    "PSMTXRotRad", "PSMTXRotTrig", "__PSMTXRotAxisRadInternal", "PSMTXRotAxisRad", "PSMTXTrans",
    "PSMTXTransApply", "PSMTXScale", "PSMTXScaleApply", "PSMTXQuat", "PSMTXMultVec", "C_MTXLookAt",
    "C_MTXLightFrustum", "C_MTXLightPerspective", "C_MTXLightOrtho", "C_MTXFrustum", "C_MTXPerspective",
    "C_MTXOrtho", "PSVECAdd", "PSVECNormalize", "PSVECMag", "PSVECDotProduct", "PSVECCrossProduct",
    "C_VECHalfAngle", "PSVECSquareDistance", "C_QUATMtx", "C_QUATSlerp",
    "nw4r_math_SinFIdx", "nw4r_math_CosFIdx", "nw4r_math_SinCosFIdx", "nw4r_math_Atan2FIdx",
    "nw4r_math_VEC2Normalize", "nw4r_math_MTX33Copy", "nw4r_math_MTX33Identity", "nw4r_math_MTX34ToMTX33",
    "nw4r_math_MTX33ToMTX34", "nw4r_math_MTX34Zero", "nw4r_math_MTX34Scale", "nw4r_math_MTX34Trans",
    "nw4r_math_MTX34RotAxisFIdx", "nw4r_math_MTX34RotXYZFIdx", "nw4r_math_VEC3TransformNormal",
    "nw4r_math_MTX44Identity", "nw4r_math_MTX44Copy",
    # engine/snd: nw4r::snd's sequencer (SeqPlayer/SeqTrack tick, the MML parser)
    "nw4r_snd_detail_MmlParser_Parse", "nw4r_snd_detail_MmlParser_ReadArg", "nw4r_snd_detail_MmlParser_CommandProc",
    "nw4r_snd_detail_MmlParser_NoteOnCommandProc", "nw4r_snd_detail_MmlSeqTrack_Parse",
    "nw4r_snd_detail_SeqTrack_ParseNextTick", "nw4r_snd_detail_SeqTrack_UpdateChannelLength",
    "nw4r_snd_detail_SeqTrack_UpdateChannelParam", "nw4r_snd_detail_SeqTrack_GetVariablePtr",
    "nw4r_snd_detail_SeqTrack_SetSeqData", "nw4r_snd_detail_SeqTrack_Open",
    "nw4r_snd_detail_SeqPlayer_GetVariablePtr", "nw4r_snd_detail_SeqPlayer_GetPlayerTrack", "nw4r_snd_detail_SeqPlayer_Update",
    "nw4r_snd_detail_SeqPlayer_ParseNextTick", "nw4r_snd_detail_SeqPlayer_UpdateTick", "nw4r_snd_detail_SeqPlayer_SkipTick",
    "nw4r_snd_detail_Channel_SetBiquadFilter", "nw4r_snd_detail_Channel_UpdateSweep",
    "nw4r_snd_detail_BasicPlayer_GetFxSend", "nw4r_snd_detail_BasicPlayer_GetRemoteOutVolume",
    "nw4r_snd_detail_Voice_SetVoiceOutParam",
    # engine/math: MSL's correctly rounded libm entry points (one VFP instruction each on ARM)
    "__ieee754_sqrt", "sqrt", "floor", "ceil",
    # engine/lyt: nw4r::lyt's animation curves and pane matrices
    "nw4r_lyt_GetHermiteCurveValue", "nw4r_lyt_GetStepCurveValue", "nw4r_lyt_AnimTransformBasic_AnimatePane",
    "nw4r_lyt_AnimTransformBasic_AnimateMaterial", "nw4r_lyt_Pane_CalculateMtx",
    # engine/ut: nw4r::ut's fonts (glyph lookup, widths, line feeds) and UTF-16 reading
    "nw4r_ut_ArchiveFontBase_GetGlyphIndex", "nw4r_ut_ArchiveFontBase_FindGlyphIndex",
    "nw4r_ut_ArchiveFontBase_FindGlyphIndexByCode", "nw4r_ut_ArchiveFontBase_GetCharWidthsFromIndex",
    "nw4r_ut_ArchiveFontBase_GetCharWidths", "nw4r_ut_ArchiveFontBase_GetCharWidth", "nw4r_ut_ArchiveFontBase_GetLineFeed",
    "nw4r_ut_TextWriterBase_GetLineHeight", "nw4r_ut_CharStrmReader_ReadNextCharUTF16",
    # engine/lyt: a TextBox's line measuring (the per-character loop)
    "nw4r_lyt_CalcLineRectImpl",
    # engine/g3d: nw4r::g3d's block copies and light settings
    "nw4r_g3d_detail_Copy32ByteBlocks", "nw4r_g3d_LightSetting_Import",
    # engine/system: the game's thread class (its stack-use check)
    "wii_Thread_GetUnusedStackSize",
    # engine/system: the Wii's HOME Button Menu is not part of the 3DS port (home_button.cpp)
    "homebutton_Manager_LoadIcon", "homebutton_Manager_Load", "homebutton_Manager_Open",
    "homebutton_Manager_Update", "homebutton_Manager_Draw",
    # engine/os: mutexes (a free or own mutex without the scheduler)
    "OSLockMutex", "OSUnlockMutex", "OSTryLockMutex",
    # engine/snd: nw4r::snd's voice layer (VoiceManager::UpdateAllVoices and below), native
    "nw4r_snd_detail_VoiceManager_UpdateAllVoices", "nw4r_snd_detail_Voice_StopFinished",
    "nw4r_snd_detail_Voice_Calc", "nw4r_snd_detail_Voice_CalcVe", "nw4r_snd_detail_Voice_CalcMix",
    "nw4r_snd_detail_Voice_CalcMixParam", "nw4r_snd_detail_Voice_CalcDpl2Pan", "nw4r_snd_detail_Voice_ResetDelta",
    "nw4r_snd_detail_Voice_Update", "nw4r_snd_detail_AxVoice_SetSrc", "nw4r_snd_detail_AxVoice_SetVe",
    "nw4r_snd_detail_AxVoice_SetMix", "nw4r_snd_detail_AxVoice_IsMixChanged", "nw4r_snd_detail_AxVoice_SetLpf",
    "nw4r_snd_detail_AxVoice_SetBiquad", "nw4r_snd_detail_AxVoice_SetRemoteFilter", "nw4r_snd_detail_AxVoice_EnableRemote",
    "nw4r_snd_detail_AxVoice_SetRmtMix", "nw4r_snd_detail_AxVoice_Sync", "nw4r_snd_detail_AxVoice_IsPlayFinished",
    "nw4r_snd_detail_AxVoice_ResetDelta", "nw4r_snd_detail_AxVoiceParamBlock_SetVoiceMix",
    "nw4r_snd_detail_AxVoiceParamBlock_SetVoiceRmtMix", "nw4r_snd_detail_AxVoiceParamBlock_SetVoiceRmtIIRCoefs",
    "nw4r_snd_detail_Util_CalcPanRatio", "nw4r_snd_detail_Util_CalcSurroundPanRatio", "nw4r_snd_detail_Util_CalcVolumeRatio",
    "nw4r_snd_detail_Util_CalcLpfFreq", "nw4r_snd_detail_Util_GetRemoteFilterCoefs", "nw4r_snd_detail_AxManager_GetOutputVolume",
    # the SDK's AX voice state and lowpass coefficients (engine/snd/ax_voice.cpp)
    "AXSetVoiceState", "AXGetLpfCoefs",
    # engine/snd: nw4r::snd's channels (ChannelManager::UpdateAllChannel and below)
    "nw4r_snd_detail_ChannelManager_UpdateAllChannel", "nw4r_snd_detail_Channel_Update",
    "nw4r_snd_detail_Util_CalcPitchRatio", "nw4r_snd_detail_EnvGenerator_GetValue", "nw4r_snd_detail_EnvGenerator_Update",
    "nw4r_snd_detail_Lfo_GetValue", "nw4r_snd_detail_Lfo_Update",
    "nw4r_snd_detail_Voice_SetVolume", "nw4r_snd_detail_Voice_SetVeVolume", "nw4r_snd_detail_Voice_SetPitch",
    "nw4r_snd_detail_Voice_SetPanMode", "nw4r_snd_detail_Voice_SetPanCurve", "nw4r_snd_detail_Voice_SetPan",
    "nw4r_snd_detail_Voice_SetSurroundPan", "nw4r_snd_detail_Voice_SetLpfFreq", "nw4r_snd_detail_Voice_SetBiquadFilter",
    "nw4r_snd_detail_Voice_SetRemoteFilter", "nw4r_snd_detail_Voice_SetOutputLine", "nw4r_snd_detail_Voice_SetMainOutVolume",
    "nw4r_snd_detail_Voice_SetMainSend", "nw4r_snd_detail_Voice_SetFxSend", "nw4r_snd_detail_Voice_SetRemoteOutVolume",
}

# Decompiled functions the runtime patches (by name): the decompiled code is emitted as
# <name>_orig and runtime/patches/ defines <name>, so every call, direct or through a pointer,
# reaches the patch. (The patch declares <name>_orig itself; decl.h stays as it is.)
PATCHED_NAMES = {
    # the channel's heaps: allocations that fail are logged (runtime/patches/heap.cpp)
    "lib_allocator_HeapBase_AllocFromCurrent",
    # GXLoadPosMtxImm: NOMA_DEBUG_GX_NAN reports matrices holding NaNs (runtime/patches/debug.cpp)
    "GXLoadPosMtxImm",
    "nw4r_g3d_ScnMdlSimple_GetScnMtxPos",  # nw4r::g3d::ScnMdl: a node's world or view matrix (debug.cpp)
    # offline mode (NOMA_OFFLINE): the channel's online steps (runtime/patches/offline.cpp)
    "comm_friends_FriendInitializer_Run",  # TitleScene's startup task: WiiConnect24 friend data, then online checks
    "comm_httpcache_Manager_AddJob",  # the download manager taking a job
    "comm_httpcache_Request_MakeUrl",  # a job's URL (logged offline: what would have been downloaded)
    "wii_ServerClock_ctor",  # the server clock: known only once a server has answered
    "wii_ServerClock_SetServerTime",  # ... from the configuration document (empty offline)
    "scene_TitleScene_PosterImageLoader_Update",  # TitleScene: preloading the posters' images (wall/*.img)
    "scene_TitleScene_PosterInfoLoader_Update",  # TitleScene: preloading the posters' details (wall/*.met)
    "scene_TitleScene_Opening_OnLogoEnd",  # TitleScene: the opening starts (held offline until it ends)
    "scene_TitleScene_Opening_OnOpeningEnd",  # ... and ends
    "scene_FirstSetupScene_StateWelcome_OnEnd",  # FirstSetupScene: after the welcome, the license agreement
    "room_ContentImage_Prefetch",  # the room's content images: none offline (records without an image)
    # the AX's aux buses: no effects where the 3DS's DSP plays the voices (engine/audio/aux.cpp)
    "AXRegisterAuxACallback", "AXRegisterAuxBCallback", "AXRegisterAuxCCallback",
}

# bump to invalidate the m2c cache when the asm rewriting changes
GEN_VERSION = "7"
# functions per m2c run and per generated source file: parts of big objects run in parallel
CHUNK = 400


def m2c_version_hash():
    h = hashlib.sha1()
    for d, _dirs, files in os.walk(os.path.join(M2C_DIR, "m2c")):
        for f in sorted(files):
            if f.endswith(".py"):
                h.update(open(os.path.join(d, f), "rb").read())
    return h.hexdigest()


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    open(path, "w").write(text)


# ".fn name, scope" ... ".endfn name"
FN_RE = re.compile(r"^\.fn (\S+), (\w+)")


ASM_NAMES = {}  # address -> the name dtk's asm uses (statics may get their address appended)
ASM_DECL_RE = re.compile(r"^# \S+ \| 0x([0-9A-F]{8}) \| size: \S+\n\.(?:fn|obj) ([^,\s]+),", re.M)


def load_asm_names(asm_dir):
    for fn in os.listdir(asm_dir):
        if fn.endswith(".s"):
            for m in ASM_DECL_RE.finditer(open(os.path.join(asm_dir, fn)).read()):
                ASM_NAMES.setdefault(int(m.group(1), 16), m.group(2))
    for a, n in ASM_NAMES.items():
        SYM_ADDR[n] = a


def asm_name(sym):
    """The name dtk's asm uses for a symbol."""
    return ASM_NAMES.get(sym.addr, sym.name) if sym.kind in ("function", "object") else sym.name


def c_ident(name):
    return re.sub(r"[^A-Za-z0-9_]", lambda m: "_x%02X_" % ord(m.group()), name)


# ---------------------------------------------------------------------------------------------
def load_prototypes(path, st=None):
    protos = {}
    for line in open(path):
        m = re.match(r"([0-9A-F]{8}) (\S+) (\S+)\((.*)\);", line.strip())
        if m:
            protos[int(m.group(1), 16)] = (m.group(2), m.group(3), m.group(4))
    ov = os.path.join(ROOT, "config/HCIJ/prototype_overrides.txt")
    if st is not None and os.path.exists(ov):
        for line in open(ov):
            m = re.match(r"^(\S+):\s*(\w+)\s*\((.*)\)\s*$", line.strip())
            if not m or m.group(1) not in st.by_name:
                continue
            params = m.group(3).strip()
            protos[st.by_name[m.group(1)].addr] = (m.group(2), m.group(1), "" if params == "void" else params)
    return protos


FULL_PARAMS = ", ".join(["u32 r%d" % r for r in range(3, 11)] + ["f64 f%d" % f for f in range(1, 9)])


def load_call_hints(path):
    """call address -> "int" / "float" (tools/infer_call_hints.py)"""
    hints = {}
    if not os.path.exists(path):
        print("warning: no %s (run tools/infer_call_hints.py)" % path)
        return hints
    for line in open(path):
        a, kind = line.split()
        hints[int(a, 16)] = kind
    return hints


def split_asm(asm):
    """an object's (rewritten) asm -> header, {function: its block}, [data block, its jump tables]"""
    lines = asm.split("\n")
    header, blocks, data = [], {}, []
    i = 0
    while i < len(lines) and not FN_RE.match(lines[i]) and not lines[i].startswith("# .text:"):
        header.append(lines[i])
        i += 1
    cur, block = None, []
    for k in range(i, len(lines)):
        line = lines[k]
        if line == ".data":
            data = lines[k:]
            break
        block.append(line)
        m = FN_RE.match(line)
        if m:
            cur = m.group(1)
        if cur and line.startswith(".endfn"):
            blocks[cur] = block
            cur, block = None, []
    return header, blocks, data


def part_asm(header, blocks, data, names):
    """asm for some of an object's functions: the header, their blocks, the jump tables they use"""
    out = list(header)
    for n in names:
        out.extend(blocks.get(n, ()))
    body = "\n".join(out)
    used = set(re.findall(r"\b(jumptable_[0-9A-F]{8})\b", body))
    if data and used:
        kept = [data[0]]
        keep = False
        for line in data[1:]:
            m = re.match(r"^\.obj (\S+),", line)
            if m:
                keep = m.group(1) in used
            if keep:
                kept.append(line)
            if line.startswith(".endobj"):
                keep = False
        out.extend(kept)
    return "\n".join(out) + "\n"


def asm_call_hints(asm, call_hints):
    """asm line number (1-based, as m2c counts) -> hint, for the calls through pointers"""
    out = {}
    for n, line in enumerate(asm.splitlines(), 1):
        if line.startswith("/* ") and ("\tbctrl" in line or "\tblrl" in line):
            try:
                a = int(line[3:11], 16)
            except ValueError:
                continue
            if a in call_hints:
                out[n] = call_hints[a]
    return out


def proto_text(name, proto):
    ret, _n, params = proto
    if params.endswith("..."):
        # varargs: the whole argument register file, so the body can build its va_list
        return "%s %s(%s)" % (ret, name, FULL_PARAMS), True
    return "%s %s(%s)" % (ret, name, params or "void"), False


def find_native(dol, st):
    """Functions that only make sense on real hardware: rfi, sc, whole-register-file saves."""
    out = set()
    for f in st.functions:
        if not f.size or f.section not in (".text", ".init"):
            continue
        for a in range(f.addr, f.addr + f.size, 4):
            w = dol.read32(a)
            if w is None:
                continue
            ins = decode(w, a)
            if ins.op in ("rfi", "sc", "mtsr", "mtsrin") or (ins.op == "mtspr" and ins.spr in (26, 27)):
                out.add(f.addr)
                break
            if ins.op in ("stmw", "lmw") and ins.rd <= 13:
                out.add(f.addr)
                break
    return out


def treatments(dol, st, protos):
    native_auto = find_native(dol, st)
    out = {}
    for f in st.functions:
        if not f.size or f.section not in (".text", ".init"):
            continue
        rname, how = region_of(f.addr)
        if f.name in NATIVE_NAMES:
            t = "native"
        elif how == "stub":
            t = "stub"
        elif f.addr in native_auto:
            t = "native"
        elif how is None:
            t = "native"
        else:
            t = "decomp"
        out[f.addr] = (t, rname)
    return out


# ---------------------------------------------------------------------------------------------
CR_BIT_NAMES = {"lt": 0, "gt": 1, "eq": 2, "so": 3, "un": 3}
CR_OPS = ("cror", "crand", "crxor", "crnor", "crnand", "creqv", "crandc", "crorc", "crnot", "crmove")
INVERSE_BRANCH = {"beq": "bne", "bne": "beq", "blt": "bge", "bge": "blt", "bgt": "ble",
                  "ble": "bgt", "bso": "bns", "bns": "bso", "bdnz": "bdz", "bdz": "bdnz"}


def cr_bit_number(tok):
    tok = tok.strip()
    m = re.match(r"^(?:4\*cr([0-7])\+|cr([0-7]))?(lt|gt|eq|so|un)$", tok)
    if m:
        field = int(m.group(1) or m.group(2) or 0)
        return 4 * field + CR_BIT_NAMES[m.group(3)]
    if re.match(r"^\d+$", tok):
        return int(tok)
    raise ValueError("CR bit operand %r" % tok)


ADDR_RE = re.compile(r"^/\* ([0-9A-F]{8}) ")


INSN_RE = re.compile(r"^(/\*[^*]*\*/)\t(\S+)\s*(.*)$")
MEM_RE = re.compile(r"^(-?(?:0x)?[0-9A-Fa-f]+)\((r\d+)\)$")


def _int(t):
    return int(t, 0)


# functions whose frame CodeWarrior aligns (name -> (frame size after the rewrite, alignment)):
# their locals must stay aligned in guest memory too (IOS wants 32-byte aligned buffers)
ALIGNED_FRAMES = {}


def rewrite_aligned_frames(lines):
    """CodeWarrior's 32-byte aligned frames
         clrlwi rA, r1, 27 / subfic rA, rA, -N / mr rB, r1 / stwux r1, r1, rA
       become an ordinary fixed frame of N+32 bytes (alignment is irrelevant in guest memory):
         stwu r1, -FS(r1), with rB-relative accesses rewritten to r1+FS, and the epilogue's
         lwz rC, 0(r1) ... mr r1, rC to addi r1, r1, FS."""
    out = []
    i = 0
    n = len(lines)
    fs = None
    old_sp = set()
    fn_name = None
    while i < n:
        mf = FN_RE.match(lines[i])
        if mf:
            fn_name = mf.group(1)
        m = [INSN_RE.match(lines[i + k]) if i + k < n else None for k in range(4)]
        if all(m) and m[0].group(2) == "clrlwi" and re.search(r",\s*r1,\s*(\d+)$", m[0].group(3)) \
                and m[3].group(2) == "stwux" and {m[1].group(2), m[2].group(2)} == {"subfic", "mr"}:
            # clrlwi rA, r1, N / {subfic rA, rA, -SIZE ; mr rB, r1} / stwux r1, r1, rA
            align = 1 << (32 - int(re.search(r",\s*r1,\s*(\d+)$", m[0].group(3)).group(1)))
            sub = m[1] if m[1].group(2) == "subfic" else m[2]
            mov = m[2] if m[2].group(2) == "mr" else m[1]
            n_bytes = -_int(sub.group(3).split(",")[2].strip())
            rb = mov.group(3).split(",")[0].strip()
            fs = (n_bytes + align + 7) & ~7
            old_sp = {rb}
            if fn_name:
                ALIGNED_FRAMES[fn_name] = (fs, align)
            out.append(m[0].group(1) + "\tstwu r1, -0x%X(r1)" % fs)
            i += 4
            continue
        mi = INSN_RE.match(lines[i])
        if fs is not None and mi:
            head, mn, ops = mi.group(1), mi.group(2), [o.strip() for o in mi.group(3).split(",")] if mi.group(3) else []
            # epilogue: lwz rC, 0x0(r1) loads the old stack pointer
            if mn == "lwz" and len(ops) == 2 and ops[1].replace(" ", "") in ("0x0(r1)", "0(r1)"):
                old_sp.add(ops[0])
                i += 1
                continue
            if mn == "mr" and len(ops) == 2 and ops[1] in old_sp:
                if ops[0] == "r1":
                    out.append(head + "\taddi r1, r1, 0x%X" % fs)
                else:
                    out.append(head + "\taddi %s, r1, 0x%X" % (ops[0], fs))
                i += 1
                continue
            new_ops = []
            for o in ops:
                mm = MEM_RE.match(o.replace(" ", ""))
                if mm and mm.group(2) in old_sp:
                    o = "0x%X(r1)" % (_int(mm.group(1)) + fs) if _int(mm.group(1)) + fs >= 0 else "-0x%X(r1)" % -(_int(mm.group(1)) + fs)
                new_ops.append(o)
            if new_ops != ops:
                out.append(head + "\t" + mn + " " + ", ".join(new_ops))
                i += 1
                continue
            if ops and ops[0] in old_sp and mn not in ("stw", "stb", "sth", "stfs", "stfd", "cmpwi", "cmplwi", "cmpw", "cmplw"):
                old_sp.discard(ops[0])  # register reused for something else
        if lines[i].startswith(".endfn"):
            fs = None
            old_sp = set()
        out.append(lines[i])
        i += 1
    return out


SYM_ADDR = {}  # asm name -> address (filled from ASM_NAMES)
LIS_SYM_RE = re.compile(r"^(/\*[^*]*\*/)\tlis (r\d+), ([^@\s]+)@ha$")
NON_DEST_OPS = {"stw", "stb", "sth", "stwu", "stbu", "sthu", "stwx", "stbx", "sthx", "stfs", "stfd",
                "stfsu", "stfdu", "stmw", "psq_st", "psq_stu", "cmpw", "cmpwi", "cmplw", "cmplwi",
                "b", "bl", "beq", "bne", "blt", "bgt", "ble", "bge", "bctr", "bctrl", "blr", "mtctr",
                "mtlr", "dcbz", "dcbf", "dcbst", "dcbi", "icbi", "mtspr", "nop"}


def split_lis_pairs(lines):
    """dtk names the upper half of an address after the symbol a later addi completes, even when
    the same register serves other offsets first (`lis r4, _f_init@ha; lwz r3, 0x310c(r4)`).
    m2c then takes r4 for the symbol's address. When a `lis rX, SYM@ha` has any use other than
    `SYM@l`, write it (and its `SYM@l` partners) as plain numbers."""
    out = list(lines)
    for i, line in enumerate(lines):
        m = LIS_SYM_RE.match(line)
        if not m:
            continue
        head, reg, sym = m.groups()
        addr = SYM_ADDR.get(sym)
        if addr is None:
            continue
        uses = []
        odd = False
        for j in range(i + 1, min(len(lines), i + 60)):
            mi = INSN_RE.match(lines[j])
            if not mi:
                if lines[j].startswith(".endfn"):
                    break
                continue
            mn, ops = mi.group(2), [o.strip() for o in mi.group(3).split(",")] if mi.group(3) else []
            uses_reg = any(re.search(r"\(%s\)$" % reg, o) or o == reg for o in ops[1:]) or \
                (mn in NON_DEST_OPS and ops and (ops[0] == reg or ops[0].endswith("(%s)" % reg)))
            if uses_reg:
                uses.append(j)
                if ("%s@l" % sym) not in mi.group(3):
                    odd = True
            if ops and ops[0] == reg and mn not in NON_DEST_OPS:
                break  # redefined
        if not odd:
            continue
        hi = ((addr + 0x8000) >> 16) & 0xFFFF
        lo = addr & 0xFFFF
        out[i] = "%s\tlis %s, 0x%X" % (head, reg, hi)
        for j in uses:
            out[j] = out[j].replace("%s@l" % sym, "%d" % (lo - 0x10000 if lo >= 0x8000 else lo))
    return out


def rewrite_asm(text, varargs_fns, jt_labels=(), jt_data=""):
    """Per-file asm fixes before m2c:
      * the varargs prologue's float check always saves f1-f8 (the register ABI passes them);
      * CR bit operands of cror & co. become numbers (m2c's patterns expect numbers);
      * tail calls not at the very end (b fn; dead blr) become bl fn; blr;
      * conditional tail calls get an explicit skip label;
      * bctr without a jump table is an indirect tail call: bctrl; blr;
      * raw bc forms: bc 20 = b, bc 16/24 = bdnz, bc 18/26 = bdz;
      * calls to __ptmf_scall (member pointer in r12) are expanded in place.
    """
    out = []
    cur = None
    jt_pending = False
    n_label = 0
    have_labels = set(re.findall(r"^\.L_([0-9A-F]{8}):", text, re.M))
    lines = text.splitlines()
    if "stwux r1, r1" in text:
        lines = rewrite_aligned_frames(lines)
    lines = split_lis_pairs(lines)
    for line in lines:
        am = ADDR_RE.match(line)
        if am and am.group(1) in jt_labels and am.group(1) not in have_labels:
            out.append(".L_%s:" % am.group(1))
            have_labels.add(am.group(1))
        m = FN_RE.match(line)
        if m:
            cur = m.group(1)
            jt_pending = False
        elif line.startswith(".endfn"):
            cur = None
        if "\t" not in line or not line.startswith("/*") or cur is None:
            out.append(line)
            continue
        head, ins = line.split("\t", 1)
        parts = ins.split(None, 1)
        mn = parts[0]
        ops = [o.strip() for o in parts[1].split(",")] if len(parts) > 1 else []
        if "jumptable_" in ins:
            jt_pending = True
        if cur in varargs_fns and mn == "bne" and ops and ops[0] == "cr1":
            out.append(head + "\tnop")
            continue
        if mn in CR_OPS:
            try:
                nums = [str(cr_bit_number(o)) for o in ops]
            except ValueError:
                out.append(line)
                continue
            if mn == "crnot":
                mn, nums = "crnor", [nums[0], nums[1], nums[1]]
            elif mn == "crmove":
                mn, nums = "cror", [nums[0], nums[1], nums[1]]
            out.append(head + "\t" + mn + " " + ", ".join(nums))
            continue
        if mn == "bc" and len(ops) == 3:
            bo = int(ops[0], 0)
            if bo == 20:
                mn, ops = "b", [ops[2]]
            elif bo in (16, 24):
                mn, ops = "bdnz", [ops[2]]
            elif bo in (18, 26):
                mn, ops = "bdz", [ops[2]]
            line = head + "\t" + mn + " " + ", ".join(ops)
        if mn == "bl" and ops and ops[0] in ("__ptmf_scall", "__ptmf_scall4"):
            # CodeWarrior's pointer-to-member call takes the member pointer in r12, outside the
            # register ABI: do its dispatch here, ending in an ordinary indirect call
            n_label += 1
            skip = ".L_ptmf_%d" % n_label
            for op in ("lwz r0, 0x0(r12)", "lwz r11, 0x4(r12)", "lwz r12, 0x8(r12)",
                       "add r3, r3, r0", "cmpwi r11, 0x0", "blt " + skip,
                       "lwzx r12, r3, r12", "lwzx r12, r12, r11"):
                out.append(head + "\t" + op)
            out.append(skip + ":")
            out.append(head + "\tmtctr r12")
            out.append(head + "\tbctrl")
            continue
        target = ops[-1] if ops else ""
        is_fn_target = target and not target.startswith(".L") and re.match(r"^[A-Za-z_@$][\w@$]*$", target)
        if mn == "b" and is_fn_target:
            out.append(head + "\tbl " + target)
            out.append(head + "\tblr")
            continue
        if mn in INVERSE_BRANCH and is_fn_target:
            n_label += 1
            skip = ".L_tailskip_%d" % n_label
            out.append(head + "\t" + INVERSE_BRANCH[mn] + " " + ", ".join(ops[:-1] + [skip]))
            out.append(head + "\tbl " + target)
            out.append(head + "\tblr")
            out.append(skip + ":")
            continue
        if mn == "bctr":
            if jt_pending:
                jt_pending = False
                out.append(line)
            else:
                out.append(head + "\tbctrl")
                out.append(head + "\tblr")
            continue
        out.append(line)
    if jt_data:
        out.append("")
        out.append(jt_data)
    return "\n".join(out) + "\n"


def load_jumptables(asm_dir, st):
    """jumptable_<addr> -> list of target addresses (dtk writes entries as fn+offset)."""
    tables = {}
    for fn in sorted(os.listdir(asm_dir)):
        if not fn.endswith(".s") or "_text" in fn:
            continue
        cur = None
        for line in open(os.path.join(asm_dir, fn)):
            if line.startswith(".obj jumptable_"):
                cur = line.split()[1].rstrip(",")
                tables[cur] = []
            elif line.startswith(".endobj"):
                cur = None
            elif cur and ".4byte" in line:
                ent = line.split(".4byte", 1)[1].strip()
                m = re.match(r"^(\S+?)(?:\+0x([0-9A-Fa-f]+))?$", ent)
                base = None
                if m:  # dtk's asm name (statics get "_<addr>" appended), else symbols.txt's
                    base = SYM_ADDR.get(m.group(1))
                    if base is None and m.group(1) in st.by_name:
                        base = st.by_name[m.group(1)].addr
                if base is None:
                    tables[cur].append(None)
                else:
                    tables[cur].append(base + int(m.group(2) or "0", 16))
    return tables


TABLE_ADDI_RE = re.compile(r"\taddi r\d+, r\d+, (\"[^\"]+\"|\S+)@l$")


def adopt_jumptables(text, dol, st, jumptables):
    """A switch's table that has a compiler name (CodeWarrior's "@4022") instead of dtk's
    jumptable_<addr> wasn't recognized as one: find the table each `lwzx; mtctr; bctr`
    indexes, read its entries from the DOL and rename it to jumptable_<addr>."""
    lines = text.split("\n")
    renames = {}
    for i, l in enumerate(lines):
        if not l.endswith("\tbctr"):
            continue
        sym = None
        for j in range(i - 1, max(i - 12, -1), -1):
            m = TABLE_ADDI_RE.search(lines[j])
            if m:
                sym = m.group(1)
                break
        if not sym or sym.startswith("jumptable_") or sym in renames:
            continue
        addr = SYM_ADDR.get(sym, SYM_ADDR.get(sym.strip('"')))
        m = ADDR_RE.match(l)
        fn = st.function_at(int(m.group(1), 16)) if m else None
        if addr is None or fn is None:
            continue
        info = st.by_addr.get(addr)
        count = info.size // 4 if info is not None and info.size else 256
        targets = []
        for k in range(count):
            w = dol.read32(addr + 4 * k)
            if w is None or not (fn.addr <= w < fn.addr + fn.size):
                break
            targets.append(w)
        if not targets or (info is not None and info.size and len(targets) != count):
            continue
        name = "jumptable_%08X" % addr
        jumptables[name] = targets
        renames[sym] = name
    for old, new in renames.items():
        text = text.replace(old + "@ha", new + "@ha").replace(old + "@l", new + "@l")
    return text


_worker_ready = False
m2c_lock = threading.Lock()


def _worker_init():
    global _worker_ready
    sys.path.insert(0, M2C_DIR)
    sys.setrecursionlimit(100000)
    _worker_ready = True


def run_m2c(job, extra=()):
    """job: (object name, asm path, [function names], context path) -> (object, text, seconds)"""
    obj, asm_path, fnames, ctx = job[:4]
    from m2c.main import parse_flags, run
    import m2c.translate
    m2c.translate.NOMA_CALL_HINTS = job[5] if len(job) > 5 else {}
    flags = ["-t", "ppc-mwcc-c", "--valid-syntax", "--globals=none", "--context", ctx,
             "--comment-style", "oneline", "--pointer-style", "left", "--no-stack-spill",
             # m2c's switches rebuilt from if-trees can drop cases (NANDSafeCloseAsync lost
             # "case 5"); jump-table switches are unaffected
             "--no-switches"] + list(extra)
    for n in fnames:
        flags += ["-f", n]
    flags.append(asm_path)
    buf = io.StringIO()
    t0 = time.time()
    try:
        with contextlib.redirect_stdout(buf):
            run(parse_flags(flags))
    except Exception as e:  # m2c crashed on the whole file: report every function as failed
        return obj, "/* M2C CRASH %s: %r */\n" % (obj, e), time.time() - t0
    return obj, buf.getvalue(), time.time() - t0


def split_functions(text, wanted):
    """m2c output -> {function name: C++ text or None (failed)}, failure messages."""
    funcs = {}
    errors = {}
    lines = text.splitlines()
    i = 0
    pending_comment = []
    while i < len(lines):
        line = lines[i]
        if line.startswith("/*"):
            block = [line]
            while not lines[i].rstrip().endswith("*/") and i + 1 < len(lines):
                i += 1
                block.append(lines[i])
            body = "\n".join(block)
            m = re.search(r"(?:Decompilation failure|Internal error) in function (\S+?):", body)
            if m:
                detail = [l.strip() for l in body.replace("/*", "").replace("*/", "").splitlines()[1:]
                          if l.strip() and not l.strip().startswith(("Traceback", "File ", "~", "^"))]
                kind = "internal error" if "Internal error" in body else "failure"
                errors[m.group(1)] = kind + ": " + (detail[-1] if kind == "internal error" and detail else
                                                    (detail[0] if detail else "?"))
            else:
                pending_comment.append(body)
            i += 1
            continue
        m = re.match(r"^[^\s/][^(]*?\b(\w+)\((.*)\) \{$", line)
        if m:
            name = m.group(1)
            block = [line]
            i += 1
            while i < len(lines) and lines[i] != "}":
                block.append(lines[i])
                i += 1
            block.append("}")
            funcs[name] = "\n".join(pending_comment + block)
            pending_comment = []
        i += 1
    return funcs, errors


def linker_symbols(dol):
    """Symbols the CodeWarrior linker defines, from the DOL's section layout."""
    secs = {(x.kind, x.index): x for x in dol.sections}
    out = {}

    def sec(name, kind, idx):
        x = secs.get((kind, idx))
        if x:
            out["_f_" + name] = x.addr
            out["_e_" + name] = x.end

    sec("init", "text", 0)
    sec("text", "text", 1)
    sec("extab", "data", 0)
    sec("extabindex", "data", 1)
    sec("ctors", "data", 2)
    sec("dtors", "data", 3)
    sec("rodata", "data", 4)
    sec("data", "data", 5)
    sec("sdata", "data", 6)
    sec("sdata2", "data", 7)
    bss_end = dol.bss_addr + dol.bss_size
    out.update({
        "_f_sbss": dol.bss_addr, "_e_sbss": secs[("data", 7)].addr,
        "_f_sbss2": secs[("data", 7)].end, "_f_bss": dol.bss_addr, "_e_bss": bss_end,
        "_stack_end": bss_end, "_stack_addr": bss_end + 0x10000,
        "_db_stack_end": bss_end + 0x10000, "_db_stack_addr": bss_end + 0x10000,
        "__ArenaLo": bss_end + 0x10000, "__ArenaHi": 0x81700000,
        "_SDA_BASE_": 0x808818A0, "_SDA2_BASE_": 0x8088BDA0,
    })
    return out


# ---------------------------------------------------------------------------------------------
FIX_GA = re.compile(r"GA_([^\s,()]+)")
FIX_GFN = re.compile(r"GFN\(([^)\s]+)\)")


LABEL_RE = re.compile(r"^(\s*)([A-Za-z_]\w*):\s*$", re.M)
LVALUE_INCDEC = re.compile(r"(--|\+\+)\((?:[su](?:8|16|32)|M2C_UNK)\) (\w+)")
LVALUE_CAST_ASSIGN = re.compile(r"\((?:[su](?:8|16|32)|M2C_UNK)\) (GS(?:ARG)?\([^()]*\)|M2C_FIELD\([^;=]*?\)) (=[^=])")


SWITCH_STMT_RE = re.compile(r"^(\s*)switch \((.*)\);(.*)$")
CASE_RE = re.compile(r"^\s*case (-?(?:0x)?[0-9A-Fa-f]+U?):\s*$")
DEFAULT_RE = re.compile(r"^\s*default:\s*$")


def fix_goto_switches(fn_text):
    """m2c's --gotos-only output writes a jump-table switch as `switch (x);` followed by loose
    `case N:` labels. Turn each into labels plus a real `switch` that jumps to them."""
    if not re.search(r"^\s*switch \(.*\);", fn_text, re.M):
        return fn_text
    lines = fn_text.split("\n")
    k = 0
    i = 0
    while i < len(lines):
        m = SWITCH_STMT_RE.match(lines[i])
        if not m:
            i += 1
            continue
        k += 1
        indent, expr = m.group(1), m.group(2)
        cases = []
        default_label = None
        j = i + 1
        while j < len(lines) and not SWITCH_STMT_RE.match(lines[j]):
            mc = CASE_RE.match(lines[j])
            if mc:
                label = "sw%d_case_%s" % (k, mc.group(1).replace("-", "m").rstrip("U"))
                cases.append((mc.group(1), label))
                lines[j] = "%s:;" % label
            elif DEFAULT_RE.match(lines[j]):
                default_label = "sw%d_default" % k
                lines[j] = "%s:;" % default_label
            j += 1
        disp = ["%sswitch (%s) {%s" % (indent, expr, m.group(3))]
        for v, label in cases:
            disp.append("%scase %s: goto %s;" % (indent, v, label))
        if default_label:
            disp.append("%sdefault: goto %s;" % (indent, default_label))
        disp.append("%s}" % indent)
        lines[i:i + 1] = disp
        i += len(disp)
    return "\n".join(lines)


LOOP_HEAD_RE = re.compile(r"^(\s*)(?:do|while \(.*\)|for \(.*\)) \{$")
LABEL_LINE_RE = re.compile(r"^(\s*)([A-Za-z_]\w*):;$")
GOTO_RE = re.compile(r"\bgoto (\w+);")


SWITCH_OPEN_RE = re.compile(r"^\s*switch \(.*\) \{")
SWITCH_NUM_RE = re.compile(r"// switch (\d+)")
SWITCH_LABEL_RE = re.compile(r"^\s*(?:case [^:]+|default):\s*// switch (\d+)")


def misplaced_switch_labels(fn_text):
    """m2c sometimes prints an "irregular" switch's case label inside a nested switch
    (`default: // switch 1` within switch 2). C gives such a label to the inner switch, so
    the outer one silently loses it; those functions are redone with --gotos-only."""
    stack = []  # (switch number, brace depth at its opening)
    depth = 0
    for line in fn_text.split("\n"):
        if SWITCH_OPEN_RE.match(line):
            m = SWITCH_NUM_RE.search(line)
            stack.append((int(m.group(1)) if m else None, depth))
        ml = SWITCH_LABEL_RE.match(line)
        if ml and stack and stack[-1][0] is not None and int(ml.group(1)) != stack[-1][0]:
            return True
        depth += line.count("{") - line.count("}")
        while stack and depth <= stack[-1][1]:
            stack.pop()
    return False


def add_loop_polls(fn_text, name, addr):
    """Give every loop a poll point. Guest code busy-waits on memory that an interrupt handler
    changes, which only works if interrupts can be taken inside the loop (NOMA_LOOP polls every
    few thousand iterations); and C++ may assume a loop without side effects terminates. An
    empty loop (`x:; goto x;`, how the SDK spins forever after a fatal error) becomes
    NOMA_SPIN, which reports it."""
    lines = fn_text.split("\n")
    label_at = {}
    for i, l in enumerate(lines):
        m = LABEL_LINE_RE.match(l)
        if m:
            label_at[m.group(2)] = i
    loop_heads = set()  # labels some later goto jumps back to
    for i, l in enumerate(lines):
        for m in GOTO_RE.finditer(l):
            if label_at.get(m.group(1), i + 1) <= i:
                loop_heads.add(m.group(1))
    out = []
    for i, l in enumerate(lines):
        out.append(l)
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        indent = nxt[:len(nxt) - len(nxt.lstrip())]
        m = LABEL_LINE_RE.match(l)
        if m and m.group(2) in loop_heads:
            if nxt.strip() == "goto %s;" % m.group(2):
                out.append('%sNOMA_SPIN("%s", 0x%08Xu);' % (indent, name, addr))
            else:
                out.append("%sNOMA_LOOP();" % indent)
        elif LOOP_HEAD_RE.match(l):
            out.append("%sNOMA_LOOP();" % indent)
    return "\n".join(out)


def postprocess(fn_text, name_map):
    """Make names C++-safe and point GA_ constants at sanitized names; C++-ify a few m2c
    constructs (a label needs a statement after it, no casts on the left of an assignment)."""
    fn_text = fix_goto_switches(fn_text)
    fn_text = FIX_GA.sub(lambda m: "GA_" + c_ident(m.group(1)), fn_text)
    fn_text = FIX_GFN.sub(lambda m: "GFN(" + c_ident(m.group(1)) + ")", fn_text)
    fn_text = LABEL_RE.sub(lambda m: m.group(0) if m.group(2) == "default" else
                           "%s%s:;" % (m.group(1), m.group(2)), fn_text)
    fn_text = LVALUE_INCDEC.sub(lambda m: m.group(1) + m.group(2), fn_text)
    fn_text = LVALUE_CAST_ASSIGN.sub(lambda m: m.group(1) + " " + m.group(2), fn_text)
    return fn_text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=min(5, os.cpu_count() or 4))
    ap.add_argument("--only", help="regex on object file names (for testing)")
    ap.add_argument("--force", action="store_true", help="ignore the m2c result cache")
    args = ap.parse_args()

    t0 = time.time()
    load_asm_names(os.path.join(ROOT, "build/HCIJ/asm"))
    dol = Dol(os.path.join(ROOT, "orig/HCIJ/sys/main.dol"))
    st = SymbolTable(os.path.join(ROOT, "config/HCIJ/symbols.txt"))
    protos = load_prototypes(os.path.join(ROOT, "build/sig/prototypes.txt"), st)
    call_hints = load_call_hints(os.path.join(ROOT, "build/sig/call_hints.txt"))
    treat = treatments(dol, st, protos)
    by_name = {asm_name(f): f for f in st.functions}
    for sym in st.syms:
        SYM_ADDR.setdefault(sym.name, sym.addr)
    for n, a in linker_symbols(dol).items():
        SYM_ADDR.setdefault(n, a)

    os.makedirs(os.path.join(OUT, "src"), exist_ok=True)
    os.makedirs(os.path.join(OUT, "include"), exist_ok=True)
    os.makedirs(os.path.join(OUT, "asm"), exist_ok=True)

    # ---- prototypes: m2c context and C++ declarations ----
    varargs = set()
    ctx = ["typedef unsigned char u8; typedef signed char s8; typedef unsigned short u16;",
           "typedef signed short s16; typedef unsigned int u32; typedef signed int s32;",
           "typedef unsigned long long u64; typedef signed long long s64; typedef float f32;",
           "typedef double f64;"]
    decl = ["// generated by tools/decomp/decompile.py -- do not edit", "#pragma once",
            '#include "guest.h"', "", "namespace g {"]
    for f in st.functions:
        if f.addr not in treat:
            continue
        p = protos.get(f.addr)
        if p is None:
            continue
        text, is_va = proto_text(asm_name(f), p)
        if is_va:
            varargs.add(asm_name(f))
        ctx.append(text + ";")
        decl.append(proto_text(c_ident(asm_name(f)), p)[0] + ";")
    decl.append("}  // namespace g")
    ctx_path = os.path.join(OUT, "context.h")
    open(ctx_path, "w").write("\n".join(ctx) + "\n")
    # (written only when changed: every decompiled file includes these, through the PCH)
    write_if_changed(os.path.join(OUT, "include", "decl.h"), "\n".join(decl) + "\n")

    # ---- guest addresses of every symbol ----
    gs = ["// generated by tools/decomp/decompile.py -- do not edit", "#pragma once", ""]
    seen = set()
    for s in st.syms:
        n = c_ident(s.name)
        if n in seen:
            continue
        seen.add(n)
        gs.append("constexpr unsigned int GA_%s = 0x%08Xu;" % (n, s.addr))
        # dtk's asm names static (local) symbols <name>_<address>
        ln = "%s_%08X" % (n, s.addr)
        if ln not in seen:
            seen.add(ln)
            gs.append("constexpr unsigned int GA_%s = 0x%08Xu;" % (ln, s.addr))
    for a, n in ASM_NAMES.items():
        n = c_ident(n)
        if n not in seen:
            seen.add(n)
            gs.append("constexpr unsigned int GA_%s = 0x%08Xu;" % (n, a))
    for n, a in linker_symbols(dol).items():
        if n not in seen:
            gs.append("constexpr unsigned int GA_%s = 0x%08Xu;" % (n, a))
    write_if_changed(os.path.join(OUT, "include", "gsyms.h"), "\n".join(gs) + "\n")

    # ---- jobs: one per dtk object with decomp functions ----
    asm_dir = os.path.join(ROOT, "build/HCIJ/asm")
    jumptables = load_jumptables(asm_dir, st)
    print("%d jump tables" % len(jumptables))
    proto_by_name = {}
    for f in st.functions:
        p = protos.get(f.addr)
        if p is not None and f.addr in treat:
            proto_by_name[asm_name(f)] = proto_text(asm_name(f), p)[0] + ";"
    m2c_hash = m2c_version_hash()
    os.makedirs(os.path.join(OUT, "ctx"), exist_ok=True)
    os.makedirs(os.path.join(OUT, "cache"), exist_ok=True)
    ident_re = re.compile(r"\b([A-Za-z_@$][\w@$]*)\b")
    jobs = []
    cached = []
    objects = []     # the dtk objects this run looked at
    unit_obj = {}    # part of an object -> the object
    unit_hints = {}  # part -> its call hints (by line of its asm)
    for fn in sorted(os.listdir(asm_dir)):
        if not fn.endswith(".s"):
            continue
        if args.only and not re.search(args.only, fn):
            continue
        text = open(os.path.join(asm_dir, fn)).read()
        text = adopt_jumptables(text, dol, st, jumptables)
        names = [m.group(1) for m in (FN_RE.match(l) for l in text.splitlines()) if m]
        want = [n for n in names if n in by_name and treat.get(by_name[n].addr, ("", ""))[0] == "decomp"]
        if not want:
            continue
        used = sorted(set(re.findall(r"\b(jumptable_[0-9A-F]{8})\b", text)))
        labels = set()
        data = []
        for jt in used:
            targets = jumptables.get(jt)
            if not targets:
                continue
            data.append(".obj %s, global" % jt)
            for t in targets:
                if t is None:
                    data.append("\t.4byte 0")
                else:
                    labels.add("%08X" % t)
                    data.append("\t.4byte .L_%08X" % t)
            data.append(".endobj %s" % jt)
        jt_data = (".data\n" + "\n".join(data)) if data else ""
        asm = rewrite_asm(text, varargs, labels, jt_data)
        obj = fn[:-2]
        objects.append(obj)
        # a big object goes to m2c, and to the compiler, in parts: they all run in parallel (the
        # largest objects would otherwise be the whole decompilation's and build's tail). Each
        # part's asm has only its functions (and the jump tables they use): m2c parses less.
        header, blocks, data = split_asm(asm)
        parts = [want[i:i + CHUNK] for i in range(0, len(want), CHUNK)]
        for k, part in enumerate(parts):
            unit = obj if len(parts) == 1 else "%s_%d" % (obj, k)
            unit_obj[unit] = obj
            uasm = part_asm(header, blocks, data, part) if len(parts) > 1 else asm
            line_hints = asm_call_hints(uasm, call_hints)
            unit_hints[unit] = line_hints
            # context: only the prototypes this part defines or references
            refs = set(ident_re.findall(uasm)) & proto_by_name.keys()
            ctx_text = "\n".join(ctx[:4] + sorted(proto_by_name[n] for n in refs)) + "\n"
            path = os.path.join(OUT, "asm", unit + ".s")
            ctx_path = os.path.join(OUT, "ctx", unit + ".h")
            write_if_changed(path, uasm)
            write_if_changed(ctx_path, ctx_text)
            key = hashlib.sha1((uasm + ctx_text + m2c_hash + GEN_VERSION + " ".join(part) +
                                repr(sorted(line_hints.items()))).encode()).hexdigest()
            cache_file = os.path.join(OUT, "cache", unit + ".sha1")
            out_file = os.path.join(OUT, "cache", unit + ".m2c")
            if (not args.force and os.path.exists(cache_file) and os.path.exists(out_file)
                    and open(cache_file).read() == key):
                cached.append((unit, part))
                continue
            jobs.append((unit, path, part, ctx_path, key, line_hints))
    print("%d parts of %d objects to decompile (%d functions), %d cached" % (
        len(jobs), len(objects), sum(len(j[2]) for j in jobs), len(cached)))

    # ---- run m2c (results are cached per object as they arrive) ----
    all_objs = [(o, w) for o, w in cached] + [(j[0], j[2]) for j in jobs]
    if jobs:
        with multiprocessing.Pool(args.jobs, initializer=_worker_init, maxtasksperchild=8) as pool:
            done = 0
            for obj, text, secs in pool.imap_unordered(run_m2c, sorted(jobs, key=lambda j: -len(j[2]))):
                key = next(j[4] for j in jobs if j[0] == obj)
                open(os.path.join(OUT, "cache", obj + ".m2c"), "w").write(text)
                open(os.path.join(OUT, "cache", obj + ".sha1"), "w").write(key)
                done += 1
                if done % 10 == 0 or done == len(jobs):
                    print("  %d/%d objects (%.0fs)" % (done, len(jobs), time.time() - t0), flush=True)

    # ---- render every object; compile-check it; retry failing functions with --gotos-only;
    # whatever still doesn't compile becomes a stub that stops with a message when called ----
    fail_list = []
    obj_funcs = {}
    for obj, want in all_objs:
        result_text = open(os.path.join(OUT, "cache", obj + ".m2c")).read()
        funcs, errors = split_functions(result_text, set(want))
        obj_funcs[obj] = (want, funcs, errors)

    def render(obj, want, funcs, errors, bad):
        out = ["// %s: decompiled from main.dol by tools/decomp/decompile.py (m2c)" % obj,
               '#include "noma_decomp.h"', "", "namespace g {", ""]

        def emit(chunk):
            out.extend(chunk.split("\n"))

        ranges = []
        n_ok = 0
        fails = []
        for n in want:
            f = by_name[n]
            body = funcs.get(n)
            reason = bad.get(n)
            if body is None or "M2C_ERROR" in body or reason:
                why = reason or errors.get(n, "M2C_ERROR in output" if body else "no output")
                first = (why.splitlines() or ["?"])[0]
                fails.append((f.addr, n, obj, first))
                emit("// %s: not decompiled yet (%s)" % (n, first[:160]))
                emit("__attribute__((weak)) " + stub_body(c_ident(n), protos.get(f.addr), unimplemented=True))
                emit("")
                if body is not None:
                    emit("#if 0  // m2c output, kept for reference")
                    emit(postprocess(body, None))
                    emit("#endif")
                    emit("")
                continue
            n_ok += 1
            emit("// %08X" % f.addr)
            start_line = len(out) + 1
            body_text = fix_return_type(postprocess(body, None), protos.get(f.addr), n)
            body_text = add_loop_polls(body_text, n, f.addr)
            if n in ALIGNED_FRAMES or asm_name(f) in ALIGNED_FRAMES:
                afs, align = ALIGNED_FRAMES.get(n) or ALIGNED_FRAMES[asm_name(f)]
                body_text = re.sub(r"\bGFRAME\(0x%X\);" % afs, "GFRAME_ALIGN(0x%X, %d);" % (afs, align),
                                   body_text)
            body_lines = body_text.split("\n")
            for k, l in enumerate(body_lines):
                if l.endswith(") {") and not l.startswith(" "):
                    if n in PATCHED_NAMES or f.name in PATCHED_NAMES:
                        cn = c_ident(n)
                        body_lines[k] = re.sub(r"\b%s\(" % re.escape(cn), cn + "_orig(", l, count=1)
                    body_lines.insert(k + 1, '    NOMA_TRACE("%s", 0x%08Xu);' % (n, f.addr))
                    break
            emit("\n".join(body_lines))
            ranges.append((start_line, len(out), n))
            emit("")
        emit("}  // namespace g")
        return "\n".join(out) + "\n", ranges, n_ok, fails

    def check(path):
        r = subprocess.run(["clang++", "-std=c++17", "-fsyntax-only", "-ferror-limit=0", "-w",
                            "-Wno-c++11-narrowing",
                            "-I", os.path.join(ROOT, "runtime"), "-I", os.path.join(OUT, "include"), path],
                           capture_output=True, text=True)
        errs = {}
        for m in re.finditer(r"^%s:(\d+):\d+: error: (.*)$" % re.escape(path), r.stderr, re.M):
            errs.setdefault(int(m.group(1)), m.group(2))
        return errs

    def check_object(unit):
        want, funcs, errors = obj_funcs[unit]
        obj = unit_obj[unit]
        bad = {}
        retried = set()
        path = os.path.join(OUT, "src", unit + ".cpp")
        asm_path = os.path.join(OUT, "asm", unit + ".s")
        ctx_path = os.path.join(OUT, "ctx", unit + ".h")
        hints = unit_hints.get(unit, {})
        suspicious = [n for n in want if n in funcs and misplaced_switch_labels(funcs[n])]
        if suspicious:
            retried.update(suspicious)
            job = (unit, asm_path, suspicious, ctx_path, "", hints)
            with m2c_lock:
                _o, text2, _t = run_m2c(job, extra=["--gotos-only"])
            f2, e2 = split_functions(text2, set(suspicious))
            for n in suspicious:
                if n in f2 and "M2C_ERROR" not in f2[n] and not misplaced_switch_labels(f2[n]):
                    funcs[n] = f2[n]
                else:
                    bad[n] = "switch labels m2c could not place"
        for attempt in range(8):
            text, ranges, n_ok, fails = render(unit, want, funcs, errors, bad)
            write_if_changed(path, text)
            errs = check(path)
            if not errs:
                return n_ok, fails, len(retried)
            new_bad = {}
            for line, msg in errs.items():
                for a, b, n in ranges:
                    if a <= line <= b:
                        new_bad.setdefault(n, "does not compile: " + msg)
            if not new_bad:  # errors outside function bodies: give up on this object
                return n_ok, fails, len(retried)
            # first retry: structure-free control flow (gotos) for the failing functions
            again = [n for n in new_bad if n not in retried]
            if again:
                retried.update(again)
                job = (unit, asm_path, again, ctx_path, "", hints)
                with m2c_lock:
                    _o, text2, _t = run_m2c(job, extra=["--gotos-only"])
                f2, e2 = split_functions(text2, set(again))
                for n in again:
                    if n in f2 and "M2C_ERROR" not in f2[n]:
                        funcs[n] = f2[n]
                    else:
                        bad[n] = new_bad[n]
            else:
                bad.update(new_bad)
        text, ranges, n_ok, fails = render(unit, want, funcs, errors, bad)
        write_if_changed(path, text)
        return n_ok, fails, len(retried)

    ok = 0
    n_retried = 0
    _worker_init()
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for n_ok, fails, nr in ex.map(check_object, [o for o, _ in all_objs]):
            ok += n_ok
            fail_list.extend(fails)
            n_retried += nr
    failed = len(fail_list)
    print("compile check: %d functions retried with --gotos-only" % n_retried)
    # sources of the objects looked at that no part wrote this time (an object split differently),
    # and, in a full run, of objects that no longer exist (dtk named them after a renamed function)
    written = {u + ".cpp" for u, _ in all_objs}
    for name in os.listdir(os.path.join(OUT, "src")):
        if not name.startswith("auto_") or not name.endswith(".cpp") or name in written:
            continue
        m = re.match(r"^(.*?)(?:_(\d+))?\.cpp$", name)
        obj = m.group(1) if m.group(2) and m.group(1) in objects else name[:-4]
        if not args.only or obj in objects:
            os.remove(os.path.join(OUT, "src", name))

    # ---- stubs for middleware ----
    stubs = ['// generated by tools/decomp/decompile.py -- middleware the 3DS port leaves out',
             '#include "noma_decomp.h"', '#include "stubs.h"', "", "namespace g {"]
    n_stub = 0
    for f in st.functions:
        t = treat.get(f.addr)
        if t and t[0] == "stub":
            stubs.append(stub_body(c_ident(asm_name(f)), protos.get(f.addr), unimplemented=False,
                                   region=t[1]))
            n_stub += 1
    stubs.append("}  // namespace g")
    write_if_changed(os.path.join(OUT, "src", "stubs.cpp"), "\n".join(stubs) + "\n")

    # ---- dispatch table: every function that exists on the host side ----
    table = ["// generated by tools/decomp/decompile.py -- guest code address -> host function",
             '#include "noma_decomp.h"', '#include "functable.h"', "",
             "const NomaFn noma_functable[] = {"]
    n_tab = 0
    natives = []
    for f in st.functions:
        t = treat.get(f.addr)
        if not t:
            continue
        if t[0] == "native":
            natives.append(f)
        if f.addr in protos:
            table.append("    {0x%08Xu, (void*)&g::%s}," % (f.addr, c_ident(asm_name(f))))
            n_tab += 1
    table.append("};")
    table.append("const unsigned noma_functable_count = %d;" % n_tab)
    write_if_changed(os.path.join(OUT, "src", "functable.cpp"), "\n".join(table) + "\n")

    weak = ['// generated by tools/decomp/decompile.py -- placeholders for hand-written functions',
            '// (runtime/ overrides them; until then they stop with a message when called)',
            '#include "noma_decomp.h"', "", "namespace g {"]
    for f in natives:
        p = protos.get(f.addr)
        if p is None:
            continue
        weak.append("__attribute__((weak)) " + stub_body(c_ident(asm_name(f)), p, unimplemented=True))
    weak.append("}  // namespace g")
    write_if_changed(os.path.join(OUT, "src", "natives_weak.cpp"), "\n".join(weak) + "\n")
    nat = ["// functions the runtime must provide (treatment: native)"]
    for f in natives:
        p = protos.get(f.addr)
        nat.append("%08X %-30s %s" % (f.addr, f.name, proto_text(f.name, p)[0] if p else "?"))
    open(os.path.join(OUT, "natives.txt"), "w").write("\n".join(nat) + "\n")

    rep = ["decompiled: %d   failed: %d   stubs: %d   natives: %d" % (ok, failed, n_stub, len(natives)), ""]
    why = collections.Counter(re.sub(r"[0-9A-Fa-fx]{6,}|line \d+", "#", w)[:90] for _, _, _, w in fail_list)
    rep.append("failure reasons:")
    for w, c in why.most_common(40):
        rep.append("  %5d  %s" % (c, w))
    rep.append("")
    for a, n, obj, w in sorted(fail_list):
        rep.append("%08X %-30s %-28s %s" % (a, n, obj, w[:100]))
    open(os.path.join(OUT, "report.txt"), "w").write("\n".join(rep) + "\n")
    print("\n".join(rep[:30]))
    print("done in %.0fs" % (time.time() - t0))


def fix_return_type(body, proto, name):
    """m2c says void when no path returns a value; the register ABI says what callers read."""
    if proto is None:
        return body
    want = proto[0]
    lines = body.split("\n")
    for i, l in enumerate(lines):
        m = re.match(r"^(\w+) (%s)\((.*)\) \{$" % re.escape(name), l)
        if m:
            have = m.group(1)
            if have == want:
                return body
            lines[i] = "%s %s(%s) {" % (want, m.group(2), m.group(3))
            text = "\n".join(lines)
            if want != "void":
                text = re.sub(r"\breturn;", "return 0;", text)
                text = text[:text.rstrip().rfind("}")] + "    return 0;\n}"
            else:
                text = re.sub(r"\breturn [^;]+;", "return;", text)
            return text
    return body


def stub_body(name, proto, unimplemented, region=None):
    if proto is None:
        return "// %s: no prototype" % name
    text, _ = proto_text(name, proto)
    ret = proto[0]
    if unimplemented:
        call = 'noma_unimplemented("%s")' % name
    else:
        call = 'noma_stub("%s", "%s")' % (name, region or "")
    if ret == "void":
        return "%s {\n    %s;\n}" % (text, call)
    return "%s {\n    %s;\n    return 0;\n}" % (text, call)


if __name__ == "__main__":
    main()
