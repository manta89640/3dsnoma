# Wii no Ma for Nintendo 3DS: the decompiled program (build/decomp, made by
# tools/decomp/decompile.py from your own WAD) on top of the Wii runtime in runtime/.
#
#   host (macOS/Linux, no graphics, for debugging):
#     cmake -S . -B build-host -G Ninja && cmake --build build-host
#   3DS:
#     cmake -S . -B build-3ds -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/platform/3ds/cmake/Toolchain-3DS.cmake
#     cmake --build build-3ds
cmake_minimum_required(VERSION 3.20)
project(noma C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# nothing reads errno after a math function: sqrt is then one instruction, without a compare
# that drains the 3DS's VFP pipeline and a call to set errno
add_compile_options(-fno-math-errno)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(DECOMP_DIR ${CMAKE_SOURCE_DIR}/build/decomp)
if(NOT EXISTS ${DECOMP_DIR}/include/decl.h)
  message(FATAL_ERROR "No decompiled sources: run tools/decomp/decompile.py first (see README)")
endif()

# ---- the decompiled program ----
file(GLOB DECOMP_SOURCES CONFIGURE_DEPENDS ${DECOMP_DIR}/src/*.cpp)
add_library(noma_decomp STATIC ${DECOMP_SOURCES})
target_include_directories(noma_decomp PUBLIC ${CMAKE_SOURCE_DIR}/runtime ${DECOMP_DIR}/include)
# the decompiled code is built at -O1; -DNOMA_DECOMP_OPT=-O2 makes 4% less code on the 3DS, but
# what its instruction scheduling gains on the ARM11 doesn't show in Azahar: it needs hardware
set(NOMA_DECOMP_OPT -O1 CACHE STRING "optimisation level of the decompiled code (not in Debug builds)")
# -ftrivial-auto-var-init=zero: m2c leaves a variable unset on paths where the PowerPC code
# reads a register it never wrote (a paired single's unused half, say); clang treats such a read
# as undefined and dropped the guard of nw4r::math::MTX34RotXYZFIdx's angle wrap on the host
target_compile_options(noma_decomp PRIVATE -w -fno-strict-aliasing -fwrapv -fno-finite-loops
  -ftrivial-auto-var-init=zero
  $<IF:$<CXX_COMPILER_ID:GNU>,-Wno-narrowing,-Wno-c++11-narrowing>
  $<$<NOT:$<CONFIG:Debug>>:${NOMA_DECOMP_OPT}>)
# no debug information for the decompiled code on the 3DS: GCC spends four fifths of its time on
# it (8.4 s instead of 1.8 s a file) and nothing uses it (the packages are stripped; crash reports
# and the profiler name functions from the symbol table)
if(NINTENDO_3DS)
  target_compile_options(noma_decomp PRIVATE -g0)
endif()
target_precompile_headers(noma_decomp PRIVATE ${CMAKE_SOURCE_DIR}/runtime/noma_decomp.h)
option(NOMA_TRACE_CALLS "log function entries (NOMA_TRACE=...)" OFF)
if(NOMA_TRACE_CALLS)
  add_compile_definitions(NOMA_TRACE_CALLS=1)
endif()
option(NOMA_PROFILE_CALLS "keep function stacks for the sampling profiler (NOMA_PROFILE=seconds)" OFF)
if(NOMA_PROFILE_CALLS)
  add_compile_definitions(NOMA_PROFILE_CALLS=1)
endif()

# ---- the engine: native 3DS code replacing the program's SDK functions ----
set(ENGINE_SOURCES
  engine/audio/aux.cpp
  engine/audio/ax_render.cpp
  engine/collision/kcl.cpp
  engine/cw/int64.cpp
  engine/cx/uncompress.cpp
  engine/g3d/anim.cpp
  engine/g3d/g3d.cpp
  engine/g3d/material.cpp
  engine/g3d/native_draw.cpp
  engine/g3d/package.cpp
  engine/g3d/texture.cpp
  engine/g3d/world.cpp
  engine/input/remote.cpp
  engine/lyt/lyt.cpp
  engine/lyt/quad.cpp
  engine/lyt/material.cpp
  engine/gx/gx_sdk.cpp
  engine/gx/tev_combiner.cpp
  engine/lyt/textbox.cpp
  engine/math/libm.cpp
  engine/math/mtx.cpp
  engine/math/nw4r_math.cpp
  engine/math/tables.cpp
  engine/os/crc.cpp
  engine/os/interrupts.cpp
  engine/os/mutex.cpp
  engine/os/time.cpp
  engine/snd/ax_voice.cpp
  engine/snd/channel.cpp
  engine/snd/seq.cpp
  engine/snd/tables.cpp
  engine/snd/voice.cpp
  engine/soundplayer/amp.cpp
  engine/soundplayer/bar_button.cpp
  engine/soundplayer/catalog.cpp
  engine/soundplayer/controls.cpp
  engine/soundplayer/music.cpp
  engine/soundplayer/scene.cpp
  engine/soundplayer/stage.cpp
  engine/soundplayer/track_list.cpp
  engine/system/allocator.cpp
  engine/system/home_button.cpp
  engine/system/host_scene.cpp
  engine/system/keyboard.cpp
  engine/system/thread.cpp
  engine/ui/bar_button.cpp
  engine/ui/caption.cpp
  engine/ui/confirm.cpp
  engine/ut/font.cpp
  engine/wuhu/bar_button.cpp
  engine/wuhu/content_placeholder.cpp
  engine/wuhu/follow_camera.cpp
  engine/wuhu/music.cpp
  engine/wuhu/player.cpp
  engine/wuhu/scene.cpp
)

# ---- the Wii runtime (memory, threads, interrupts, hardware and IOS emulation) ----
set(RUNTIME_SOURCES
  runtime/boot.cpp
  runtime/cpu.cpp
  runtime/dispatch.cpp
  runtime/irq.cpp
  runtime/load_stats.cpp
  runtime/memory.cpp
  runtime/natives.cpp
  runtime/threads.cpp
  runtime/patches/debug.cpp
  runtime/patches/graphics.cpp
  runtime/patches/heap.cpp
  runtime/patches/menus.cpp
  runtime/patches/offline.cpp
  runtime/patches/players.cpp
  runtime/patches/profile.cpp
  runtime/patches/vf.cpp
  runtime/patches/wii_parts.cpp
  runtime/patches/wiilink.cpp
  runtime/patches/wipes.cpp
  runtime/hle/report.cpp
  runtime/trace.cpp
  runtime/profile.cpp
  runtime/hw/devices.cpp
  runtime/hw/dsp.cpp
  runtime/hw/dsp_hle.cpp
  runtime/hw/dsp_ax.cpp
  runtime/mii.cpp
  runtime/hw/ios/input_script.cpp
  runtime/hw/exi.cpp
  runtime/hw/gx/cp.cpp
  runtime/hw/gx/fifo.cpp
  runtime/hw/gx/pica_vsh.cpp
  runtime/hw/gx/raster.cpp
  runtime/hw/gx/texture.cpp
  runtime/hw/gx/tnl.cpp
  runtime/hw/gx/vertex.cpp
  runtime/hw/gx/xform.cpp
  runtime/hw/gx/xfb.cpp
  runtime/hw/ipc.cpp
  runtime/hw/mmio.cpp
  runtime/hw/vi.cpp
  runtime/hw/ios/di.cpp
  runtime/hw/ios/es.cpp
  runtime/hw/ios/fs.cpp
  runtime/hw/ios/ios.cpp
  runtime/hw/ios/net.cpp
  runtime/hw/ios/usb_hid.cpp
  third-party/libco/libco.c
)

# the GPU emulation does per-pixel work: optimise it even in debug builds
set_source_files_properties(runtime/hw/gx/raster.cpp runtime/hw/gx/texture.cpp
  runtime/hw/gx/vertex.cpp runtime/hw/gx/xform.cpp runtime/hw/gx/cp.cpp runtime/hw/gx/tnl.cpp
  engine/g3d/soft_backend.cpp
  PROPERTIES COMPILE_OPTIONS -O2)

if(NINTENDO_3DS)
  # the GX renderer's vertex shader, assembled and embedded
  find_program(PICASSO picasso HINTS ${DEVKITPRO}/tools/bin ${CTRULIB}/../tools/bin REQUIRED)
  set(GX_SHBIN ${CMAKE_CURRENT_BINARY_DIR}/gx.shbin)
  set(GX_SHBIN_C ${CMAKE_CURRENT_BINARY_DIR}/gx_shbin.c)
  add_custom_command(
    OUTPUT ${GX_SHBIN_C}
    COMMAND ${PICASSO} -o ${GX_SHBIN} ${CMAKE_SOURCE_DIR}/platform/3ds/shaders/gx.v.pica
    COMMAND ${CMAKE_COMMAND} -DIN=${GX_SHBIN} -DOUT=${GX_SHBIN_C} -DNAME=gx_shbin
            -P ${CMAKE_SOURCE_DIR}/platform/3ds/cmake/bin2c.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/platform/3ds/shaders/gx.v.pica ${CMAKE_SOURCE_DIR}/platform/3ds/cmake/bin2c.cmake
    COMMENT "Assembling gx.v.pica"
    VERBATIM)
  # memcpy/memmove/memset for the ARM11 (LDM/STM, PLD): devkitARM's newlib has the generic C ones
  enable_language(ASM)
  add_executable(noma ${ENGINE_SOURCES} ${RUNTIME_SOURCES} platform/3ds/main.cpp platform/3ds/gx_pica.cpp
                 platform/3ds/mem_arm11.S ${GX_SHBIN_C})
  target_include_directories(noma PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/runtime
    ${CMAKE_SOURCE_DIR}/platform/3ds ${DECOMP_DIR}/include ${CTRULIB}/include)
  target_link_libraries(noma noma_decomp citro3d ctru m)
else()
  add_executable(noma-host ${ENGINE_SOURCES} ${RUNTIME_SOURCES} platform/host/main.cpp platform/host/png.cpp
                           platform/host/selftest.cpp platform/host/selftest_g3d.cpp platform/host/selftest_walk.cpp
                           engine/g3d/package_writer.cpp
                           engine/g3d/soft_backend.cpp platform/host/selftest_g3drender.cpp)
  target_include_directories(noma-host PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/runtime ${DECOMP_DIR}/include)
  target_link_libraries(noma-host noma_decomp)
  # ng3d-bake: a package's GX textures in their PICA formats, offline (tools/ng3d_bake.cpp)
  add_executable(ng3d-bake tools/ng3d_bake.cpp engine/g3d/package.cpp engine/g3d/texture.cpp
                           engine/g3d/package_writer.cpp)
  target_include_directories(ng3d-bake PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/runtime)
endif()
