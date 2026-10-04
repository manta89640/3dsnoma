# Optimisations

Everything that can make the port faster or smaller, done or not: what it is, where, what it
gains (measured, or marked est.), what it costs, and whether it changes results. Update it as work
lands.

Rules:

- **Game logic stays bit-exact.** A native replacement of decompiled code must give the same
  results, checked by a differential test (`NOMA_SELFTEST=...` in `platform/host/selftest.cpp`).
- **Rendering only has to look right.** The PICA computes in 24-bit floats anyway.
- **Measure.** Azahar's profile (`NOMA_PROFILE=N` in a `build-3ds-prof` build, see the README) shows
  where the time goes on the 3DS. Azahar doesn't model ARM11 timing (azahar PR #2262), so confirm
  gains on hardware, and on the Old 3DS, which has no L2 cache.
- For kernels, count ARM11 instructions: compile with the 3DS flags and run under Unicorn
  ("ARM11 kernel bench" below).
- **Spikes, not only averages.** `NOMA_LOAD_STATS=ms` (env.txt) logs every frame longer than `ms`
  with what happened in it (each guest thread's CPU time, idle time, content reads by archive file
  and how long the guest waited, LZ/Huffman bytes and time, file system jobs, heap allocations,
  texture uploads; in profile builds that frame's own top functions, the GPU thread's included), a
  line per loading burst, and every 10 s the frame-time distribution (p50/p90/p99, worst). Azahar
  models the SD card from hardware measurements (a read costs ~0.52 ms plus ~183 ns a byte, about
  5.5 MB/s; it charges FS opens too) but not SD writes; and a log line costs ~2 ms there
  (svcOutputDebugString), so a slow frame's lines land in the next frame. The decoders' times in
  those lines include the AX mixing done in their poll points: judge kernels by instruction counts.

## Done

| What | Where | Result |
|---|---|---|
| Native AX voice renderer: the Wii DSP's voice pipeline in C++, every voice, no 24-voice limit | `engine/audio/ax_render.cpp` | Replaced the emulated reference. Bit-exact (`NOMA_SELFTEST=ax`). |
| Block renderer: ADPCM decoded a frame at a time straight from RAM; resampler planned in closed form, envelope folded in; muted voices and zero-volume buses only accounted for; PB copied a word at a time | `engine/audio/ax_render.cpp` | Room voice: 14,719 → 4,802 ARM11 instructions per 3 ms frame. Bit-exact on host and ARM. |
| ARM11 DSP instructions in the mixer: SMLAD for the ADPCM predictor, PKHBT for its history, SSAT with shift for clamps, SMLAWB/SMLAWT to mix two samples per load (no clamp needed at volumes ≤ 1.0), REV16 for PB byte swaps | same | Included in the number above. |
| AX voices scheduled for the ARM11's pipeline (ARM DDI 0360F ch. 15: MUL/MLA issue in 2 cycles with 4 to the result, SMULxy/SMLAWy/SMLAD in 1 with 3; loads 3; a shifted operand, a base or a multiplier wants its value a cycle early). ADPCM: runs of whole frames in one loop, a frame read with one LDM, each nibble masked out of the frame's words and scaled by SMLAWT (no MUL), the predictor as SMLATB + SMLABB so one 1-cycle multiply and the SSAT are all that lie between one sample and the next. The voice that is mixed into main L and R only at fixed volumes up to 1.0, without filters (91.7% of the room's voice frames, `NOMA_DEBUG_AX` counts them by path), in one pass: interpolated with SMLAWB plus the fraction's sign (exact), enveloped with SMULWB (no clamp up to 1.0), mixed with SMLAWB, in a software-pipelined loop of ARM assembly (GCC's code stalled on nearly every multiply and load); the PB read and written in place, only the words used | `engine/audio/ax_render.cpp` | Room voice (`tools/armbench/armbench.py`): 4,777 → 3,026 instructions, est. 8,439 → 3,861 ARM11 cycles a frame (`--cycles`; every access an L1 hit; PB words at offsets that aren't 4-byte aligned are read in halves, which costs about what an unaligned access would and doesn't depend on it). Bit-exact: `NOMA_SELFTEST=ax` and the ARM run on 8,000 random voices, half shaped like the game's, after a fixed list of edge voices (envelope ramps onto 0, 1.0 and just past it on runs of ±32767 samples, volumes at and above 1.0, 511/512/513 input samples); every mutation of the new paths is caught but one buffer bound's. |
| The AX frame around the voices: SETUP fills a zero bus with memset and a ramp by addition; OUTPUT with the master volume held (the rule, at 1.0) without the ramp's division per sample (the ARM11 has no divide instruction: each was a library call) and through the buffers' host pointers; the PB list walked in place; no debug peak scan; the AI DMA's latch reads the frame a word at a time | `runtime/hw/dsp_ax.cpp`, `runtime/hw/dsp.cpp` | A whole frame (`armbench.py --frame N`, est. ARM11 cycles): no voices 14,278 → 7,075; 8 voices 82,027 → 37,916; 24 voices 218,564 → 99,320, i.e. 816 → 371 µs of the 3 ms at the Old 3DS's 268 MHz (272 → 124 µs at 804). The output's checksum is the original code's. `NOMA_SELFTEST=ax` runs 600 whole frames against the definitions (ramping buses, every kind of voice list, the master volume held, changed, at 1.0). Measured with the voice renderer and the voice update below, Azahar room (release builds, 160 s each, the same save; emulated timing, so not a hardware number): 30.93 → 34.69 game fps from 40 to 160 s (+12.2%; +15.3% in the stretch with 30-32 voices); profile builds: (ax) 12.6% → 7.6% of core 0's samples, the sound thread 23.9% → 20.5%. |
| nw4r::snd's voice update: the game's player hands every voice its output parameters every frame, which flags pitch, envelope, mix and filter for recalculation (measured on the host in the room: every active voice, every frame). Voice::CalcMixParam's results kept per voice, channel and output with the bits of its 19 inputs (the same bits, the same results); AxVoice::SetMix's 24 PB words and AxVoice::Sync's envelope written in word pairs, at the mix's targets without the step arithmetic | `engine/snd/voice.cpp`, `engine/snd/ax_voice.cpp` | `armbench.py --snd` (voices like the room's): est. 3,094 → 2,047 ARM11 cycles a voice a frame. The same state after every frame as the original on 200 random scenarios (`armbench.py --snd-diff 29eb580`: every output mode, pan curve and mode, one or two channels and outputs, remote speakers, ramps, voices starting, pausing and finishing; whole parameter sets or single fields changing, NaN, infinities, −0); every mutation of the new code is caught but an equivalent one. |
| The Wii Remote, native: KPAD and WPAD answered with the 3DS's controls; the Bluetooth stack (BTE, WUD), the emulated Bluetooth controller and Wii Remote, the remote's speaker stream and rumble are gone | `engine/input/remote.cpp` | No Bluetooth pairing at boot, no 100 Hz report traffic through IPC, no speaker audio encoding. The pointer goes exactly where it's put. |
| 3DS texture memory: compact PICA formats (LA4/LA8/RGB565/RGBA5551) instead of RGBA8, LRU eviction | `platform/3ds/gx_pica.cpp` | Glyphs and textures fit again. |
| The transform unit as a PICA vertex shader (`gx.v.pica`, 138 instructions): the CPU only decodes the stream and follows the indices (model-space position, normal, color, up to three texgen sources, 64-byte vertices); the shader does the position matrix (a palette of 42 matrix rows as uniforms, indexed per vertex), the projection folded with the 3DS viewport transform, GX lighting (spot/none attenuation, diffuse modes, ambient/material from register or vertex, up to 8 lights in a loop) and texgens (2×4/3×4 matrices, post matrices with normalisation, q division); culling by the GPU | `platform/3ds/shaders/gx.v.pica`, `runtime/hw/gx/tnl.cpp`, `platform/3ds/gx_pica.cpp` | Measured, Azahar room: GPU-thread vertex cost 11 → 7 ms per frame before the cache below, 1.5 → 20 fps overall with it. Verified by running the real shader binary in a PICA interpreter (`runtime/hw/gx/pica_vsh.cpp`, `NOMA_CHECK_GX_DRAW=pica NOMA_GX_SHBIN=…`) against the reference transform: 780k room draws, 0 differ; a dump of one frame's draw calls (`NOMA_DUMP_BATCHES=N`) replays on the host with `tools/pica_replay.cpp`. |
| Indexed, batched draws: vertices written once, quads/strips/fans as u16 indices; PICA state set up again only when a GX register changed (dirty bits per part of the state, a cache of TEV→combiner translations); consecutive draws with the same state and uniforms as one | `platform/3ds/gx_pica.cpp` | Room: ~1,240 draws in ~160-215 batches per frame. |
| Frames flush only the vertex/index ranges they used (`C3D_FrameEnd(0)` flushed the whole linear heap, guest memory included); texture upload with row/column tables and a loop per format | same | Not visible in Azahar (no cache model); a per-frame cost on hardware. |
| Texture pixels kept once: the RGBA8 decode is freed after the PICA upload | `runtime/hw/gx/texture.cpp`, `gx_pica.cpp` | Measured: 3DS heap in setup 29.7 → 19.5 MB at the same point. |
| One pass per frame: the game's renderer leaves its Wii anti-aliasing mode, which drew every frame twice as two EFB bands | `runtime/patches/graphics.cpp` | est. up to 2× on the GPU thread in 3D scenes; measuring. |
| Display-list geometry cache: a list's draws keep their shader vertices in a 64K-vertex pool in GPU memory from frame to frame, found again by list address/size and draw offset, valid while the list's bytes and the vertex arrays it indexes (write generations), the CP registers and what the shader wants of a vertex stay the same; only the palette slot bytes (a parallel buffer) are rewritten, and only when the matrices got other slots | `platform/3ds/gx_pica.cpp`, `runtime/hw/gx/tnl.cpp` | Measured, Azahar room: ~960 of ~1,130 draws per frame from the cache; per-frame vertex work 11 → ~1 ms for list draws. `NOMA_GX_TIMERS=N` logs the GPU thread's milliseconds per step per frame. |
| Draw calls and uniform uploads of our own: the renderer writes the PICA's draw commands directly and uploads only the uniform vectors that changed, in runs; citro3d's context update (every flag, both shaders' 96 dirty uniform slots) runs only after its own state changed. Constants go by group (the lights most of them, changing least); the command decoder marks what a BP write changed (g_gx.bp_dirty) instead of calling the renderer per register and decodes runs of commands per call; the layouts' quads take a path reading only their fields; palette entries are found by (kind, row) and the slot a draw had last time | `platform/3ds/gx_pica.cpp`, `runtime/hw/gx/cp.cpp`, `runtime/hw/gx/tnl.cpp` | Measured, Azahar room (release build): 28.5-29 → 29-33 game fps; GPU thread idle 13% → 36-40%, so core 0 (sound mixing ~25%, the game's own code) is the limit now. |
| The locked cache as guest memory: the 16 KB at 0xE0000000 where NW4R computes its model matrices (and LCStoreData DMAs them out) had no page, so every load and store there took mmio_read/mmio_write and a byte loop; now it is a page of `g_page` like RAM, and its DMA a block copy | `runtime/memory.cpp`, `runtime/cpu.cpp`, `runtime/hw/mmio.cpp` | Measured, Azahar room (release, 160 s each, same SD): 30.5 → 34.5 game fps. The g3d matrix functions (CalcWorld/CalcView, the DMA's `mtspr`, 9% of core 0 in a profile build) left the profile's top list. Found with `NOMA_PROFILE_UNDER=app_Renderer_DrawPasses`. |
| Display lists the game writes again every frame keep their cached draws: when a list's bytes changed, each draw is checked against a 64-bit hash of its own vertex data instead of the whole list's records being dropped (two lists in the room, 132 draws, were rebuilt into fresh pool space every frame, so the pool filled and started over every ~34 frames with a frame rebuilding all ~5,200 list vertices). The pool then shrank to what the scenes use (40K vertices, 128K indices) | `platform/3ds/gx_pica.cpp` | Measured, Azahar room: list vertices made per frame 1,094 → 0; GPU-thread vertex time 5.0 → 4.2 ms a frame; the cache settles at 10.6K vertices. 2.1 MB of linear heap back. |
| The matrix palette without copies and compares: the decoder keeps a write generation per XF matrix row, so a palette entry is current while its rows are older than its copy (it used to copy the rows from XF memory and `memcmp` them at every lookup after any XF write); room for new rows from a second-chance clock over the 14 groups instead of a full LRU search; XF loads reported once per command | `runtime/hw/gx/gx.h`, `cp.cpp`, `tnl.cpp` | ARM11 instructions (`armbench --gx`): a layout quad's palette work 1,144 → 431, a cached draw's slots 1,318 → ~600 (about 1,100 draws a frame use it). Host check (`NOMA_CHECK_GX_DRAW=pica`): 1.64M room draws, 0 differ; taking stale rows as current makes 21,759 flushes differ. |
| Textures straight from the GX tiles into the PICA's tiles: the renderer takes the GX bytes (`gx_texture_raw`: same cache and versions, no decode) and writes each format's texels in its PICA format, padded with the edge texels as before; only a repeating texture that must be resampled to a power of two still goes through RGBA8 | `platform/3ds/pica_tex.h`, `runtime/hw/gx/texture.cpp`, `gx_pica.cpp` | ARM11 instructions a texel (decode + tiling → direct): I4 31.4 → 14.8, IA8 38.1 → 15.3, RGB5A3 54.5 → 37.3, RGBA8 43.3 → 27.9, CMPR 47.2 → 28.5, C8 51.7 → 23.2. Azahar room entry: the GPU thread's texture work 469 → 247 ms in the frame loading 101 textures, 84 → 52 ms in the next. Texture memory byte-identical to decode + tiling (host test, 6,000 random textures in every format). No w×h×4 temporary on the heap. |
| Smaller per-batch costs: the changed uniform vectors found a run at a time with count-trailing-zeros (was a bit-by-bit walk of 95 flags per batch); the EFB read-back's per-pixel scaling through per-copy column/row tables | `platform/3ds/gx_pica.cpp` | ~500 instructions less per batch (~160 batches a frame); EFB copy test images identical. |
| Measuring tools: `tools/armbench/armbench.py --gx` (the GPU thread's per-draw kernels and texture conversion in ARM11 instructions, `--gx-funcs KIND` per function); `NOMA_PROFILE_TOP`, `NOMA_PROFILE_UNDER=function`; `NOMA_GX_TIMERS` with palette and list-cache counters and a line for every frame over `NOMA_GX_SPIKE_MS` (default 100) with its own breakdown; the PICA's own drawing time in the renderer's 600-frame log line (`C3D_GetDrawingTime`: meaningful on hardware only) | `tools/armbench/`, `runtime/profile.cpp`, `gx_pica.cpp` | |
| The content players left out: Adobe Flash (4,216 functions) and MobiClip (693) are stubs; Flash's init makes nothing (the manager stays "not ready"), the movie manager starts with no work area and no movie; the shop, which was Flash content, is hidden | `runtime/patches/players.cpp`, `runtime/patches/menus.cpp`, `tools/decomp/regions.py` | 3DS code 22.55 → 15.38 MB with the guest-memory change. Nothing builds Flash's 5.6 MB MEM1 + 24 MB MEM2 heaps, its sound thread or MobiClip's 4.2 MB. |
| VF volume files made in 16 KB writes: `/tmp/skua.vf` took 27,067 NAND writes of 512 zero bytes (each an IOS request) at every start | `runtime/patches/vf.cpp` | 1,060 writes; the format 0.84 → 0.03 s on the host, 12.2 → 0.55 s on a host throttled to the 3DS's speed (est. 16 s of the opening on the 3DS). Same file bytes. |
| Interrupts at every loop poll, and poll points in the LZ/Huffman natives (charged like the loops they replace): a font's glyph sheet decoded for 28 ms (host; ~10× on the 3DS) with no retrace or IOS reply taken | `runtime/irq.cpp`, `engine/cx/uncompress.cpp` | Longest stretch without a poll at startup 28.2 → 0.6 ms (host); the title's icons no longer stall behind loading. |
| Guest memory in 1 MB pages with a biased table (`g_page[a >> 20] + a`, no mask); a constant address in MEM1 (every global) needs no MMIO check, a constant 0xCC/0xCD address goes straight to the device emulation | `runtime/guest.h`, `runtime/memory.cpp` | 3DS code 22.55 → 20.09 MB (−10.9%); a global's load is 5 instructions with no branch. MEM1 backed at its real 24 MB (was 32). |
| CX decompression's fast paths: LZ in whole flag groups (literal runs found with CLZ, matches copied a word at a time), Huffman through a table of each tree's first 10 bits (kept while the context holds the same tree); same output and context state at every return as the streaming code, same poll points | `engine/cx/uncompress.cpp` (reference: `cx_reference.h`) | ARM11 instructions per output byte (`tools/armbench/armbench.py --cx`): room model (LZ11) 13.8 → 10.4, layouts 8.3 → 5.2, font glyph sheets (Huffman, ~9 MB at start) 55–72 → 34–40. Exact (`NOMA_SELFTEST=cx`: 3,000 LZ and 3,000 Huffman random streams in random pieces). |
| Gather-pipe stores inline: a store to the constant address 0xCC008000 (about 1,900 in the decompiled code: GX commands and immediate-mode vertices) appends to the 32-byte gather buffer in place, with a call only for the burst at its end | `runtime/guest.h` (`gather_put`), `runtime/hw/gx/fifo.cpp` | A 4-byte write is about 8 instructions instead of mmio_write's dispatch, gx_fifo_write and a 4-step byte loop (est. 40). Same bytes in the FIFO. |
| The SDK's software keyboard and ATOK left out: 1,592 of their 1,604 functions are stubs. `tools/decomp/reachable.py` follows calls, address loads and vtable/table entries from where execution starts (`__start`, `main`, the .ctors table, the natives' own references) and found only 12 one-line accessors that the linker shared with the channel's classes still reached; they stay decompiled (`KEEP` in `regions.py`). Its 14 static constructors fill only its own tables (no live code reads them) and run as stubs | `tools/decomp/regions.py`, `tools/decomp/reachable.py` | 552 KB of PowerPC code; 3DS code 15.05 → 13.63 MB. No stub hit beyond the constructors on the way through the room, その他, メッセージを送る, the Mii pick and the mood choice. |
| The channel's contents read on an I/O thread: an ES content read stays pending while every guest thread runs on, the bytes go straight into guest memory, and the device poll sends the replies in order (the content handle keeps its own position, so seeks don't wait; closing waits for the reads in flight) | `runtime/hw/ios/ios.cpp`, `es.cpp`, `platform/*/main.cpp` (`platform_io_thread_start`; `NOMA_IO_INLINE=1` reads inline) | The start-up's 408 reads (10.2 MB) no longer stop the emulator: about a second on a 3DS SD card. Loading itself isn't shorter: the game reads and decompresses each piece in turn (host at a simulated 4 MB/s, `NOMA_READ_MBPS=4`: last read at 29.9 s either way). |
| Libraries behind natives decompiled only where they can still run: WPAD, the Bluetooth stack, the HOME Button Menu and PD/KPR/KBD/HID are "dead" regions, where decompile.py stubs what `tools/decomp/reachable.py` can't reach (1,470 functions, 435 KB of PowerPC; 77 stay) | `tools/decomp/regions.py`, `decompile.py` | 3DS code 13.63 → 12.66 MB. No stub hit beyond the boot constructors in the room (HOME, 1, 2, +, −, B pressed) or the first start. |
| Loading spikes measured frame by frame: `NOMA_LOAD_STATS` (see Rules), per-spike profiler samples (`profile_frame_end`) | `runtime/load_stats.cpp`, `runtime/profile.cpp`, hooks in IOS, CX, VI, irq, heap | Baseline (Azahar, release, start to the room): the start's long frame 2.72 s (the 6 MB sound archive read: 1.1 s), the room made in one 3.15 s frame, the room shown at 25.2 s; in the room 29-34 fps, p99 ~50 ms. |
| The file system's requests on the I/O thread: NAND opens, reads, writes, closes, `/dev/fs`, ES's content open and close are jobs, in the order they came (IOS's file system is one thread too); only the guest thread that sent one waits, a content's close no longer waits for every read in flight; `/tmp` (in memory) is done at once while no job waits. Opens take one SD access (fopen, then fstat; libctru's stat opens the file too), read-only files a 4 KB stdio buffer (64 KB made every small read a 64 KB card read) | `runtime/hw/ios/ios.cpp`, `fs.cpp`, `es.cpp` | Measured, Azahar: a NAND open stopped every guest thread for ~60 ms (now ~10 ms on the I/O thread), a small read 12.6 ms, a close ~10 ms: 2.05 s of such stops during the start, 0.68 s in the frame that makes the room. Room-making frame 3.15 → 2.50 s; the room shown 3.3 s sooner. The save written at a host shutdown is byte-identical with `NOMA_IO_INLINE=1`. |
| Huffman glyph sheets: the 10-bit table also gives every whole symbol its bits hold (up to an output word), runs of the 1-bit code (the empty background: two thirds of the symbols, in runs of 16 or more) are counted with CLZ and stored a word at a time; both go on across input words and stop at a symbol's end with the streaming code's state; the output goes straight to host memory | `engine/cx/uncompress.cpp` | ARM11 instructions per output byte over 16 sheets of both fonts: 41.2 → 26.0 (sparse sheets 33.7 → 16.3, dense kanji 48.3 → 34.4). Azahar: the font's 4.4 MB 939 → 747 ms. Exact: `NOMA_SELFTEST=cx` now also decodes every LZ file (715) and glyph sheet (136) in `./content` in random pieces; host UBSan harness over all sheets. |
| The heaps' NaN fill of new blocks native (`lib_allocator_Fill`, 0x8018A13C): every block lib::allocator hands out is filled with `lib_allocator_sFillValue` (0x7FFFFFFF) and 0xFF in the last size & 3 bytes; 28 MB at the start and 29 MB while the room is made went through the decompiled loop a word at a time | `engine/system/allocator.cpp` | Measured, Azahar: room-making frame 2.50 → 1.67 s, the start's long frame 2.51 → 2.18 s; the room shown at 20.1 s. `NOMA_SELFTEST=fill` (3,000 fills, mutations caught); the host save byte-identical. |
| The 3DS log buffered (16 KB, written at most every 500 ms; the I/O thread writes what a stopped program left, fatal errors and quitting at once): every line was an FS write from the CPU's thread, ~400 lines in a start | `platform/3ds/main.cpp` | Azahar: the start's long frame 2.18 → 2.12 s (Azahar charges little for SD writes; on hardware each line was an FS call). |
| The save file's CRC-16 native (`OSCalcCRC16`, 0x802DD830): computed half a byte at a time through a table of 16 over the 480 KB save when it is read and when it is written; now a byte at a time through a table of 256 made from the game's 16 (the same steps, by construction) | `engine/os/crc.cpp` | Measured, Azahar, with the two rows below (one run): the room-making frame 1.65 → 1.48 s, the main thread's share of it 633 → 540 ms. `NOMA_SELFTEST=crc` against the loop (400 blocks up to 1 MB, any alignment; mutations caught); the host save byte-identical. |
| Small files read whole: when a read of at most 32 bytes starts an archive file of at most 32 KB (CNT's size query reads a compressed file's header, then the file from its start, rounded up to 32 bytes; csv files come 32 bytes at a time), the I/O thread reads the file once and copies what follows of it from there (the archives' tables come from the guest's own read of them; 4 files kept, 128 KB at most) | `runtime/hw/ios/ios.cpp` | Host with the SD card's latency modelled (`NOMA_READ_MBPS=5 NOMA_READ_US=520`): room-making reads 654 → 521 ms. Azahar: the bodies' reads (186 in a start) take no SD time now, but Azahar charges a file-sized read about what the two did; the guest's waits for reads in the room-making frame 540 → 507 ms. |
| A content's file kept open when the channel closes it, for its next open (content 2 is opened 3 times in a start; an open and a close cost ~19 ms of SD access in Azahar) | `runtime/hw/ios/es.cpp` | Azahar: the file system's waits in the start's long frame 397 → 333 ms; that frame 2.12 → 2.01 s (with the two rows above). |
| Guest memory as large as the game needs: MEM2 48 MB, not 64 (the room and its menus run in 44, 42 can't make the room; Flash and MobiClip are left out). The 3DS heaps split by need: the normal heap (at most 128 MB of addresses, 0x08000000 on) gets all but 48 MB, the linear heap keeps 16 MB for the GPU and sound plus MEM1; MEM2 goes in the normal heap (nothing reads guest memory by DMA) | `runtime/runtime.h` (`MEM2_SIZE`), `platform/3ds/main.cpp` (`__system_allocateHeaps`, `platform_alloc_guest`) | Guest memory 96 → 72 MB; what's saved goes to the program's own heap. |

## In progress

| What | Where | Expected | Notes |
|---|---|---|---|
| Mixing on its own thread on the New 3DS's system core (`NOMA_AX_THREAD=1`) | `runtime/hw/dsp_hle.cpp`, `platform/3ds/main.cpp` | Audio off the main thread on the New 3DS | Off by default. The wait is a kernel event now (`(ax wait)` in the profile), but Azahar runs the cores one after another: there each frame's handoff is a wait, boot screens ran at 1–2.5 fps (inline: 50–59) and the room ~10% slower. To measure on hardware; if it pays off there, choose it on hardware only. |

## Candidates

### Audio

Measured in the room (host, 3 minutes, `NOMA_DEBUG_AX`): up to 27 voices a frame; 91.7% of voice
frames take the one-pass path (fixed envelope 83%, ramping 8.3%, none at exactly 1.0 or above it),
8.2% are muted (their samples are only decoded), 0.1% use another bus, a filter or SRC type 2. Every
voice is ADPCM. nw4r::snd recalculates every voice's pitch, envelope, mix and filter every frame.
Estimates below are ARM11 cycles from `armbench.py --cycles` (L1 hits only).

| What | Expected | Effort | Exact? |
|---|---|---|---|
| The sound thread's other parts, benched like the voice update: Channel::Update (envelope, LFO, sweep, 18 voice setters per channel per frame) and the sequencer's tick | (snd channels) and (snd sequences) were 2.9% and 2.3% of core 0 in Azahar's room profile | Medium | Yes, with a `--snd-diff`-style scenario for them |
| PLD (data preload) of the next voice's PB and samples while this one renders: the L1 data cache takes up to three outstanding read misses (ARM DDI 0360F §1.3.2), and between two AX frames the game's own code evicts the 16 KB cache, so a voice frame meets ~10 misses (7 PB lines, 2-3 sample lines); the Old 3DS has no L2 | Hides FCRAM latency per voice; unmeasurable here (Unicorn and Azahar model no caches) | Small | Yes |
| Main L and R interleaved in the bus buffer, so the one-pass loop updates a sample's pair with LDRD/STRD (−2 instructions a sample) | est. ~200 cycles a voice frame | Medium (every bus user: dsp_ax.cpp, the generic mixing, the tests) | Yes |
| Render each voice's samples to a small buffer and mix voices into the buses two at a time (each bus word loaded and stored once per two voices), the interpolation loop then free to work on two samples at a time | est. ~15 instead of ~19 cycles a sample | Medium | Yes (the buses' wrapping sums don't depend on the order) |
| Aux buses zeroed only when something mixes into or reads them (SETUP fills all twelve; the room uses main L, R and S) | est. ~1,800 cycles a frame | Small | Yes |
| ADPCM's partial frames (where a voice frame starts and ends inside an ADPCM frame) entered into the unrolled frame code instead of the nibble loop | est. ~100 cycles a voice frame | Small | Yes |
| The voice update per AX voice (pitch, envelope, mix, filter in one visit, each object's host address resolved once) instead of per kind of update | est. ~200 of the 2,047 cycles a voice | Small | Yes (each AX voice's state is its own) |
| Decode-once cache: non-streamed ADPCM decoded to PCM16 once, keyed by address, end, loop, coefficients, start/loop context and memory write generation; a voice may use it where its position and history match the cache's | The ADPCM's ~7.5 cycles a sample for looped instrument samples, for 3.5× their memory as PCM16 (the Old 3DS has none to spare) | Medium | Yes, if the key is complete |
| Biquad: the 5-tap sum with SMLALD (64-bit) | Only if the game turns biquads on (it hasn't so far) | Small | Yes |

### Wii parts with no purpose on a 3DS

What the program does only because it runs on a Wii, and what replaces it.

| Part | Status |
|---|---|
| Wii Remote (Bluetooth, WPAD, KPAD, speaker, rumble) | Done: native (`engine/input/remote.cpp`). |
| HOME Button Menu | Done: never loads (`engine/system/home_button.cpp`). |
| Software keyboard (the SDK's sofkeybd with ATOK and KPR: a 4.49 MB heap, three dictionaries and a sound archive in every scene that takes text) | Done: the 3DS's system keyboard; the scenes' keyboard object is native (`engine/system/keyboard.cpp`, `platform_text_input`). The SDK keyboard's and ATOK's code is left out too (below). |
| The Wii Remote strap screen at the start ("use the strap", until a button or 20 s) | Done: the title and pre-title scenes are made without it (`runtime/patches/wii_parts.cpp`). |
| The first-run setup's video quality step (high or normal quality for streaming), and the line announcing it | Done: the setup goes from the family's Miis to its end; the message loses that line (`wii_parts.cpp`). |
| The same setting in the room's options (OptionScene::StateVideo, the top menu's video page) | Done: its button is hidden, the family and the terms of use take the middle places (`runtime/patches/menus.cpp`). |
| Texts about the Wii's own parts: the "Wiiメニューへ" buttons and questions, the Mii Channel (no Mii yet, its warnings), pointing the Wii Remote (room help), the Wii Points button (theater help) | Done: edited in place when the texts load (`wii_MessageData_Load`, by message index): "おわる", "Wiiの間をおわります", "Miiをえらぶ", touch the lower screen, the warnings shortened. |
| Leaving for another Wii program (Wii Menu, Data Management, Wii Shop, Mii Channel, Internet Channel, region settings: `app_Exit_Dispatch` codes) | Done for the Mii Channel: the 3DS's Mii Selector, then the app starts again with the Mii. The others end the app (after a 120-frame fade: 8 s at 15 fps, to shorten). The options' region/address entry and the theater's Wii Points button are hidden (`menus.cpp`). |
| Frames rounded up to whole TV retraces (`VIWaitForRetrace` once a frame) | Done: no wait when a retrace has passed since the last one; 60 fps stays the cap. About half a retrace a frame at the room's frame rate. |
| The Wii Remote speaker's alarm (nw4r::snd, every 6.67 ms, for good) | Done: never set (`engine/input/remote.cpp`). |
| Mii database: holding SELECT at start (Mii Selector) replaced every Mii with the one picked | Done: the Mii is added (or replaces itself), the others stay (`rfl_add_mii`, self-tested). |
| Background Wii services: the Wii Shop (EC) manager thread polling every 16 ms (fn_800160B8, made in fn_80015CFC), the weather thread (fn_801A6B00), the WiiConnect24 manager thread (fn_80038B9C, files reopened every ~300 s), the address book (PD), the TV programme cache (2.5 MiB), 78 MB of arena cleared at boot | To do: offline, don't start them (each guest thread is also 256 KB of host heap on the 3DS). |
| Shopping and content playback build Flash (5.6 MiB MEM1 + 24 MiB MEM2), MobiClip (4.2 MiB) and the keyboard before showing the offline error; Flash's sound thread then stays | Done: Flash and MobiClip are left out (stubs; `runtime/patches/players.cpp`). The shop was Flash content (its own code is only the entrance), so the room bar's ショッピング button is hidden (`menus.cpp`); content playback is only reached from server content. The "can't connect" error lost its "check the Wii's internet settings" line. |
| The software keyboard and its ATOK dictionaries (4.49 MiB MEM1, 3.85 MB read) built on entering 番組を見る without typing | Done: the 3DS's own keyboard (nothing of the SDK's is built). |
| Closing from the 3DS HOME Menu exits at once, past the channel's own shutdown (a save could be cut off) | To do: complete the pending power event (STM) instead. |
| A 3DS-native control scheme: the game's controller manager (0x802A2DA0: KPAD set-up; 0x802A2ED0: the per-frame read) and the screens' pointer handling replaced by the 3DS's own (touch to select, the D-pad and buttons for lists), with no Wii Remote in between | Planned: the next step after the native KPAD. `WiimoteInput` (`engine/input/remote.h`) is the one place the platform layers feed. |
| GameCube controller (PAD over SI) | To check: the game only calls PADInit; the SI emulation answers "no controllers". |
| USB keyboard (KBD, HID, KPR) | To check: one device-change request stays pending; likely nothing per frame. |
| Disc drive (DVD), WiiConnect24 and the network daemons (NCD, NWC24, SO) | To check for periodic work in the room (offline, their requests fail at once). |
| IOS IPC | Stays for files (NAND), but most of its traffic was the Bluetooth link. In the room, nothing polls IOS any more (host count with `NOMA_DEBUG_IOS`). |
| NAND `/tmp`, wiped by the Wii at every boot: so the channel formats its cache volumes again at every launch, `/tmp/skua.vf` (13.7 MB, 27,067 writes of 512 bytes, 2 sectors not zero) and `/tmp/finch.vf` (21 MB), about 35 MB of zeros written to the SD card per launch | Done: `/tmp` is in memory (`runtime/hw/ios/fs.cpp`), sparse (pages only ever written with zeros aren't stored), empty at every start as on the Wii. Keeping it on the card instead crashes the channel at start: it can't take its volumes from an earlier launch. No SD writes (seconds on a real 3DS), no card wear. |

### Guest code

| What | Where | Expected | Effort | Risk |
|---|---|---|---|---|
| Compile `noma_decomp` at `-O2` (or `-Os` for cold objects). `-O1` has no instruction scheduling, which hurts on the in-order ARM11, and no small-function inlining. `-DNOMA_DECOMP_OPT=-O2` builds it so. Measured: every host self-test passes and the host runs normally; 3DS code 15.05 → 14.44 MB; in Azahar (which charges per instruction, so scheduling can't show) the room runs at 16.9 fps against 17.2 at `-O1`, the same tree. | `CMakeLists.txt` | Only on hardware, from scheduling | Done but for a hardware A/B | None found |
| Cheaper polls: the function-entry and loop polls (`NOMA_LOOP`) read a flag a timer sets, instead of decrementing a global | runtime | Small, everywhere | 1 day | Low |
| Profile-guided code layout: order `.text.<fn>` sections from the sampling profiler (22.6 MB of code against a 16 KB instruction cache), then GCC PGO with `.gcda` files written to the SD card | linker script | est. 5–15% | 2–4 days | None |
| The guest accessors inline: at `-O1` GCC calls `gstore_traits<T>::load/store` and the `gref<gp<...>>` operators out of line (counted in a 3DS release build: ~63K call sites for `long`, ~26K `unsigned long`, ~17K `float`, ~25K `gref`); `always_inline` on them would drop the call and let callers keep values in r0-r3/s0-s15 | `runtime/guest.h` | Broad (Azahar charges per instruction, so it would show there); several MB more code, so an I-cache question on hardware | Hours + a hardware A/B | Code size |
| `f32` instead of `f64` for values that are single precision on the Wii (`fadds`/`fmuls` results): identical bits, fewer conversions | m2c type inference | Broad, modest | 2–4 days | Only where provably single precision |

### Graphics

Where the room's frame goes (Azahar, profile build, after this round): core 0 limits the frame
rate (the game's main thread ~54% of its samples, the render passes under it ~34% of all of them;
the sound thread ~38%); the GPU thread is ~40% idle; the PICA itself does little (~7,100 vertices
and ~160 draw calls a frame, about 30 shader instructions per layout vertex, 400×240 pixels), so
no GPU-side work pays before the CPU sides do. On the Old 3DS the GPU thread shares the system core
(at most 80% of it).

| What | Where | Expected | Effort | Risk |
|---|---|---|---|---|
| The GX SDK natively on core 0: the GX calls are ~10% of core 0's samples in a profile build (GXLoadTexObjPreLoaded, __GXSetDirtyState, GXSetTevSwapModeTable, GXLoadTexMtxImm, GXSetArray, GXSetTevColorS10, ... per material and draw). `engine/gx/gx_sdk.cpp` (unwired) has the hottest ones but writes through `gx_fifo_write`, which is now slower than the decompiled code's inline `gather_put`: switch it, diff-test against the decompiled code (FIFO bytes and `__GXData`), wire through NATIVE_NAMES | `engine/gx/gx_sdk.cpp` | est. 3–5% of core 0 | 2 days | Low with a diff test |
| The layouts' quads natively (nw4r::lyt's DrawQuad family fn_80464570/fn_80464AD0 and the pane draw fn_80460EF0: ~5% of core 0 in a profile build), then g3d's material setup (fn_80443630, ResMatChan) | `engine/lyt`, `engine/g3d` | est. 3–5% of core 0 | Days | Must match the streams |
| Our own PICA state writes instead of `C3D_DrawElements` after a state change (~150 a frame): citro3d's context update walks both shaders' 96 uniform flags and rebinds every dirty part on each | `platform/3ds/gx_pica.cpp` | est. ~0.5 ms a frame on the GPU thread | 1–2 days | Low |
| Cached draws keep their palette slots: almost every cached draw (~800 a frame) gets other slots than last frame and rewrites them, because each draw loads a new matrix into the same XF row; a slot per draw that stays its own (or fixed attributes for fixed-matrix draws) would make cached draws free of palette work | `runtime/hw/gx/tnl.cpp`, `gx_pica.cpp` | est. ~1–2 ms a frame on the GPU thread | 2 days | Batch count |
| Textures: I4/I8 as L4/L8 with alpha read from red (half of LA4/LA8); CMPR → ETC1/ETC1A4 at staging time, looked up by content hash; hot UI textures in VRAM. (GX tiles straight to PICA tiles: done, above.) | `runtime/hw/gx/texture.cpp`, `tools/stage_data.py` | Model textures 12.7 → 3.2 MB | ~1 week | ETC1 loses some colour detail |
| EFB copies with the transfer engine instead of a CPU read-back mid-frame (Mii faces): measured, a copy's conversion ~28 ms on the GPU thread at room entry, plus the GPU sync | `platform/3ds/gx_pica.cpp` | One GPU stall and ~28 ms less per copy | 2–3 days | RGBA8/565/5551/4444 only, ≥ 64 px wide |
| Aspect: the game draws for 16:9 (SYSCONF IPL.AR) and the 3DS screen is 5:3, so the picture is ~6.5% narrow with the TV overscan margin showing: 4:3 mode or a crop of ~21 px per side in the transform, scissor and read-back | `platform/3ds/gx_pica.cpp` | Fills the screen | 1 day | Touch mapping must match |
| A native nw4r::lyt renderer for the layouts; on the Old 3DS, a frameskip that still decodes GX state | `engine/lyt` | Large | Weeks | Must match lyt materials |

### Loading

Measured state after the round above (Azahar, release, start to the room): the room is shown at 19.6
s (25.2 s before the round); the start's long frame (2.72 → 2.01 s) is I/O-bound: the 6 MB sound
archive alone is 1.1 s, file system jobs 0.25 s, the main thread's CPU 0.47 s; the room-making frame
(3.15 → 1.48 s) is the sound thread ~39%, waits for reads ~15% (243 reads, 1.7 MB), LZ ~6% (1.7 →
3.0 MB, 150 ms), the allocator's own search ~4%, the rest the scene's own code; the next frame (1.2
s) is the GPU thread's first textures and the save written to the card. With the audio round also on
main (c9dfb61): the room-making frame 1.27 s, the room shown at 19.35 s; in the room ~33 fps, p99
~50 ms (the steady state is CPU-bound, not loading).

| What | Where | Expected | Effort | Exact? |
|---|---|---|---|---|
| LZ: literal runs and matches stored a word at a time where they don't overlap (now byte loops), the flag group unrolled | `engine/cx/uncompress.cpp` | 10.8 instructions a byte on the room model; est. −30%, ~50 ms per room | 1 day | Yes (`NOMA_SELFTEST=cx` game data) |
| The channel's heap search (fn_8018978C, fn_801893A0: the expanded heap's free-list walk and its virtual calls) | `engine/system` | ~4% of the room-making frame | Days | Must choose the same blocks |
| `svcOutputDebugString` per log line behind a switch: ~2 ms a line in Azahar (cheap on hardware), ~400 lines in a start, which distorts every Azahar timing of loading | `platform/3ds/main.cpp` | Azahar only | Minutes | — (the user's call: the Azahar log window shows it) |

### Memory and the Old 3DS

| What | Expected | Effort | Risk |
|---|---|---|---|
| Old 3DS memory diet: guest memory is 72 MB (MEM1 24 MB, MEM2 48 MB). Next: build cold code with `-Os` and `-fipa-icf`. The SDK's keyboard and ATOK are stubs now (above); Then the Bluetooth stack, WPAD and the HOME Button Menu (above); what nothing reaches elsewhere (app, NW4R, MSL, CNT: about 600 functions, 110 KB of PowerPC) could follow. Measured in the room (Azahar): normal heap 68.7 MB used (48 MB of it MEM2), linear 37.8 MB, code 12.7 MB: ~120 MB against the Old 3DS's 96 MB at most. | Needed to run on the Old 3DS at all | Days | Out of memory in untested scenes |

### Determinism

| What | Why |
|---|---|
| `-ffp-contract=off` for host builds | Apple clang fuses `a*b+c` into FMADD by default and the 3DS's VFP never does, so host and 3DS results can differ in the last bit. |

## Ruled out

- **Faster CPU transform (`xform.cpp`: one reciprocal for s/t/q, ×1/255).** The transform runs in
  the PICA's vertex shader now; `xform.cpp` is only the host's reference.

- **Mixing voices on the 3DS DSP (ndsp).** It runs at 32,728 Hz in 160-sample frames with its own
  interpolation, has 24 channels, and gives no per-3 ms state write-back. It can't be exact and is
  hard to keep in sync. Custom Teak firmware would be weeks of work.
- **Recompressing game data at install.** The SD card is slow and LZ data expands about 3×; fast
  decoders get the gain without it. Checked again (2026-10-04) with Azahar's hardware-derived SD
  model (~183 ns a byte): the room's g3d `.lex` files are 1.97:1, so storing them expanded costs
  ~183 ns per extra byte read against ~40 ns per output byte of LZ on the Old 3DS (10.8 instructions);
  the one big uncompressed file of the start, the 6 MB sound archive (ADPCM), compresses to 85% at
  best (zlib): ≤ 0.17 s at the start, for a staging step and the CPU to undo it.
- **Reading the start's big reads ahead** (tried 2026-10-04: each start notes the reads of 512 KB or
  more in its first 40 s in a plan file; the next reads them ahead on the I/O thread in 32-64 KB
  pieces while no request waits, and a matching read takes the bytes). Azahar: the font and the Mii
  parts arrived fully read ahead (their waits 566 → 94 ms), but the sound archive, asked 1.2-1.4 s
  after the I/O thread's start, had only 1.1-1.7 of 5.9 MB: the start's long frame 2.16 → 2.05-2.09 s;
  the room came no sooner (the opening paces the start), the opening's worst frames got worse (100
  and 83 ms against 67 and 50: requests waiting for pieces), and it held 6 MB of host memory for
  seconds (the Old 3DS has none to spare). Reverted.
- **Maths that needs FMA, NEON, or correctly rounded libraries for game logic.** The ARM11 has
  none of them, and they change results.
- **Mixing in floating point** (as engines with SIMD floats do): the ARM11's VFP has no vectors
  worth the name and its results wouldn't be the DSP's integers; the ARMv6 DSP instructions (16-bit
  halves, saturation, 32×16 multiplies) do the AX's arithmetic exactly.
- **The ADPCM predictor without the saturation in its chain** (a plain shift, the clamp checked
  after the frame): the check costs more instructions than the cycle of latency it saves; the
  in-order ARM11 is then issue-bound, not latency-bound.
- **Folding the envelope into the mix volumes** (one multiply instead of two): the two roundings
  (`(s × ve) >> 15`, then `(e × vol) >> 15`) can't be one, so it can't be exact.

## ARM11 kernel bench

Compile an engine kernel with the 3DS build's flags, link a small driver
(`arm-none-eabi-g++ -nostartfiles -Wl,-e,bench`), and run it under Unicorn (`pip install unicorn
pyelftools capstone`). Counting executed instructions per call shows each change's effect without a
device. Running the same inputs through the old and new code and hashing every output compares
the ARM-only paths (DSP intrinsics, inline asm), which the host self-tests can't reach.

`tools/armbench/armbench.py`: the AX voice renderer by kind of voice and its exactness on random
voices (default), `--lines KIND` per source line, `--frame N` a whole AX frame with N voices,
`--snd N` nw4r::snd's voice update, `--snd-diff REV` that update against a git revision's,
`--cx` the decompressors. `--cycles` adds an estimate of ARM11 cycles
(`tools/armbench/arm11_timing.py`): the MPCore TRM's issue cycles, result latencies, Early/Late
registers and load/store unit (ARM DDI 0360F, chapter 15) applied to the instructions as they run,
with its branch predictor (128 entries, 2-bit counters, static fallback) and return stack. It
assumes every access hits the L1 cache and doesn't fold branches: a lower bound, for comparing
versions of a kernel. Instruction counts and the estimate both ignore what the 3DS's memory
costs: on the Old 3DS (268 MHz, no L2; the New 3DS has 804 MHz and a 2 MB L2, per 3dbrew's hardware
page) a miss to FCRAM is far dearer than on the host.
