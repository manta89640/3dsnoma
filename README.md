# Wii no Ma for Nintendo 3DS

A port of *Wii no Ma* (Wii Room, the Japanese video-on-demand channel, title `00010001-HCIJ`, as
patched by WiiLink) to the Nintendo 3DS, built as a **decompilation**: the channel's PowerPC
program is machine-decompiled to C++ and compiled for the 3DS's ARM CPU. Below the decompiled
program, a runtime emulates the Wii hardware the program talks to (interrupts, video, audio DSP,
the IOS services, the GX GPU), so the program itself runs unmodified, with its own SDK and
middleware. Where the Wii's own parts have no purpose on a 3DS, native code takes their place:
the Wii Remote is the 3DS's controls (`engine/input/remote.cpp`), the HOME Button Menu is left
out. The content players, Adobe Flash and MobiClip, are left out too (`runtime/patches/players.cpp`).
The port is offline, and the channel's scenes that only the servers fed are left out
(`engine/system/scrapped_scenes.cpp`): いろんな間, the shop, the TV programmes, the votes, deliveries
and coupons, the browser, and the Wii's own settings and online checks; their buttons are hidden
(`runtime/patches/menus.cpp`). The port adds a screen of its own to the
room, the Sound Player (`engine/soundplayer`): the channel's music, with the household's Miis
dancing to it.

**No Nintendo data is in this repository.** You need your own copy of the channel: a WAD made
from your own Wii (and your console's keys to decrypt it). Everything generated from it (the
decompiled sources, assembly, extracted files) stays out of git.

## Status

- The whole program decompiles and compiles (35,941 functions at the last decompile run: 27,492
  decompiled, none failing, 285 native; 8,164 stubs: the MetroTRK debugger, the RSO linker, Flash
  and MobiClip, the SDK's keyboard and ATOK, and what nothing reaches any more of the Wii Remote's
  Bluetooth stack, WPAD, the HOME Button Menu and the USB keyboard). Library code is checked against
  reference vectors and the natives against the code they replace (`NOMA_SELFTEST=all`: libm,
  printf, strtol/strtod, 64-bit arithmetic, AES, calendar, the Mii database, the paired-single
  matrix library, the AX voices, CX, the heap fill, CRC, the stack scan, EFB copies, textures, the
  native g3d; see the table below).
- Host and 3DS (tested in Azahar) both: boot through the SDK, show the
  loading animation, play the channel's sound (the AX voices mixed natively,
  `engine/audio/ax_render.cpp`; streamed through ndsp on the 3DS), take the Wii Remote pointer
  (3DS: touch screen or circle pad). "おわる" (the Wii Menu button) shuts the channel down and quits.
- Offline: there is no network (`runtime/hw/ios/net.cpp`: configured, its link down) and no
  online mode. The channel's online steps are skipped inside its own flow
  (`runtime/patches/offline.cpp`): its start-up downloads finish as empty documents, the license
  agreement is left out, and what only a server provides (videos, posters, messages) is absent.
  The opening animation plays to its end (online it played while the start-up data downloaded),
  and what needs a server shows the channel's own "can't connect" message (a video sample: error
  50220). The scenes only the servers fed are left out (above): what only they reached isn't
  compiled (`tools/decomp/decompile.py` LEFT_OUT: about 3,600 functions).
- The first start's setup runs from the welcome through the Mii, profile and family into the room
  (LivingRoomScene), its tutorial and menus; the video settings step (streaming quality) is left
  out (`runtime/patches/wii_parts.cpp`). The console clock is the host's local time: the room shows today's date.
- Miis: on the 3DS the Miis of the system's Mii Maker database (`CFL_DB.dat`) are synced into
  the Wii Mii database the channel's face library reads, at every start (`runtime/mii.cpp`; parts
  the Wii doesn't have become the nearest Wii ones). Where that database can't be read, the
  system Mii Selector runs when there is no Mii database yet, or with SELECT held, and the chosen
  Mii goes in.
- EFB copies to textures work on the 3DS GPU too (the render target is read back mid-frame);
  `NOMA_EFB_COPY_TEST=N` in env.txt checks the path on frame N.
- 3DS textures take the smallest PICA format that holds them exactly (the fonts' glyph sheets as
  L4: an eighth of RGBA8), and under memory pressure the least recently used ones make room; MEM1
  is part of the program's own memory, MEM2 is in the normal heap (the linear heap only if 64 MB of
  it is free). Every 600 frames the log reports textures, memory and vertices.
- The engine: native 3DS code is replacing the program's SDK and library functions, hot paths
  first (`engine/`): the OS's interrupt control, clock and mutexes, CodeWarrior's 64-bit helpers,
  CX decompression, the MTX/VEC/QUAT library and nw4r::math, MSL's sqrt/floor/ceil, nw4r::snd's
  voices, channels and sequencer, nw4r::lyt's animation curves, pane matrices, quads, material
  set-up and line measuring, nw4r::ut's glyph lookup, the GX library's hottest calls, nw4r::g3d's
  model draws (`engine/g3d/live_draw.cpp`), the heap fill and search, the CRC and the stack scan
  (the list: `tools/decomp/decompile.py` NATIVE_NAMES and PATCHED_NAMES; what they gained:
  `docs/optimisations.md`). The AX voices are mixed by native code (`engine/audio/ax_render.cpp`)
  and the 3DS's DSP plays the mix, and the Wii's HOME Button Menu is not part of the port
  (`engine/system`).
- Speed: the decompiled code is unoptimised and the 3DS is slow for it. The channel's sound
  thread (priority 4, woken every 3 ms AX frame) can't finish a frame in time there, so the
  audio DMA holds the next frame back while threads above the main thread's priority have used
  more than half the time since the last one: audio plays late instead of starving the game
  (`NOMA_AUDIO_GUARD=0` turns this off); AX frames the CPU took late are made up for (up to
  100 ms), as they set the music's tempo. On the 3DS the GPU has a thread
  of its own on another core (`runtime/hw/gx/fifo.cpp`): it decodes the command stream and draws
  while the main thread runs the guest. On a real New 3DS the AX frames are also mixed on a core
  of their own (the system core; given up for mixing on the game's thread when the game waits for
  it longer than the mixing takes, and tried again 30 s later), and Wuhu Island's World makes its frame on the third core beside
  the GPU's thread (`platform/3ds/main.cpp` has the layout; `NOMA_CORES` below). With `NOMA_DEBUG_CONSOLE=1`
  the bottom screen, while no screen of the channel's uses it, shows the log under a status area
  that shows, once a second, the game's frames a second, then milliseconds a frame: the
  game's CPU time (its thread, less its waits), its waits, the GPU's drawing time; below it the AX
  mixing's milliseconds a frame and its core (`c0`: the game's own; on a thread, `w`: the game's
  waits for it) and the heap in use of its size in MB (without it the bottom screen is white then,
  and only a fatal error is written there); the log gets the same every 10 s
  (`NOMA_STATUS_LOG`), as `status:` lines. A profiling build (`-DNOMA_PROFILE_CALLS=ON`) logs
  where the time goes every `NOMA_PROFILE=seconds`: per guest thread, and per function (self
  and total), with the runtime's own work as `(gx)`, `(ax)`, `(ios)`, `(devices)`, `(idle)`.
- Not yet: the channel's dynamically linked Opera
  browser module (`wwwlib-rvl.lz7`, an RSO module with ~4.8 MB of PowerPC code, linked through
  `rso/douga.sel`): the RSO linker is stubbed, so whatever the channel shows with the browser
  won't work until that module is decompiled too. Nor the help's videos (使い方): they were the
  server's, streamed (the channel's content holds only their thumbnails), and MobiClip is left out,
  so choosing one ends it at once and the list stays (`runtime/patches/players.cpp`).

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
   `tools/wuhu/stage_wuhu.py DATA --wsr EXTRACTION` writes `DATA/wuhu/island.ng3d` (NG3D version 2:
   a package staged before the speed round is refused, the reason in the log; stage it again), baked with
   the host build's `ng3d-bake` and with its cells' visibility from `ng3d-pvs` (build the host
   targets first); [docs/wuhu.md](docs/wuhu.md) says what goes into it.
   Wii Sports' and Wii Sports Resort's music for the Sound Player, from the same disc:
   `tools/music/stage_music.py DATA [--extract ROOT]` (ROOT: the disc split into `launcher/`,
   `wii-sports/`, `wii-sports-resort/`; default `../wii-sports-extract`) writes
   `DATA/music/sports.tsv` and `DATA/music/sports/*.nstm` (about 150 MB), encoded by the host
   build's `nstm-enc`.

## Running on the host

    cd runs/data && ../../build-host/noma-host --data .

Useful environment variables:

| variable | effect |
| --- | --- |
| `NOMA_DUMP_FRAMES=N` | write every Nth displayed frame to `frames/NNNNN.png` |
| `NOMA_DUMP_FROM=F` | draw nothing before displayed frame F (with `NOMA_DUMP_FRAMES`: a later screen reached at the game's own speed) |
| `NOMA_INPUT="3:A,6:AB:1,8:P(0.2;-0.1)"` | scripted Wii Remote input (time: buttons[: seconds], or pointer), timed by the game's own frames (60 to a second), so the same script works however fast the machine runs; a time can count from a text instead, `M0x91+1.5:A` pressing A 1.5 s after the game first asks for message 0x91, which keeps scripts on their screens wherever loading puts them; `P(off)` takes the pointer off the screen until the next `P(x;y)`; `OFF` presses the Wii's power button (as closing from the HOME Menu does); buttons, written together, are `A B 1 2 + - H` and the D-pad's `U D L R` (`+` is the PLUS button, not a separator: `A+B` holds A, PLUS and B), the 3DS's own `[L] [R] [ZL] [ZR]` (`5:[L]:2`), and the 3DS's names for the others as the platform maps them, `[X]` (1), `[Y]` (2), `[START]` (+), `[SELECT]` (−); `T(x;y)` touches the bottom screen at pixel x, y (0..319, 0..239) for the event's seconds (a tap: 0.15 s), moving the pointer as the touch screen does, and `T(x0;y0;x1;y1)` drags over them; for native scenes (the island), `S(x;y)` tilts the stick (the circle pad in stick mode; -1..1, y up) and `C(x;y)` the camera stick (the C-stick), for the event's seconds when given (`4:S(0;1):2.5` walks forward 2.5 s), else until the next `S` (`C`); `S(0;0)` lets go |
| `NOMA_DEBUG_MSG=1` | log each text (BMG message) the game asks for, the first time, with its id and the game time: the screens a run went through, without frames |
| `NOMA_PROFILE=seconds` | profiling builds (`-DNOMA_PROFILE_CALLS=ON`): where the time goes; `NOMA_PROFILE_TOP=N` lines per list, `NOMA_PROFILE_UNDER=name` also ranks what runs below that function |
| `NOMA_NO_RENDER=1` | host: don't rasterize (the software renderer dominates host profiles) |
| `NOMA_GX_INLINE=1` | 3DS: decode and draw GX commands on the main thread instead of the GPU's own thread |
| `NOMA_GX_THREAD=1` | host: give the GPU its own thread, as on the 3DS (host runs are inline otherwise) |
| `NOMA_GX_TIMERS=N`, `NOMA_GX_SPIKE_MS=ms` | 3DS: the GPU thread's milliseconds per frame in each part of the draw path over N frames, with counts; frames longer than `ms` (default 100) logged on their own |
| `NOMA_CHECK_PICA_STATE=1` | 3DS: decode every command list as the GPU reads it and compare each draw's PICA registers with what the renderer meant (slow) |
| `NOMA_CHECK_G3D_LIVE=1` | host: the game's own model draws (nw4r::g3d DrawResMdlDirectly, `engine/g3d/live_draw.cpp` draws them natively otherwise), with the native derivation of each (`engine/g3d/live_*`: the shapes' vertices and matrices, the materials' TEV, textures, pixel state, channels, lights, texgens and texture matrices) compared draw by draw with the GPU registers the game's draw leaves; `=native`: the native draws, compared with the derivation as the GPU's decoder takes them; `=ab`: the native draws in even game frames and the game's in odd ones (dump frames two in a row to see the same scene drawn both ways). `NOMA_CHECK_G3D_LIVE_MUTATE=1` breaks one register on purpose: it must be caught |
| `NOMA_TEST_CARD=1` | a calibration card for the screens, once, right after the room is first made (`engine/ui/test_card.h`): the channel's text at 8 to 24 pixels high on the top screen and 8 to 16 on the bottom one, each row naming its size, and the channel's small buttons at 24 to 56 pixels square to tap (each counts its taps); B or the Home tab goes back to the room. For setting the smallest text and touch targets from what reads and hits well on the console |
| `NOMA_TOP_SSAA=1` | 3DS: the top screen drawn at twice its size each way (800x480) and filtered down to it by the display transfer: smooth edges (the Miis' outlines, the room's frames), sharper text, and the frame's copies to textures (the Mii faces) from twice the samples; four times the pixels for the GPU to fill and 3 MB of VRAM, so off by default until timed on a console (falls back to 400x240 without the VRAM) |
| `NOMA_BOTTOM_SSAA=1` | 3DS: the same for the bottom screen (640x480, 2.4 MB of VRAM): smoother text there, and the lists' separator lines all drawn (at 1x the next row's bar can start on a line's pixel row and cover it); not together with `NOMA_TOP_SSAA` beside the island's shadow map |
| `NOMA_TEX_MIPS=0` | 3DS: no smaller images (mipmaps) for the grey textures (GX I4, I8, IA4, IA8: the font's glyph sheets above all), which otherwise get them at upload and are sampled between them, so text drawn at a third of its sheet's size keeps its strokes |
| `NOMA_ETC1=0/1` | 3DS: CMPR textures as ETC1 (a quarter of the memory, lossy, slower to upload); the default is on for the Old 3DS, off for the New 3DS |
| `NOMA_DEBUG_GX_NAN` | report the first matrices holding a NaN (GXLoadPosMtxImm, g3d node matrices, XF loads; runs GX inline) |
| `NOMA_DEBUG_GX`, `_IRQ`, `_IOS`, `_ES`, `_THREADS`, `_POLL`, `_EE`, `_MEM` | logging of the emulated hardware |
| `NOMA_DEBUG_NET`, `NOMA_DEBUG_DRAWS=N` | the guest's (failing) socket requests; every draw of the Nth frame (bounds, TEV, textures, matrices) |
| `NOMA_TRACE=word,...`, `NOMA_TRACE_LAST=N`, `NOMA_WATCH=addr` | call tracing and a memory write watch (build with `-DNOMA_TRACE_CALLS=ON`) |
| `NOMA_TRACE_BT=name,addr` | trace builds: the host stack (the guest's call chain) whenever those functions are entered |
| `NOMA_TRACE_OPEN=/dev/...` | print the last traced calls when the guest opens that IOS path (trace builds) |
| `NOMA_DUMP_MEM=1`, `NOMA_DUMP_MEM_OPEN=/dev/...` | write MEM1/MEM2 to `mem1.bin`/`mem2.bin` on exit, or whenever the guest opens that path |
| `NOMA_SELFTEST=all` (or `aes`, `printf`, `math`, `strtol`, `int64`, `time`, `rfl`, `mtx`, `ax`, `cx`, `fill`, `crc`, `stack`, `efb`, `tex`, `g3d`, `g3drender`, `input`) | run decompiled library code on reference vectors, and the natives against the code they replace, then exit (host): 0 when every test passed or was skipped for want of its data, 1 when one failed (its line says MISMATCH or FAILED), 2 for an unknown test; the last line counts them |
| `NOMA_SELFTEST=walk` with `NOMA_KCL_DIR=.../StageArc.carc/Island` | Wuhu Island's collision, walking and camera on Wii Sports Resort's own collision files (from your extraction): scripted walks (a wall, the fountain, the volcano trail, a tree, the shore, the edge of a drop too deep to take, a ledge low enough to drop from, a slope too steep to stand on from above and from on it, the suspension bridge, a tapped and a held jump, a run out to sea and a swim back to the shore) and 25 minutes of random walking and swimming at two Mii sizes, every tick checked (on the ground, in no wall, no deep water unless swimming, a swimmer at a water surface with no land over it, the camera's view clear), and the queries timed; `NOMA_KCL_GROUND=dir` also checks the ground against research rasters, `NOMA_KCL_TRUNKS=file.csv` adds tree trunks, `NOMA_WALK_TRAIL=file.csv` the volcano trail's waypoints, `NOMA_WALK_K` / `NOMA_WALK_K2` set the Mii's scale (10 and 9), `NOMA_WALK_OUT=dir` writes the paths (`tools/wuhu/plot_walks.py` draws them) (host) |
| `NOMA_SELFTEST=vehicles` with `NOMA_G3D_PACKAGE=.../wuhu/island.ng3d` | Wuhu Island's cars and boats (`engine/wuhu/vehicles.h`) on the staged package and its collision: each taken and driven (ahead, ahead steering, back), every tick checked (a car on drivable ground, a boat on deep water with no land over it, no new overlap with another vehicle), and a way out beside each (host) |
| `NOMA_SELFTEST=g3drender` with `NOMA_G3D_PACKAGE=.../wuhu/island.ng3d` | the host's software backend for native draws (`engine/g3d/soft_backend`): its depth against the GX path's on random triangles (f32 positions, then compact ones), then each `view_*` camera of the package drawn (both passes, under the GX state of a 640x456 frame) to `NOMA_G3D_OUT` (PNGs; default `g3drender`), timed, checked through the GX stream, compared with `tools/g3d/preview_views.py`'s renders of the same cameras when `NOMA_G3D_REF` names their directory (PSNR, pixels off, a difference picture), and the overlays drawn again under a range of depth biases (`NOMA_G3D_FOV`, `NOMA_G3D_FRAME`, `NOMA_G3D_VIEWS`; the shadow map `NOMA_G3D_SHADOW_MAP` texels a side about each view's target, 512 by default, 0 none; `NOMA_G3D_CAMERA=ex,ey,ez,tx,ty,tz` puts the views' camera elsewhere) (host) |
| `NOMA_SOFT3D_THREADS=N` | host: the threads the software backend draws native draws (the island) with (default: as many as the machine has, at most 8) |
| `NOMA_SOFT3D_SHADOW_DUMP=file.pgm` | host: the software backend writes each shadow map it draws to the file (white: nothing; darker: nearer the sun) |
| `NOMA_DUMP_AUDIO=file.wav`, `NOMA_DEBUG_AX` | record what the audio DMA plays; AX mixer statistics |
| `NOMA_WUHU_START=name`, `NOMA_WUHU_AT=x,y,z[,yaw]`, `NOMA_WUHU_LOG_EVERY=N`, `NOMA_WUHU_PLACEHOLDER=1` | Wuhu Island: start at another of the package's markers (a landmark, a warp place, or a view preset such as `view_town`, the Mii then standing where it looks, the camera behind), or on the ground at or below a place of one's own (island units, facing yaw degrees); the frame's line (fps, the World's milliseconds a frame and, with the helper core, the game's waits for it, what was drawn -- cells after the visibility rows, instances posed, items, details culled, triangles, combiner translations --, where the Mii is) every N frames instead of 600; the scene's shell over a sky-blue stand-in, without the island's package (`NOMA_WUHU_LOAD_MS` its loading time) |
| `NOMA_WUHU_BY_FRAMES=1` | Wuhu Island: the Mii, the camera and the World's animations move 1/60 s a game frame, as before they went by the time a frame takes (the display's frames since the last, at most 3): repeatable host runs |
| `NOMA_G3D_STATS=N` | 3DS: every N frames the island's native draws: draws, prelit draws, triangles, the PICA backend's milliseconds a frame (and its worst), material and matrix changes, uniform vectors and command words a frame, items not drawn, what is resident and the linear heap left; the shadow map's casters, their triangles, the milliseconds of its pass, the receiving items and the decals faded a frame |
| `NOMA_WUHU_SHADOWS=0`, `NOMA_WUHU_SHADOW_MAP=N` | Wuhu Island: no shadow map (the trees' and props' silhouette decals and the cars' and the Mii's blobs stay); its size, N texels a side (512 by default; 256 and 1024 to try on the console) (docs/wuhu.md "Shadows") |
| `NOMA_WUHU_SPEED_OFF=pvs,detail,anim,sort` | Wuhu Island: the named speed-ups of the island's World off (`engine/g3d/world.h` WorldFeature: the cells' visibility rows, detail batches culled by distance, far props animated at a lower rate, the opaque items grouped by material; `all`), for comparisons on the console |
| `NOMA_WUHU_LOAD_MS=ms`, `NOMA_WUHU_TEST_PACKAGE=file.ng3d` | Wuhu Island's stand-in content (`NOMA_WUHU_PLACEHOLDER=1`): how long it "loads" (1500 by default), and a package whose warp markers (kind 3) the exploration menu lists, a warp then giving the stand-in's sky another colour (tests of the scene's shell) |
| `NOMA_WUHU_MUSIC_TEST=N,M[,L]` | the island's music without the island: at game time N s the room's music pauses, L s later (1 if not given: the island loading) 「広い世界へ」 plays, M s after that the room's theme comes back; the BGM player's state logged (`tools/music/dump_check.py` reads a recording of it) |
| `NOMA_SLOWDOWN=us`, `NOMA_FS_WRITE_DELAY_US=us`, `NOMA_READ_MBPS=N` | slow guest code / file writes / content reads (to N MB/s) down to reproduce 3DS timing on the host |
| `NOMA_IO_INLINE=1` | read the channel's contents and do the file system's requests on the CPU's thread instead of the I/O thread |
| `NOMA_CORES=single`, `=multi` | 3DS: where the work beside the game runs. Unset, on a real New 3DS the AX mixing has a thread on the system core (a `.3dsx`: on the third core) and Wuhu Island's World makes its frame on the third core (`NOMA_JOB_INLINE=1`: not that); in an emulator (Azahar, Citra: recognised) and on an Old 3DS both stay on the game's thread, as Azahar runs the cores one after another. `single`: on the game's thread anywhere; `multi`: on their threads on any New 3DS, an emulator's too (an Old 3DS has no third core for them), and on the host (to test those paths). The GPU's thread is the same in all |
| `NOMA_AX_THREAD=0`, `=1`, `=2` | 3DS: the AX mixing on the game's thread, or on a thread of its own on core 1 (the system core) or 2, whatever `NOMA_CORES` says; with a thread the program doesn't give it up for mixing on the game's thread when the game waits for it longer than the mixing takes (unset, it does) (host: any value but 0 gives it a thread) |
| `NOMA_LOAD_STATS=ms` | log every frame longer than `ms` with what it did (threads, reads by file, decompression, file system, allocations; profile builds: its top functions), each loading burst, and the frame-time distribution every 10 s (`NOMA_LOAD_STATS_READS=1`: every content read; `NOMA_LOAD_STATS_JOBS=1`: every I/O job of 5 ms or more) |
| `NOMA_DEBUG_CONSOLE=1` | 3DS: the log and the status area on the bottom screen while no screen of the channel's uses it (otherwise it is white, and only a fatal error is written there) |
| `NOMA_STATUS_LOG=s` | 3DS: the status area's numbers written to the log every `s` seconds (default 10; `0`: never), averaged over them: `status:` lines with frames a second, the CPU's and the waits' milliseconds a frame, the GPU's last frame, the AX mixing and its core, the heap |
| `NOMA_OLD3DS=1` | an Old 3DS's controls anywhere (no ZL, ZR or C-stick, also for host scripts) and, on a New 3DS, its layout and clock: the GPU's thread on the system core, nothing on the third core, 268 MHz without the L2 cache, CMPR textures as ETC1 (as near as a New 3DS comes to an Old one; not its memory) |
| `NOMA_DUMP_3DS=1` | host, with `NOMA_DUMP_FRAMES`: each saved frame also as the 3DS's two screens, `frames/NNNNN_3ds.png` (400x480): the frame on the top screen as the 3DS maps it, and the bottom screen with the touch (a filled square) and the pointer (crosses, on both screens) |

On the 3DS, the same switches go in `sdmc:/3ds/noma/env.txt` as `KEY=VALUE` lines (including
`NOMA_INPUT`, for unattended runs in an emulator); there `NOMA_SHOT_EVERY=N` also saves every
Nth frame as BMPs in `sdmc:/3ds/noma/shots`: the top screen as `NNNNN.bmp`, the bottom screen
beside it as `NNNNN_bottom.bmp`.

## Controls on the 3DS

The 3DS stands in for a Wii Remote pointed at the screen: touch the bottom screen (or use the
circle pad) to point, A/B as A/B, X = 1, Y = 2, START = +, SELECT = −, the D-pad as the D-pad.
Left alone for 3 seconds, the pointer leaves the screen as a remote put down would, so nothing
stays highlighted; the next touch brings it back.
At the start, A or a tap skips the rest of the opening ([docs/two_screens.md](docs/two_screens.md) has how the
two screens are put together). In the room the top screen shows the room alone (no bar, no hand) and the bottom one the room's
buttons, the channel's own, worked by touch (`engine/room/panel.h`): the bar along the bottom edge
as the channel has it (the way back, ホームシアター, サウンド, 出発) and the その他 column's buttons two
by two above it (メッセージボード, メッセージを送る, 使い方, 設定). The way back asks the room's question
(Wiiの間をおわります) on the bottom screen; B there is いいえ. The screens those buttons lead to that
are still the channel's own (the message board, sending a message, 使い方, the options, the theater)
are made for a 4:3 screen and shown whole on the bottom screen, worked by touch, the top screen
white meanwhile, and the first start's setup the same way; the channel's dialogs (its system prompt: errors, the save erase's question)
show on the bottom screen over any scene, and the room's wipes cover both screens. The Wii
Remote's pointer (the hand) isn't drawn for the touch (what is touched is pointed at); the circle
pad still moves the pointer about the bottom screen, the hand showing then, so every screen also
works with the buttons alone (the circle pad and A).
The game still reads a Wii Remote (KPAD), but nothing of one is emulated: `engine/input/remote.cpp`
answers KPAD and WPAD with the 3DS's controls, so the Wii's Bluetooth stack never starts, and
there's no remote speaker or rumble.
The Wii's HOME Button Menu is not part of the port (the 3DS's own HOME button suspends the app):
`engine/system/home_button.cpp` keeps it from loading or opening. Closing the app from the HOME
Menu presses the Wii's power button for the game, which ends as it does on the Wii (a save in
progress finishes first); the app ends on its own if the game hasn't within 10 seconds.
On Wuhu Island the top screen shows the island and the bottom one its Home tab, the exploration
menu, the places to warp to and the question about going back, which the touch screen works
directly. The circle pad walks the Mii (tilted more than about half way, it runs); B held sprints
(ZR too on a New 3DS), and a sprint off a ledge glides down under the Skydiving parachute; A jumps
(a tap a short hop, held a higher one). Walked on into deep water (or off a low edge into it) the
Mii swims, and wades or climbs out where the shore allows. Near a parked car or a boat at anchor (a
boat from the water too), X (the Wii Remote's 1) gets in and out again; in it the circle pad's up
and down speed up and back, its left and right steer. The C-stick (New 3DS) or L and R turn the
camera round the Mii, the D-pad tilts the camera or brings it in and out, and Y brings it round
behind the Mii. START or the Home tab open the exploration menu (部屋にもどる / ワープ / 探検をつづける:
the D-pad or the circle pad and A, or touch; START closes it), whose ワープ lists the places to go,
and B backs out of each.

## The Sound Player

A screen of the port's own for listening to the channel's music: the room's サウンド (on the
bottom screen, right of the theater's) opens it, through the room's own circle wipe, and the Home
tab at the bottom left (or B) goes back to the room, whose music comes back by itself.

- The tracks are the channel's own, found in its sound archive (`snd/douga.brsar`, which the
  channel keeps in memory): the 43 pieces of music (the sequences its BGM player plays, labelled
  `SEQ_BGM_*`; the silent placeholder and the one filed twice left out), then 17 jingles (company
  logos, the Miis' feelings, the 5 pm chime). The archive has only labels; the list's names are
  those put into words. They play through the channel's own BGM player (`sound::BGMManager`),
  which crossfades between pieces and keeps the room's theme paused until the room asks for it
  again.
- After them, if staged (`tools/music/stage_music.py`), the music of Wii Sports and Wii Sports
  Resort (and of the disc's menu), under a heading a game and one for each game's jingles: 162
  pieces (the streams, the fanfares and jingles, Resort's two sequences played through
  `tools/music`'s model of the sequencer; pieces the disc has twice listed once), named in
  Japanese as the channel's are. They play as native streams (`engine/audio/stream.h`: NSTM
  files, 32 kHz DSP-ADPCM, read from the SD card a frame ahead and mixed into the AX output), each
  brought to the same loudness, with a tempo found from its onsets for the dances. L and R jump
  from heading to heading.
- On the bottom screen: the list, in the channel's list window with the message board's rows (the
  room's cyan for focus), four rows at a time (dragged with the stylus, the tabs on its edges,
  held, or the D-pad), the
  video player's own controls (previous, play/pause, next, stop) and the Home tab, worked by touch
  (a tap presses what it is on). The track playing has a cyan bar with ♪ (grey with ∥ when
  paused). On the top screen: the title over the stage.
- The household's Miis dance at a table in the TV programme's white studio, an amp beside them,
  every family member (one to eight) placed so that none hides another. The dances are the
  channel's own (the TV caster's, the DSi relay's, the messages' cheers), their frames set from
  the music's sequencer every frame, so the moves land on the beat at any frame rate, and change
  every 8 bars (of 3 beats in the island's waltz, of 4 in the channel's pieces); paused or
  stopped, the Miis wait calmly. The amp is drawn by the port (no such model exists in the
  channel), its speaker and light pulsing on the beat.
- Buttons: the D-pad's up and down move the list's highlight and A plays it, left and right play
  the previous and next track, START pauses and resumes, SELECT stops, B goes back.

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
  Sound Player), `wuhu/` (walkable Wuhu Island, in progress: `docs/wuhu_design.md`; the bar's 出発
  button, the scene and its exploration menu, the island's loading, the Mii walking it, its collision
  and camera).
- `runtime/` — the Wii below the program: memory, threads, interrupts, CPU intrinsics,
  devices (`hw/`: PI, VI, DSP, AI, EXI, SI, IPC, GX in `hw/gx/`), IOS (`hw/ios/`: file system,
  ES, USB HID with nothing plugged in, ...); `patches/` changes what
  some decompiled functions do
  (offline mode, debugging aids; the original stays as `<name>_orig`), `profile.cpp` the
  sampling profiler.
- `platform/host`, `platform/3ds` — entry points, presentation, input; `gx_pica.cpp` is the GX
  renderer on the 3DS GPU.
- `tools/` — WAD extraction, signature inference, the decompilation driver.
- `tools/music/` — the port's own music: the island's score (`hiroi_sekai.py`: 「広い世界へ」, a
  stately 3/4 at 108 bpm on the channel's orchestral instruments, D major rising to E), an RSEQ
  assembler, disassembler and score compiler (`rseq.py`; `rseq.py header` makes
  `engine/wuhu/hiroi_sekai_seq.h`), a reader of the channel's sound archive and a model of its
  sequencer (`brsar.py`), a preview renderer with the channel's own instruments (`sndrender.py`),
  checks of a score and of a recording of the port playing it (`check.py`, `dump_check.py`). They
  read the channel's `snd/douga.brsar` from your own extraction.
- `config/HCIJ` — dtk configuration and symbols.
