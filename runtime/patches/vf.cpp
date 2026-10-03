// The VF library (the SDK's FAT volumes kept in NAND files, "VFF"): the channel's caches
// /tmp/skua.vf and /tmp/finch.vf. A volume file is made at its full size before it is formatted:
// a 32-byte header, then zeros. The SDK writes those zeros 512 bytes at a time, so /tmp/skua.vf
// (13.7 MB) took 27,067 NAND writes, each an IOS request and a wait for its reply, at every start
// (IOS empties /tmp whenever a title starts). On the 3DS that was 16 s of the opening, with the
// startup loading that follows held back behind it. The zeros now go in pieces of 16 KB from a
// zeroed buffer on the guest stack: 32 times fewer requests for the same file (its bytes, the NAND
// calls' results and the error handling are the SDK's). Checked on the host: /tmp/skua.vf hashes the
// same after it is made and after it is formatted, and so do /tmp/finch.vf and /tmp/sys/siskin.bin.
#include "noma_decomp.h"
#include "runtime.h"

namespace g {

u32 VFi_CreateNandFile_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7);

namespace {

const u32 NAND_RESULT_ALLOC_FAILED = (u32)-2, NAND_RESULT_BUSY = (u32)-3;
const u32 NAND_KIND_PLAIN = (u32)-10;  // r7: plain NAND (not a table kind, not the private NAND)
const u32 CHUNK = 16u << 10;           // zeros a write
const u32 FRAME = 0x380;               // the SDK's own frame: NANDFileInfo at +0x40, header at +0x100
const u32 STACK_MARGIN = 0x1000;       // for the NAND calls below it, with room to spare

// The SDK's retry around each NAND call: while the answer is BUSY or ALLOC_FAILED, sleep the
// configured milliseconds and try again, at most the configured number of times (0 if never).
template <class F>
u32 nand_retry(F op) {
    u32 r = 0;
    for (s32 tries = GV(s32, GA_VFi_NandRetryCount); tries > 0; tries--) {
        r = op();
        if (r != NAND_RESULT_BUSY && r != NAND_RESULT_ALLOC_FAILED)
            break;
        // OSMillisecondsToTicks: the timebase runs at a quarter of the bus clock
        u32 per_ms = GV(u32, 0x800000F8) / 4000;
        s64 ticks = (s64)(u64)per_ms * (s64)GV(s32, GA_VFi_NandRetryWaitMs);
        OSSleepTicks((u32)((u64)ticks >> 32), (u32)ticks);
    }
    return r;
}

}  // namespace

// r3: the file's size (a multiple of 32), r4: its path, r5: the header's version, r6: access
// flags, r7: the NAND kind. Returns 0 or a NAND result. Plain NAND is the only kind this channel
// uses (VFi_CreateNandFileDefault); other kinds, and a thread without the stack for the buffer,
// take the SDK's code.
u32 VFi_CreateNandFile(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7) {
    u32 thread = guest::load32(0x800000E4);  // __OSCurrentThread
    u32 stack_end = thread ? guest::load32(thread + 0x308) : 0;  // OSThread::stackEnd (lowest)
    if (r7 != NAND_KIND_PLAIN || (r3 & 0x1F) || !stack_end || guest::g_sp <= stack_end ||
        guest::g_sp - stack_end < FRAME + CHUNK + STACK_MARGIN + 64)
        return VFi_CreateNandFile_orig(r3, r4, r5, r6, r7);
    GuestFrame frame(FRAME + CHUNK + 64, 64);
    const u32 info = frame.sp + 0x40, header = frame.sp + 0x100, zeros = frame.sp + FRAME;
    const u32 path = r4;
    u8 perm = 0;  // the access flags as NAND permissions
    if (r6 & 1)
        perm = 0x10;
    if (r6 & 2)
        perm |= 0x20;
    if (r6 & 4)
        perm |= 4;
    if (r6 & 8)
        perm |= 8;
    if (r6 & 0x10)
        perm |= 1;
    if (r6 & 0x20)
        perm |= 2;
    u32 r = nand_retry([&] { return NANDCreate(path, perm, 0); });
    if ((s16)r != 0)
        return (u32)(s16)r;
    r = nand_retry([&] { return NANDOpen(path, info, 2); });
    if (r != 0)
        return (u32)(s16)r;
    // a write failed: close, delete what was made, and answer the write's result
    auto fail = [&](u32 w) {
        nand_retry([&] { return NANDClose(info); });
        nand_retry([&] { return NANDDelete(path); });
        return w;
    };
    VFi_MakeVffHeader(header, r3, r5, 0);
    u32 w = nand_retry([&] { return NANDWrite(info, header, 0x20); });
    if ((s32)w < 0)
        return fail(w);
    VFi_memset(zeros, 0, CHUNK);
    for (u32 left = r3 - 0x20; left;) {
        u32 n = left < CHUNK ? left : CHUNK;
        w = nand_retry([&] { return NANDWrite(info, zeros, n); });
        if ((s32)w < 0)
            return fail(w);
        left -= n;
    }
    nand_retry([&] { return NANDClose(info); });
    return 0;
}

}  // namespace g
