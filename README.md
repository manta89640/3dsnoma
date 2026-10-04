# Wii no Ma for Nintendo 3DS

A port of *Wii no Ma* (Wii Room, the Japanese video-on-demand channel, title `00010001-HCIJ`, as
patched by WiiLink) to the Nintendo 3DS, built as a **decompilation**: the channel's PowerPC
program is machine-decompiled to C++ and compiled for the 3DS's ARM CPU. Below the decompiled
program, a runtime emulates the Wii hardware the program talks to (interrupts, video, audio DSP,
the IOS services, the GX GPU), so the program itself runs unmodified, with its own SDK and
middleware. Where the Wii's own parts have no purpose on a 3DS, native code takes their place:
the Wii Remote is the 3DS's controls (`engine/input/remote.cpp`), the HOME Button Menu is left
out. The content players, Adobe Flash and MobiClip, are left out too (`runtime/patches/players.cpp`):
the shop, whose catalogues were Flash, is hidden from the room (`runtime/patches/menus.cpp`), as are
the menu buttons for the Wii's own settings and services. The port adds a screen of its own to the
room, the Sound Player (`engine/soundplayer`): the channel's music, with the household's Miis
dancing to it.

**No Nintendo data is in this repository.** You need your own copy of the channel: a WAD made
from your own Wii (and your console's keys to decrypt it). Everything generated from it (the
decompiled sources, assembly, extracted files) stays out of git.

## Status

- The whole program decompiles and compiles (30,562 functions, none failing; stubs: the MetroTRK
  debugger, the RSO linker, and Flash and MobiClip's 4,909 functions). Library code is checked against reference vectors (`NOMA_SELFTEST=all`:
  libm, printf, strtol/strtod, 64-bit arithmetic, AES, calendar, the Mii database CRC, the
  paired-single matrix library).
- Host and 3DS (tested in Azahar) both: boot through the SDK, show the
  loading animation, play the channel's sound (the AX voices mixed natively,
  `engine/audio/ax_render.cpp`; streamed through ndsp on the 3DS), take the Wii Remote pointer
  (3DS: touch screen or circle pad), and talk to the network: RC24's
  `/notify` and WiiLink's `conf/first.bin`. "おわる" (the Wii Menu button) shuts the channel down and quits.
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
   Wuhu Island (work in progress) is made from your own Wii Sports Resort disc's files:
   `tools/wuhu/stage_wuhu.py DATA --wsr EXTRACTION` writes `DATA/wuhu/island.ng3d`;
   [docs/wuhu.md](docs/wuhu.md) says what goes into it.

## Running on the host

    cd runs/data && ../../build-host/noma-host --data .

Useful environment variables:

| variable | effect |
| --- | --- |
| `NOMA_DUMP_FRAMES=N` | write every Nth displayed frame to `frames/NNNNN.png` |
| `NOMA_INPUT="3:A,6:A+B:1,8:P(0.2;-0.1)"` | scripted Wii Remote input (time: buttons[: seconds], or pointer), timed by the game's own frames (60 to a second), so the same script works however fast the machine runs; a time can count from a text instead, `M0x91+1.5:A` pressing A 1.5 s after the game first asks for message 0x91, which keeps scripts on their screens wherever loading puts them; `P(off)` takes the pointer off the screen until the next `P(x;y)`; `OFF` presses the Wii's power button (as closing from the HOME Menu does); buttons are `A B 1 2 + - H` and the D-pad's `U D L R`, and the 3DS's own `[L] [R] [ZL] [ZR]` (`5:[L]:2`); for native scenes (the island), `S(x;y)` tilts the stick (the circle pad in stick mode; -1..1, y up) and `C(x;y)` the camera stick (the C-stick), for the event's seconds when given (`4:S(0;1):2.5` walks forward 2.5 s), else until the next `S` (`C`); `S(0;0)` lets go |
| `NOMA_DEBUG_MSG=1` | log each text (BMG message) the game asks for, the first time, with its id and the game time: the screens a run went through, without frames |
| `NOMA_OFFLINE=0` | go online (the default is offline: no network, online steps skipped) |
| `NOMA_PROFILE=seconds` | profiling builds (`-DNOMA_PROFILE_CALLS=ON`): where the time goes; `NOMA_PROFILE_TOP=N` lines per list, `NOMA_PROFILE_UNDER=name` also ranks what runs below that function |
| `NOMA_NO_RENDER=1` | host: don't rasterize (the software renderer dominates host profiles) |
| `NOMA_GX_INLINE=1` | 3DS: decode and draw GX commands on the main thread instead of the GPU's own thread |
| `NOMA_GX_THREAD=1` | host: give the GPU its own thread, as on the 3DS (host runs are inline otherwise) |
| `NOMA_GX_TIMERS=N`, `NOMA_GX_SPIKE_MS=ms` | 3DS: the GPU thread's milliseconds per frame in each part of the draw path over N frames, with counts; frames longer than `ms` (default 100) logged on their own |
| `NOMA_CHECK_PICA_STATE=1` | 3DS: decode every command list as the GPU reads it and compare each draw's PICA registers with what the renderer meant (slow) |
| `NOMA_ETC1=0/1` | 3DS: CMPR textures as ETC1 (a quarter of the memory, lossy, slower to upload); the default is on for the Old 3DS, off for the New 3DS |
| `NOMA_DEBUG_GX_NAN` | report the first matrices holding a NaN (GXLoadPosMtxImm, g3d node matrices, XF loads; runs GX inline) |
| `NOMA_DEBUG_GX`, `_IRQ`, `_IOS`, `_ES`, `_THREADS`, `_POLL`, `_EE`, `_MEM` | logging of the emulated hardware |
| `NOMA_DEBUG_NET`, `NOMA_DEBUG_DRAWS=N` | socket traffic; every draw of the Nth frame (bounds, TEV, textures, matrices) |
| `NOMA_TRACE=word,...`, `NOMA_TRACE_LAST=N`, `NOMA_WATCH=addr` | call tracing and a memory write watch (build with `-DNOMA_TRACE_CALLS=ON`) |
| `NOMA_TRACE_BT=name,addr` | trace builds: the host stack (the guest's call chain) whenever those functions are entered |
| `NOMA_TRACE_OPEN=/dev/...` | print the last traced calls when the guest opens that IOS path (trace builds) |
| `NOMA_DUMP_MEM=1`, `NOMA_DUMP_MEM_OPEN=/dev/...` | write MEM1/MEM2 to `mem1.bin`/`mem2.bin` on exit, or whenever the guest opens that path |
| `NOMA_SELFTEST=all` (or `aes`, `printf`, `math`, `strtol`, `int64`, `time`, `rfl`, `mtx`, `ax`, `cx`, `fill`, `crc`, `efb`, `tex`, `input`) | run decompiled library code on reference vectors, and the natives against the code they replace, then exit (host) |
| `NOMA_SELFTEST=walk` with `NOMA_KCL_DIR=.../StageArc.carc/Island` | Wuhu Island's collision, walking and camera on Wii Sports Resort's own collision files (from your extraction): scripted walks (a wall, the fountain, the volcano trail, a tree, the shore, a ledge, a slope too steep to stand on, the suspension bridge) and 20 minutes of random walking at two Mii sizes, every tick checked (on the ground, in no wall, no deep water, the camera's view clear), and the queries timed; `NOMA_KCL_GROUND=dir` also checks the ground against research rasters, `NOMA_KCL_TRUNKS=file.csv` adds tree trunks, `NOMA_WALK_TRAIL=file.csv` the volcano trail's waypoints, `NOMA_WALK_K` / `NOMA_WALK_K2` set the Mii's scale, `NOMA_WALK_OUT=dir` writes the paths (`tools/wuhu/plot_walks.py` draws them) (host) |
| `NOMA_SELFTEST=g3drender` with `NOMA_G3D_PACKAGE=.../wuhu/island.ng3d` | the host's software backend for native draws (`engine/g3d/soft_backend`): its depth against the GX path's on random triangles, then each `view_*` camera of the package drawn (both passes, under the GX state of a 640x456 frame) to `NOMA_G3D_OUT` (PNGs; default `g3drender`), timed, checked through the GX stream, compared with `tools/g3d/preview_views.py`'s renders of the same cameras when `NOMA_G3D_REF` names their directory (PSNR, pixels off, a difference picture), and the overlays drawn again under a range of depth biases (`NOMA_G3D_FOV`, `NOMA_G3D_FRAME`, `NOMA_G3D_VIEWS`) (host) |
| `NOMA_SOFT3D_THREADS=N` | host: the threads the software backend draws native draws (the island) with (default: as many as the machine has, at most 8) |
| `NOMA_DUMP_AUDIO=file.wav`, `NOMA_DEBUG_AX` | record what the audio DMA plays; AX mixer statistics |
| `NOMA_WUHU_MUSIC_TEST=N,M[,L]` | the island's music without the island: at game time N s the room's music pauses, L s later (1 if not given: the island loading) 「広い世界へ」 plays, M s after that the room's theme comes back; the BGM player's state logged (`tools/music/dump_check.py` reads a recording of it) |
| `NOMA_SLOWDOWN=us`, `NOMA_FS_WRITE_DELAY_US=us`, `NOMA_READ_MBPS=N` | slow guest code / file writes / content reads (to N MB/s) down to reproduce 3DS timing on the host |
| `NOMA_IO_INLINE=1` | read the channel's contents and do the file system's requests on the CPU's thread instead of the I/O thread |
| `NOMA_LOAD_STATS=ms` | log every frame longer than `ms` with what it did (threads, reads by file, decompression, file system, allocations; profile builds: its top functions), each loading burst, and the frame-time distribution every 10 s (`NOMA_LOAD_STATS_READS=1`: every content read; `NOMA_LOAD_STATS_JOBS=1`: every I/O job of 5 ms or more) |

On the 3DS, the same switches go in `sdmc:/3ds/noma/env.txt` as `KEY=VALUE` lines (including
`NOMA_INPUT`, for unattended runs in an emulator); there `NOMA_SHOT_EVERY=N` also saves every
Nth frame as a BMP in `sdmc:/3ds/noma/shots`.

## Controls on the 3DS

The 3DS stands in for a Wii Remote pointed at the screen: touch the bottom screen (or use the
circle pad) to point, A/B as A/B, X = 1, Y = 2, START = +, SELECT = −, the D-pad as the D-pad.
Left alone for 3 seconds, the pointer leaves the screen as a remote put down would, so the room's
menus (その他, the bottom bar) close; the next touch brings it back.
The game still reads a Wii Remote (KPAD), but nothing of one is emulated: `engine/input/remote.cpp`
answers KPAD and WPAD with the 3DS's controls, so the Wii's Bluetooth stack never starts, and
there's no remote speaker or rumble.
The Wii's HOME Button Menu is not part of the port (the 3DS's own HOME button suspends the app):
`engine/system/home_button.cpp` keeps it from loading or opening. Closing the app from the HOME
Menu presses the Wii's power button for the game, which ends as it does on the Wii (a save in
progress finishes first); the app ends on its own if the game hasn't within 10 seconds.

## The Sound Player

A screen of the port's own for listening to the channel's music: the sixth button of the room's
bottom bar (サウンド, right of いろんな間) opens it, through the room's own circle wipe, and the Home
tab at the bottom left (or B) goes back to the room, whose music comes back by itself.

- The tracks are the channel's own, found in its sound archive (`snd/douga.brsar`, which the
  channel keeps in memory): the 43 pieces of music (the sequences its BGM player plays, labelled
  `SEQ_BGM_*`; the silent placeholder and the one filed twice left out), then 17 jingles (company
  logos, the Miis' feelings, the 5 pm chime). The archive has only labels; the list's names are
  those put into words. They play through the channel's own BGM player (`sound::BGMManager`),
  which crossfades between pieces and keeps the room's theme paused until the room asks for it
  again.
- Left: the list, in the channel's list window with the message board's rows (the room's cyan
  for focus), scrolling a row at a time (the tabs on its edges, held, or the D-pad). The track
  playing has a cyan bar with ♪ (grey with ∥ when paused). Right: the title, and the video
  player's own controls (previous, play/pause, next, stop).
- The household's Miis dance at a table in the TV programme's white studio, an amp beside them,
  every family member (one to eight) placed so that none hides another. The dances are the
  channel's own (the TV caster's, the DSi relay's, the messages' cheers), their frames set from
  the music's sequencer every frame, so the moves land on the beat at any frame rate; paused or
  stopped, the Miis wait calmly. The amp is drawn by the port (no such model exists in the
  channel), its speaker and light pulsing on the beat.
- Buttons: the pointer and A as everywhere; the D-pad's up and down move the list's highlight
  and A plays it (with the pointer away), left and right play the previous and next track,
  + pauses and resumes, − stops, B goes back.

It is a scene of its own (`engine/soundplayer`), made in native code but run by the channel's
scene manager like the theater or the options: `engine/system/host_scene.cpp` gives the
channel's `lib::Scene` a native factory and virtuals, through guest code addresses for native
functions (`guest::host_fn_address`, `runtime/dispatch.cpp`). The bar's button is added where the
bar makes its own (`runtime/patches/menus.cpp`). `NOMA_SP_BEATS=1` logs where the music is and
what each Mii plays, every frame.

## Layout

- `engine/` — native 3DS code in place of the program's SDK functions (the base of the engine
  the port is becoming): `os/` (interrupts, clock), `cw/` (compiler runtime helpers), `input/`
  (the controls), `audio/` (the AX voices), `system/` (threads, the HOME Button Menu left out,
  scenes written in native code), `ui/` (what the native scenes share: the channel's layouts,
  buttons, wipes and 3D pieces as they use them, pictures made at run time), `soundplayer/` (the
  Sound Player), `wuhu/` (walkable Wuhu Island, in progress: `docs/wuhu_design.md`; so far the
  bar's 出発 button and the scene's shell).
- `runtime/` — the Wii below the program: memory, threads, interrupts, CPU intrinsics,
  devices (`hw/`: PI, VI, DSP, AI, EXI, SI, IPC, GX in `hw/gx/`), IOS (`hw/ios/`: file system,
  ES, USB HID with nothing plugged in, ...); `patches/` changes what
  some decompiled functions do
  (offline mode, debugging aids; the original stays as `<name>_orig`), `profile.cpp` the
  sampling profiler.
- `platform/host`, `platform/3ds` — entry points, presentation, input; `gx_pica.cpp` is the GX
  renderer on the 3DS GPU.
- `tools/` — WAD extraction, signature inference, the decompilation driver.
- `tools/music/` — the port's own music: the island's score (`hiroi_sekai.py`), an RSEQ assembler,
  disassembler and score compiler (`rseq.py`; `rseq.py header` makes `engine/wuhu/hiroi_sekai_seq.h`),
  a reader of the channel's sound archive and a model of its sequencer (`brsar.py`), a preview
  renderer with the channel's own instruments (`sndrender.py`), checks of a score and of a
  recording of the port playing it (`check.py`, `dump_check.py`). They read the channel's
  `snd/douga.brsar` from your own extraction.
- `config/HCIJ` — dtk configuration and symbols.
