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
| 3DS texture memory: compact PICA formats (LA4/LA8/RGB565/RGBA5551) instead of RGBA8, LRU eviction | `platform/3ds/gx_pica.cpp` | Glyphs and textures fit again. |
| Fused vertex path, indexed batched draws | `platform/3ds/gx_pica.cpp`, `runtime/hw/gx/` | Room ≈ 14.7 fps in Azahar. |

## In progress

| What | Where | Expected | Notes |
|---|---|---|---|
| Measure the block renderer in the room on the 3DS, then move mixing back to its own thread | `runtime/hw/dsp_hle.cpp`, `platform/3ds/main.cpp` | Audio off the main thread on the New 3DS | The thread exists and is disabled on the 3DS. With the old renderer it took most of each frame, and `dsp_ax_wait()` busy-spun the main thread. Before turning it on: block on an event instead of spinning. |

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

### Guest code

| What | Where | Expected | Effort | Risk |
|---|---|---|---|---|
| Compile `noma_decomp` at `-O2` (or `-Os` for cold objects). `-O1` has no instruction scheduling, which hurts on the in-order ARM11, and no small-function inlining. | `CMakeLists.txt` | est. 10–30% on all guest code | Hours plus compile time | Undefined behaviour in generated code may surface: run every self-test |
| Gather-pipe fast path: about 2,100 decomp stores go to the constant address 0xCC008000 through store → mmio_write → gx_fifo_write's byte loop. Append inline when `__builtin_constant_p(addr)` (Dolphin's FastWrite). | `runtime/guest.h`, `runtime/hw/gx/fifo.cpp` | est. 5–10× cheaper per GX write | Hours | None |
| Fold other constant-address MMIO at compile time. A biased page table (`table[a >> 24] + a`) saves the mask in every load and store. | `runtime/guest.h` | Smaller, faster accessors | Medium | Low |
| Cheaper polls: the function-entry and loop polls (`NOMA_LOOP`) read a flag a timer sets, instead of decrementing a global | runtime | Small, everywhere | 1 day | Low |
| Profile-guided code layout: order `.text.<fn>` sections from the sampling profiler (22.6 MB of code against a 16 KB instruction cache), then GCC PGO with `.gcda` files written to the SD card | linker script | est. 5–15% | 2–4 days | None |
| `f32` instead of `f64` for values that are single precision on the Wii (`fadds`/`fmuls` results): identical bits, fewer conversions | m2c type inference | Broad, modest | 2–4 days | Only where provably single precision |

### Graphics

| What | Where | Expected | Effort | Risk |
|---|---|---|---|---|
| Per-draw cost (the room issues about 1,600 draws per frame, about 6 vertices each, in about 108 batches): dirty bits per piece of PICA state; a cache of TEV→texenv translations keyed by the TEV registers; the per-draw plan cached until XF/CP registers change; templates for the commonest vertex formats; PICA culling chosen by the viewport's sign; a static quad index pattern; fixed attributes for unused texcoords (44 → 28-byte vertices) | `platform/3ds/gx_pica.cpp`, `runtime/hw/gx/` | est. ≥ 2× on the GPU thread | 3–5 days | Low (culling winding must follow viewport flips) |
| Vertex path: skip s/q when the texgen is 2×4 (q is exactly 1.0), which is exact. One reciprocal instead of three divides, ×1/255 instead of /255 (±1 LSB). VFP11 divides take 19/33 cycles and aren't pipelined. | `runtime/hw/gx/xform.cpp` | ≥ 38 cycles per textured vertex | Hours | Colours ±1 LSB |
| Transform and lighting on the PICA: one shader with the GX matrices as a uniform palette (MOVA; two consecutive MOVAs hang the GPU), lights in a LOOP, projective texgen through texcoord0w | new `.v.pica` | Per-vertex CPU cost near zero for 3D | 1–2 weeks | 24-bit floats; multi-pass depth must come from one path |
| Display-list geometry cache: each DL converted once into PICA buffers, keyed by address, size, content hash, VCD/VAT and array bases | `runtime/hw/gx/cp.cpp` | 3D model decoding near zero | 1–2 weeks after T&L | Invalidation |
| Textures: GX tiles straight to PICA tiles (no RGBA8 intermediate); I4/I8 as L4/L8 with alpha read from red; CMPR → ETC1/ETC1A4 at staging time, looked up by content hash; hot UI textures in VRAM (not L4 on unit 1) | `runtime/hw/gx/texture.cpp`, `tools/stage_data.py` | Model textures 12.7 → 3.2 MB; faster uploads | ~1 week | ETC1 loses some colour detail |
| EFB copies with the transfer engine instead of a CPU readback mid-frame (Mii faces) | `platform/3ds/gx_pica.cpp` | One GPU stall less per copy | 2–3 days | RGBA8/565/5551/4444 only, ≥ 64 px wide |
| A native nw4r::lyt renderer for the layouts; on the Old 3DS, a frameskip that still decodes GX state | `engine/lyt` | Large | Weeks | Must match lyt materials |

### Loading

| What | Where | Expected | Effort | Exact? |
|---|---|---|---|---|
| LZ10/LZ11: a fast loop (a flag byte per iteration, 4-byte match copies when distance ≥ 4) while ≥ 33 input bytes and room remain, with the exact streaming code for the tail. Must never write past the output size (it's guest memory). | `engine/cx/uncompress.cpp` | est. 2–4× on 44 MB of LZ data | 1–2 days | Yes |
| Huffman: a 2^10 lookup table built from the tree, refilled a word at a time | same | est. 4–6× | 1–2 days | Yes |
| Asynchronous file reads on a worker thread, replying through `ios_complete`, straight into guest RAM (no temporary vector) | `runtime/hw/ios/fs.cpp` | Loads overlap decompression and animation; a read no longer blocks every guest thread | 2–3 days | Reply ordering |

### Memory and the Old 3DS

| What | Expected | Effort | Risk |
|---|---|---|---|
| Old 3DS memory diet: guest memory alone takes 96 MB (MEM1 as 32 MB for the 16 MB pages, plus MEM2), which is the whole Old 3DS application region. Use 1 MB pages so only MEM1's 24 MB is backed, measure the game's MEM2 high-water mark, and build cold code with `-Os` and `-fipa-icf`. | Needed to run on the Old 3DS at all | ~1 week | Out of memory in untested scenes |

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
