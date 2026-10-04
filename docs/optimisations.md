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

## Done

| What | Where | Result |
|---|---|---|
| Native AX voice renderer: the Wii DSP's voice pipeline in C++, every voice, no 24-voice limit | `engine/audio/ax_render.cpp` | Replaced the emulated reference. Bit-exact (`NOMA_SELFTEST=ax`). |
| Block renderer: ADPCM decoded a frame at a time straight from RAM; resampler planned in closed form, envelope folded in; muted voices and zero-volume buses only accounted for; PB copied a word at a time | `engine/audio/ax_render.cpp` | Room voice: 14,719 → 4,802 ARM11 instructions per 3 ms frame. Bit-exact on host and ARM. |
| ARM11 DSP instructions in the mixer: SMLAD for the ADPCM predictor, PKHBT for its history, SSAT with shift for clamps, SMLAWB/SMLAWT to mix two samples per load (no clamp needed at volumes ≤ 1.0), REV16 for PB byte swaps | same | Included in the number above. |
| The Wii Remote, native: KPAD and WPAD answered with the 3DS's controls; the Bluetooth stack (BTE, WUD), the emulated Bluetooth controller and Wii Remote, the remote's speaker stream and rumble are gone | `engine/input/remote.cpp` | No Bluetooth pairing at boot, no 100 Hz report traffic through IPC, no speaker audio encoding. The pointer goes exactly where it's put. |
| 3DS texture memory: compact PICA formats (LA4/LA8/RGB565/RGBA5551) instead of RGBA8, LRU eviction | `platform/3ds/gx_pica.cpp` | Glyphs and textures fit again. |
| The transform unit as a PICA vertex shader (`gx.v.pica`, 138 instructions): the CPU only decodes the stream and follows the indices (model-space position, normal, color, up to three texgen sources, 64-byte vertices); the shader does the position matrix (a palette of 42 matrix rows as uniforms, indexed per vertex), the projection folded with the 3DS viewport transform, GX lighting (spot/none attenuation, diffuse modes, ambient/material from register or vertex, up to 8 lights in a loop) and texgens (2×4/3×4 matrices, post matrices with normalisation, q division); culling by the GPU | `platform/3ds/shaders/gx.v.pica`, `runtime/hw/gx/tnl.cpp`, `platform/3ds/gx_pica.cpp` | Measured, Azahar room: GPU-thread vertex cost 11 → 7 ms per frame before the cache below, 1.5 → 20 fps overall with it. Verified by running the real shader binary in a PICA interpreter (`runtime/hw/gx/pica_vsh.cpp`, `NOMA_CHECK_GX_DRAW=pica NOMA_GX_SHBIN=…`) against the reference transform: 780k room draws, 0 differ; a dump of one frame's draw calls (`NOMA_DUMP_BATCHES=N`) replays on the host with `tools/pica_replay.cpp`. |
| Indexed, batched draws: vertices written once, quads/strips/fans as u16 indices; PICA state set up again only when a GX register changed (dirty bits per part of the state, a cache of TEV→combiner translations); consecutive draws with the same state and uniforms as one | `platform/3ds/gx_pica.cpp` | Room: ~1,240 draws in ~160-215 batches per frame. |
| Frames flush only the vertex/index ranges they used (`C3D_FrameEnd(0)` flushed the whole linear heap, guest memory included); texture upload with row/column tables and a loop per format | same | Not visible in Azahar (no cache model); a per-frame cost on hardware. |
| Texture pixels kept once: the RGBA8 decode is freed after the PICA upload | `runtime/hw/gx/texture.cpp`, `gx_pica.cpp` | Measured: 3DS heap in setup 29.7 → 19.5 MB at the same point. |
| One pass per frame: the game's renderer leaves its Wii anti-aliasing mode, which drew every frame twice as two EFB bands | `runtime/patches/graphics.cpp` | est. up to 2× on the GPU thread in 3D scenes; measuring. |
| Display-list geometry cache: a list's draws keep their shader vertices in a 64K-vertex pool in GPU memory from frame to frame, found again by list address/size and draw offset, valid while the list's bytes and the vertex arrays it indexes (write generations), the CP registers and what the shader wants of a vertex stay the same; only the palette slot bytes (a parallel buffer) are rewritten, and only when the matrices got other slots | `platform/3ds/gx_pica.cpp`, `runtime/hw/gx/tnl.cpp` | Measured, Azahar room: ~960 of ~1,130 draws per frame from the cache; per-frame vertex work 11 → ~1 ms for list draws. `NOMA_GX_TIMERS=N` logs the GPU thread's milliseconds per step per frame. |
| The content players left out: Adobe Flash (4,216 functions) and MobiClip (693) are stubs; Flash's init makes nothing (the manager stays "not ready"), the movie manager starts with no work area and no movie; the shop, which was Flash content, is hidden | `runtime/patches/players.cpp`, `runtime/patches/menus.cpp`, `tools/decomp/regions.py` | 3DS code 22.55 → 15.38 MB with the guest-memory change. Nothing builds Flash's 5.6 MB MEM1 + 24 MB MEM2 heaps, its sound thread or MobiClip's 4.2 MB. |
| VF volume files made in 16 KB writes: `/tmp/skua.vf` took 27,067 NAND writes of 512 zero bytes (each an IOS request) at every start | `runtime/patches/vf.cpp` | 1,060 writes; the format 0.84 → 0.03 s on the host, 12.2 → 0.55 s on a host throttled to the 3DS's speed (est. 16 s of the opening on the 3DS). Same file bytes. |
| Interrupts at every loop poll, and poll points in the LZ/Huffman natives (charged like the loops they replace): a font's glyph sheet decoded for 28 ms (host; ~10× on the 3DS) with no retrace or IOS reply taken | `runtime/irq.cpp`, `engine/cx/uncompress.cpp` | Longest stretch without a poll at startup 28.2 → 0.6 ms (host); the title's icons no longer stall behind loading. |
| Guest memory in 1 MB pages with a biased table (`g_page[a >> 20] + a`, no mask); a constant address in MEM1 (every global) needs no MMIO check, a constant 0xCC/0xCD address goes straight to the device emulation | `runtime/guest.h`, `runtime/memory.cpp` | 3DS code 22.55 → 20.09 MB (−10.9%); a global's load is 5 instructions with no branch. MEM1 backed at its real 24 MB (was 32). |
| CX decompression's fast paths: LZ in whole flag groups (literal runs found with CLZ, matches copied a word at a time), Huffman through a table of each tree's first 10 bits (kept while the context holds the same tree); same output and context state at every return as the streaming code, same poll points | `engine/cx/uncompress.cpp` (reference: `cx_reference.h`) | ARM11 instructions per output byte (`tools/armbench/armbench.py --cx`): room model (LZ11) 13.8 → 10.4, layouts 8.3 → 5.2, font glyph sheets (Huffman, ~9 MB at start) 55–72 → 34–40. Exact (`NOMA_SELFTEST=cx`: 3,000 LZ and 3,000 Huffman random streams in random pieces). |
| Gather-pipe stores inline: a store to the constant address 0xCC008000 (about 1,900 in the decompiled code: GX commands and immediate-mode vertices) appends to the 32-byte gather buffer in place, with a call only for the burst at its end | `runtime/guest.h` (`gather_put`), `runtime/hw/gx/fifo.cpp` | A 4-byte write is about 8 instructions instead of mmio_write's dispatch, gx_fifo_write and a 4-step byte loop (est. 40). Same bytes in the FIFO. |
| The SDK's software keyboard and ATOK left out: 1,592 of their 1,604 functions are stubs. `tools/decomp/reachable.py` follows calls, address loads and vtable/table entries from where execution starts (`__start`, `main`, the .ctors table, the natives' own references) and found only 12 one-line accessors that the linker shared with the channel's classes still reached; they stay decompiled (`KEEP` in `regions.py`). Its 14 static constructors fill only its own tables (no live code reads them) and run as stubs | `tools/decomp/regions.py`, `tools/decomp/reachable.py` | 552 KB of PowerPC code; 3DS code 15.05 → 13.63 MB. No stub hit beyond the constructors on the way through the room, その他, メッセージを送る, the Mii pick and the mood choice. |
| The channel's contents read on an I/O thread: an ES content read stays pending while every guest thread runs on, the bytes go straight into guest memory, and the device poll sends the replies in order (the content handle keeps its own position, so seeks don't wait; closing waits for the reads in flight) | `runtime/hw/ios/ios.cpp`, `es.cpp`, `platform/*/main.cpp` (`platform_io_thread_start`; `NOMA_IO_INLINE=1` reads inline) | The start-up's 408 reads (10.2 MB) no longer stop the emulator: about a second on a 3DS SD card. Loading itself isn't shorter: the game reads and decompresses each piece in turn (host at a simulated 4 MB/s, `NOMA_READ_MBPS=4`: last read at 29.9 s either way). |
| Libraries behind natives decompiled only where they can still run: WPAD, the Bluetooth stack, the HOME Button Menu and PD/KPR/KBD/HID are "dead" regions, where decompile.py stubs what `tools/decomp/reachable.py` can't reach (1,470 functions, 435 KB of PowerPC; 77 stay) | `tools/decomp/regions.py`, `decompile.py` | 3DS code 13.63 → 12.66 MB. No stub hit beyond the boot constructors in the room (HOME, 1, 2, +, −, B pressed) or the first start. |
| Guest memory as large as the game needs: MEM2 48 MB, not 64 (the room and its menus run in 44, 42 can't make the room; Flash and MobiClip are left out). The 3DS heaps split by need: the normal heap (at most 128 MB of addresses, 0x08000000 on) gets all but 48 MB, the linear heap keeps 16 MB for the GPU and sound plus MEM1; MEM2 goes in the normal heap (nothing reads guest memory by DMA) | `runtime/runtime.h` (`MEM2_SIZE`), `platform/3ds/main.cpp` (`__system_allocateHeaps`, `platform_alloc_guest`) | Guest memory 96 → 72 MB; what's saved goes to the program's own heap. |

## In progress

| What | Where | Expected | Notes |
|---|---|---|---|
| Mixing on its own thread on the New 3DS's system core (`NOMA_AX_THREAD=1`) | `runtime/hw/dsp_hle.cpp`, `platform/3ds/main.cpp` | Audio off the main thread on the New 3DS | Off by default. The wait is a kernel event now (`(ax wait)` in the profile), but Azahar runs the cores one after another: there each frame's handoff is a wait, boot screens ran at 1–2.5 fps (inline: 50–59) and the room ~10% slower. To measure on hardware; if it pays off there, choose it on hardware only. |

## Candidates

### Audio

Measured in the room (host, 120,000 voice frames): every voice is ADPCM through the standard
resampler at about 1.2 input samples per output, mixed only into main L+R at fixed volumes ≤ 1.0.
77% of voices have a fixed envelope, 18% a ramping one, 4% are muted. No aux buses, no surround, no
filters.

| What | Expected | Effort | Exact? |
|---|---|---|---|
| ADPCM: keep the rounding constant in a register so each sample is one MLA, not MUL+ADD | −2 instructions per byte | Small | Yes |
| ADPCM: decode whole frames in one loop (one header read, then 7 bytes) until the end address, a page edge or the request; checks only where the end falls inside a frame | ≈ −200 instructions per voice frame | Small | Yes |
| Read only the PB words the voice uses, and write back only the ones it changed | ≈ −250 instructions per voice frame | Small | Yes |
| Resampler, upsampling (ratio < 1): consecutive outputs share their two input samples | Fewer loads | Small | Yes |
| Biquad: the 5-tap sum with SMLALD (64-bit) | Only if the game turns biquads on (it hasn't so far) | Small | Yes |
| Decode-once cache: non-streamed ADPCM decoded to PCM16 once, keyed by address, end, loop, coefficients, start/loop context and memory write generation | The ADPCM cost disappears for looped instrument samples | Medium | Yes, if the key is complete |
| Mix two voices per pass over the buses (each bus word loaded and stored once per two voices) | Fewer bus loads and stores | Medium | Yes |

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
| `f32` instead of `f64` for values that are single precision on the Wii (`fadds`/`fmuls` results): identical bits, fewer conversions | m2c type inference | Broad, modest | 2–4 days | Only where provably single precision |

### Graphics

| What | Where | Expected | Effort | Risk |
|---|---|---|---|---|
| Per-draw cost (the room issues about 1,240 draws per frame, about 6 vertices each; dirty bits, the TEV translation cache, the plan cache and GPU culling are done): a draw submission of our own instead of citro3d's per-call context update; palette slots shared across draws so that consecutive draws with other matrices/materials stay one batch; templates for the commonest vertex formats; fixed attributes for unused texcoords | `platform/3ds/gx_pica.cpp`, `runtime/hw/gx/` | est. 1.5× on the GPU thread | 2–3 days | Low |
| Vertex path: skip s/q when the texgen is 2×4 (q is exactly 1.0), which is exact. One reciprocal instead of three divides, ×1/255 instead of /255 (±1 LSB). VFP11 divides take 19/33 cycles and aren't pipelined. | `runtime/hw/gx/xform.cpp` | ≥ 38 cycles per textured vertex | Hours | Colours ±1 LSB |
| Textures: GX tiles straight to PICA tiles (no RGBA8 intermediate); I4/I8 as L4/L8 with alpha read from red; CMPR → ETC1/ETC1A4 at staging time, looked up by content hash; hot UI textures in VRAM (not L4 on unit 1) | `runtime/hw/gx/texture.cpp`, `tools/stage_data.py` | Model textures 12.7 → 3.2 MB; faster uploads | ~1 week | ETC1 loses some colour detail |
| EFB copies with the transfer engine instead of a CPU readback mid-frame (Mii faces) | `platform/3ds/gx_pica.cpp` | One GPU stall less per copy | 2–3 days | RGBA8/565/5551/4444 only, ≥ 64 px wide |
| A native nw4r::lyt renderer for the layouts; on the Old 3DS, a frameskip that still decodes GX state | `engine/lyt` | Large | Weeks | Must match lyt materials |

### Loading

| What | Where | Expected | Effort | Exact? |
|---|---|---|---|---|
| Huffman: several short codes per table entry (glyph sheets average about 2 bits a symbol, so the per-symbol work dominates) | `engine/cx/uncompress.cpp` | ≈ 34 → 15 instructions a byte on glyph sheets | 1 day | Yes, with the word-boundary state kept |

### Memory and the Old 3DS

| What | Expected | Effort | Risk |
|---|---|---|---|
| Old 3DS memory diet: guest memory is 72 MB (MEM1 24 MB, MEM2 48 MB). Next: build cold code with `-Os` and `-fipa-icf`. The SDK's keyboard and ATOK are stubs now (above); Then the Bluetooth stack, WPAD and the HOME Button Menu (above); what nothing reaches elsewhere (app, NW4R, MSL, CNT: about 600 functions, 110 KB of PowerPC) could follow. Measured in the room (Azahar): normal heap 68.7 MB used (48 MB of it MEM2), linear 37.8 MB, code 12.7 MB: ~120 MB against the Old 3DS's 96 MB at most. | Needed to run on the Old 3DS at all | Days | Out of memory in untested scenes |

### Determinism

| What | Why |
|---|---|
| `-ffp-contract=off` for host builds | Apple clang fuses `a*b+c` into FMADD by default and the 3DS's VFP never does, so host and 3DS results can differ in the last bit. |

## Ruled out

- **Mixing voices on the 3DS DSP (ndsp).** It runs at 32,728 Hz in 160-sample frames with its own
  interpolation, has 24 channels, and gives no per-3 ms state write-back. It can't be exact and is
  hard to keep in sync. Custom Teak firmware would be weeks of work.
- **Recompressing game data at install.** The SD card is slow and LZ data expands about 3×; fast
  decoders get the gain without it.
- **Maths that needs FMA, NEON, or correctly rounded libraries for game logic.** The ARM11 has
  none of them, and they change results.

## ARM11 kernel bench

Compile an engine kernel with the 3DS build's flags, link a small driver
(`arm-none-eabi-g++ -nostartfiles -Wl,-e,bench`), and run it under Unicorn (`pip install unicorn
pyelftools`). Counting executed instructions per call shows each change's effect without a
device. Running the same inputs through the old and new code and hashing every output compares
the ARM-only paths (DSP intrinsics, inline asm), which the host self-tests can't reach.
