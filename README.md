# Wii no Ma for Nintendo 3DS

A port of *Wii no Ma* (Wii Room, the Japanese video-on-demand channel, title `00010001-HCIJ`, as
patched by WiiLink) to the Nintendo 3DS, built as a **decompilation**: the channel's PowerPC
program is machine-decompiled to C++ and compiled for the 3DS's ARM CPU. Below the decompiled
program, a runtime emulates the Wii hardware the program talks to (interrupts, video, audio DSP,
the IOS services, the GX GPU), so the program itself runs unmodified, with its own SDK and
middleware. Where the Wii's own parts have no purpose on a 3DS, native code takes their place:
the Wii Remote is the 3DS's controls (`engine/input/remote.cpp`), the HOME Button Menu is left
out.

**No Nintendo data is in this repository.** You need your own copy of the channel: a WAD made
from your own Wii (and your console's keys to decrypt it). Everything generated from it (the
decompiled sources, assembly, extracted files) stays out of git.

## Status

- The whole program decompiles and compiles (35,666 functions, none failing; only the MetroTRK
  debugger and the RSO linker are stubs). Library code is checked against reference vectors (`NOMA_SELFTEST=all`:
  libm, printf, strtol/strtod, 64-bit arithmetic, AES, calendar, the Mii database CRC, the
  paired-single matrix library).
- Host and 3DS (tested in Azahar) both: boot through the SDK, show the strap safety screen and
  the loading animation, play the channel's sound (the AX voices mixed natively,
  `engine/audio/ax_render.cpp`; streamed through ndsp on the 3DS), take the Wii Remote pointer
  (3DS: touch screen or circle pad), and talk to the network: RC24's
  `/notify` and WiiLink's `conf/first.bin`. "Wii Menu" shuts the channel down and quits.
- Offline by default (`NOMA_OFFLINE`, `=0` to go online): there is no network, and the channel's
  online steps are skipped inside its own flow (`runtime/patches/offline.cpp`): its start-up
  downloads finish as empty documents, the license agreement is left out, and what only a server
  provides (videos, posters, messages, TV listings) is absent. The opening animation plays to its
  end (online it plays while the start-up data downloads), and what needs a server shows the
  channel's own "can't connect" message (Shopping, a video sample: error 50220). Online, WiiLink's server answers
  the WAD this was developed on with error 354607 (its patch predates the server's current
  User-Agent check).
- The first start's setup runs from the welcome through the Mii, profile, family and video
  settings into the room (LivingRoomScene), its tutorial and menus (host; the 3DS reaches the room
  too, slowly). The console clock is the host's local time: the room shows today's date.
- Miis: on the 3DS the system Mii Selector runs at the first start (or with SELECT held), and
  the chosen Mii goes into the Wii Mii database the channel's face library reads.
- EFB copies to textures work on the 3DS GPU too (the render target is read back mid-frame);
  `NOMA_EFB_COPY_TEST=N` in env.txt checks the path on frame N.
- 3DS textures take the smallest PICA format that holds them exactly (fonts as LA4: a quarter of
  RGBA8), and under memory pressure the least recently used ones make room; guest memory fills most
  of the linear heap. Every 600 frames the log reports textures, memory and vertices.
- The engine: native 3DS code is replacing the program's SDK and library functions, hot paths
  first (`engine/`): the OS's interrupt control, clock and mutexes, CodeWarrior's 64-bit helpers,
  CX decompression, the MTX/VEC/QUAT library and nw4r::math, MSL's sqrt/floor/ceil, nw4r::snd's
  voices, channels and sequencer, nw4r::lyt's animation curves and pane matrices, nw4r::ut's glyph
  lookup. The AX voices play on the 3DS's DSP (`engine/audio`; looped DSP-ADPCM is decoded to PCM16
  once, as the 3DS's DSP can't start a loop inside an ADPCM frame), and the Wii's HOME Button Menu
  is not part of the port (`engine/system`).
- Speed: the decompiled code is unoptimised and the 3DS is slow for it. The channel's sound
  thread (priority 4, woken every 3 ms AX frame) can't finish a frame in time there, so the
  audio DMA holds the next frame back while threads above the main thread's priority have used
  more than half the time since the last one: audio plays late instead of starving the game
  (`NOMA_AUDIO_GUARD=0` turns this off); with the voices on the 3DS's DSP, frames the CPU took late
  are made up for (up to 100 ms), as they set the music's tempo. On the 3DS the GPU has a thread
  of its own on another core (`runtime/hw/gx/fifo.cpp`): it decodes the command stream and draws
  while the main thread runs the guest. A profiling build (`-DNOMA_PROFILE_CALLS=ON`) logs
  where the time goes every `NOMA_PROFILE=seconds`: per guest thread, and per function (self
  and total), with the runtime's own work as `(gx)`, `(ax)`, `(ios)`, `(devices)`, `(idle)`.
- Not yet: the speed work itself
  (next step), and the channel's dynamically linked Opera
  browser module (`wwwlib-rvl.lz7`, an RSO module with ~4.8 MB of PowerPC code, linked through
  `rso/douga.sel`): the RSO linker is stubbed, so whatever the channel shows with the browser
  won't work until that module is decompiled too.

## Building

Requirements: Python 3, `openssl`, CMake ≥ 3.20 and Ninja, a C++17 compiler (host build),
[decomp-toolkit](https://github.com/encounter/decomp-toolkit) (`dtk`), the m2c decompiler with
this project's patches, and for the 3DS devkitARM with libctru and citro3d.

1. Extract your WAD and prepare the program:

       tools/wad_extract.py wii-no-ma-jp.wad extracted/wad --key keys.bin   # BootMii keys.bin
       tools/unpack_all.py extracted/wad extracted/fs
       tools/prepare_dol.py extracted/fs/00000025.dec orig/HCIJ/sys/main.dol

   (`orig/HCIJ/files` needs the channel's `rso/douga.sel` from the unpacked contents.)

2. Split it into assembly and decompile:

       dtk dol split config/HCIJ/config.yml build/HCIJ
       tools/infer_signatures.py      # register-ABI prototypes for every function (~10 min)
       tools/infer_call_hints.py      # int/float results of calls through pointers
       tools/decomp/decompile.py      # m2c on every function -> build/decomp (~5 min)

   The decompiler and the compiler both work in parts of at most 400 functions (big objects
   become `build/decomp/src/<object>_<n>.cpp`), so every core stays busy; m2c's results are
   cached per part.

   Symbols get their real names as the work reaches them: `tools/name_symbol.py OLD=NEW ...`
   renames them in `config/HCIJ/symbols.txt` and in the hand-written code, and splits again;
   then run `tools/decomp/decompile.py` (prototypes are kept by address).

3. Build:

       cmake -S . -B build-host -G Ninja && cmake --build build-host           # macOS/Linux
       cmake -S . -B build-3ds -G Ninja \
             -DCMAKE_TOOLCHAIN_FILE=$PWD/platform/3ds/cmake/Toolchain-3DS.cmake
       cmake --build build-3ds                                                # 3DS

4. Put the data where the program looks for it (`sdmc:/3ds/noma` on the 3DS):

       tools/stage_data.py runs/data                       # host
       tools/stage_data.py /path/to/sdcard/3ds/noma        # 3DS

   Optionally `--rfl-db RFL_DB.dat` with the Mii database from your Wii's NAND
   (`/shared2/menu/FaceLib`); on the 3DS the Mii Selector makes one at the first start.
   For sound on the 3DS, ndsp needs `sdmc:/3ds/dspfirm.cdc` (dumped with DSP1).

## Running on the host

    cd runs/data && ../../build-host/noma-host --data .

Useful environment variables:

| variable | effect |
| --- | --- |
| `NOMA_DUMP_FRAMES=N` | write every Nth displayed frame to `frames/NNNNN.png` |
| `NOMA_INPUT="3:A,6:A+B:1,8:P(0.2;-0.1)"` | scripted Wii Remote input (time: buttons[: seconds], or pointer), timed by the game's own frames (60 to a second), so the same script works however fast the machine runs |
| `NOMA_OFFLINE=0` | go online (the default is offline: no network, online steps skipped) |
| `NOMA_PROFILE=seconds` | profiling builds (`-DNOMA_PROFILE_CALLS=ON`): where the time goes |
| `NOMA_NO_RENDER=1` | host: don't rasterize (the software renderer dominates host profiles) |
| `NOMA_GX_INLINE=1` | 3DS: decode and draw GX commands on the main thread instead of the GPU's own thread |
| `NOMA_GX_THREAD=1` | host: give the GPU its own thread, as on the 3DS (host runs are inline otherwise) |
| `NOMA_DEBUG_GX_NAN` | report the first matrices holding a NaN (GXLoadPosMtxImm, g3d node matrices, XF loads; runs GX inline) |
| `NOMA_DEBUG_GX`, `_IRQ`, `_IOS`, `_ES`, `_THREADS`, `_POLL`, `_EE`, `_MEM` | logging of the emulated hardware |
| `NOMA_DEBUG_NET`, `NOMA_DEBUG_DRAWS=N` | socket traffic; every draw of the Nth frame (bounds, TEV, textures, matrices) |
| `NOMA_TRACE=word,...`, `NOMA_TRACE_LAST=N`, `NOMA_WATCH=addr` | call tracing and a memory write watch (build with `-DNOMA_TRACE_CALLS=ON`) |
| `NOMA_TRACE_BT=name,addr` | trace builds: the host stack (the guest's call chain) whenever those functions are entered |
| `NOMA_TRACE_OPEN=/dev/...` | print the last traced calls when the guest opens that IOS path (trace builds) |
| `NOMA_DUMP_MEM=1`, `NOMA_DUMP_MEM_OPEN=/dev/...` | write MEM1/MEM2 to `mem1.bin`/`mem2.bin` on exit, or whenever the guest opens that path |
| `NOMA_SELFTEST=all` (or `aes`, `printf`, `math`, `strtol`, `int64`, `time`, `rfl`, `mtx`) | run decompiled library code on reference vectors and exit (host) |
| `NOMA_DUMP_AUDIO=file.wav`, `NOMA_DEBUG_AX` | record what the audio DMA plays; AX mixer statistics |
| `NOMA_SLOWDOWN=us`, `NOMA_FS_WRITE_DELAY_US=us` | slow guest code / file writes down to reproduce 3DS timing on the host |

On the 3DS, the same switches go in `sdmc:/3ds/noma/env.txt` as `KEY=VALUE` lines (including
`NOMA_INPUT`, for unattended runs in an emulator); there `NOMA_SHOT_EVERY=N` also saves every
Nth frame as a BMP in `sdmc:/3ds/noma/shots`.

## Controls on the 3DS

The 3DS stands in for a Wii Remote pointed at the screen: touch the bottom screen (or use the
circle pad) to point, A/B as A/B, X = 1, Y = 2, START = +, SELECT = −, the D-pad as the D-pad.
The game still reads a Wii Remote (KPAD), but nothing of one is emulated: `engine/input/remote.cpp`
answers KPAD and WPAD with the 3DS's controls, so the Wii's Bluetooth stack never starts, and
there's no remote speaker or rumble.
The Wii's HOME Button Menu is not part of the port (the 3DS's own HOME button suspends the app):
`engine/system/home_button.cpp` keeps it from loading or opening.

## Layout

- `engine/` — native 3DS code in place of the program's SDK functions (the base of the engine
  the port is becoming): `os/` (interrupts, clock), `cw/` (compiler runtime helpers), `input/`
  (the controls), `audio/` (the AX voices), `system/` (threads, the HOME Button Menu left out).
- `runtime/` — the Wii below the program: memory, threads, interrupts, CPU intrinsics,
  devices (`hw/`: PI, VI, DSP, AI, EXI, SI, IPC, GX in `hw/gx/`), IOS (`hw/ios/`: file system,
  ES, USB HID with nothing plugged in, ...); `patches/` changes what
  some decompiled functions do
  (offline mode, debugging aids; the original stays as `<name>_orig`), `profile.cpp` the
  sampling profiler.
- `platform/host`, `platform/3ds` — entry points, presentation, input; `gx_pica.cpp` is the GX
  renderer on the 3DS GPU.
- `tools/` — WAD extraction, signature inference, the decompilation driver.
- `config/HCIJ` — dtk configuration and symbols.
